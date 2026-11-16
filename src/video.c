/* Video player. Picture: the system AvPlayer decodes H.264 on the PS4's video hardware and
 * hands back NV12 frames, which a thread converts to ARGB for the screen. Sound: AvPlayer's
 * own audio stream never delivered data on FW 11.00, so the AAC track is decoded by the app
 * (mp4audio.c) and played by the music player (audio.c), kept in step with the picture.
 *
 * What is known to work on the console (1.01 tests) and must stay that way:
 *  - our own file callbacks (letting AvPlayer open the file itself crashed the console)
 *  - textures in direct memory (plain malloc'd textures crashed it)
 *  - basePriority not 0 (0 crashed it), autoStart off, Start after the READY event
 *  - NV12 chroma plane at pitch * align16(height)
 * Every AvPlayer call happens on the one video thread, and the handle is closed by that
 * thread after its last use - never while it may still be inside GetVideoData. */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "audio.h"
#include "compat.h"
#include "dlog.h"
#include "fsops.h"
#include "gfx.h"
#include "pad.h"
#include "video.h"

/* ---- AvPlayer (declared here: the toolchain header has no prototypes) ---- */
typedef struct {
    void *object;
    void *(*allocate)(void *p, uint32_t align, uint32_t size);
    void  (*deallocate)(void *p, void *mem);
    void *(*allocate_texture)(void *p, uint32_t align, uint32_t size);
    void  (*deallocate_texture)(void *p, void *mem);
} AvMem;
typedef struct {
    void *object;
    int      (*open)(void *p, const char *name);
    int      (*close)(void *p);
    int      (*read_offset)(void *p, uint8_t *buf, uint64_t pos, uint32_t len);
    uint64_t (*size)(void *p);
} AvFile;
typedef struct {
    void *object;
    void (*event)(void *p, int32_t id, int32_t source, void *data);
} AvEvent;
typedef struct {
    AvMem   mem;
    AvFile  file;
    AvEvent event;
    int32_t  debug_level;
    uint32_t base_priority;
    int32_t  num_frame_buffers;
    uint8_t  auto_start;
    uint8_t  reserved[3];
    const char *default_language;
} AvInit;
typedef struct {
    void    *data;              /* NV12 */
    uint8_t  reserved[4];
    uint64_t timestamp;         /* ms */
    uint32_t width, height;
    float    aspect;
    char     lang[4];
    uint32_t framerate;
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    uint32_t pitch;
    uint8_t  luma_bits, chroma_bits, full_range;
    uint8_t  reserved2[37];
    uint8_t  pad[64];           /* room in case the system's struct is bigger */
} AvFrameEx;

void    *sceAvPlayerInit(AvInit *init);
int      sceAvPlayerAddSource(void *h, const char *path);
int      sceAvPlayerStreamCount(void *h);
int      sceAvPlayerGetStreamInfo(void *h, uint32_t id, void *info);
int      sceAvPlayerEnableStream(void *h, uint32_t id);
int      sceAvPlayerStart(void *h);
int      sceAvPlayerStop(void *h);
int      sceAvPlayerClose(void *h);
int      sceAvPlayerPause(void *h);
int      sceAvPlayerResume(void *h);
int      sceAvPlayerJumpToTime(void *h, uint64_t ms);
uint64_t sceAvPlayerCurrentTime(void *h);
uint8_t  sceAvPlayerIsActive(void *h);
uint8_t  sceAvPlayerGetVideoDataEx(void *h, AvFrameEx *f);

#define EV_STATE_STOP  0x01
#define EV_STATE_READY 0x02
#define EV_STATE_PLAY  0x03
#define EV_STATE_PAUSE 0x04
#define EV_STATE_BUFF  0x05

#define MEM_WB_ONION 0
#define PROT_RW_ALL  0x33
#define DM_ALIGN     0x10000

