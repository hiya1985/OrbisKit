/* Music player: one thread decodes the current track, resamples it to 48 kHz stereo and
 * hands 1024-frame buffers to sceAudioOut (which blocks until the previous one played, so
 * the thread is paced by the audio hardware). The UI only sets request flags under the mutex
 * and reads a status copy - it never waits on decoding. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "compat.h"
#include "decoders.h"
#include "dlog.h"
#include "fsops.h"

#define OUT_RATE   48000
#define GRAIN      1024          /* frames per sceAudioOutOutput call (~21 ms) */
#define CHUNK      2048          /* frames decoded at a time */

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int port = -1;            /* opened once, kept for the app's lifetime */

static struct {
    /* playlist (owned by the player thread while it runs) */
    char **list;
    int n, *order, idx;
    /* requests from the UI */
    volatile int stop, skip, restart;
    volatile int single;         /* the sound of a video: one file, never repeats or moves on */
    volatile int seek_pending;
    double seek_to;              /* seconds, absolute */
    volatile int paused, volume, mode;
    /* status */
    volatile int running;
    double pos_s, len_s;
    int rate, channels;
    char name[256], folder[256], format[8], err[200];
    float levels[AUDIO_BARS];
} P = { .volume = 80 };

/* Music files only: .mp4/.m4v/.mov are videos (their sound is played by the video player). */
int audio_is_audio_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v") || !strcasecmp(dot, ".mov"))) return 0;
    return dec_supported(name);
}

const char *audio_mode_name(int mode)
{
    static const char *n[AMODE_COUNT] = { "Play folder once", "Repeat all", "Repeat one", "Shuffle" };
    return (mode >= 0 && mode < AMODE_COUNT) ? n[mode] : "?";
}

static void shuffle_order(int keep_first)
{
    for (int i = 0; i < P.n; i++) P.order[i] = i;
    if (P.n < 2) return;
    /* the track the user picked plays first; the rest is random */
    int first = keep_first;
    P.order[0] = first; P.order[first] = 0;
    for (int i = P.n - 1; i > 1; i--) {
        int j = 1 + rand() % i;
        int t = P.order[i]; P.order[i] = P.order[j]; P.order[j] = t;
    }
}

/* Copies the decoder's frames into stereo floats (mono is doubled, extra channels dropped
 * after a simple centre/surround fold-down into left and right). */
static void to_stereo(const float *in, int ch, int frames, float *out)
{
    for (int i = 0; i < frames; i++) {
        const float *f = in + i * ch;
        float l, r;
        if (ch == 1) l = r = f[0];
        else if (ch == 2) { l = f[0]; r = f[1]; }
        else {
            /* 5.1 order in all four formats: L R C LFE SL SR (WAV/FLAC/MP3); Vorbis: L C R ... */
            l = f[0] * 0.6f; r = f[1] * 0.6f;
            if (ch >= 3) { l += f[2] * 0.4f; r += f[2] * 0.4f; }
            if (ch >= 6) { l += f[4] * 0.3f; r += f[5] * 0.3f; }
        }
        out[i * 2] = l;
        out[i * 2 + 1] = r;
    }
}

static int open_port(void)
{
    if (port >= 0) return 0;
    int r = sceAudioOutInit();
    dlog("audio: sceAudioOutInit 0x%08X", r);   /* "already initialised" is fine too */
    port = sceAudioOutOpen(AUDIO_OUT_USER_SYSTEM, AUDIO_OUT_PORT_MAIN, 0, GRAIN, OUT_RATE, AUDIO_OUT_S16_STEREO);
    dlog("audio: sceAudioOutOpen -> 0x%08X", port);
    if (port < 0) { port = -1; return -1; }
    return 0;
}

static void set_levels(const int16_t *buf)
{
    float lv[AUDIO_BARS];
    int per = GRAIN / AUDIO_BARS;
    for (int b = 0; b < AUDIO_BARS; b++) {
        float peak = 0;
        for (int i = b * per; i < (b + 1) * per; i++) {
            int v = buf[i * 2] + buf[i * 2 + 1];
            if (v < 0) v = -v;
            if (v > peak) peak = (float)v;
        }
        lv[b] = peak / 65536.0f;
    }
    pthread_mutex_lock(&mtx);
    for (int b = 0; b < AUDIO_BARS; b++)
        P.levels[b] = lv[b] > P.levels[b] ? lv[b] : P.levels[b] * 0.85f;   /* fall off slowly */
    pthread_mutex_unlock(&mtx);
}

