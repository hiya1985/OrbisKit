#include "osk.h"
#include "gfx.h"
#include "pad.h"
#include <stdio.h>
#include <string.h>

#define COLS 12
#define ROWS 4
#define SPECIAL 6
#define MAXLEN 1000

enum { LAY_LOWER, LAY_UPPER, LAY_SYMBOLS };

static const char *layouts[3][ROWS] = {
    { "1234567890-_", "qwertyuiop()", "asdfghjkl.,'", "zxcvbnm!@#+=" },
    { "1234567890-_", "QWERTYUIOP[]", "ASDFGHJKL;,&", "ZXCVBNM$%~^=" },
    { "1234567890-=", "!@#$%^&*()_+", "[]{}\\|;:'\",.", "<>/?`~-_=+.," },
};
static const char *special[SPECIAL] = { "Shift", "#+=", "Space", "Delete", "Done", "Cancel" };

static struct {
    int active, accepted, purpose;
    int row, col, layout, blink;
    int cursor;                 /* insert position in text */
    char title[96];
    char text[MAXLEN + 1];
} k;

void osk_open(const char *title, const char *initial, int purpose)
{
    memset(&k, 0, sizeof k);
    k.active = 1;
    k.purpose = purpose;
    k.row = 1;
    snprintf(k.title, sizeof k.title, "%s", title);
    snprintf(k.text, sizeof k.text, "%s", initial ? initial : "");
    k.cursor = (int)strlen(k.text);
}

int osk_active(void) { return k.active; }
int osk_accepted(void) { return k.accepted; }
int osk_purpose(void) { return k.purpose; }
const char *osk_text(void) { return k.text; }

static void type_char(char c)
{
    int l = (int)strlen(k.text);
    if (l >= MAXLEN) return;
    memmove(k.text + k.cursor + 1, k.text + k.cursor, l - k.cursor + 1);
    k.text[k.cursor++] = c;
}

static void backspace(void)
{
    if (!k.cursor) return;
    memmove(k.text + k.cursor - 1, k.text + k.cursor, strlen(k.text) - k.cursor + 1);
    k.cursor--;
}

static int finish(int ok)
{
    k.accepted = ok;
    k.active = 0;
    return 1;
}

int osk_update(void)
{
    if (!k.active) return 0;
    k.blink++;
    int width = (k.row == ROWS) ? SPECIAL : COLS;

    if (pad_pressed(BTN_LEFT))  k.col = (k.col + width - 1) % width;
    if (pad_pressed(BTN_RIGHT)) k.col = (k.col + 1) % width;
    int up = pad_pressed(BTN_UP), down = pad_pressed(BTN_DOWN);
    if (up || down) {
        int old = k.row;
        k.row = (k.row + (up ? ROWS : 1)) % (ROWS + 1);
        if (old == ROWS && k.row != ROWS) k.col = k.col * COLS / SPECIAL + 1;
        else if (old != ROWS && k.row == ROWS) k.col = k.col * SPECIAL / COLS;
        int w = (k.row == ROWS) ? SPECIAL : COLS;
        if (k.col >= w) k.col = w - 1;
    }

    int len = (int)strlen(k.text);
    if (pad_pressed(BTN_L1) && k.cursor > 0) k.cursor--;
    if (pad_pressed(BTN_R1) && k.cursor < len) k.cursor++;
    if (pad_pressed(BTN_SQUARE)) backspace();
    if (pad_pressed(BTN_TRIANGLE)) type_char(' ');
    if (pad_pressed(BTN_L2)) k.layout = (k.layout == LAY_UPPER) ? LAY_LOWER : LAY_UPPER;
    if (pad_pressed(BTN_R2)) k.layout = (k.layout == LAY_SYMBOLS) ? LAY_LOWER : LAY_SYMBOLS;
    if (pad_pressed(BTN_OPTIONS)) return finish(1);
    if (pad_pressed(BTN_CIRCLE)) return finish(0);

    if (pad_pressed(BTN_CROSS)) {
        if (k.row < ROWS) {
            type_char(layouts[k.layout][k.row][k.col]);
        } else {
            switch (k.col) {
            case 0: k.layout = (k.layout == LAY_UPPER) ? LAY_LOWER : LAY_UPPER; break;
            case 1: k.layout = (k.layout == LAY_SYMBOLS) ? LAY_LOWER : LAY_SYMBOLS; break;
            case 2: type_char(' '); break;
            case 3: backspace(); break;
            case 4: return finish(1);
            case 5: return finish(0);
            }
        }
    }
    return 0;
}

