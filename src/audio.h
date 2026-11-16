#pragma once
/* Background music player. Files are decoded in the app (decoders.c), resampled to 48 kHz
 * and played through sceAudioOut on a thread, so music keeps playing while you browse. */

enum { AMODE_ORDER, AMODE_REPEAT_ALL, AMODE_REPEAT_ONE, AMODE_SHUFFLE, AMODE_COUNT };
#define AUDIO_BARS 24

typedef struct {
    int  active;            /* a playlist is loaded (playing or paused) */
    int  paused;
    int  index, count;      /* position in the playlist (1-based for display: index + 1) */
    double pos_s, len_s;    /* len_s < 0: length unknown */
    int  volume;            /* 0..100 */
    int  mode;              /* AMODE_* */
    int  rate, channels;
    char name[256];         /* file name of the current track */
    char folder[256];
    char format[8];
    char err[200];          /* last error (a file that couldn't be played), "" if none */
    float levels[AUDIO_BARS];
} AudioStatus;

int  audio_is_audio_name(const char *name);
/* Plays paths[start], then the rest of the list. The paths are copied. 0 on success. */
int  audio_play(char **paths, int n, int start, char *err, int errn);
void audio_stop(void);
void audio_pause_toggle(void);
void audio_skip(int dir);          /* -1 previous (or restart if past 3 s), +1 next */
void audio_seek(int secs);         /* relative, e.g. -10 / +10 */
void audio_volume(int delta);      /* percent points */
void audio_cycle_mode(void);
void audio_status(AudioStatus *st);
int  audio_active(void);
const char *audio_mode_name(int mode);
/* Restores the saved volume / mode (settings.ini). */
void audio_set_prefs(int volume, int mode);
/* Video sound: plays one file's sound track only (from start_s seconds), no playlist. */
int  audio_play_single(const char *path, double start_s, char *err, int errn);
void audio_seek_to(double secs);   /* absolute */
void audio_set_paused(int on);
double audio_position(void);       /* seconds played of the current track */