/* ---- state ---- */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static struct {
    volatile int active, running, stop;
    volatile int ready, ended, failed, got_frame;
    volatile int pause_req, paused;
    volatile int seek_req;           /* seconds, relative; applied by the thread */
    volatile int seek_pending;
    volatile double vtime;           /* seconds, picture position */
    int fd;
    uint64_t fsize;
    char path[1024], name[256], err[200];
    uint32_t *rgb[2];
    int front, have_frame;
    int vw, vh, fps;
    int sound;                       /* the sound track is playing */
    char sound_err[160];
    int silent;                      /* the file has no sound track at all */
    double dur;                      /* seconds, from the file's mvhd box; 0 = unknown */
    int osd_t, info;
} V = { .fd = -1 };

/* direct-memory textures handed to AvPlayer, so they can be released again */
#define MAX_TEX 64
static struct { void *addr; int64_t phys; size_t len; } tex[MAX_TEX];
static pthread_mutex_t tex_mtx = PTHREAD_MUTEX_INITIALIZER;

int video_is_video_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v") || !strcasecmp(dot, ".mov"));
}

int video_active(void) { return V.active; }

/* ---- AvPlayer callbacks ---- */
static void *cb_alloc(void *p, uint32_t align, uint32_t size)
{
    (void)p;
    void *m = NULL;
    if (align < 16) align = 16;
    if (posix_memalign(&m, align, size ? size : 1) != 0) return NULL;
    return m;
}

static void cb_free(void *p, void *mem) { (void)p; free(mem); }

static void *cb_alloc_tex(void *p, uint32_t align, uint32_t size)
{
    (void)p;
    size_t len = ((size_t)size + DM_ALIGN - 1) & ~(size_t)(DM_ALIGN - 1);
    size_t al = align > DM_ALIGN ? align : DM_ALIGN;
    int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len, al, MEM_WB_ONION, &phys) != 0) {
        dlog("video: texture alloc %u failed", size);
        return NULL;
    }
    void *addr = NULL;
    if (sceKernelMapDirectMemory(&addr, len, PROT_RW_ALL, 0, phys, al) != 0) {
        sceKernelReleaseDirectMemory(phys, len);
        dlog("video: texture map %u failed", size);
        return NULL;
    }
    pthread_mutex_lock(&tex_mtx);
    int i;
    for (i = 0; i < MAX_TEX && tex[i].addr; i++) ;
    if (i < MAX_TEX) { tex[i].addr = addr; tex[i].phys = phys; tex[i].len = len; }
    pthread_mutex_unlock(&tex_mtx);
    if (i == MAX_TEX) dlog("video: texture table full (memory will leak)");
    return addr;
}

static void cb_free_tex(void *p, void *mem)
{
    (void)p;
    pthread_mutex_lock(&tex_mtx);
    for (int i = 0; i < MAX_TEX; i++)
        if (tex[i].addr == mem) {
            sceKernelMunmap(tex[i].addr, tex[i].len);
            sceKernelReleaseDirectMemory(tex[i].phys, tex[i].len);
            tex[i].addr = NULL;
            break;
        }
    pthread_mutex_unlock(&tex_mtx);
}

static int cb_open(void *p, const char *name)
{
    (void)p; (void)name;
    return V.fd >= 0 ? 0 : -1;   /* the file is opened by video_open */
}

static int cb_close(void *p) { (void)p; return 0; }

static int cb_read(void *p, uint8_t *buf, uint64_t pos, uint32_t len)
{
    (void)p;
    if (pos >= V.fsize) return 0;
    ssize_t r = pread(V.fd, buf, len, (off_t)pos);
    return r < 0 ? -1 : (int)r;
}

static uint64_t cb_size(void *p) { (void)p; return V.fsize; }

static void cb_event(void *p, int32_t id, int32_t source, void *data)
{
    (void)p; (void)source; (void)data;
    if (id == EV_STATE_READY) V.ready = 1;
    if (id != EV_STATE_BUFF) dlog("video: event 0x%X", id);
}

/* ---- NV12 -> ARGB, scaled to fit the screen (letterboxed) ---- */
static inline uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }

/* One frame's conversion job, split into row bands for several threads. */
typedef struct {
    const uint8_t *Y, *UV;
    int pitch, w, h, ow, oh, ox, oy, y0, y1;
    int cy, yo, crv, cgu, cgv, cbu;
    const int *xmap;
    uint32_t *out;
} Band;

static void *convert_band(void *arg)
{
    const Band *b = arg;
    int last_sy = -1;
    for (int y = b->y0; y < b->y1; y++) {
        uint32_t *row = b->out + (size_t)y * SCREEN_W;
        if (y < b->oy || y >= b->oy + b->oh) { memset(row, 0, SCREEN_W * 4); last_sy = -1; continue; }
        int sy = (y - b->oy) * b->h / b->oh;
        if (sy == last_sy) {            /* scaled up: same source row as the line above */
            memcpy(row, row - SCREEN_W, SCREEN_W * 4);
            continue;
        }
        last_sy = sy;
        const uint8_t *yr = b->Y + (size_t)sy * b->pitch, *uvr = b->UV + (size_t)(sy >> 1) * b->pitch;
        if (b->ox) { memset(row, 0, b->ox * 4); memset(row + b->ox + b->ow, 0, (SCREEN_W - b->ox - b->ow) * 4); }
        uint32_t *o = row + b->ox;
        for (int x = 0; x < b->ow; x++) {
            int sx = b->xmap[x];
            int c = (yr[sx] - b->yo) * b->cy;
            int d = uvr[sx & ~1] - 128, e = uvr[(sx & ~1) + 1] - 128;
            int r = (c + b->crv * e + 128) >> 8;
            int g = (c - b->cgu * d - b->cgv * e + 128) >> 8;
            int bl = (c + b->cbu * d + 128) >> 8;
            o[x] = 0xFF000000u | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(bl);
        }
    }
    return NULL;
}

#define CONVERT_THREADS 4   /* one frame on one core was too slow: video ran at ~75 % speed */

static void convert(const AvFrameEx *f, uint32_t *out)
{
    int w = (int)f->width, h = (int)f->height;
    int pitch = (int)f->pitch;
    if (pitch < w || pitch > 8192) pitch = w;
    Band base;
    base.Y = f->data;
    base.UV = base.Y + (size_t)pitch * ((h + 15) & ~15);
    base.pitch = pitch; base.w = w; base.h = h;
    /* fit inside 1920x1080 keeping the shape */
    base.ow = SCREEN_W; base.oh = SCREEN_W * h / w;
    if (base.oh > SCREEN_H) { base.oh = SCREEN_H; base.ow = SCREEN_H * w / h; }
    base.ox = (SCREEN_W - base.ow) / 2; base.oy = (SCREEN_H - base.oh) / 2;
    static int xmap[SCREEN_W], mw, mow;
    if (mw != w || mow != base.ow) {
        for (int x = 0; x < base.ow; x++) xmap[x] = x * w / base.ow;
        mw = w; mow = base.ow;
    }
    base.xmap = xmap;
    base.out = out;
    /* BT.709 for HD, BT.601 for SD; limited ("TV") range unless the stream says full */
    int hd = h >= 720, full = f->full_range;
    base.cy = full ? 256 : 298; base.yo = full ? 0 : 16;
    base.crv = hd ? 459 : 409; base.cgu = hd ? 55 : 100; base.cgv = hd ? 136 : 208; base.cbu = hd ? 541 : 516;
    if (full) { base.crv = hd ? 403 : 359; base.cgu = hd ? 48 : 88; base.cgv = hd ? 120 : 183; base.cbu = hd ? 475 : 454; }

    Band band[CONVERT_THREADS];
    pthread_t th[CONVERT_THREADS];
    int started[CONVERT_THREADS] = { 0 };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    for (int i = 0; i < CONVERT_THREADS; i++) {
        band[i] = base;
        band[i].y0 = SCREEN_H * i / CONVERT_THREADS;
        band[i].y1 = SCREEN_H * (i + 1) / CONVERT_THREADS;
        /* band 0 runs here; the others on helper threads (or here too if one can't start) */
        if (i > 0) started[i] = pthread_create(&th[i], &attr, convert_band, &band[i]) == 0;
    }
    pthread_attr_destroy(&attr);
    convert_band(&band[0]);
    for (int i = 1; i < CONVERT_THREADS; i++) {
        if (started[i]) pthread_join(th[i], NULL);
        else convert_band(&band[i]);
    }
}

