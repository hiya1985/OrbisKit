/* MP4 sound track reader: minimp4 (CC0, github.com/lieff/minimp4) finds the AAC samples,
 * the Helix AAC decoder (RPSL, src/ext/helix-aac) decodes them. AAC-LC and HE-AAC, mono or
 * stereo - the audio that PS4 captures, phones and most video files use. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#define MINIMP4_IMPLEMENTATION
#include "ext/minimp4.h"
#pragma clang diagnostic pop

#include "ext/helix-aac/aacdec.h"
#include "dlog.h"
#include "mp4audio.h"

#define CACHE   (256 * 1024)
#define MAXFRAME (64 * 1024)      /* largest AAC sample we accept */

struct Mp4Audio {
    int fd;
    int64_t size;
    /* read cache: minimp4 parses the index one byte at a time */
    unsigned char *cache;
    int64_t cache_off;
    int cache_len;
    MP4D_demux_t mp4;
    int mp4_open;
    unsigned track;
    unsigned nsamples, cur;        /* next AAC sample to decode */
    HAACDecoder aac;
    int channels, rate, spf;       /* spf: output frames per AAC sample (1024, or 2048 with SBR) */
    uint64_t frames;
    unsigned char *in;
    short pcm[AAC_MAX_NCHANS * AAC_MAX_NSAMPS * 2];
    float pend[2 * AAC_MAX_NSAMPS * 2];   /* decoded frames not handed out yet */
    int pend_n, pend_pos;
    int skip;                      /* frames to drop after a seek */
};

static int read_cb(int64_t off, void *buf, size_t n, void *token)
{
    Mp4Audio *a = token;
    if (off < 0 || off + (int64_t)n > a->size) return 1;
    unsigned char *dst = buf;
    while (n) {
        if (off >= a->cache_off && off < a->cache_off + a->cache_len) {
            size_t k = (size_t)(a->cache_off + a->cache_len - off);
            if (k > n) k = n;
            memcpy(dst, a->cache + (off - a->cache_off), k);
            dst += k; off += k; n -= k;
            continue;
        }
        if (n >= CACHE) {   /* big read: straight from the file */
            ssize_t r = pread(a->fd, dst, n, off);
            return r == (ssize_t)n ? 0 : 1;
        }
        ssize_t r = pread(a->fd, a->cache, CACHE, off);
        if (r <= 0) return 1;
        a->cache_off = off;
        a->cache_len = (int)r;
    }
    return 0;
}

static const int sf_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

/* Decodes the next AAC sample into pend[]. 1 ok, 0 end of track. */
static int decode_next(Mp4Audio *a)
{
    while (a->cur < a->nsamples) {
        unsigned bytes = 0, dts, pts, dur;
        int sync;
        MP4D_file_offset_t off = MP4D_frame_offset(&a->mp4, a->track, a->cur++, &bytes, &dts, &pts, &dur, &sync);
        if (!bytes || bytes > MAXFRAME || read_cb((int64_t)off, a->in, bytes, a)) continue;
        unsigned char *p = a->in;
        int left = (int)bytes;
        if (AACDecode(a->aac, &p, &left, a->pcm) != ERR_AAC_NONE) continue;   /* damaged sample: skip it */
        AACFrameInfo fi;
        AACGetLastFrameInfo(a->aac, &fi);
        if (fi.nChans < 1 || fi.outputSamps <= 0) continue;
        int n = fi.outputSamps / fi.nChans;
        if (!a->rate) { a->rate = fi.sampRateOut; a->channels = fi.nChans; a->spf = n; }
        /* the format can't change mid-file for the player: map to the first frame's layout */
        for (int i = 0; i < n; i++) {
            float l = a->pcm[i * fi.nChans] / 32768.0f;
            float r = fi.nChans > 1 ? a->pcm[i * fi.nChans + 1] / 32768.0f : l;
            if (a->channels == 1) a->pend[i] = (l + r) * 0.5f;
            else { a->pend[i * 2] = l; a->pend[i * 2 + 1] = r; }
        }
        a->pend_n = n;
        a->pend_pos = 0;
        return 1;
    }
    return 0;
}