/* Plays one track. Returns 1 when it ended by itself, 0 when stopped/skipped, -1 on error. */
static int play_track(const char *path, int16_t *out, float *dec, float *ib)
{
    char err[160];
    Decoder *d = dec_open(path, err, sizeof err);
    if (!d) {
        dlog("audio: %s: %s", path, err);
        pthread_mutex_lock(&mtx);
        snprintf(P.err, sizeof P.err, "%s: %s", path_base(path), err);
        pthread_mutex_unlock(&mtx);
        return -1;
    }
    int ch = dec_channels(d), rate = dec_rate(d);
    uint64_t frames = dec_frames(d);
    double step = (double)rate / OUT_RATE;
    dlog("audio: playing %s (%s, %d ch, %d Hz, %s frames)", path, dec_format(d), ch, rate,
         frames == DEC_UNKNOWN_LEN ? "unknown" : "known");

    pthread_mutex_lock(&mtx);
    snprintf(P.name, sizeof P.name, "%s", path_base(path));
    {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", path);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        snprintf(P.folder, sizeof P.folder, "%s", dir[0] ? dir : "/");
    }
    snprintf(P.format, sizeof P.format, "%s", dec_format(d));
    P.rate = rate;
    P.channels = ch;
    P.pos_s = 0;
    P.len_s = frames == DEC_UNKNOWN_LEN ? -1 : (double)frames / rate;
    pthread_mutex_unlock(&mtx);

    /* ib holds stereo source frames; ipos is the read position inside it (fractional) */
    int ilen = 0;
    double ipos = 0, src_pos = 0;    /* src_pos: frames of the source played so far */
    int ended = 0, result = 1;

    for (;;) {
        if (P.stop || P.skip || P.restart) { result = 0; break; }
        if (P.seek_pending) {
            pthread_mutex_lock(&mtx);
            double t = P.seek_to;
            P.seek_pending = 0;
            pthread_mutex_unlock(&mtx);
            if (t < 0) t = 0;
            if (P.len_s > 0 && t > P.len_s - 1) t = P.len_s - 1;
            uint64_t f = (uint64_t)(t * rate);
            if (dec_seek(d, f) == 0) { src_pos = (double)f; ilen = 0; ipos = 0; ended = 0; }
        }
        if (P.paused) {
            memset(out, 0, GRAIN * 4);
            sceAudioOutOutput(port, out);   /* silence keeps the port's timing steady */
            set_levels(out);
            continue;
        }
        float vol = P.volume / 100.0f;
        vol = vol * vol;                     /* closer to how loudness is heard */
        int k;
        for (k = 0; k < GRAIN; k++) {
            int i = (int)ipos;
            while (i + 1 >= ilen && !ended) {
                /* keep the last frame (for interpolation), decode more after it */
                int keep = 0;
                if (ilen > 0) { ib[0] = ib[(ilen - 1) * 2]; ib[1] = ib[(ilen - 1) * 2 + 1]; keep = 1; ipos -= ilen - 1; }
                else ipos = 0;
                int got = dec_read(d, dec, CHUNK);
                if (got <= 0) { ended = 1; ilen = keep; break; }
                to_stereo(dec, ch, got, ib + keep * 2);
                ilen = keep + got;
                i = (int)ipos;
            }
            if (i + 1 >= ilen) break;        /* end of the track */
            float fr = (float)(ipos - i);
            for (int c = 0; c < 2; c++) {
                float a = ib[i * 2 + c], b = ib[(i + 1) * 2 + c];
                float v = (a + (b - a) * fr) * vol;
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                out[k * 2 + c] = (int16_t)lrintf(v * 32767.0f);
            }
            ipos += step;
            src_pos += step;
        }
        if (k == 0) break;
        if (k < GRAIN) memset(out + k * 2, 0, (GRAIN - k) * 4);
        sceAudioOutOutput(port, out);
        set_levels(out);
        pthread_mutex_lock(&mtx);
        P.pos_s = src_pos / rate;
        pthread_mutex_unlock(&mtx);
        if (k < GRAIN) break;
    }
    dec_close(d);
    return result;
}

