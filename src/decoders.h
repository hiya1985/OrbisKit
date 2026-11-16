#pragma once
/* One interface over the in-app audio decoders: MP3 (dr_mp3), FLAC (dr_flac),
 * WAV (dr_wav) and OGG Vorbis (stb_vorbis). Everything is decoded on the CPU inside
 * the app - the PS4's own audio decoder libraries are not used (they refused to start). */
#include <stdint.h>

typedef struct Decoder Decoder;

#define DEC_UNKNOWN_LEN UINT64_MAX

/* Known by extension: .mp3 .flac .wav .ogg .oga, and the AAC sound of .m4a .mp4 .m4v .mov */
int      dec_supported(const char *name);
Decoder *dec_open(const char *path, char *err, int errn);
int      dec_channels(const Decoder *d);
int      dec_rate(const Decoder *d);
uint64_t dec_frames(const Decoder *d);            /* DEC_UNKNOWN_LEN if not known */
const char *dec_format(const Decoder *d);         /* "MP3", "FLAC", ... */
/* Reads up to `frames` interleaved float frames (dec_channels() floats each). 0 = end. */
int      dec_read(Decoder *d, float *out, int frames);
int      dec_seek(Decoder *d, uint64_t frame);    /* 0 on success */
void     dec_close(Decoder *d);
