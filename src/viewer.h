#pragma once
/* Read-only text / hex viewer for files. */

int  viewer_open(const char *path, char *err, int errn);   /* 0 on success */
int  viewer_active(void);
void viewer_input(void);
void viewer_draw(void);
