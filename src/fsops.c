#include "fsops.h"
#include "compat.h"
#include "dlog.h"
#include "hash.h"
#include "zip.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define COPY_BUF (4 * 1024 * 1024)

Job g_job;
char *search_results[SEARCH_MAX];
uint64_t search_sizes[SEARCH_MAX];
int search_count;
static int skip_existing;   /* JOB_SYNC: don't recopy files of the same size */
static char **job_src;
static int job_nsrc;
static char job_dst[1024];
static char job_usb[16];      /* "/mnt/usbN" when the job writes to a USB drive, else "" */
static char *copybuf;

int path_join(char *out, int n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    int w = (l && dir[l - 1] == '/') ? snprintf(out, n, "%s%s", dir, name)
                                     : snprintf(out, n, "%s/%s", dir, name);
    /* a truncated path is a DIFFERENT path (e.g. ".../abcdef" -> ".../abc"): callers that
       delete or overwrite must check this and skip the item */
    return (w < 0 || w >= n) ? -1 : 0;
}

const char *path_base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int is_dir(const char *path)
{
    struct kstat st;
    return k_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

void fmt_size(char *out, int n, uint64_t b)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    if (i == 0) snprintf(out, n, "%llu B", (unsigned long long)b);
    else snprintf(out, n, "%.2f %s", v, u[i]);
}

static void set_err(const char *what, const char *path)
{
    g_job.errors++;
    snprintf(g_job.err, sizeof g_job.err, "%s failed: %s (%s)", what, path_base(path), strerror(errno));
    dlog("job error: %s failed: %s (%s)", what, path, strerror(errno));
}

static void measure(const char *path, uint64_t *bytes, int *files)
{
    struct kstat st;
    if (k_lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *e;
        char sub[1024];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (path_join(sub, sizeof sub, path, e->d_name)) continue;   /* cut path = another file */
            measure(sub, bytes, files);
        }
        closedir(d);
    } else {
        *bytes += st.st_size;
    }
    (*files)++;
}

/* Copies into "<dst>.orbiskit-tmp" and renames it over dst only when every byte was written,
 * so a full disk or a pulled USB stick never destroys a file that was already at dst (sync).
 * The temp name is OrbisKit's own (a user's "x.part" must never be overwritten), and it is
 * created with O_EXCL; one found there is a leftover from an interrupted copy. */
#define TMP_SUFFIX ".orbiskit-tmp"
static int copy_file(const char *src, const char *dst)
{
    char part[1040];
    if (snprintf(part, sizeof part, "%s" TMP_SUFFIX, dst) >= (int)sizeof part) {
        errno = ENAMETOOLONG;
        set_err("Create", dst);
        return -1;
    }
    /* O_NONBLOCK: a FIFO must not block open() forever (cancel couldn't reach it) */
    int in = open(src, O_RDONLY | O_NONBLOCK, 0);
    if (in < 0) { set_err("Open", src); return -1; }
    struct kstat is;
    if (k_fstat(in, &is) != 0 || !S_ISREG(is.st_mode)) {   /* swapped since the lstat */
        errno = EINVAL; set_err("Copy (not a regular file)", src); close(in); return -1;
    }
    fcntl(in, F_SETFL, 0);
    int out = open(part, O_WRONLY | O_CREAT | O_EXCL, 0777);
    if (out < 0 && errno == EEXIST && unlink(part) == 0)
        out = open(part, O_WRONLY | O_CREAT | O_EXCL, 0777);
    if (out < 0) { set_err("Create", dst); close(in); return -1; }
    int rc = 0;
    for (;;) {
        if (g_job.cancel) { rc = -1; break; }
        ssize_t r = read(in, copybuf, COPY_BUF);
        if (r < 0) { set_err("Read", src); rc = -1; break; }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, copybuf + off, r - off);
            if (w <= 0) { if (w == 0) errno = ENOSPC; set_err("Write", dst); rc = -1; break; }
            off += w;
        }
        if (rc) break;
        g_job.bytes_done += r;
    }
    close(in);
    /* exFAT/USB write errors can show up only when the file is closed */
    if (close(out) != 0 && !rc) { set_err("Write", dst); rc = -1; }
    if (!rc && rename(part, dst) != 0) { set_err("Rename", dst); rc = -1; }
    if (rc) unlink(part);
    return rc;
}

/* The USB drive the job writes to was pulled: stop the whole job with a clear message
 * instead of walking the rest of the source tree and failing every file. */
static int usb_gone(void)
{
    if (!job_usb[0] || is_mount_point(job_usb)) return 0;
    g_job.cancel = 1;
    g_job.errors++;
    snprintf(g_job.err, sizeof g_job.err, "USB drive removed - job stopped");
    return 1;
}