static void *player_thread(void *arg)
{
    (void)arg;
    int16_t *out = malloc(GRAIN * 4);
    float *dec = malloc(CHUNK * 8 * sizeof(float));          /* up to 8 channels */
    float *ib = malloc((CHUNK + 1) * 2 * sizeof(float));
    int fails = 0;
    if (!out || !dec || !ib) goto done;

    while (!P.stop) {
        pthread_mutex_lock(&mtx);
        int cur = P.order[P.idx];
        pthread_mutex_unlock(&mtx);
        P.restart = 0;
        int r = play_track(P.list[cur], out, dec, ib);
        if (P.stop) break;
        int skip = P.skip;
        P.skip = 0;
        if (P.restart) continue;             /* "previous" within the first 3 s: same track again */
        if (r < 0) {
            if (++fails >= P.n) { dlog("audio: no playable file in the list"); break; }
        } else fails = 0;
        if (P.single) break;
        if (r == 1 && P.mode == AMODE_REPEAT_ONE) continue;
        int dir = skip < 0 ? -1 : 1, last = 0;
        pthread_mutex_lock(&mtx);
        int next = P.idx + dir;
        if (next >= P.n) {
            if (P.mode == AMODE_ORDER && !skip) last = 1;   /* played the whole folder */
            next = 0;
            if (P.mode == AMODE_SHUFFLE) {
                /* new random round; don't start it with the track that just played */
                shuffle_order(rand() % P.n);
                if (P.n > 1 && P.order[0] == cur) { P.order[0] = P.order[1]; P.order[1] = cur; }
            }
        }
        if (next < 0) next = P.n - 1;
        P.idx = next;
        pthread_mutex_unlock(&mtx);
        if (last) break;
    }
done:
    if (port >= 0) {
        sceAudioOutOutput(port, NULL);   /* wait for the last buffer */
        /* close the output when nothing plays: no open audio port is left when the app is closed */
        sceAudioOutClose(port);
        port = -1;
    }
    free(out); free(dec); free(ib);
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < P.n; i++) free(P.list[i]);
    free(P.list); free(P.order);
    P.list = NULL; P.order = NULL; P.n = 0;
    memset(P.levels, 0, sizeof P.levels);
    P.running = 0;
    pthread_mutex_unlock(&mtx);
    dlog("audio: player stopped");
    return NULL;
}

int audio_active(void) { return P.running; }

void audio_stop(void)
{
    if (!P.running) return;
    P.stop = 1;
    /* the thread finishes the current ~21 ms buffer and exits */
    for (int i = 0; i < 200 && P.running; i++) sceKernelUsleep(10000);
}

static int next_single;   /* audio_play_single: set under the same lock that starts the thread */

int audio_play(char **paths, int n, int start, char *err, int errn)
{
    if (n <= 0 || start < 0 || start >= n) { snprintf(err, errn, "Nothing to play"); return -1; }
    audio_stop();
    if (P.running) { snprintf(err, errn, "The player is still busy - try again"); return -1; }
    if (open_port() != 0) { snprintf(err, errn, "Couldn't open the audio output (0x%08X)", port); return -1; }
    char **l = calloc(n, sizeof *l);
    int *o = calloc(n, sizeof *o);
    if (!l || !o) { free(l); free(o); snprintf(err, errn, "Out of memory"); return -1; }
    int k = 0, first = 0;
    for (int i = 0; i < n; i++) {
        if (!paths[i] || !(l[k] = strdup(paths[i]))) continue;
        if (i == start) first = k;
        k++;
    }
    if (!k) { free(l); free(o); snprintf(err, errn, "Out of memory"); return -1; }
    pthread_mutex_lock(&mtx);
    P.list = l; P.order = o; P.n = k;
    P.stop = P.skip = P.restart = P.seek_pending = P.paused = 0;
    P.single = next_single;
    P.err[0] = P.name[0] = 0;
    P.pos_s = 0; P.len_s = -1;
    if (P.mode == AMODE_SHUFFLE) { shuffle_order(first); P.idx = 0; }
    else { for (int i = 0; i < k; i++) o[i] = i; P.idx = first; }
    P.running = 1;
    pthread_mutex_unlock(&mtx);
    if (thread_start(player_thread, NULL, STACK_JOB, 1) != 0) {
        pthread_mutex_lock(&mtx);
        for (int i = 0; i < k; i++) free(l[i]);
        free(l); free(o);
        P.list = NULL; P.order = NULL; P.n = 0; P.running = 0;
        pthread_mutex_unlock(&mtx);
        snprintf(err, errn, "Couldn't start the player thread");
        return -1;
    }
    return 0;
}

