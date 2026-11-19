/* Leftover package finder (ported from the OrbisSweep test app, same tested rules):
 *  - .pkg header: magic 7F 'C' 'N' 'T', content ID at 0x40 (36 chars, title ID = chars 7..15),
 *    content type at 0x74 (BE): 0x1A game/app, 0x1B/0x1C DLC, 0x1E update.
 *  - version inside the pkg: entry table (count BE @0x10, offset BE @0x18, 32-byte entries),
 *    param.sfo = entry id 0x1000 (not encrypted) -> APP_VER (or VERSION).
 *  - installed version: /system_data/priv/appmeta/<TID>/param.sfo APP_VER (includes the update).
 *  - installed: game /user/app/<TID>, update /user/patch/<TID>, DLC /user/addcont/<TID>/<label>.
 *  - leftover = installed and pkg version <= installed version (a newer pkg is kept). */
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

#include "dlog.h"
#include "fsops.h"
#include "leftovers.h"

Leftover lo_items[LO_MAX];
volatile int lo_count;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int running, cancel_req, checked, pkgs;
static char current[512];

const char *lo_type_name(int type)
{
    switch (type) {
    case 0x1A: return "Game";
    case 0x1B: case 0x1C: return "DLC";
    case 0x1E: return "Update";
    }
    return "?";
}

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* APP_VER (preferred) or VERSION from a param.sfo image, and TITLE if title is given. */
static void sfo_read(const unsigned char *s, size_t n, char *out, char *title, int titlen)
{
    out[0] = 0;
    if (title && titlen > 0) title[0] = 0;
    if (n < 0x14 || memcmp(s, "\0PSF", 4)) return;
    uint32_t keys = le32(s + 8), data = le32(s + 12), cnt = le32(s + 16);
    char ver[8] = "";
    for (uint32_t i = 0; i < cnt && i < 256; i++) {
        size_t e = 0x14 + i * 16;
        if (e + 16 > n) break;
        size_t ko = keys + le16(s + e), dofs = data + le32(s + e + 12);
        uint32_t len = le32(s + e + 4);
        if (ko >= n || dofs + 5 > n || len < 5) continue;
        const char *key = (const char *)s + ko;
        if (memchr(key, 0, n - ko) == NULL) continue;
        if (!strcmp(key, "APP_VER")) { memcpy(out, s + dofs, 5); out[5] = 0; }
        else if (!strcmp(key, "VERSION")) { memcpy(ver, s + dofs, 5); ver[5] = 0; }
        else if (title && titlen > 0 && !strcmp(key, "TITLE")) {
            size_t m = len < (uint32_t)titlen ? len : (size_t)titlen - 1;
            if (dofs + m > n) m = n - dofs;
            memcpy(title, s + dofs, m);
            title[m] = 0;
        }
    }
    if (!out[0]) snprintf(out, 8, "%s", ver);
}

static int read_at(int fd, uint64_t off, void *buf, size_t n)
{
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -1;
    return read(fd, buf, n) == (ssize_t)n ? 0 : -1;
}

/* Reads the header of a .pkg. 0 if it is a package we understand. */
int pkg_header(const char *path, char *cid, int *type, char *ver, char *title, int titlen)
{
    if (title && titlen > 0) title[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char h[0x80];
    int r = -1;
    if (read_at(fd, 0, h, sizeof h) || be32(h) != 0x7F434E54) goto out;
    memcpy(cid, h + 0x40, 36);
    cid[36] = 0;
    *type = (int)be32(h + 0x74);
    ver[0] = 0;
    uint32_t cnt = be32(h + 0x10), tab = be32(h + 0x18);
    for (uint32_t i = 0; i < cnt && i < 4096; i++) {
        unsigned char e[32];
        if (read_at(fd, tab + (uint64_t)i * 32, e, sizeof e)) break;
        if (be32(e) != 0x1000) continue;
        uint32_t off = be32(e + 0x10), size = be32(e + 0x14);
        if (size < 0x14 || size > 64 * 1024) break;
        unsigned char *s = malloc(size);
        if (s && !read_at(fd, off, s, size)) sfo_read(s, size, ver, title, titlen);
        free(s);
        break;
    }
    r = 0;
out:
    close(fd);
    return r;
}

static void installed_version(const char *tid, char *out)
{
    char p[128];
    out[0] = 0;
    snprintf(p, sizeof p, "/system_data/priv/appmeta/%s/param.sfo", tid);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return;
    unsigned char buf[16384];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n > 0) sfo_read(buf, (size_t)n, out, NULL, 0);
}

static int ver_num(const char *v)   /* "01.05" -> 105, -1 if not a version */
{
    int a, b;
    return (v[0] && sscanf(v, "%d.%d", &a, &b) == 2) ? a * 100 + b : -1;
}

