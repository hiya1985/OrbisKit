#include "zip.h"
#include "fsops.h"
#include "sysinfo.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEFLATE_ENTRY (256u * 1024 * 1024)   /* deflate entries are inflated in memory */
#define CHUNK (1024 * 1024)
#define FREE_MARGIN (1ull * 1024 * 1024)         /* left free on the destination when extracting */

/* from stb_image (stb_impl.c) */
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void zerr(const char *fmt, const char *name)
{
    g_job.errors++;
    snprintf(g_job.err, sizeof g_job.err, fmt, name);
}

/* Rejects absolute paths and ".." components (zip-slip). */
static int safe_name(const char *n)
{
    if (!*n || n[0] == '/') return 0;
    const char *p = n;
    while (*p) {
        const char *s = strchr(p, '/');
        size_t len = s ? (size_t)(s - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!s) break;
        p = s + 1;
    }
    return 1;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t w = write(fd, buf, len);
        if (w <= 0) return -1;
        buf += w;
        len -= w;
    }
    return 0;
}

/* ======================= zip writer ======================= */

#define MAX_COMPRESS (64u * 1024 * 1024)   /* bigger files are stored uncompressed */

/* from stb_image_write (stb_impl.c): returns a zlib stream (2-byte header + deflate + adler32) */
unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

typedef struct {
    char *name;
    uint32_t crc, csize, usize, off;
    uint16_t method, dtime, ddate;
    int dir;
} ZEnt;

static ZEnt *zents;
static int znent, zcap;
static int zfd;
static uint64_t zpos;
static uint64_t zcdsize;   /* bytes the central directory will need so far */
static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void w16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int zwrite(const void *buf, size_t n)
{
    if (write_all(zfd, buf, n)) return -1;
    zpos += n;
    return 0;
}

static void dos_time(time_t t, uint16_t *dt, uint16_t *dd)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 80) { *dt = 0; *dd = (1 << 5) | 1; return; }
    *dt = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    *dd = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

static ZEnt *new_ent(const char *name)
{
    if (znent == zcap) {
        int nc = zcap ? zcap * 2 : 256;
        ZEnt *n = realloc(zents, nc * sizeof(ZEnt));
        if (!n) return NULL;
        zents = n;
        zcap = nc;
    }
    ZEnt *e = &zents[znent];
    memset(e, 0, sizeof *e);
    e->name = strdup(name);
    if (!e->name) return NULL;
    znent++;
    zcdsize += 46 + strlen(name);    /* its central directory record, written at the end */
    e->off = (uint32_t)zpos;
    return e;
}

static int write_local_header(const ZEnt *e)
{
    unsigned char h[30];
    size_t nl = strlen(e->name);
    w32(h, 0x04034b50);
    w16(h + 4, 20);
    w16(h + 6, 0x0800);              /* UTF-8 names */
    w16(h + 8, e->method);
    w16(h + 10, e->dtime);
    w16(h + 12, e->ddate);
    w32(h + 14, e->crc);
    w32(h + 18, e->csize);
    w32(h + 22, e->usize);
    w16(h + 26, (uint16_t)nl);
    w16(h + 28, 0);
    return zwrite(h, 30) || zwrite(e->name, nl);
}

static const char *zout;   /* the archive being written - never add it to itself */
/* Set when an entry fails AFTER its header was written: the archive is then inconsistent,
 * so zip_create deletes it and reports failure instead of saving a corrupt file. */
static int zfatal;

/* Room kept free below 4 GB for the central directory (zip32 offsets are 32-bit). */
#define ZIP_LIMIT (0xFFFFFFFFull - 16ull * 1024 * 1024)

