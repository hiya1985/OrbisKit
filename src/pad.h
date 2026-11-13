#pragma once
#include <stdint.h>

#define BTN_L3       0x00000002
#define BTN_R3       0x00000004
#define BTN_OPTIONS  0x00000008
#define BTN_UP       0x00000010
#define BTN_RIGHT    0x00000020
#define BTN_DOWN     0x00000040
#define BTN_LEFT     0x00000080
#define BTN_L2       0x00000100
#define BTN_R2       0x00000200
#define BTN_L1       0x00000400
#define BTN_R1       0x00000800
#define BTN_TRIANGLE 0x00001000
#define BTN_CIRCLE   0x00002000
#define BTN_CROSS    0x00004000
#define BTN_SQUARE   0x00008000
#define BTN_TOUCHPAD 0x00100000

int  pad_init(void);
void pad_update(void);
/* True on the press frame; d-pad buttons also auto-repeat while held. */
int  pad_pressed(uint32_t btn);