void osk_draw(void)
{
    if (!k.active) return;
    const int kw = 84, kh = 66, gap = 8;
    int w = COLS * (kw + gap) + 72, h = 560;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 20;
    int fw = gfx_font_w();

    gfx_rect_blend(0, 0, SCREEN_W, SCREEN_H, 0xFF000000, 160);
    gfx_rect(x - 3, y - 3, w + 6, h + 6, 0xFF3A7BD5);
    gfx_rect(x, y, w, h, 0xFF1A2129);
    gfx_text(x + 36, y + 20, k.title, 0xFF3A7BD5, w - 72);

    /* text field: show a window of the text that keeps the cursor visible */
    gfx_rect(x + 36, y + 70, w - 72, 56, 0xFF0F1318);
    int visible = (w - 112) / fw;
    int len = (int)strlen(k.text);
    int start = 0;
    if (k.cursor > visible - 4) start = k.cursor - (visible - 4);
    if (start > len) start = len;
    char shown[256];
    int n = len - start < visible ? len - start : visible;
    if (n > (int)sizeof shown - 1) n = sizeof shown - 1;
    memcpy(shown, k.text + start, n);
    shown[n] = 0;
    gfx_text(x + 56, y + 81, shown, 0xFFE8ECEF, 0);
    if ((k.blink / 30) % 2 == 0)
        gfx_rect(x + 56 + (k.cursor - start) * fw - 1, y + 82, 3, gfx_font_h() - 4, 0xFF3A7BD5);

    int gy = y + 150;
    for (int r = 0; r < ROWS; r++) {
        const char *row = layouts[k.layout][r];
        for (int c = 0; c < COLS; c++) {
            int kx = x + 36 + c * (kw + gap), ky = gy + r * (kh + gap);
            int sel = (k.row == r && k.col == c);
            gfx_rect(kx, ky, kw, kh, sel ? 0xFF3A7BD5 : 0xFF27313C);
            char s[2] = { row[c], 0 };
            gfx_text(kx + (kw - fw) / 2, ky + (kh - gfx_font_h()) / 2, s, 0xFFFFFFFF, 0);
        }
    }
    int sy = gy + ROWS * (kh + gap);
    int sw = (COLS * (kw + gap) - gap - (SPECIAL - 1) * gap) / SPECIAL;
    for (int c = 0; c < SPECIAL; c++) {
        int kx = x + 36 + c * (sw + gap);
        int sel = (k.row == ROWS && k.col == c);
        int on = (c == 0 && k.layout == LAY_UPPER) || (c == 1 && k.layout == LAY_SYMBOLS);
        uint32_t bg = sel ? 0xFF3A7BD5 : on ? 0xFF4A5A6A : 0xFF27313C;
        gfx_rect(kx, sy, sw, kh, bg);
        gfx_text(kx + (sw - gfx_text_width(special[c])) / 2, sy + (kh - gfx_font_h()) / 2, special[c], 0xFFFFFFFF, 0);
    }
    static const char *hint =
        "[X] Type  [SQ] Del  [TRI] Space  [L1/R1] Cursor  [L2] Shift  [R2] Symbols  [OPT] Done  [O] Cancel";
    int hw = gfx_hint_width(hint);
    gfx_hint((SCREEN_W - hw) / 2, y + h + 24, hint, 0xFFB0B8C0, 0);
}
