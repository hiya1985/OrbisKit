#include "imgview.h"
#include "fsops.h"
#include "gfx.h"
#include <stb/stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PIXELS (16 * 1000 * 1000)   /* ~64 MB decoded (e.g. 4900x3260); keeps well inside app memory */
#define MAX_UPSCALE 4

static struct {
    int active, info;
    char path[1024];
    int src_w, src_h;
    int w, h;          /* scaled size */
    uint32_t *px;      /* scaled ARGB */
} iv;

int is_image_name(const char *name)
{
    static const char *ext[] = { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga" };
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

void imgview_close(void)
{
    free(iv.px);
    iv.px = NULL;
    iv.active = 0;
}

int imgview_open(const char *path, char *err, int errn)
{
    int w, h, comp;
    if (!stbi_info(path, &w, &h, &comp)) {
        snprintf(err, errn, "Not a supported image (%s)", stbi_failure_reason());
        return -1;
    }
    if ((long long)w * h > MAX_PIXELS) {
        snprintf(err, errn, "Image too large (%dx%d)", w, h);
        return -1;
    }
    unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
    if (!rgba) {
        snprintf(err, errn, "Decode failed: %s", stbi_failure_reason());
        return -1;
    }

    /* fit to screen, keep aspect ratio */
    double sx = (double)SCREEN_W / w, sy = (double)SCREEN_H / h;
    double s = sx < sy ? sx : sy;
    if (s > MAX_UPSCALE) s = MAX_UPSCALE;
    int dw = (int)(w * s), dh = (int)(h * s);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint32_t *out = malloc((size_t)dw * dh * 4);
    if (!out) {
        stbi_image_free(rgba);
        snprintf(err, errn, "Out of memory");
        return -1;
    }

    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * h / dh), y1 = (int)((long long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * w / dw), x1 = (int)((long long)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            /* box filter when shrinking (averages the covered source pixels) */
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            int stepy = (y1 - y0 > 4) ? (y1 - y0) / 4 : 1;   /* sample at most ~4x4 */
            int stepx = (x1 - x0 > 4) ? (x1 - x0) / 4 : 1;
            for (int yy = y0; yy < y1; yy += stepy)
                for (int xx = x0; xx < x1; xx += stepx) {
                    const unsigned char *p = rgba + ((size_t)yy * w + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            r /= n; g /= n; b /= n; a /= n;
            /* blend transparency over a dark grey */
            r = (r * a + 24 * (255 - a)) / 255;
            g = (g * a + 24 * (255 - a)) / 255;
            b = (b * a + 24 * (255 - a)) / 255;
            out[(size_t)y * dw + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    stbi_image_free(rgba);

    imgview_close();
    iv.px = out;
    iv.w = dw;
    iv.h = dh;
    iv.src_w = w;
    iv.src_h = h;
    snprintf(iv.path, sizeof iv.path, "%s", path);
    iv.active = 1;
    return 0;
}

int imgview_active(void) { return iv.active; }
void imgview_toggle_info(void) { iv.info ^= 1; }

void imgview_draw(const char *position)
{
    if (!iv.active) return;
    gfx_clear(0xFF000000);
    gfx_blit((SCREEN_W - iv.w) / 2, (SCREEN_H - iv.h) / 2, iv.w, iv.h, iv.px);
    if (!iv.info) {
        char buf[300];
        snprintf(buf, sizeof buf, "%s   %dx%d%s%s", path_base(iv.path), iv.src_w, iv.src_h,
                 position ? "   " : "", position ? position : "");
        int w = gfx_text_width(buf) + 40;
        if (w > SCREEN_W - 80) w = SCREEN_W - 80;
        gfx_rect_blend(40, 30, w, 54, 0xFF000000, 180);
        gfx_text(60, 40, buf, 0xFFFFFFFF, w - 40);
        const char *hint = "[LEFT/RIGHT] Prev/Next  [TRIANGLE] Hide info  [O] Close";
        int hw = gfx_hint_width(hint) + 40;
        gfx_rect_blend(40, SCREEN_H - 84, hw, 54, 0xFF000000, 180);
        gfx_hint(60, SCREEN_H - 74, hint, 0xFFB0B8C0, 0);
    }
}
