#include "pad.h"
#include "compat.h"
#include "dlog.h"
#include <string.h>

static int handle = -1;
static uint32_t buttons;
static int hold[32];

int pad_init(void)
{
    struct { int priority; } params = { 700 };
    sceUserServiceInitialize(&params);
    int uid = -1;
    sceUserServiceGetInitialUser(&uid);
    scePadInit();
    handle = scePadOpen(uid, 0, 0, NULL);
    return handle;
}

void pad_update(void)
{
    uint8_t data[256];
    memset(data, 0, sizeof data);
    if (handle >= 0 && scePadReadState(handle, data) == 0) {
        memcpy(&buttons, data, sizeof buttons);   /* first field is the button mask */
        /* left stick follows at bytes 4 (x) and 5 (y), 0..255 with 128 centred:
           treat a firm push as the matching d-pad direction */
        uint8_t lx = data[4], ly = data[5];
        if (lx == 0 && ly == 0) lx = ly = 128;   /* all-zero = no controller data */
        if (ly < 40)  buttons |= BTN_UP;
        if (ly > 215) buttons |= BTN_DOWN;
        if (lx < 40)  buttons |= BTN_LEFT;
        if (lx > 215) buttons |= BTN_RIGHT;
    } else {
        buttons = 0;
    }
    for (int i = 0; i < 32; i++)
        hold[i] = (buttons & (1u << i)) ? hold[i] + 1 : 0;
}

int pad_pressed(uint32_t btn)
{
    int i = __builtin_ctz(btn);
    int h = hold[i];
    if (h == 1) return 1;
    int repeat = btn & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
    return repeat && h > 20 && (h - 20) % 4 == 0;
}