/* ---- the video thread: owns the AvPlayer handle ---- */
static void fail(const char *fmt, int code)
{
    pthread_mutex_lock(&mtx);
    snprintf(V.err, sizeof V.err, fmt, code);
    pthread_mutex_unlock(&mtx);
    dlog("video: %s", V.err);
    V.failed = 1;
}

static void *video_thread(void *arg)
{
    (void)arg;
    /* loaded for each video and unloaded after it: nothing of AvPlayer is left running
       when the app is closed (the PS4 reported a crash on closing after a video) */
    int r = sceSysmoduleLoadModule(SYSMODULE_AV_PLAYER);
    dlog("video: load AvPlayer module 0x%08X", r);
    AvInit init;
    memset(&init, 0, sizeof init);
    init.mem.allocate = cb_alloc;
    init.mem.deallocate = cb_free;
    init.mem.allocate_texture = cb_alloc_tex;
    init.mem.deallocate_texture = cb_free_tex;
    init.file.open = cb_open;
    init.file.close = cb_close;
    init.file.read_offset = cb_read;
    init.file.size = cb_size;
    init.event.event = cb_event;
    init.debug_level = 0;
    init.base_priority = 700;
    init.num_frame_buffers = 2;
    init.auto_start = 0;
    init.default_language = "eng";

    void *h = sceAvPlayerInit(&init);
    dlog("video: init -> %p", h);
    if (!h) { fail("The system video player didn't start", 0); goto out; }
    r = sceAvPlayerAddSource(h, V.path);
    dlog("video: add source -> 0x%08X", r);
    if (r < 0) { fail("This video can't be opened (0x%08X)", r); goto close; }

    for (int i = 0; i < 1000 && !V.ready && !V.stop; i++) sceKernelUsleep(10000);   /* up to 10 s */
    if (V.stop) goto close;
    if (!V.ready) { fail("The video didn't load (timeout)%.0d", 0); goto close; }

    int n = sceAvPlayerStreamCount(h), video_streams = 0;
    for (int i = 0; i < n && i < 16; i++) {
        uint8_t info[128];
        memset(info, 0, sizeof info);
        if (sceAvPlayerGetStreamInfo(h, i, info) < 0) continue;
        uint32_t type = *(uint32_t *)info;
        dlog("video: stream %d type %u", i, type);
        if (type == 0 && !video_streams) {   /* first video stream only; sound is ours */
            r = sceAvPlayerEnableStream(h, i);
            dlog("video: enable stream %d -> 0x%08X", i, r);
            if (r >= 0) video_streams++;
        }
    }
    if (!video_streams) { fail("No picture in this file (sound only? open it as music)%.0d", 0); goto close; }
    r = sceAvPlayerStart(h);
    dlog("video: start -> 0x%08X", r);

    uint64_t t0 = sceKernelGetProcessTime(), last_sync = 0;
    while (!V.stop) {
        if (V.pause_req) {
            V.pause_req = 0;
            if (V.paused) { sceAvPlayerResume(h); V.paused = 0; audio_set_paused(0); }
            else { sceAvPlayerPause(h); V.paused = 1; audio_set_paused(1); }
        }
        if (V.seek_pending) {
            int d = V.seek_req;
            V.seek_req = 0;
            V.seek_pending = 0;
            double t = sceAvPlayerCurrentTime(h) / 1000.0 + d;
            if (V.dur > 0 && t > V.dur - 1) t = V.dur - 1;   /* past the end: AvPlayer refuses (0x806A0002) */
            if (t < 0) t = 0;
            r = sceAvPlayerJumpToTime(h, (uint64_t)(t * 1000));
            dlog("video: jump to %.1f s -> 0x%08X", t, r);
            if (V.sound) audio_seek_to(t);
            last_sync = sceKernelGetProcessTime();   /* let the jump settle before re-syncing */
        }
        AvFrameEx f;
        memset(&f, 0, sizeof f);
        if (sceAvPlayerGetVideoDataEx(h, &f) && f.data && f.width && f.height && f.width <= 4096 && f.height <= 2304) {
            int back = !V.front;
            convert(&f, V.rgb[back]);
            pthread_mutex_lock(&mtx);
            V.front = back;
            V.have_frame = 1;
            V.vw = f.width; V.vh = f.height; V.fps = f.framerate;
            pthread_mutex_unlock(&mtx);
            V.vtime = f.timestamp / 1000.0;
            if (!V.got_frame) {
                V.got_frame = 1;
                dlog("video: first frame %ux%u pitch %u fps %u range %u", f.width, f.height, f.pitch, f.framerate, f.full_range);
                char err[160];
                if (audio_play_single(V.path, V.vtime, err, sizeof err) == 0) V.sound = 1;
                else {
                    /* a video made without sound is normal, not an error */
                    V.silent = strstr(err, "No AAC sound track") != NULL;
                    snprintf(V.sound_err, sizeof V.sound_err, V.silent ? "No sound in this video" : "No sound: %s", err);
                    dlog("video: %s", V.sound_err);
                }
            }
        } else {
            if (!V.got_frame && sceKernelGetProcessTime() - t0 > 10000000ULL) {
                fail("The video didn't show a picture (unsupported format?)%.0d", 0);
                break;
            }
            if (V.got_frame && !sceAvPlayerIsActive(h)) { V.ended = 1; dlog("video: ended"); break; }
            sceKernelUsleep(4000);
        }
        /* keep the sound with the picture: re-sync when they drift apart */
        uint64_t now = sceKernelGetProcessTime();
        if (V.sound && !V.paused && now - last_sync > 2000000ULL) {
            last_sync = now;
            double v = sceAvPlayerCurrentTime(h) / 1000.0, a = audio_position();
            if (audio_active() && fabs(v - a) > 0.30) {
                dlog("video: re-sync sound %.2f -> %.2f", a, v);
                audio_seek_to(v);
            }
        }
    }
close:
    sceAvPlayerStop(h);
    sceAvPlayerClose(h);
    dlog("video: closed");
out:
    {
        int u = sceSysmoduleUnloadModule(SYSMODULE_AV_PLAYER);
        dlog("video: unload AvPlayer module 0x%08X", u);
    }
    V.running = 0;
    return NULL;
}

