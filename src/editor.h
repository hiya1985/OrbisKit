#pragma once
/* Line-based text editor (edits one line at a time with the on-screen keyboard). */

int  editor_open(const char *path, char *err, int errn);    /* 0 on success */
int  editor_active(void);
/* Handles input. May request the keyboard: returns 1 and fills *initial when a
 * line should be edited (the caller opens the OSK and later calls editor_apply). */
int  editor_input(const char **initial);
void editor_apply(const char *text);     /* result of the OSK for the requested line */
void editor_cancel(void);                /* the OSK was cancelled: drop a just-added empty line */
void editor_draw(void);
int  editor_confirming(void);            /* the "unsaved changes" dialog is open */
const char *editor_message(void);        /* status to show as a toast, or NULL */
