#pragma once
/* Sound track of .mp4 / .m4a / .m4v / .mov files: the AAC samples are read with minimp4
 * and decoded in the app with the Helix AAC decoder (the PS4's own decoders refused to
 * start in earlier tests, and AvPlayer's audio stream never delivered data). */
#include <stdint.h>

typedef struct Mp4Audio Mp4Audio;

Mp4Audio *mp4a_open(const char *path, char *err, int errn);
int       mp4a_channels(const Mp4Audio *a);     /* 1 or 2 */
int       mp4a_rate(const Mp4Audio *a);
uint64_t  mp4a_frames(const Mp4Audio *a);       /* 0 if unknown */
int       mp4a_read(Mp4Audio *a, float *out, int frames);   /* interleaved; 0 = end */
int       mp4a_seek(Mp4Audio *a, uint64_t frame);           /* 0 on success */
void      mp4a_close(Mp4Audio *a);