static int zip_add(const char *full, const char *rel)
{
    if (g_job.cancel || zfatal) return -1;
    if (zout && !strcmp(full, zout)) return 0;
    struct kstat st;
    if (k_lstat(full, &st) != 0) { zerr("Can't read %s", rel); return -1; }
    snprintf(g_job.current, sizeof g_job.current, "%s", full);
    if (znent >= 65000) { zerr("Too many files for one zip (65000 max): %s", rel); return -1; }
    /* the whole entry (header + data) must still fit below the 4 GB zip limit */
    /* ... and so must the central directory (65000 long names can pass the 16 MB reserve) */
    uint64_t need = 30 + strlen(rel) + 1 + (S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0);
    need += 46 + strlen(rel) + 1 + 22;
    if (zpos + zcdsize + need > 0xFFFFFFFFull || zpos + need > ZIP_LIMIT) { zerr("Zip would pass 4 GB - skipped: %s", rel); return -1; }

    if (S_ISDIR(st.st_mode)) {
        char dname[1024];
        snprintf(dname, sizeof dname, "%s/", rel);
        ZEnt *e = new_ent(dname);
        if (!e) return -1;
        e->dir = 1;
        dos_time(st.st_mtime, &e->dtime, &e->ddate);
        if (write_local_header(e)) { zfatal = 1; zerr("Write failed (disk full?): %s", rel); return -1; }
        g_job.files_done++;
        DIR *d = opendir(full);
        if (!d) return 0;
        struct dirent *de;
        char f2[1024], r2[1024];
        int rc = 0;
        while ((de = readdir(d)) && !g_job.cancel) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            path_join(f2, sizeof f2, full, de->d_name);
            snprintf(r2, sizeof r2, "%s/%s", rel, de->d_name);
            if (zip_add(f2, r2) && g_job.cancel) { rc = -1; break; }
        }
        closedir(d);
        return rc;
    }
    if (!S_ISREG(st.st_mode)) return 0;           /* skip devices, sockets, links */
    if ((uint64_t)st.st_size >= 0xFFFFFFFFull) { zerr("Skipped (over 4 GB): %s", rel); return -1; }

    int in = open(full, O_RDONLY, 0);
    if (in < 0) { zerr("Can't open %s", rel); return -1; }
    ZEnt *e = new_ent(rel);
    if (!e) { close(in); return -1; }
    dos_time(st.st_mtime, &e->dtime, &e->ddate);
    e->usize = (uint32_t)st.st_size;
    int rc = 0;

    if ((uint64_t)st.st_size <= MAX_COMPRESS) {
        /* small enough: read whole file, try deflate, keep whichever is smaller */
        unsigned char *buf = malloc(st.st_size ? st.st_size : 1);
        ssize_t got = 0;
        while (buf && got < st.st_size) {
            ssize_t r = read(in, buf + got, st.st_size - got);
            if (r <= 0) break;
            got += r;
        }
        if (!buf || got != st.st_size) {
            free(buf); close(in); zerr("Read failed: %s", rel);
            free(zents[--znent].name);     /* nothing written yet: just drop the entry */
            return -1;
        }
        e->crc = crc_update(0, buf, got);
        int clen = 0;
        unsigned char *z = got > 64 ? stbi_zlib_compress(buf, (int)got, &clen, 8) : NULL;
        if (z && clen > 6 && clen - 6 < got) {
            e->method = 8;
            e->csize = clen - 6;
            rc = write_local_header(e) || zwrite(z + 2, clen - 6);
        } else {
            e->method = 0;
            e->csize = (uint32_t)got;
            rc = write_local_header(e) || zwrite(buf, got);
        }
        free(z);
        free(buf);
        g_job.bytes_done += got;
    } else {
        /* large file: stream it stored, then patch CRC into the header */
        e->method = 0;
        e->csize = e->usize;
        char *chunk = malloc(CHUNK);
        if (!chunk) { close(in); free(zents[--znent].name); zerr("Out of memory: %s", rel); return -1; }
        rc = write_local_header(e);
        uint32_t crc = 0;
        uint64_t total = 0;
        ssize_t r = 0;
        while (!rc && !g_job.cancel && (r = read(in, chunk, CHUNK)) > 0) {
            crc = crc_update(crc, (unsigned char *)chunk, r);
            rc = zwrite(chunk, r);
            g_job.bytes_done += r;
            total += r;
        }
        free(chunk);
        /* a read error or a file that shrank leaves the header's size wrong: fatal */
        if (!rc && !g_job.cancel && (r < 0 || total != e->usize)) {
            rc = -1;
            zerr("Read failed part-way: %s", rel);
        }
        e->crc = crc;
        unsigned char c[4];
        w32(c, crc);
        if (!rc && (lseek(zfd, e->off + 14, SEEK_SET) < 0 || write_all(zfd, (char *)c, 4) ||
                    lseek(zfd, 0, SEEK_END) < 0)) rc = -1;
    }
    close(in);
    if (rc) {
        /* the entry's header is already in the archive, so the archive can't be completed */
        zfatal = 1;
        if (!strstr(g_job.err, rel)) zerr("Write failed (disk full?): %s", rel);
    }
    g_job.files_done++;
    return rc ? -1 : 0;
}