/* ---- UI side ---- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Walks the top-level MP4 boxes. 0 if there is a 'moov' (the index a player needs);
 * *dur gets the length from its 'mvhd'. The PS4's background-recording buffer
 * (content_tmp) has none: AvPlayer would just sit there, so it is refused up front. */
static int mp4_check(int fd, uint64_t size, double *dur)
{
    uint64_t off = 0;
    *dur = 0;
    for (int n = 0; n < 4096 && off + 8 <= size; n++) {
        uint8_t h[16];
        if (pread(fd, h, 16, (off_t)off) < 8) return -1;
        uint64_t len = rd32(h), hdr = 8;
        if (len == 1) { len = (uint64_t)rd32(h + 8) << 32 | rd32(h + 12); hdr = 16; }
        else if (len == 0) len = size - off;           /* box runs to the end of the file */
        if (len < hdr) return -1;
        if (!memcmp(h + 4, "moov", 4)) {
            /* mvhd is normally the first box inside moov */
            uint8_t m[40];
            if (pread(fd, m, sizeof m, (off_t)(off + hdr)) == sizeof m && !memcmp(m + 4, "mvhd", 4)) {
                const uint8_t *b = m + 8;              /* version + flags */
                uint32_t ts;
                uint64_t d;
                if (b[0] == 1) { ts = rd32(b + 20); d = (uint64_t)rd32(b + 24) << 32 | rd32(b + 28); }
                else { ts = rd32(b + 12); d = rd32(b + 16); }
                if (ts) *dur = (double)d / ts;
            }
            return 0;
        }
        off += len;
    }
    return -1;
}

