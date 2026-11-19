#pragma once
/* Package installer: installs a .pkg (game, update, DLC) from internal storage or USB through
 * the PS4's own background installer (BGFT), the same service the Debug Settings Package
 * Installer uses. Progress shows in the PS4's Notifications > Downloads. Needs the sandbox
 * escape (GoldHEN). Safe to call from any thread. */

int  pkginst_is_pkg_name(const char *name);           /* ends in .pkg */
/* Starts installing path. 0 on success (msg = what was started), -1 on error (msg = why). */
int  pkginst_install(const char *path, char *msg, int msgn);

/* The installs started in this run, for a progress line. */
#define PKGINST_MAX 16
typedef struct {
    char title[64];
    int  task;
    unsigned long long done, total;   /* bytes */
    int  error;                       /* BGFT error code, 0 = none */
    int  finished;
} PkgTask;
int  pkginst_tasks(PkgTask *out, int max);            /* refreshes progress; returns count */
