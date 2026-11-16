/* Audio decoders, all public domain / MIT-0 single-file libraries (src/ext):
 *   dr_mp3, dr_flac, dr_wav (github.com/mackron/dr_libs), stb_vorbis (github.com/nothings/stb). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
/* the toolchain's wchar_t (2 bytes) differs from clang's L"" literals (4): the unused _w file
   functions would not compile otherwise */
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"
#define DR_FLAC_NO_WCHAR
#define DR_WAV_NO_WCHAR

#define DR_MP3_IMPLEMENTATION
#include "ext/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG          /* .ogg goes to stb_vorbis */
#include "ext/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "ext/dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "ext/stb_vorbis.c"

#pragma clang diagnostic pop

#include "decoders.h"
#include "mp4audio.h"

enum { D_MP3, D_FLAC, D_WAV, D_OGG, D_MP4 };

struct Decoder {
    int type, channels, rate;
    uint64_t frames;
    drmp3 mp3;
    drwav wav;
    drflac *flac;
    stb_vorbis *ogg;
    Mp4Audio *m4;
};

static int ext_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return -1;
    if (!strcasecmp(dot, ".mp3")) return D_MP3;
    if (!strcasecmp(dot, ".flac")) return D_FLAC;
    if (!strcasecmp(dot, ".wav")) return D_WAV;
    if (!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga")) return D_OGG;
    if (!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v") ||
        !strcasecmp(dot, ".mov")) return D_MP4;
    return -1;
}

int dec_supported(const char *name) { return ext_type(name) >= 0; }

Decoder *dec_open(const char *path, char *err, int errn)
{
    int t = ext_type(path);
    if (t < 0) { snprintf(err, errn, "Not a supported audio file"); return NULL; }
    Decoder *d = calloc(1, sizeof *d);
    if (!d) { snprintf(err, errn, "Out of memory"); return NULL; }
    d->type = t;
    d->frames = DEC_UNKNOWN_LEN;
    switch (t) {
    case D_MP3:
        if (!drmp3_init_file(&d->mp3, path, NULL)) goto bad;
        d->channels = d->mp3.channels;
        d->rate = d->mp3.sampleRate;
        /* from the Xing/Info header when there is one; never a full-file scan (too slow) */
        if (d->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX) d->frames = d->mp3.totalPCMFrameCount;
        break;
    case D_FLAC:
        if (!(d->flac = drflac_open_file(path, NULL))) goto bad;
        d->channels = d->flac->channels;
        d->rate = d->flac->sampleRate;
        if (d->flac->totalPCMFrameCount) d->frames = d->flac->totalPCMFrameCount;
        break;
    case D_WAV:
        if (!drwav_init_file(&d->wav, path, NULL)) goto bad;
        d->channels = d->wav.channels;
        d->rate = d->wav.sampleRate;
        d->frames = d->wav.totalPCMFrameCount;
        break;
    case D_OGG: {
        int e = 0;
        if (!(d->ogg = stb_vorbis_open_filename(path, &e, NULL))) goto bad;
        stb_vorbis_info vi = stb_vorbis_get_info(d->ogg);
        d->channels = vi.channels;
        d->rate = vi.sample_rate;
        unsigned int n = stb_vorbis_stream_length_in_samples(d->ogg);
        if (n) d->frames = n;
        break;
    }
    case D_MP4:
        if (!(d->m4 = mp4a_open(path, err, errn))) { free(d); return NULL; }   /* err is specific */
        d->channels = mp4a_channels(d->m4);
        d->rate = mp4a_rate(d->m4);
        if (mp4a_frames(d->m4)) d->frames = mp4a_frames(d->m4);
        break;
    }
    if (d->channels < 1 || d->channels > 8 || d->rate < 8000 || d->rate > 192000) {
        snprintf(err, errn, "Unsupported format (%d channels, %d Hz)", d->channels, d->rate);
        dec_close(d);
        return NULL;
    }
    return d;
bad:
    snprintf(err, errn, "Can't read this file (damaged, or not really %s)", strrchr(path, '.'));
    free(d);
    return NULL;
}

int dec_channels(const Decoder *d) { return d->channels; }
int dec_rate(const Decoder *d) { return d->rate; }
uint64_t dec_frames(const Decoder *d) { return d->frames; }

const char *dec_format(const Decoder *d)
{
    static const char *n[] = { "MP3", "FLAC", "WAV", "OGG", "AAC" };
    return n[d->type];
}

int dec_read(Decoder *d, float *out, int frames)
{
    switch (d->type) {
    case D_MP3:  return (int)drmp3_read_pcm_frames_f32(&d->mp3, frames, out);
    case D_FLAC: return (int)drflac_read_pcm_frames_f32(d->flac, frames, out);
    case D_WAV:  return (int)drwav_read_pcm_frames_f32(&d->wav, frames, out);
    case D_OGG:  return stb_vorbis_get_samples_float_interleaved(d->ogg, d->channels, out, frames * d->channels);
    case D_MP4:  return mp4a_read(d->m4, out, frames);
    }
    return 0;
}

int dec_seek(Decoder *d, uint64_t frame)
{
    switch (d->type) {
    case D_MP3:  return drmp3_seek_to_pcm_frame(&d->mp3, frame) ? 0 : -1;
    case D_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame) ? 0 : -1;
    case D_WAV:  return drwav_seek_to_pcm_frame(&d->wav, frame) ? 0 : -1;
    case D_OGG:  return stb_vorbis_seek(d->ogg, (unsigned int)frame) ? 0 : -1;
    case D_MP4:  return mp4a_seek(d->m4, frame);
    }
    return -1;
}

void dec_close(Decoder *d)
{
    if (!d) return;
    switch (d->type) {
    case D_MP3:  drmp3_uninit(&d->mp3); break;
    case D_FLAC: drflac_close(d->flac); break;
    case D_WAV:  drwav_uninit(&d->wav); break;
    case D_OGG:  stb_vorbis_close(d->ogg); break;
    case D_MP4:  mp4a_close(d->m4); break;
    }
    free(d);
}
