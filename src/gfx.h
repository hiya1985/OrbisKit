#pragma once
#include <stdint.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

int  gfx_init(void);
void gfx_clear(uint32_t color);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
/* Fill with alpha 0..255 over the existing content. */
void gfx_rect_blend(int x, int y, int w, int h, uint32_t color, int alpha);
/* Draws text; stops at maxw pixels (0 = unlimited). Returns width drawn. */
int  gfx_text(int x, int y, const char *s, uint32_t color, int maxw);
int  gfx_text_width(const char *s);
/* Like gfx_text, but "[X]", "[O]", "[L1/R1]"... are drawn as button icons. Only for the app's
 * own hint strings: file names and file contents go through gfx_text so they show as typed. */
int  gfx_hint(int x, int y, const char *s, uint32_t color, int maxw);
int  gfx_hint_width(const char *s);
/* Copies a w*h ARGB image to (x,y), clipped to the screen. */
void gfx_blit(int x, int y, int w, int h, const uint32_t *px);
int  gfx_font_w(void);
int  gfx_font_h(void);
void gfx_present(void);
