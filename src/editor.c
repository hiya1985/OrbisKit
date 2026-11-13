#include "editor.h"
#include "dlog.h"
#include "fsops.h"
#include "gfx.h"
#include "pad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FILE   (1024 * 1024)
#define MAX_LINES  20000
#define MAX_LINE   1000          /* on-screen keyboard limit */
#define TOP        240
#define LINE_H     38
#define ROWS       19
#define MX         80

static struct {
    int active, modified, crlf, final_nl;
    char path[1024];
    char **lines;
    int n, cap, sel, scroll;
    int confirm_exit;
    const char *msg;
} ed;

static void free_lines(void)
{
    for (int i = 0; i < ed.n; i++) free(ed.lines[i]);
    free(ed.lines);
    ed.lines = NULL;
    ed.n = ed.cap = 0;
}

static int insert_line(int at, const char *s, size_t len)
{
    if (ed.n >= MAX_LINES) return -1;
    if (ed.n == ed.cap) {
        int nc = ed.cap ? ed.cap * 2 : 256;
        char **nl = realloc(ed.lines, nc * sizeof(char *));
        if (!nl) return -1;
        ed.lines = nl;
        ed.cap = nc;
    }
    char *line = malloc(len + 1);
    if (!line) return -1;
    memcpy(line, s, len);
    line[len] = 0;
    memmove(ed.lines + at + 1, ed.lines + at, (ed.n - at) * sizeof(char *));
    ed.lines[at] = line;
    ed.n++;
    return 0;
}

int editor_open(const char *path, char *err, int errn)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "Can't open file"); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); snprintf(err, errn, "Can't read file size"); return -1; }
    if (size > MAX_FILE) { fclose(f); snprintf(err, errn, "Too big to edit (1 MB max)"); return -1; }
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); snprintf(err, errn, "Out of memory"); return -1; }
    size = (long)fread(buf, 1, size, f);
    fclose(f);
    if (memchr(buf, 0, size)) { free(buf); snprintf(err, errn, "Binary file - can't edit as text"); return -1; }

    free_lines();
    memset(&ed, 0, sizeof ed);
    snprintf(ed.path, sizeof ed.path, "%s", path);
    /* line-ending style = that of the first line break (a stray \r elsewhere must not
       convert a whole LF file to CRLF on save) */
    const char *nl = memchr(buf, '\n', size);
    ed.crlf = (nl && nl > buf && nl[-1] == '\r');
    ed.final_nl = size > 0 && buf[size - 1] == '\n';
    long start = 0;
    for (long i = 0; i <= size; i++) {
        if (i == size || buf[i] == '\n') {
            if (i == size && start == size && size > 0) break;   /* nothing after final newline */
            long end = i;
            /* only strip \r in a CRLF file: in an LF file it is content and must survive a save */
            if (ed.crlf && end > start && buf[end - 1] == '\r') end--;
            if (insert_line(ed.n, buf + start, end - start)) {
                free(buf);
                free_lines();
                snprintf(err, errn, "Too many lines (%d max)", MAX_LINES);
                return -1;
            }
            start = i + 1;
        }
    }
    free(buf);
    if (!ed.n && insert_line(0, "", 0)) { free_lines(); snprintf(err, errn, "Out of memory"); return -1; }
    ed.active = 1;
    return 0;
}

int editor_confirming(void) { return ed.active && ed.confirm_exit; }

int editor_active(void) { return ed.active; }

const char *editor_message(void)
{
    const char *m = ed.msg;
    ed.msg = NULL;
    return m;
}