int zip_create(char **src, int nsrc, const char *outpath)
{
    crc_init();
    znent = 0;
    zpos = 0;
    zcdsize = 0;
    zfatal = 0;
    zout = outpath;
    zfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (zfd < 0) { zerr("Can't create %s", path_base(outpath)); return -1; }

    for (int i = 0; i < nsrc && !g_job.cancel && !zfatal; i++) {
        if (!strcmp(src[i], outpath)) continue;
        zip_add(src[i], path_base(src[i]));
    }

    int rc = zfatal ? -1 : 0;
    if (!g_job.cancel && !zfatal) {
        uint32_t cd_off = (uint32_t)zpos;
        for (int i = 0; i < znent && !rc; i++) {
            ZEnt *e = &zents[i];
            unsigned char h[46];
            size_t nl = strlen(e->name);
            w32(h, 0x02014b50);
            w16(h + 4, 0x0314);           /* made by: Unix, v2.0 */
            w16(h + 6, 20);
            w16(h + 8, 0x0800);
            w16(h + 10, e->method);
            w16(h + 12, e->dtime);
            w16(h + 14, e->ddate);
            w32(h + 16, e->crc);
            w32(h + 20, e->csize);
            w32(h + 24, e->usize);
            w16(h + 28, (uint16_t)nl);
            w16(h + 30, 0);
            w16(h + 32, 0);
            w16(h + 34, 0);
            w16(h + 36, 0);
            w32(h + 38, e->dir ? (040755u << 16) | 0x10 : (0100644u << 16));
            w32(h + 42, e->off);
            rc = zwrite(h, 46) || zwrite(e->name, nl);
        }
        unsigned char end[22];
        w32(end, 0x06054b50);
        w16(end + 4, 0);
        w16(end + 6, 0);
        w16(end + 8, (uint16_t)znent);
        w16(end + 10, (uint16_t)znent);
        w32(end + 12, (uint32_t)zpos - cd_off);
        w32(end + 16, cd_off);
        w16(end + 20, 0);
        if (!rc) rc = zwrite(end, 22);
        if (rc) zerr("Write failed: %s", path_base(outpath));
    }
    if (close(zfd) != 0 && !rc && !g_job.cancel) { rc = -1; zerr("Write failed: %s", path_base(outpath)); }
    for (int i = 0; i < znent; i++) free(zents[i].name);
    znent = 0;
    if (g_job.cancel || rc) unlink(outpath);
    return rc;
}

/* ======================= zip reader ======================= */