static void check_pkg(const char *path, uint64_t size)
{
    char cid[40], ver[8], tid[10], label[17], inst[8] = "", p[160];
    int type;
    if (pkg_header(path, cid, &type, ver, NULL, 0) != 0) return;
    pkgs++;
    if (strlen(cid) < 36) return;
    memcpy(tid, cid + 7, 9); tid[9] = 0;
    memcpy(label, cid + 20, 16); label[16] = 0;
    for (int i = 0; i < 9; i++)
        if (!((tid[i] >= 'A' && tid[i] <= 'Z') || (tid[i] >= '0' && tid[i] <= '9'))) return;
    for (int i = 0; i < 16; i++)
        if (!((label[i] >= 'A' && label[i] <= 'Z') || (label[i] >= '0' && label[i] <= '9') || label[i] == '_' || label[i] == '-')) return;

    int leftover = 0;
    if (type == 0x1A || type == 0x1E) {
        snprintf(p, sizeof p, type == 0x1A ? "/user/app/%s" : "/user/patch/%s", tid);
        if (!is_dir(p)) return;
        installed_version(tid, inst);
        int pv = ver_num(ver), iv = ver_num(inst);
        leftover = pv >= 0 && iv >= 0 && pv <= iv;   /* unknown version: keep the file */
    } else if (type == 0x1B || type == 0x1C) {
        snprintf(p, sizeof p, "/user/addcont/%s/%s", tid, label);
        leftover = is_dir(p);
    }
    if (!leftover) return;
    pthread_mutex_lock(&mtx);
    if (lo_count < LO_MAX) {
        Leftover *l = &lo_items[lo_count];
        memset(l, 0, sizeof *l);
        snprintf(l->path, sizeof l->path, "%s", path);
        snprintf(l->title_id, sizeof l->title_id, "%s", tid);
        snprintf(l->label, sizeof l->label, "%s", label);
        snprintf(l->pkg_ver, sizeof l->pkg_ver, "%s", ver);
        snprintf(l->inst_ver, sizeof l->inst_ver, "%s", inst);
        l->type = type;
        l->size = size;
        lo_count++;
    }
    pthread_mutex_unlock(&mtx);
    dlog("leftover: %s (%s %s, pkg %s, installed %s)", path, lo_type_name(type), tid, ver, inst);
}

static void walk(const char *dir, int depth)
{
    if (cancel_req || depth > 12 || !strcmp(dir, "/data/GoldHEN")) return;
    DIR *d = opendir(dir);
    if (!d) return;
    pthread_mutex_lock(&mtx);
    snprintf(current, sizeof current, "%s", dir);
    pthread_mutex_unlock(&mtx);
    struct dirent *e;
    char full[1024];
    while (!cancel_req && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (path_join(full, sizeof full, dir, e->d_name)) continue;
        struct kstat st;
        if (k_lstat(full, &st) != 0) continue;
        checked++;
        if (S_ISDIR(st.st_mode)) walk(full, depth + 1);
        else if (S_ISREG(st.st_mode) && st.st_size > 0x1000) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcasecmp(e->d_name + l - 4, ".pkg")) check_pkg(full, st.st_size);
        }
    }
    closedir(d);
}

static void *scan_thread(void *arg)
{
    (void)arg;
    dlog("leftovers: scan started");
    walk("/data", 0);
    static const char *usb[] = { "/mnt/usb0", "/mnt/usb1" };
    for (int i = 0; i < 2; i++)
        if (is_mount_point(usb[i])) walk(usb[i], 0);
    dlog("leftovers: scan done, %d checked, %d pkg, %d leftover(s)%s", checked, pkgs, lo_count,
         cancel_req ? " (cancelled)" : "");
    running = 0;
    return NULL;
}

int lo_start(void)
{
    if (running) return -1;
    lo_count = 0;
    checked = pkgs = 0;
    cancel_req = 0;
    current[0] = 0;
    running = 1;
    if (thread_start(scan_thread, NULL, STACK_JOB, 1) != 0) { running = 0; return -1; }
    return 0;
}

int  lo_running(void) { return running; }
void lo_cancel(void) { cancel_req = 1; }

void lo_progress(int *c, int *p, char *cur, int n)
{
    pthread_mutex_lock(&mtx);
    *c = checked;
    *p = pkgs;
    snprintf(cur, n, "%s", current);
    pthread_mutex_unlock(&mtx);
}

int lo_delete_marked(uint64_t *freed, char *err, int errn)
{
    int done = 0, k = 0;
    *freed = 0;
    err[0] = 0;
    if (running) { snprintf(err, errn, "The scan is still running"); return 0; }
    for (int i = 0; i < lo_count; i++) {
        Leftover *l = &lo_items[i];
        int removed = 0;
        if (l->marked) {
            /* still the same regular file? (it could have been replaced since the scan) */
            struct kstat st;
            if (k_lstat(l->path, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != l->size)
                snprintf(err, errn, "%s changed since the scan - skipped", path_base(l->path));
            else if (unlink(l->path) != 0)
                snprintf(err, errn, "%s: %s", path_base(l->path), strerror(errno));
            else {
                removed = 1;
                done++;
                *freed += l->size;
                dlog("leftovers: deleted %s", l->path);
            }
        }
        if (!removed) { if (k != i) lo_items[k] = *l; k++; }
    }
    lo_count = k;
    return done;
}