void audio_pause_toggle(void) { if (P.running) P.paused ^= 1; }

void audio_skip(int dir)
{
    if (!P.running) return;
    if (dir < 0 && P.pos_s > 3.0) { P.restart = 1; return; }
    P.skip = dir < 0 ? -1 : 1;
}

int audio_play_single(const char *path, double start_s, char *err, int errn)
{
    char *one[1] = { (char *)path };
    next_single = 1;
    int r = audio_play(one, 1, 0, err, errn);
    next_single = 0;
    if (r != 0) return -1;
    if (start_s > 0.05) audio_seek_to(start_s);
    return 0;
}

void audio_seek_to(double secs)
{
    if (!P.running) return;
    pthread_mutex_lock(&mtx);
    P.seek_to = secs < 0 ? 0 : secs;
    P.seek_pending = 1;
    pthread_mutex_unlock(&mtx);
}

void audio_set_paused(int on) { if (P.running) P.paused = !!on; }

double audio_position(void)
{
    pthread_mutex_lock(&mtx);
    double p = P.seek_pending ? P.seek_to : P.pos_s;
    pthread_mutex_unlock(&mtx);
    return p;
}

void audio_seek(int secs)
{
    if (!P.running) return;
    pthread_mutex_lock(&mtx);
    /* several presses before the thread reacts add up */
    double base = P.seek_pending ? P.seek_to : P.pos_s;
    P.seek_to = base + secs;
    P.seek_pending = 1;
    pthread_mutex_unlock(&mtx);
}

void audio_volume(int delta)
{
    int v = P.volume + delta;
    P.volume = v < 0 ? 0 : v > 100 ? 100 : v;
}

void audio_cycle_mode(void)
{
    pthread_mutex_lock(&mtx);
    int m = (P.mode + 1) % AMODE_COUNT;
    if (P.running && P.n) {
        int cur = P.order[P.idx];
        if (m == AMODE_SHUFFLE) shuffle_order(cur);
        else for (int i = 0; i < P.n; i++) P.order[i] = i;
        P.idx = (m == AMODE_SHUFFLE) ? 0 : cur;
    }
    P.mode = m;
    pthread_mutex_unlock(&mtx);
}

void audio_status(AudioStatus *st)
{
    pthread_mutex_lock(&mtx);
    memset(st, 0, sizeof *st);
    st->active = P.running;
    st->paused = P.paused;
    st->index = P.idx;
    st->count = P.n;
    st->pos_s = P.pos_s;
    st->len_s = P.len_s;
    st->volume = P.volume;
    st->mode = P.mode;
    st->rate = P.rate;
    st->channels = P.channels;
    snprintf(st->name, sizeof st->name, "%s", P.name);
    snprintf(st->folder, sizeof st->folder, "%s", P.folder);
    snprintf(st->format, sizeof st->format, "%s", P.format);
    snprintf(st->err, sizeof st->err, "%s", P.err);
    memcpy(st->levels, P.levels, sizeof st->levels);
    pthread_mutex_unlock(&mtx);
}

void audio_set_prefs(int volume, int mode)
{
    if (volume >= 0 && volume <= 100) P.volume = volume;
    if (mode >= 0 && mode < AMODE_COUNT) P.mode = mode;
}