int video_open(const char *path, char *err, int errn)
{
    if (V.active || V.running) { snprintf(err, errn, "A video is already open"); return -1; }
    memset(&V, 0, sizeof V);
    V.fd = open(path, O_RDONLY);
    if (V.fd < 0) { snprintf(err, errn, "Can't open the file"); return -1; }
    V.fsize = (uint64_t)lseek(V.fd, 0, SEEK_END);
    if (mp4_check(V.fd, V.fsize, &V.dur) != 0) {
        close(V.fd); V.fd = -1;
        snprintf(err, errn, strstr(path, "content_tmp") ?
                 "This is the PS4's background-recording buffer, not a finished video - it can't be played" :
                 "Not a playable video (the file has no index - an unfinished recording?)");
        return -1;
    }
    snprintf(V.path, sizeof V.path, "%s", path);
    snprintf(V.name, sizeof V.name, "%s", path_base(path));
    V.rgb[0] = malloc(SCREEN_W * SCREEN_H * 4);
    V.rgb[1] = malloc(SCREEN_W * SCREEN_H * 4);
    if (!V.rgb[0] || !V.rgb[1]) {
        free(V.rgb[0]); free(V.rgb[1]);
        close(V.fd); V.fd = -1;
        snprintf(err, errn, "Out of memory");
        return -1;
    }
    audio_stop();               /* music off: the video's sound uses the player */
    V.active = V.running = 1;
    V.osd_t = 240;
    dlog("video: open %s (%llu bytes, %.1f s)", path, (unsigned long long)V.fsize, V.dur);
    if (thread_start(video_thread, NULL, STACK_JOB, 1) != 0) {
        V.active = V.running = 0;
        free(V.rgb[0]); free(V.rgb[1]);
        close(V.fd); V.fd = -1;
        snprintf(err, errn, "Couldn't start the video thread");
        return -1;
    }
    return 0;
}

void video_close(void)
{
    if (!V.active) return;
    V.stop = 1;
    for (int i = 0; i < 500 && V.running; i++) sceKernelUsleep(10000);   /* up to 5 s */
    if (V.sound) audio_stop();
    if (V.running) {
        /* the thread is stuck inside AvPlayer: leave its buffers alone (a small leak beats a crash) */
        dlog("video: thread didn't stop in time");
        V.active = 0;
        return;
    }
    free(V.rgb[0]); free(V.rgb[1]);
    V.rgb[0] = V.rgb[1] = NULL;
    if (V.fd >= 0) close(V.fd);
    V.fd = -1;
    V.active = 0;
}

void video_input(void)
{
    if (pad_pressed(BTN_CIRCLE) || ((V.failed || V.ended) && pad_pressed(BTN_CROSS))) { video_close(); return; }
    if (V.failed || V.ended) return;
    int any = 0;
    if (pad_pressed(BTN_CROSS)) { V.pause_req = 1; any = 1; }
    if (pad_pressed(BTN_LEFT))  { V.seek_req -= 10; V.seek_pending = 1; any = 1; }
    if (pad_pressed(BTN_RIGHT)) { V.seek_req += 10; V.seek_pending = 1; any = 1; }
    if (pad_pressed(BTN_L1))    { V.seek_req -= 60; V.seek_pending = 1; any = 1; }
    if (pad_pressed(BTN_R1))    { V.seek_req += 60; V.seek_pending = 1; any = 1; }
    if (pad_pressed(BTN_UP))    { audio_volume(5); any = 1; }
    if (pad_pressed(BTN_DOWN))  { audio_volume(-5); any = 1; }
    if (pad_pressed(BTN_TRIANGLE)) { V.info ^= 1; any = 1; }
    if (any) V.osd_t = 240;
}

