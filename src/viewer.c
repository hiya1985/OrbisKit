#include "viewer.h"
#include "fsops.h"
#include "gfx.h"
#include "pad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LOAD   (4 * 1024 * 1024)   /* only the first 4 MB is shown */
#define HDR_Y      190
#define TOP        240
#define LINE_H     38
#define ROWS       19
#define BOTTOM_Y   992
#define MX         80
#define HEX_COLS   16

static struct {
    int active, hex;
    char path[1024];
    unsigned char *data;
    long len, file_size;
    long *lines;      /* start offsets of text lines */
    int nlines;
    int top;          /* first visible row */
} v;

static void build_lines(void)
{
    free(v.lines);
    v.nlines = 0;
    int cap = 1024;
    v.lines = malloc(cap * sizeof(long));
    if (!v.lines) return;
    v.lines[v.nlines++] = 0;
    for (long i = 0; i < v.len; i++) {
        if (v.data[i] == '\n' && i + 1 < v.len) {
            if (v.nlines == cap) {
                long *n = realloc(v.lines, (cap *= 2) * sizeof(long));
                if (!n) return;
                v.lines = n;
            }
            v.lines[v.nlines++] = i + 1;
        }
    }
}

int viewer_open(const char *path, char *err, int errn)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "Can't open file"); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); snprintf(err, errn, "Can't read file size"); return -1; }
    long n =size > MAX_LOAD ? MAX_LOAD : size;
    unsigned char *d = malloc(n + 1);
    if (!d) { fclose(f); snprintf(err, errn, "Out of memory"); return -1; }
    n = (long)fread(d, 1, n, f);
    fclose(f);
    d[n] = 0;

    free(v.data);
    v.data = d;
    v.len = n;
    v.file_size = size;
    v.top = 0;
    snprintf(v.path, sizeof v.path, "%s", path);

    /* binary if there are NUL bytes or many control chars at the start */
    long probe = n < 4096 ? n : 4096, ctrl = 0;
    for (long i = 0; i < probe; i++) {
        if (!d[i]) { ctrl = probe; break; }
        if (d[i] < 9 || (d[i] > 13 && d[i] < 32)) ctrl++;
    }
    v.hex = (n > 0 && ctrl * 10 > probe);
    build_lines();
    v.active = 1;
    return 0;
}

int viewer_active(void) { return v.active; }

static int total_rows(void)
{
    return v.hex ? (int)((v.len + HEX_COLS - 1) / HEX_COLS) : v.nlines;
}

void viewer_input(void)
{
    int max = total_rows() - ROWS;
    if (max < 0) max = 0;
    if (pad_pressed(BTN_UP)) v.top--;
    if (pad_pressed(BTN_DOWN)) v.top++;
    if (pad_pressed(BTN_LEFT)) v.top -= ROWS;
    if (pad_pressed(BTN_RIGHT)) v.top += ROWS;
    if (pad_pressed(BTN_L2)) v.top = 0;
    if (pad_pressed(BTN_R2)) v.top = max;
    if (pad_pressed(BTN_SQUARE)) { v.hex ^= 1; v.top = 0; }
    if (v.top > max) v.top = max;
    if (v.top < 0) v.top = 0;
    if (pad_pressed(BTN_CIRCLE)) {
        v.active = 0;
        free(v.data); v.data = NULL;
        free(v.lines); v.lines = NULL;
    }
}

static void draw_text_mode(void)
{
    char line[256];
    for (int r = 0; r < ROWS; r++) {
        int i = v.top + r;
        if (i >= v.nlines) break;
        long s = v.lines[i];
        long e = (i + 1 < v.nlines) ? v.lines[i + 1] : v.len;
        int k = 0;
        for (long p = s; p < e && k < (int)sizeof line - 1; p++) {
            unsigned char c = v.data[p];
            if (c == '\n' || c == '\r') continue;
            if (c == '\t') { for (int t = 0; t < 4 && k < (int)sizeof line - 1; t++) line[k++] = ' '; continue; }
            line[k++] = (c < 32) ? '.' : (char)c;
        }
        line[k] = 0;
        int y = TOP + r * LINE_H;
        char num[16];
        snprintf(num, sizeof num, "%6d", i + 1);
        gfx_text(MX, y, num, 0xFF5A6570, 0);
        gfx_text(MX + 130, y, line, 0xFFE8ECEF, SCREEN_W - 2 * MX - 140);
    }
}

static void draw_hex_mode(void)
{
    char buf[128];
    for (int r = 0; r < ROWS; r++) {
        long off = (long)(v.top + r) * HEX_COLS;
        if (off >= v.len) break;
        int y = TOP + r * LINE_H;
        snprintf(buf, sizeof buf, "%08lX", off);
        gfx_text(MX, y, buf, 0xFF5A6570, 0);
        int k = 0;
        char asc[HEX_COLS + 1];
        for (int c = 0; c < HEX_COLS; c++) {
            if (off + c < v.len) {
                unsigned char b = v.data[off + c];
                k += snprintf(buf + k, sizeof buf - k, "%02X ", b);
                asc[c] = (b >= 32 && b < 127) ? (char)b : '.';
            } else {
                k += snprintf(buf + k, sizeof buf - k, "   ");
                asc[c] = ' ';
            }
            if (c == 7) buf[k++] = ' ', buf[k] = 0;
        }
        asc[HEX_COLS] = 0;
        gfx_text(MX + 170, y, buf, 0xFFE8ECEF, 0);
        gfx_text(MX + 170 + 50 * gfx_font_w() + 20, y, asc, 0xFF57C17B, 0);
    }
}

void viewer_draw(void)
{
    if (!v.active) return;
    gfx_rect(0, 176, SCREEN_W, BOTTOM_Y - 176, 0xFF0F1318);
    char hdr[128], sz[32];
    fmt_size(sz, sizeof sz, v.file_size);
    snprintf(hdr, sizeof hdr, "%s  |  %s%s", v.hex ? "HEX" : "TEXT", sz,
             v.file_size > v.len ? "  (first 4 MB shown)" : "");
    gfx_text(MX, HDR_Y, path_base(v.path), 0xFF3A7BD5, 1100);
    gfx_text(SCREEN_W - MX - gfx_text_width(hdr), HDR_Y, hdr, 0xFF8995A1, 0);

    if (v.hex) draw_hex_mode(); else draw_text_mode();

    int tr = total_rows();
    if (tr > ROWS) {
        int h = ROWS * LINE_H, bh = h * ROWS / tr;
        if (bh < 30) bh = 30;
        int by = TOP + (int)((long long)(h - bh) * v.top / (tr - ROWS));   /* 64-bit: millions of lines */
        gfx_rect(SCREEN_W - MX + 20, TOP, 6, h, 0xFF2A333D);
        gfx_rect(SCREEN_W - MX + 20, by, 6, bh, 0xFF3A7BD5);
    }
}