static int save(void)
{
    char tmp[1100], bak[1100];
    snprintf(tmp, sizeof tmp, "%s.tbox_tmp", ed.path);
    snprintf(bak, sizeof bak, "%s.bak", ed.path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { ed.msg = "Save failed: can't write here"; return -1; }
    const char *eol = ed.crlf ? "\r\n" : "\n";
    int ok = 1;
    for (int i = 0; i < ed.n && ok; i++) {
        if (fputs(ed.lines[i], f) < 0) ok = 0;
        if ((i < ed.n - 1 || ed.final_nl) && fputs(eol, f) < 0) ok = 0;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) { unlink(tmp); ed.msg = "Save failed (disk full?)"; return -1; }
    unlink(bak);
    int have_bak = rename(ed.path, bak) == 0;   /* keep the previous version as .bak */
    if (rename(tmp, ed.path) != 0) {
        if (have_bak) rename(bak, ed.path);
        unlink(tmp);
        ed.msg = "Save failed";
        return -1;
    }
    ed.modified = 0;
    ed.msg = have_bak ? "Saved (previous version kept as .bak)" : "Saved (no .bak could be made)";
    dlog("editor: saved %s", ed.path);
    return 0;
}

static void close_editor(void)
{
    free_lines();
    ed.active = 0;
    ed.confirm_exit = 0;
}

int editor_input(const char **initial)
{
    if (ed.confirm_exit) {
        if (pad_pressed(BTN_CROSS)) { if (save() == 0) close_editor(); else ed.confirm_exit = 0; }
        else if (pad_pressed(BTN_SQUARE)) close_editor();
        else if (pad_pressed(BTN_CIRCLE)) ed.confirm_exit = 0;
        return 0;
    }
    if (pad_pressed(BTN_UP)) ed.sel--;
    if (pad_pressed(BTN_DOWN)) ed.sel++;
    if (pad_pressed(BTN_LEFT)) ed.sel -= ROWS;
    if (pad_pressed(BTN_RIGHT)) ed.sel += ROWS;
    if (pad_pressed(BTN_L2)) ed.sel = 0;
    if (pad_pressed(BTN_R2)) ed.sel = ed.n - 1;
    if (ed.sel >= ed.n) ed.sel = ed.n - 1;
    if (ed.sel < 0) ed.sel = 0;
    if (ed.sel < ed.scroll) ed.scroll = ed.sel;
    if (ed.sel >= ed.scroll + ROWS) ed.scroll = ed.sel - ROWS + 1;

    if (pad_pressed(BTN_CROSS)) {
        if (strlen(ed.lines[ed.sel]) > MAX_LINE) { ed.msg = "Line too long to edit here (1000 chars max)"; return 0; }
        *initial = ed.lines[ed.sel];
        return 1;
    }
    if (pad_pressed(BTN_TRIANGLE)) {          /* new line below, then edit it */
        if (insert_line(ed.sel + 1, "", 0) == 0) {
            ed.sel++;
            ed.modified = 1;
            if (ed.sel >= ed.scroll + ROWS) ed.scroll = ed.sel - ROWS + 1;
            *initial = "";
            return 1;
        }
        ed.msg = "Too many lines";
    }
    if (pad_pressed(BTN_SQUARE)) {            /* delete line */
        if (ed.n > 1) {
            free(ed.lines[ed.sel]);
            memmove(ed.lines + ed.sel, ed.lines + ed.sel + 1, (ed.n - ed.sel - 1) * sizeof(char *));
            ed.n--;
            if (ed.sel >= ed.n) ed.sel = ed.n - 1;
        } else {
            ed.lines[0][0] = 0;
        }
        ed.modified = 1;
    }
    if (pad_pressed(BTN_OPTIONS)) save();
    if (pad_pressed(BTN_CIRCLE)) {
        if (ed.modified) ed.confirm_exit = 1;
        else close_editor();
    }
    return 0;
}

void editor_apply(const char *text)
{
    if (!ed.active || ed.sel >= ed.n) return;
    if (!strcmp(text, ed.lines[ed.sel])) return;
    char *dup = strdup(text);
    if (!dup) return;
    free(ed.lines[ed.sel]);
    ed.lines[ed.sel] = dup;
    ed.modified = 1;
}

void editor_draw(void)
{
    if (!ed.active) return;
    gfx_rect(0, 176, SCREEN_W, 992 - 176, 0xFF0F1318);
    char hdr[160];
    snprintf(hdr, sizeof hdr, "%s%s", ed.modified ? "* " : "", path_base(ed.path));
    gfx_text(MX, 190, hdr, ed.modified ? 0xFFFFC857 : 0xFF3A7BD5, 1100);
    snprintf(hdr, sizeof hdr, "EDIT  |  line %d / %d%s", ed.sel + 1, ed.n, ed.crlf ? "  CRLF" : "");
    gfx_text(SCREEN_W - MX - gfx_text_width(hdr), 190, hdr, 0xFF8995A1, 0);

    char line[256];
    for (int r = 0; r < ROWS; r++) {
        int i = ed.scroll + r;
        if (i >= ed.n) break;
        int y = TOP + r * LINE_H;
        if (i == ed.sel) gfx_rect(MX - 10, y - 2, SCREEN_W - 2 * MX + 20, LINE_H - 2, 0xFF27466B);
        char num[16];
        snprintf(num, sizeof num, "%6d", i + 1);
        gfx_text(MX, y, num, 0xFF5A6570, 0);
        int k = 0;
        for (const char *p = ed.lines[i]; *p && k < (int)sizeof line - 1; p++) {
            if (*p == '\t') { for (int t = 0; t < 4 && k < (int)sizeof line - 1; t++) line[k++] = ' '; }
            else line[k++] = ((unsigned char)*p < 32) ? '.' : *p;
        }
        line[k] = 0;
        gfx_text(MX + 130, y, line, 0xFFE8ECEF, SCREEN_W - 2 * MX - 140);
    }

    if (ed.confirm_exit) {
        int w = 1100, h = 200, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
        gfx_rect_blend(0, 0, SCREEN_W, SCREEN_H, 0xFF000000, 140);
        gfx_rect(x - 3, y - 3, w + 6, h + 6, 0xFFFFC857);
        gfx_rect(x, y, w, h, 0xFF1A2129);
        gfx_text(x + 40, y + 40, "You have unsaved changes.", 0xFFE8ECEF, 0);
        gfx_hint(x + 40, y + 120, "[X] Save and close    [SQUARE] Discard    [O] Keep editing", 0xFF8995A1, 0);
    }
}
