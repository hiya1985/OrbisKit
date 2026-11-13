#include "dlog.h"
#include "compat.h"
#include "fsops.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static FILE *lf;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/* The app used to be called PS4 Toolbox. Delete its old data folder so only the new
 * /data/OrbisKit is left. Careful: only a real folder (not a symlink to somewhere else), and
 * only the files the old app wrote there - anything else the user put in it is kept. */
static int old_app_file(const char *n)
{
    return !strcmp(n, "settings.ini") || !strcmp(n, "log.txt") || !strcmp(n, "log_prev.txt") ||
           (!strncmp(n, "web_download_", 13) && strstr(n, ".zip"));
}

static void remove_old_data(void)
{
    const char *old = "/data/PS4Toolbox";
    struct kstat st;
    if (k_lstat(old, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(old);
    if (!d) return;
    struct dirent *e;
    char p[512];
    while ((e = readdir(d))) {
        if (!old_app_file(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s", old, e->d_name);
        if (k_lstat(p, &st) == 0 && S_ISREG(st.st_mode)) unlink(p);
    }
    closedir(d);
    rmdir(old);   /* fails (and keeps it) if something else is still inside */
}

void dlog_init(void)
{
    remove_old_data();
    mkdirs("/data/OrbisKit");
    rename(DLOG_FILE, "/data/OrbisKit/log_prev.txt");
    lf = fopen(DLOG_FILE, "w");
}

void dlog(const char *fmt, ...)
{
    if (!lf) return;
    unsigned long long ms = sceKernelGetProcessTime() / 1000ULL;
    pthread_mutex_lock(&mtx);
    fprintf(lf, "[%6llu.%03llu] ", ms / 1000, ms % 1000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fputc('\n', lf);
    fflush(lf);   /* keep the log intact if the app crashes */
    pthread_mutex_unlock(&mtx);
}