Mp4Audio *mp4a_open(const char *path, char *err, int errn)
{
    Mp4Audio *a = calloc(1, sizeof *a);
    if (!a) { snprintf(err, errn, "Out of memory"); return NULL; }
    a->fd = -1;
    a->cache = malloc(CACHE);
    a->in = malloc(MAXFRAME);
    if (!a->cache || !a->in) { snprintf(err, errn, "Out of memory"); goto fail; }
    if ((a->fd = open(path, O_RDONLY)) < 0) { snprintf(err, errn, "Can't open the file"); goto fail; }
    a->size = lseek(a->fd, 0, SEEK_END);
    if (a->size <= 0) { snprintf(err, errn, "Empty file"); goto fail; }
    if (!MP4D_open(&a->mp4, read_cb, a, a->size)) {
        snprintf(err, errn, "Not a playable MP4 (no index - a recording that was never finished?)");
        goto fail;
    }
    a->mp4_open = 1;
    unsigned i;
    for (i = 0; i < a->mp4.track_count; i++) {
        MP4D_track_t *t = &a->mp4.track[i];
        if (t->handler_type == MP4D_HANDLER_TYPE_SOUN && t->object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3 &&
            t->sample_count) break;
    }
    if (i == a->mp4.track_count) { snprintf(err, errn, "No AAC sound track in this file"); goto fail; }
    a->track = i;
    MP4D_track_t *t = &a->mp4.track[i];
    a->nsamples = t->sample_count;

    /* AudioSpecificConfig: object type (5 bits), rate index (4), channel config (4) */
    int rate = t->SampleDescription.audio.samplerate_hz, ch = t->SampleDescription.audio.channelcount;
    if (t->dsi && t->dsi_bytes >= 2) {
        int sfi = ((t->dsi[0] & 7) << 1) | (t->dsi[1] >> 7);
        int cfg = (t->dsi[1] >> 3) & 15;
        if (sfi < 13) rate = sf_rates[sfi];
        if (cfg >= 1 && cfg <= 2) ch = cfg;
        else if (cfg > 2) { snprintf(err, errn, "Surround sound (%d channels) is not supported yet", cfg == 7 ? 8 : cfg); goto fail; }
    }
    if (ch < 1 || ch > 2 || rate < 7350) { snprintf(err, errn, "Unsupported sound format (%d ch, %d Hz)", ch, rate); goto fail; }

    if (!(a->aac = AACInitDecoder())) { snprintf(err, errn, "Out of memory (AAC decoder)"); goto fail; }
    AACFrameInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.nChans = ch;
    fi.sampRateCore = rate;
    fi.profile = AAC_PROFILE_LC;
    if (AACSetRawBlockParams(a->aac, 0, &fi) != ERR_AAC_NONE) { snprintf(err, errn, "AAC decoder setup failed"); goto fail; }

    /* the first sample tells the real output rate/channels (HE-AAC doubles the rate) */
    if (!decode_next(a)) { snprintf(err, errn, "Couldn't decode the sound track"); goto fail; }
    uint64_t dur = ((uint64_t)t->duration_hi << 32) | t->duration_lo;
    if (t->timescale && dur) a->frames = dur * (uint64_t)a->rate / t->timescale;
    else a->frames = (uint64_t)a->nsamples * a->spf;
    dlog("mp4 audio: %s: %u samples, %d Hz, %d ch, spf %d, track rate %d", path, a->nsamples, a->rate,
         a->channels, a->spf, rate);
    /* keep the decoded first frame: playback starts with it */
    return a;
fail:
    mp4a_close(a);
    return NULL;
}

int mp4a_channels(const Mp4Audio *a) { return a->channels; }
int mp4a_rate(const Mp4Audio *a) { return a->rate; }
uint64_t mp4a_frames(const Mp4Audio *a) { return a->frames; }

int mp4a_read(Mp4Audio *a, float *out, int frames)
{
    int done = 0;
    while (done < frames) {
        if (a->pend_pos >= a->pend_n && !decode_next(a)) break;
        if (a->skip) {
            int k = a->pend_n - a->pend_pos;
            if (k > a->skip) k = a->skip;
            a->pend_pos += k;
            a->skip -= k;
            continue;
        }
        int k = a->pend_n - a->pend_pos;
        if (k > frames - done) k = frames - done;
        memcpy(out + done * a->channels, a->pend + a->pend_pos * a->channels, (size_t)k * a->channels * sizeof(float));
        a->pend_pos += k;
        done += k;
    }
    return done;
}

int mp4a_seek(Mp4Audio *a, uint64_t frame)
{
    if (!a->spf) return -1;
    uint64_t s = frame / (uint64_t)a->spf;
    if (s >= a->nsamples) s = a->nsamples ? a->nsamples - 1 : 0;
    a->cur = (unsigned)s;
    a->skip = (int)(frame - s * (uint64_t)a->spf);
    if (a->skip >= a->spf) a->skip = 0;
    a->pend_n = a->pend_pos = 0;
    AACFlushCodec(a->aac);
    return 0;
}

void mp4a_close(Mp4Audio *a)
{
    if (!a) return;
    if (a->aac) AACFreeDecoder(a->aac);
    if (a->mp4_open) MP4D_close(&a->mp4);
    if (a->fd >= 0) close(a->fd);
    free(a->cache);
    free(a->in);
    free(a);
}