static int copy_rec(const char *src, const char *dst)
{
    if (g_job.cancel) return -1;
    if ((g_job.files_done & 63) == 0 && usb_gone()) return -1;
    struct kstat st;
    if (k_lstat(src, &st) != 0) { set_err("Stat", src); return -1; }
    snprintf(g_job.current, sizeof g_job.current, "%s", src);
    int rc = 0;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0777) != 0) {
            struct kstat dt;
            if (errno != EEXIST) { set_err("Mkdir", dst); return -1; }
            /* an existing symlink would send the copy somewhere else; a file can't take children */
            if (k_lstat(dst, &dt) != 0 || !S_ISDIR(dt.st_mode)) {
                errno = EEXIST; set_err("Mkdir (not a folder there)", dst); return -1;
            }
        }
        DIR *d = opendir(src);
        if (!d) { set_err("Opendir", src); return -1; }
        struct dirent *e;
        char s2[1024], d2[1024];
        while ((errno = 0, e = readdir(d)) && !g_job.cancel) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (path_join(s2, sizeof s2, src, e->d_name) || path_join(d2, sizeof d2, dst, e->d_name)) {
                errno = ENAMETOOLONG;
                set_err("Path too long", s2);
                rc = -1;
                continue;
            }
            if (copy_rec(s2, d2)) rc = -1;   /* keep going, report error */
        }
        /* readdir() returns NULL for errors too (e.g. the USB stick was pulled): then the copy
           is incomplete, and a move must not delete the source */
        if (!e && errno) { set_err("Read folder", src); rc = -1; }
        closedir(d);
    } else {
        struct kstat dt;
        int regular = S_ISREG(st.st_mode);
        if (S_ISLNK(st.st_mode)) {                     /* copy what a link points to, if a file */
            struct kstat ts;
            regular = k_stat(src, &ts) == 0 && S_ISREG(ts.st_mode);
            st.st_size = regular ? ts.st_size : 0;
        }
        if (!regular) {
            /* FIFOs, sockets and devices (/dev/zero would fill the disk): skip */
            errno = EINVAL;
            set_err("Skipped special file", src);
            rc = -1;
        } else if (skip_existing && k_lstat(dst, &dt) == 0 && S_ISREG(dt.st_mode) && dt.st_size == st.st_size) {
            /* already synced: nothing to transfer, so take it out of the total
               (counting it as done would make the speed and time left wrong) */
            uint64_t sz = (uint64_t)st.st_size;
            g_job.bytes_total = g_job.bytes_total > sz ? g_job.bytes_total - sz : 0;
        } else {
            rc = copy_file(src, dst);
            if (rc) usb_gone();
        }
    }
    g_job.files_done++;
    return rc;
}

/* ---------- search ---------- */
static const char *ci_strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++)
        if (!strncasecmp(h, n, nl)) return h;
    return NULL;
}

/* Keeps the BIGFILES_MAX largest files, sorted largest first. */
static void bigfiles_rec(const char *path)
{
    if (g_job.cancel) return;
    if (!strcmp(path, "/dev") || !strcmp(path, "/proc")) return;
    DIR *d = opendir(path);
    if (!d) return;
    snprintf(g_job.current, sizeof g_job.current, "%s", path);
    struct dirent *e;
    char sub[1024];
    while ((e = readdir(d)) && !g_job.cancel) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (path_join(sub, sizeof sub, path, e->d_name)) continue;
        struct kstat st;
        if (k_lstat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { bigfiles_rec(sub); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        g_job.files_done++;
        g_job.bytes_total += st.st_size;
        uint64_t sz = st.st_size;
        if (search_count == BIGFILES_MAX && sz <= search_sizes[BIGFILES_MAX - 1]) continue;
        int pos = search_count < BIGFILES_MAX ? search_count : BIGFILES_MAX - 1;
        if (search_count == BIGFILES_MAX) free(search_results[pos]);   /* drop the smallest */
        else search_count++;
        while (pos > 0 && search_sizes[pos - 1] < sz) {
            search_results[pos] = search_results[pos - 1];
            search_sizes[pos] = search_sizes[pos - 1];
            pos--;
        }
        search_results[pos] = strdup(sub);
        search_sizes[pos] = sz;
        g_job.files_total = search_count;
    }
    closedir(d);
}

static void search_rec(const char *path, const char *query)
{
    if (g_job.cancel || search_count >= SEARCH_MAX) return;
    if (!strcmp(path, "/dev") || !strcmp(path, "/proc")) return;
    DIR *d = opendir(path);
    if (!d) return;
    snprintf(g_job.current, sizeof g_job.current, "%s", path);
    struct dirent *e;
    char sub[1024];
    while ((e = readdir(d)) && !g_job.cancel && search_count < SEARCH_MAX) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (path_join(sub, sizeof sub, path, e->d_name)) continue;
        g_job.files_done++;
        if (ci_strstr(e->d_name, query)) {
            char *dup = strdup(sub);
        if (dup) search_results[search_count++] = dup;
            g_job.files_total = search_count;
        }
        struct kstat st;
        if (k_lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) search_rec(sub, query);
    }
    closedir(d);
}

