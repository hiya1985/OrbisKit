#pragma once

/* Escape the app sandbox for full filesystem access.
 * Uses the GoldHEN SDK when built with USE_GOLDHEN; otherwise a no-op. */
int jb_escape(void);
/* 1 if we can see the real root filesystem. */
int jb_has_root(void);
int jb_built_with_goldhen(void);