static void fmt_t(char *o, int n, double s)
{
    if (s < 0) { snprintf(o, n, "--:--"); return; }
    long t = (long)s;
    if (t >= 3600) snprintf(o, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else snprintf(o, n, "%ld:%02ld", t / 60, t % 60);
}

void video_draw(void)
{
    pthread_mutex_lock(&mtx);
    if (V.have_frame) gfx_blit(0, 0, SCREEN_W, SCREEN_H, V.rgb[V.front]);
    else gfx_clear(0xFF000000);
    int vw = V.vw, vh = V.vh, fps = V.fps;
    char err[200];
    snprintf(err, sizeof err, "%s", V.err);
    pthread_mutex_unlock(&mtx);

    if (V.failed || V.ended) {
        gfx_rect_blend(0, SCREEN_H / 2 - 90, SCREEN_W, 180, 0xFF000000, 200);
        gfx_text(120, SCREEN_H / 2 - 60, V.name, 0xFFFFFFFF, SCREEN_W - 240);
        gfx_text(120, SCREEN_H / 2 - 10, V.failed ? err : "End of video", V.failed ? 0xFFE5534B : 0xFF8995A1, SCREEN_W - 240);
        gfx_hint(120, SCREEN_H / 2 + 40, "[X] / [O] Close", 0xFF8995A1, 0);
        return;
    }
    if (!V.have_frame) {
        gfx_text(120, SCREEN_H / 2 - 20, "Loading video...", 0xFF8995A1, 0);
        gfx_text(120, SCREEN_H / 2 + 30, V.name, 0xFF8995A1, SCREEN_W - 240);
        return;
    }
    if (V.osd_t > 0 || V.paused) {
        if (V.osd_t > 0) V.osd_t--;
        AudioStatus st;
        audio_status(&st);
        double pos = V.vtime, len = V.dur > 0 ? V.dur : V.sound ? st.len_s : -1;
        char a[32], b[32], buf[400];
        int y = SCREEN_H - 190;
        gfx_rect_blend(0, y, SCREEN_W, 190, 0xFF000000, 190);
        gfx_text(80, y + 20, V.name, 0xFFFFFFFF, SCREEN_W - 600);
        snprintf(buf, sizeof buf, "%s%s", V.paused ? "PAUSED   " : "", V.sound ? "" : V.sound_err);
        gfx_text(SCREEN_W - 80 - gfx_text_width(buf), y + 20, buf,
                 V.paused ? 0xFFFFC857 : V.silent ? 0xFF8995A1 : 0xFFE5534B, 520);
        fmt_t(a, sizeof a, pos);
        fmt_t(b, sizeof b, len);
        gfx_rect(80, y + 76, SCREEN_W - 160, 10, 0xFF2A333D);
        if (len > 0) {
            double fr = pos / len;
            if (fr > 1) fr = 1;
            if (fr < 0) fr = 0;
            gfx_rect(80, y + 76, (int)((SCREEN_W - 160) * fr), 10, 0xFF3A7BD5);
        }
        snprintf(buf, sizeof buf, "%s / %s", a, b);
        gfx_text(80, y + 96, buf, 0xFFE8ECEF, 0);
        if (V.info) {
            snprintf(buf, sizeof buf, "%dx%d  %d fps  volume %d%%", vw, vh, fps, st.volume);
            gfx_text(SCREEN_W - 80 - gfx_text_width(buf), y + 96, buf, 0xFF8995A1, 0);
        }
        gfx_hint(80, y + 140, "[X] Pause  [LEFT/RIGHT] -10s/+10s  [L1/R1] -1 min/+1 min  [UP/DOWN] Volume  [TRIANGLE] Info  [O] Close",
                 0xFF8995A1, SCREEN_W - 160);
    }
}