/* dev = the filesystem the delete started on. A folder on another one is a mount point (a USB
 * drive, a mounted game image, a nullfs view of system data): it is skipped, never emptied. */
static int delete_rec_dev(const char *path, uint32_t dev)
{
    if (g_job.cancel) return -1;
    struct kstat st;
    if (k_lstat(path, &st) != 0) { set_err("Stat", path); return -1; }
    snprintf(g_job.current, sizeof g_job.current, "%s", path);
    int rc = 0;
    if (S_ISDIR(st.st_mode) && st.st_dev != dev) {
        g_job.errors++;
        snprintf(g_job.err, sizeof g_job.err, "Skipped mounted drive: %s", path_base(path));
        dlog("delete: skipped mount point %s", path);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            char sub[1024];
            while ((e = readdir(d)) && !g_job.cancel) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (path_join(sub, sizeof sub, path, e->d_name)) {
                    /* never delete a truncated (= different) path */
                    errno = ENAMETOOLONG;
                    set_err("Path too long", sub);
                    rc = -1;
                    continue;
                }
                if (delete_rec_dev(sub, dev)) rc = -1;
            }
            closedir(d);
        }
        if (!g_job.cancel && rmdir(path) != 0) { set_err("Remove dir", path); rc = -1; }
    } else {
        if (unlink(path) != 0) { set_err("Delete", path); rc = -1; }
    }
    g_job.files_done++;
    return rc;
}

/* True if path is a folder with a different filesystem than its parent: a mount point
 * such as /mnt/usb0. Deleting it would empty the whole drive. */
/* "/mnt/usb0/" -> "/mnt/usb0". Returns -1 if too long or the last part is "." or "..":
 * then the "parent" found by cutting at the last '/' would be wrong. */
static int norm_tail(char *out, int n, const char *path)
{
    if (snprintf(out, n, "%s", path) >= n) return -1;
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    const char *b = path_base(out);
    return (!strcmp(b, ".") || !strcmp(b, "..")) ? -1 : 0;
}

