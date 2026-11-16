#pragma once
/* Full-screen video player: the picture comes from the PS4's AvPlayer (hardware H.264),
 * the sound from the app's own AAC decoder through the music player (audio.c). */

int  video_is_video_name(const char *name);   /* .mp4 .m4v .mov */
int  video_open(const char *path, char *err, int errn);   /* 0 on success */
int  video_active(void);
void video_input(void);
void video_draw(void);
void video_close(void);