int zip_extract(const char *zippath, const char *dstdir)
{
    FILE *f = fopen(zippath, "rb");
    if (!f) { zerr("Can't open %s", path_base(zippath)); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);

    /* find End Of Central Directory record (in the last 64 KB + 22 bytes) */
    long tail = size < 65557 ? size : 65557;
    unsigned char *tb = malloc(tail);
    if (!tb) { fclose(f); return -1; }
    fseek(f, size - tail, SEEK_SET);
    if ((long)fread(tb, 1, tail, f) != tail) tail = 0;
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(tb + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(tb); fclose(f); zerr("%s is not a valid zip", path_base(zippath)); return -1; }
    unsigned entries = rd16(tb + eocd + 10);
    uint32_t cd_size = rd32(tb + eocd + 12), cd_off = rd32(tb + eocd + 16);
    free(tb);
    if (cd_off == 0xFFFFFFFFu || entries == 0xFFFF) {
        fclose(f);
        zerr("%s uses zip64 (not supported)", path_base(zippath));
        return -1;
    }

    /* the central directory must lie inside the file (don't malloc a bogus 4 GB size) */
    if ((uint64_t)cd_off + cd_size > (uint64_t)size) {
        fclose(f);
        zerr("%s is damaged", path_base(zippath));
        return -1;
    }
    crc_init();
    unsigned char *cd = malloc(cd_size ? cd_size : 1);
    if (!cd) { fclose(f); return -1; }
    fseek(f, cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size) {
        free(cd); fclose(f);
        zerr("%s is truncated", path_base(zippath));
        return -1;
    }

    /* pass 1: totals for the progress bar (and the free space check) */
    uint64_t declared = 0, written = 0;
    for (uint64_t pos = 0, i = 0; i < entries && pos + 46 <= cd_size; i++) {
        const unsigned char *p = cd + pos;
        if (rd32(p) != 0x02014b50) break;
        declared += rd32(p + 24);
        g_job.bytes_total += rd32(p + 24);
        g_job.files_total++;
        pos += 46 + rd16(p + 28) + rd16(p + 30) + rd16(p + 32);
    }

    /* don't fill the disk (65000 entries x 256 MB is possible): fail before writing anything */
    unsigned long long dtot, dfree;
    if (declared && disk_space(dstdir, &dtot, &dfree) == 0 &&
        (dfree < FREE_MARGIN || declared > dfree - FREE_MARGIN)) {
        char need[32];
        snprintf(need, sizeof need, "%llu MB", (unsigned long long)((declared + 1048575) / 1048576));
        free(cd); fclose(f);
        zerr("Not enough free space (need %s)", need);
        return -1;
    }

    char *chunk = malloc(CHUNK);
    if (!chunk) { free(cd); fclose(f); zerr("Out of memory extracting %s", path_base(zippath)); return -1; }
    int rc = 0;
    for (uint64_t pos = 0, i = 0; i < entries && pos + 46 <= cd_size && !g_job.cancel; i++) {
        const unsigned char *p = cd + pos;
        if (rd32(p) != 0x02014b50) break;
        uint16_t flags = rd16(p + 8), method = rd16(p + 10);
        uint32_t crc = rd32(p + 16), csize = rd32(p + 20), usize = rd32(p + 24);
        uint16_t nlen = rd16(p + 28);
        uint32_t lho = rd32(p + 42);
        /* the name must lie inside the central directory we read (malformed zips) */
        if ((uint64_t)pos + 46 + nlen > cd_size) { zerr("%s is damaged", path_base(zippath)); rc = -1; break; }
        pos += 46 + nlen + rd16(p + 30) + rd16(p + 32);

        char name[512];
        /* a cut-off name is a different name (and may lose its trailing '/'): skip it */
        if (nlen >= sizeof name) { zerr("Skipped (name too long): %s", "entry"); rc = -1; continue; }
        int n = nlen;
        memcpy(name, p + 46, n);
        name[n] = 0;
        for (char *c = name; *c; c++) if (*c == '\\') *c = '/';
        snprintf(g_job.current, sizeof g_job.current, "%s", name);

        if (!safe_name(name)) { zerr("Skipped unsafe path: %s", name); rc = -1; continue; }
        char out[1024];
        if (path_join(out, sizeof out, dstdir, name)) { zerr("Skipped (path too long): %s", name); rc = -1; continue; }

        if (name[n - 1] == '/') {          /* directory entry */
            mkdirs(out);
            g_job.files_done++;
            continue;
        }
        if (flags & 1) { zerr("Skipped encrypted file: %s", name); rc = -1; continue; }
        if (method != 0 && method != 8) { zerr("Unsupported compression: %s", name); rc = -1; continue; }

        /* create parent folders */
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", out);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; mkdirs(parent); }

        unsigned char lh[30];
        fseek(f, (long)lho, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) { zerr("Bad entry header: %s", name); rc = -1; continue; }
        fseek(f, (long)lho + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);   /* no 32-bit wrap */

        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (fd < 0) { zerr("Can't create %s", name); rc = -1; continue; }
        int ok = 1;
        uint32_t got_crc = 0;

        if (method == 0) {
            uint32_t left = csize;
            while (left && ok && !g_job.cancel) {
                size_t want = left < CHUNK ? left : CHUNK;
                /* sizes that lie (stored data bigger than declared) could still fill the disk */
                if (written + want > declared) { zerr("Zip sizes are wrong (more data than declared): %s", name); ok = 0; break; }
                written += want;
                if (fread(chunk, 1, want, f) != want || write_all(fd, chunk, want)) ok = 0;
                else got_crc = crc_update(got_crc, (unsigned char *)chunk, want);
                left -= want;
                g_job.bytes_done += want;
            }
        } else if (csize > MAX_DEFLATE_ENTRY || usize > MAX_DEFLATE_ENTRY) {
            zerr("Too large to inflate in memory: %s", name);
            ok = 0;
        } else {
            char *cbuf = malloc(csize ? csize : 1), *ubuf = malloc(usize ? usize : 1);
            if (written + usize > declared) { zerr("Zip sizes are wrong (more data than declared): %s", name); ok = 0; }
            else if (!cbuf || !ubuf || fread(cbuf, 1, csize, f) != csize) ok = 0;
            else if (usize && stbi_zlib_decode_noheader_buffer(ubuf, usize, cbuf, csize) != (int)usize) ok = 0;
            else if (write_all(fd, ubuf, usize)) ok = 0;
            else got_crc = crc_update(0, (unsigned char *)ubuf, usize);
            free(cbuf);
            free(ubuf);
            written += usize;
            g_job.bytes_done += usize;
        }
        if (close(fd) != 0) ok = 0;
        /* corrupt or tampered data (or sizes that don't match the data) */
        if (ok && !g_job.cancel && got_crc != crc) { zerr("CRC mismatch (corrupt zip): %s", name); ok = 0; }
        if (!ok || g_job.cancel) {
            unlink(out);
            if (!g_job.cancel && !strstr(g_job.err, name)) zerr("Failed to extract %s", name);
            rc = -1;
        }
        g_job.files_done++;
    }
    free(chunk);
    free(cd);
    fclose(f);
    return rc;
}