int is_mount_point(const char *path_in)
{
    struct kstat st, pt;
    char path[1024], parent[1024];
    if (norm_tail(path, sizeof path, path_in)) return 1;   /* can't tell: treat as one (never deleted) */
    if (k_lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    snprintf(parent, sizeof parent, "%s", path);
    char *sl = strrchr(parent, '/');
    if (!sl) return 0;
    if (sl == parent) sl[1] = 0; else *sl = 0;
    return k_stat(parent, &pt) == 0 && pt.st_dev != st.st_dev;
}

static int delete_rec(const char *path_in)
{
    struct kstat st;
    char path[1024];
    if (norm_tail(path, sizeof path, path_in)) {
        errno = EINVAL; set_err("Delete (bad path)", path_in); return -1;
    }
    if (k_lstat(path, &st) != 0) { set_err("Stat", path); return -1; }
    if (is_mount_point(path)) {
        g_job.errors++;
        snprintf(g_job.err, sizeof g_job.err, "Can't delete a mounted drive: %s", path_base(path));
        return -1;
    }
    return delete_rec_dev(path, st.st_dev);
}

/* True if dst is src or lies inside it. Compares the folders themselves (device + inode), not
 * the path text, so "/data/./a", "//data/a" or a symlinked parent can't slip past. */
static int inside_of(const char *dst, const char *src)
{
    struct kstat ss, ds;
    if (k_stat(src, &ss) != 0 || !S_ISDIR(ss.st_mode)) return 0;
    char p[1024];
    if (snprintf(p, sizeof p, "%s", dst) >= (int)sizeof p) return 1;   /* can't tell: refuse */
    for (;;) {
        if (k_stat(p, &ds) == 0 && ds.st_dev == ss.st_dev && ds.st_ino == ss.st_ino) return 1;
        char *sl = strrchr(p, '/');
        if (!sl || sl == p) break;
        *sl = 0;
    }
    return 0;
}

/* "name" -> "name (copy)", "name (copy 2)", ... until it doesn't exist */
/* Returns -1 (and sets the job error) if no free name fits, rather than handing back a path
 * that exists or was cut short - either would overwrite something. */
static int unique_dest(char *out, int n, const char *dir, const char *name)
{
    struct kstat st;
    if (path_join(out, n, dir, name)) goto fail;
    if (k_lstat(out, &st) != 0) return 0;
    for (int i = 1; i < 1000; i++) {
        char nm[300];
        int w = i == 1 ? snprintf(nm, sizeof nm, "%s (copy)", name)
                       : snprintf(nm, sizeof nm, "%s (copy %d)", name, i);
        if (w >= (int)sizeof nm || path_join(out, n, dir, nm)) goto fail;
        if (k_lstat(out, &st) != 0) return 0;
    }
fail:
    g_job.errors++;
    snprintf(g_job.err, sizeof g_job.err, "No free name for a copy of %s", name);
    return -1;
}

static void extract_all(void)
{
    for (int i = 0; i < job_nsrc && !g_job.cancel; i++) {
        /* "archive.zip" -> folder "archive" (or "archive (copy)" if it exists) */
        char name[300], dst[1024];
        snprintf(name, sizeof name, "%s", path_base(job_src[i]));
        char *dot = strrchr(name, '.');
        if (dot && dot != name) *dot = 0;
        if (unique_dest(dst, sizeof dst, job_dst, name)) continue;
        if (mkdirs(dst) != 0) { set_err("Mkdir", dst); continue; }
        if (zip_extract(job_src[i], dst) != 0) rmdir(dst);   /* only removed if still empty */
    }
}

static void hash_file(const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { set_err("Open", path); return; }
    struct kstat st;
    if (k_fstat(fd, &st) == 0) g_job.bytes_total = st.st_size;
    g_job.files_total = 1;
    snprintf(g_job.current, sizeof g_job.current, "%s", path);
    Sha256 sha;
    sha256_init(&sha);
    uint32_t crc = 0;
    ssize_t r = 0;
    while (!g_job.cancel && (r = read(fd, copybuf, COPY_BUF)) > 0) {
        crc = crc32_update(crc, copybuf, r);
        sha256_update(&sha, copybuf, r);
        g_job.bytes_done += r;
    }
    if (r < 0) set_err("Read", path);
    close(fd);
    uint8_t d[32];
    sha256_final(&sha, d);
    snprintf(g_job.hash_crc, sizeof g_job.hash_crc, "%08X", crc);
    for (int i = 0; i < 32; i++) snprintf(g_job.hash_sha + i * 2, 3, "%02x", d[i]);
    g_job.files_done = 1;
}

static void *job_thread(void *arg)
{
    (void)arg;
    if (g_job.type == JOB_HASH) {
        hash_file(job_src[0]);
        goto done;
    }
    skip_existing = (g_job.type == JOB_SYNC);
    if (g_job.type == JOB_EXTRACT) {
        extract_all();
        goto done;
    }
    if (g_job.type == JOB_SEARCH || g_job.type == JOB_BIGFILES) {
        int old = search_count;
        search_count = 0;            /* zero first, so nothing reads entries being freed */
        for (int i = 0; i < old; i++) { free(search_results[i]); search_results[i] = NULL; }
        if (g_job.type == JOB_SEARCH) search_rec(job_src[0], job_dst);
        else bigfiles_rec(job_src[0]);
        goto done;
    }
    for (int i = 0; i < job_nsrc; i++)
        measure(job_src[i], (uint64_t *)&g_job.bytes_total, (int *)&g_job.files_total);
    if (g_job.type == JOB_SIZE) goto done;
    g_job.start_us = sceKernelGetProcessTime();   /* speed counts transfer time, not the scan */
    if (g_job.type == JOB_ZIP) {
        zip_create(job_src, job_nsrc, job_dst);
        goto done;
    }

    job_usb[0] = 0;
    if (!strncmp(job_dst, "/mnt/usb", 8) && job_dst[8] >= '0' && job_dst[8] <= '9' &&
        (job_dst[9] == '/' || job_dst[9] == 0)) {
        snprintf(job_usb, sizeof job_usb, "%.9s", job_dst);
        if (usb_gone()) goto done;   /* pulled before the job started: don't write under /mnt */
    }
    if (g_job.type != JOB_DELETE && mkdirs(job_dst) != 0) { set_err("Mkdir", job_dst); goto done; }

    for (int i = 0; i < job_nsrc && !g_job.cancel; i++) {
        const char *src = job_src[i];
        char dst[1024];
        size_t sl = strlen(src);
        while (sl && src[sl - 1] == '/') sl--;   /* "/data/a/" or "/" must still match below */

        if (g_job.type == JOB_DELETE) {
            delete_rec(src);
            continue;
        }
        /* Refuse to copy a folder into itself */
        if ((!strncmp(job_dst, src, sl) && (job_dst[sl] == '/' || job_dst[sl] == 0)) || inside_of(job_dst, src)) {
            g_job.errors++;
            snprintf(g_job.err, sizeof g_job.err, "Can't put a folder inside itself: %s", path_base(src));
            continue;
        }
        /* a truncated destination is a DIFFERENT path that could get overwritten: skip */
        if (path_join(dst, sizeof dst, job_dst, path_base(src))) {
            errno = ENAMETOOLONG;
            set_err("Path too long", src);
            continue;
        }
        if (g_job.type == JOB_SYNC) {
            copy_rec(src, dst);
        } else if (g_job.type == JOB_COPY) {
            if (unique_dest(dst, sizeof dst, job_dst, path_base(src))) continue;
            copy_rec(src, dst);
        } else { /* move */
            struct kstat st;
            int errs = g_job.errors;   /* the source is deleted only after an error-free copy */
            if (!strcmp(dst, src)) continue;
            if (k_lstat(dst, &st) == 0) {
                g_job.errors++;
                snprintf(g_job.err, sizeof g_job.err, "Already exists: %s", path_base(dst));
                continue;
            }
            if (rename(src, dst) == 0) {
                int f = 0; uint64_t b = 0;
                measure(dst, &b, &f);
                g_job.bytes_done += b;
                g_job.files_done += f;
            } else if (errno != EXDEV) {
                set_err("Move", src);       /* a real error, not another drive: don't copy */
            } else if (copy_rec(src, dst) == 0 && !g_job.cancel && g_job.errors == errs) {
                /* cross-device move: copy then delete the original */
                int fd = g_job.files_done;
                delete_rec(src);
                g_job.files_done = fd;
            } else if (!g_job.cancel) {
                snprintf(g_job.err, sizeof g_job.err, "Move failed: original kept, partial copy in %s",
                         path_base(dst));
            }
        }
    }

done:
    for (int i = 0; i < job_nsrc; i++) free(job_src[i]);
    free(job_src);
    job_src = NULL;
    g_job.finished = 1;   /* first: job_start_ex refuses while either flag is set */
    g_job.running = 0;
    return NULL;
}

int thread_start(void *(*fn)(void *), void *arg, size_t stack, int detach)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_t t;
    int r = pthread_create(&t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        dlog("thread_start: pthread_create failed (%d), stack %u", r, (unsigned)stack);
        return -1;
    }
    if (detach) pthread_detach(t);
    return 0;
}

