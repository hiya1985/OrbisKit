#pragma once
/* Full-screen picture viewer (PNG, JPG, BMP, GIF, TGA). */

int  is_image_name(const char *name);
int  imgview_open(const char *path, char *err, int errn);   /* 0 on success */
int  imgview_active(void);
void imgview_close(void);
void imgview_draw(const char *position);   /* e.g. "3 / 12" or NULL */
void imgview_toggle_info(void);
