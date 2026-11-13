#pragma once
/* Simple on-screen keyboard drawn by the app (no system IME needed). */

void osk_open(const char *title, const char *initial, int purpose);
int  osk_active(void);
/* Handles input. Returns 1 on the frame the keyboard closes. */
int  osk_update(void);
int  osk_accepted(void);          /* 1 if closed with Done */
int  osk_purpose(void);
const char *osk_text(void);
void osk_draw(void);