static pthread_mutex_t job_mtx = PTHREAD_MUTEX_INITIALIZER;   /* TV and web can both start jobs */

static int job_start_ex(JobType type, char **src, int nsrc, const char *dst, int quiet);

int job_start(JobType type, char **src, int nsrc, const char *dst)
{
    return job_start_ex(type, src, nsrc, dst, 0);
}

int job_start_quiet(JobType type, char **src, int nsrc, const char *dst)
{
    return job_start_ex(type, src, nsrc, dst, 1);
}

static int job_start_ex(JobType type, char **src, int nsrc, const char *dst, int quiet)
{
    if (nsrc <= 0) return -1;
    pthread_mutex_lock(&job_mtx);
    /* also wait for the UI to have collected the previous result */
    if (g_job.running || g_job.finished || (!copybuf && !(copybuf = malloc(COPY_BUF)))) {
        pthread_mutex_unlock(&job_mtx);
        return -1;
    }
    memset(&g_job, 0, sizeof g_job);
    g_job.type = type;
    g_job.quiet = quiet;
    g_job.start_us = sceKernelGetProcessTime();
    job_src = calloc(nsrc, sizeof(char *));
    if (!job_src) { pthread_mutex_unlock(&job_mtx); return -1; }
    for (int i = 0; i < nsrc; i++) {
        if (!(job_src[i] = strdup(src[i]))) {
            while (i--) free(job_src[i]);
            free(job_src); job_src = NULL;
            pthread_mutex_unlock(&job_mtx);
            return -1;
        }
    }
    job_nsrc = nsrc;
    snprintf(job_dst, sizeof job_dst, "%s", dst ? dst : "");
    g_job.running = 1;
    if (thread_start(job_thread, NULL, STACK_JOB, 1) != 0) {
        g_job.running = 0;
        pthread_mutex_unlock(&job_mtx);
        return -1;
    }
    pthread_mutex_unlock(&job_mtx);
    return 0;
}
