#include "gfx.h"
#include "compat.h"
#include "font_data.h"
#include <stdlib.h>
#include <string.h>

#define FB_SIZE   0x800000          /* 1920*1080*4 rounded up to 2MB */
#define PIX_FMT   0x80000000u       /* A8R8G8B8 sRGB */
#define TILE_LIN  1
#define MEM_WC_GARLIC 3
#define PROT_RW_ALL   0x33

static int video = -1;
static void *fb[2];
static uint32_t *back;          /* cached RAM back buffer (WC memory is slow to read) */
static int cur;
static SceKernelEqueue flipq;

int gfx_init(void)
{
    video = sceVideoOutOpen(255, 0, 0, NULL);
    if (video < 0) return video;

    int64_t phys = 0;
    int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), FB_SIZE * 2,
                                          0x200000, MEM_WC_GARLIC, &phys);
    if (r) return r;
    void *mem = NULL;
    r = sceKernelMapDirectMemory(&mem, FB_SIZE * 2, PROT_RW_ALL, 0, phys, 0x200000);
    if (r) return r;
    memset(mem, 0, FB_SIZE * 2);
    fb[0] = mem;
    fb[1] = (char *)mem + FB_SIZE;

    uint8_t attr[64];
    memset(attr, 0, sizeof attr);
    sceVideoOutSetBufferAttribute(attr, PIX_FMT, TILE_LIN, 0, SCREEN_W, SCREEN_H, SCREEN_W);
    r = sceVideoOutRegisterBuffers(video, 0, fb, 2, attr);
    if (r) return r;
    sceVideoOutSetFlipRate(video, 0);
    sceKernelCreateEqueue(&flipq, "tbox_flip");
    sceVideoOutAddFlipEvent(flipq, video, NULL);

    back = (uint32_t *)malloc(SCREEN_W * SCREEN_H * 4);
    return back ? 0 : -1;
}

void gfx_present(void)
{
    memcpy(fb[cur], back, SCREEN_W * SCREEN_H * 4);
    sceVideoOutSubmitFlip(video, cur, 1 /* vsync */, 0);
    uint8_t ev[64];
    int n = 0;
    sceKernelWaitEqueue(flipq, ev, 1, &n, NULL);
    cur ^= 1;
}

void gfx_clear(uint32_t color)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) back[i] = color;
}

static int clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > SCREEN_W) *w = SCREEN_W - *x;
    if (*y + *h > SCREEN_H) *h = SCREEN_H - *y;
    return *w > 0 && *h > 0;
}

void gfx_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!clip(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = back + (y + j) * SCREEN_W + x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
    unsigned ia = 255 - a;
    uint32_t rb = (((src & 0xFF00FF) * a + (dst & 0xFF00FF) * ia) >> 8) & 0xFF00FF;
    uint32_t g  = (((src & 0x00FF00) * a + (dst & 0x00FF00) * ia) >> 8) & 0x00FF00;
    return 0xFF000000u | rb | g;
}

void gfx_rect_blend(int x, int y, int w, int h, uint32_t color, int alpha)
{
    if (!clip(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = back + (y + j) * SCREEN_W + x;
        for (int i = 0; i < w; i++) row[i] = blend(row[i], color, (unsigned)alpha);
    }
}

static void draw_glyph(int x, int y, int ch, uint32_t color)
{
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = font_data[ch - 32];
    for (int j = 0; j < FONT_H; j++) {
        int py = y + j;
        if (py < 0 || py >= SCREEN_H) continue;
        uint32_t *row = back + py * SCREEN_W;
        for (int i = 0; i < FONT_W; i++) {
            int px = x + i;
            unsigned a = g[j * FONT_W + i];
            if (!a || px < 0 || px >= SCREEN_W) continue;
            row[px] = (a >= 250) ? (color | 0xFF000000u) : blend(row[px], color, a);
        }
    }
}

/* The font is ASCII only: decode UTF-8 and transliterate what we can
 * (accented Latin letters -> base letter, (TM) -> "TM", etc.), else '?'. */
static const char latin1[] =   /* U+00C0 .. U+00FF */
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";

static const char *translit(const unsigned char **pp)
{
    static char out[4];
    const unsigned char *p = *pp;
    unsigned cp;
    int len;
    if (*p < 0x80) { out[0] = (char)*p; out[1] = 0; *pp = p + 1; return out; }
    if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
    else { *pp = p + 1; return "?"; }
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = p + i; return "?"; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp = p + len;
    if (cp >= 0xC0 && cp <= 0xFF) { out[0] = latin1[cp - 0xC0]; out[1] = 0; return out; }
    switch (cp) {
    case 0x2122: return "TM";
    case 0x00AE: return "(R)";
    case 0x00A9: return "(C)";
    case 0x2013: case 0x2014: return "-";
    case 0x2018: case 0x2019: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2026: return "...";
    case 0x00A0: case 0x3000: return " ";
    case 0x00D7: return "x";
    }
    return "?";
}

/* ---------- controller button icons ----------
 * In text drawn with gfx_hint (never gfx_text, which draws file names and file contents as-is), "[X]", "[O]", "[SQUARE]", "[TRIANGLE]", "[OPTIONS]",
 * "[L1]".."[R2]", "[D-PAD]", "[LEFT/RIGHT]" and "[UP/DOWN]" (also short forms like "[SQ]",
 * and "/" lists like "[L1/R1]") are drawn as PlayStation button icons instead of words. */
enum { BI_CROSS = 1, BI_CIRCLE, BI_SQUARE, BI_TRIANGLE, BI_PILL, BI_DPAD, BI_DPAD_LR, BI_DPAD_UD };
typedef struct { int kind; const char *label; } BtnIcon;
#define BTN_MAXPARTS 4
typedef struct { int n, len, width; BtnIcon part[BTN_MAXPARTS]; } BtnTok;

#define ICON_D   30      /* face button / d-pad size */
#define ICON_GAP 4

static int btn_lookup(const char *s, int n, BtnIcon *bi)
{
    static const struct { const char *name; int kind; const char *label; } tab[] = {
        { "X", BI_CROSS, 0 }, { "CROSS", BI_CROSS, 0 }, { "O", BI_CIRCLE, 0 }, { "CIRCLE", BI_CIRCLE, 0 },
        { "SQ", BI_SQUARE, 0 }, { "SQUARE", BI_SQUARE, 0 }, { "TRI", BI_TRIANGLE, 0 }, { "TRIANGLE", BI_TRIANGLE, 0 },
        { "L1", BI_PILL, "L1" }, { "R1", BI_PILL, "R1" }, { "L2", BI_PILL, "L2" }, { "R2", BI_PILL, "R2" },
        { "OPT", BI_PILL, "OPTIONS" }, { "OPTIONS", BI_PILL, "OPTIONS" },
        { "D-PAD", BI_DPAD, 0 }, { "DPAD", BI_DPAD, 0 },
        { "LEFT/RIGHT", BI_DPAD_LR, 0 }, { "UP/DOWN", BI_DPAD_UD, 0 },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if ((int)strlen(tab[i].name) == n && !memcmp(tab[i].name, s, (size_t)n)) {
            bi->kind = tab[i].kind; bi->label = tab[i].label; return 1;
        }
    return 0;
}

static int btn_width(const BtnIcon *bi)
{
    return (bi->kind == BI_PILL ? (int)strlen(bi->label) * FONT_W + 14 : ICON_D) + ICON_GAP;
}

/* p points at '['. Fills t and returns 1 if the bracket holds button names. */
static int btn_parse(const unsigned char *p, BtnTok *t)
{
    const unsigned char *e = p + 1;
    while (*e && *e != ']' && e - p < 24) e++;
    if (*e != ']' || e == p + 1) return 0;
    const char *in = (const char *)p + 1;
    int n = (int)(e - p - 1);
    t->n = 0;
    if (btn_lookup(in, n, &t->part[0])) t->n = 1;
    else {
        for (int i = 0, st = 0; i <= n; i++) {
            if (i < n && in[i] != '/') continue;
            if (t->n == BTN_MAXPARTS || !btn_lookup(in + st, i - st, &t->part[t->n])) return 0;
            t->n++;
            st = i + 1;
        }
    }
    t->len = n + 2;
    t->width = 0;
    for (int i = 0; i < t->n; i++) t->width += btn_width(&t->part[i]) + (i ? FONT_W : 0);
    return 1;
}

static inline void plot(int x, int y, uint32_t color, float cov)
{
    if (cov <= 0.0f || x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    uint32_t *px = back + y * SCREEN_W + x;
    *px = cov >= 1.0f ? (color | 0xFF000000u) : blend(*px, color, (unsigned)(cov * 255.0f));
}

static inline float cov_of(float d) { d = 0.5f - d; return d < 0 ? 0 : d > 1 ? 1 : d; }
static inline float fabs_(float v) { return v < 0 ? -v : v; }
static inline float fmax_(float a, float b) { return a > b ? a : b; }
static inline float fmin_(float a, float b) { return a < b ? a : b; }
static inline float len2(float x, float y) { return __builtin_sqrtf(x * x + y * y); }

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float h = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    return len2(px - ax - dx * h, py - ay - dy * h);
}

/* Signed distance to a box with half size (bx, by) and corner radius r, centred on 0. */
static float box_dist(float px, float py, float bx, float by, float r)
{
    float qx = fabs_(px) - bx + r, qy = fabs_(py) - by + r;
    return len2(fmax_(qx, 0), fmax_(qy, 0)) + fmin_(fmax_(qx, qy), 0) - r;
}

#define ICON_BG    0xFF3B434Du
#define COL_CROSS  0xFF7EB3F0u
#define COL_CIRCLE 0xFFF06A6Eu
#define COL_SQUARE 0xFFE38FD3u
#define COL_TRI    0xFF45D9A6u

static void draw_face(int x, int y, int kind)
{
    float cx = x + ICON_D / 2.0f, cy = y + FONT_H / 2.0f;
    uint32_t col = kind == BI_CROSS ? COL_CROSS : kind == BI_CIRCLE ? COL_CIRCLE
                 : kind == BI_SQUARE ? COL_SQUARE : COL_TRI;
    for (int j = (int)cy - 16; j <= (int)cy + 16; j++)
        for (int i = x; i < x + ICON_D; i++) {
            float fx = i + 0.5f - cx, fy = j + 0.5f - cy, d;
            plot(i, j, ICON_BG, cov_of(len2(fx, fy) - ICON_D / 2.0f));
            switch (kind) {
            case BI_CROSS:
                d = fmin_(seg_dist(fx, fy, -6, -6, 6, 6), seg_dist(fx, fy, -6, 6, 6, -6)) - 1.7f; break;
            case BI_CIRCLE:
                d = fabs_(len2(fx, fy) - 7.0f) - 1.7f; break;
            case BI_SQUARE:
                d = fabs_(box_dist(fx, fy, 6.5f, 6.5f, 0)) - 1.7f; break;
            default:
                d = fmin_(fmin_(seg_dist(fx, fy, 0, -7, 7.5f, 5.5f), seg_dist(fx, fy, 7.5f, 5.5f, -7.5f, 5.5f)),
                          seg_dist(fx, fy, -7.5f, 5.5f, 0, -7)) - 1.7f; break;
            }
            plot(i, j, col, cov_of(d));
        }
}

static void draw_pill(int x, int y, const char *label)
{
    int w = (int)strlen(label) * FONT_W + 14;
    float cx = x + w / 2.0f, cy = y + FONT_H / 2.0f;
    for (int j = (int)cy - 14; j <= (int)cy + 14; j++)
        for (int i = x; i < x + w; i++)
            plot(i, j, 0xFFC9D0D8u, cov_of(box_dist(i + 0.5f - cx, j + 0.5f - cy, w / 2.0f, 13.0f, 7.0f)));
    for (int k = 0; label[k]; k++) draw_glyph(x + 7 + k * FONT_W, y, (unsigned char)label[k], 0xFF141A20u);
}

static void draw_dpad(int x, int y, int kind)
{
    float cx = x + ICON_D / 2.0f, cy = y + FONT_H / 2.0f;
    for (int j = (int)cy - 16; j <= (int)cy + 16; j++)
        for (int i = x; i < x + ICON_D; i++) {
            float fx = i + 0.5f - cx, fy = j + 0.5f - cy;
            float d = fmin_(box_dist(fx, fy, 5.0f, 14.0f, 2.0f), box_dist(fx, fy, 14.0f, 5.0f, 2.0f));
            int lit = kind == BI_DPAD || (kind == BI_DPAD_LR ? fabs_(fx) > 5.5f : fabs_(fy) > 5.5f);
            plot(i, j, lit ? 0xFFE6EAEEu : 0xFF5C6672u, cov_of(d));
        }
}

static int draw_btn_tok(int cx, int y, const BtnTok *t, uint32_t color)
{
    for (int i = 0; i < t->n; i++) {
        const BtnIcon *bi = &t->part[i];
        if (i) { draw_glyph(cx, y, '/', color); cx += FONT_W; }
        if (bi->kind == BI_PILL) draw_pill(cx, y, bi->label);
        else if (bi->kind >= BI_DPAD) draw_dpad(cx, y, bi->kind);
        else draw_face(cx, y, bi->kind);
        cx += btn_width(bi);
    }
    return cx;
}

static int g_icons;   /* set only while gfx_hint draws */

int gfx_text(int x, int y, const char *s, uint32_t color, int maxw)
{
    int cx = x;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        BtnTok bt;
        if (g_icons && *p == '[' && btn_parse(p, &bt)) {
            if (maxw && cx + bt.width - x > maxw) return cx - x;
            cx = draw_btn_tok(cx, y, &bt, color);
            p += bt.len;
            continue;
        }
        const char *t = translit(&p);
        for (; *t; t++) {
            /* cut: keep the last cell free for '~' (drawing it over a glyph smudged both) */
            int more = t[1] || *p;
            if (maxw && more && cx + 2 * FONT_W - x > maxw) {
                if (cx + FONT_W - x <= maxw) { draw_glyph(cx, y, '~', color); cx += FONT_W; }
                return cx - x;
            }
            if (maxw && cx + FONT_W - x > maxw) return cx - x;
            draw_glyph(cx, y, (unsigned char)*t, color);
            cx += FONT_W;
        }
    }
    return cx - x;
}

int gfx_text_width(const char *s)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        BtnTok bt;
        if (g_icons && *p == '[' && btn_parse(p, &bt)) { w += bt.width; p += bt.len; continue; }
        w += (int)strlen(translit(&p)) * FONT_W;
    }
    return w;
}

int gfx_hint(int x, int y, const char *s, uint32_t color, int maxw)
{
    g_icons = 1;
    int w = gfx_text(x, y, s, color, maxw);
    g_icons = 0;
    return w;
}

int gfx_hint_width(const char *s)
{
    g_icons = 1;
    int w = gfx_text_width(s);
    g_icons = 0;
    return w;
}

void gfx_blit(int x, int y, int w, int h, const uint32_t *px)
{
    int sx = 0, sy = 0, cw = w, ch = h;
    if (x < 0) { sx = -x; cw += x; x = 0; }
    if (y < 0) { sy = -y; ch += y; y = 0; }
    if (x + cw > SCREEN_W) cw = SCREEN_W - x;
    if (y + ch > SCREEN_H) ch = SCREEN_H - y;
    if (cw <= 0 || ch <= 0) return;
    for (int j = 0; j < ch; j++)
        memcpy(back + (y + j) * SCREEN_W + x, px + (size_t)(sy + j) * w + sx, cw * 4);
}

int gfx_font_w(void) { return FONT_W; }
int gfx_font_h(void) { return FONT_H; }
