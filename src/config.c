#include "config.h"
#include "fsops.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config cfg;

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

void config_load(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[1200];   /* must hold "last_path=" + a full 1023-char path */
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        int val = atoi(v);
        if (!strcmp(line, "web_autostart")) cfg.web_autostart = !!val;
        else if (!strcmp(line, "sort_mode") && val >= 0 && val < 3) cfg.sort_mode = val;
        else if (!strcmp(line, "theme") && val >= 0 && val < 4) cfg.theme = val;
        /* "tab" (v1.10+): index into the tab list that starts with Home. The older "last_tab"
           key counted from Files, so it is ignored rather than reopening the wrong tab. */
        else if (!strcmp(line, "tab") && val >= 0 && val < 16) cfg.last_tab = val;
        else if (!strcmp(line, "start_last")) cfg.start_last = !!val;
        else if (!strcmp(line, "tiles")) {
            /* "tiles=1,2,3,..." - range-checked by main.c when used */
            const char *q = v;
            for (int i = 0; i < 8 && *q; i++) {
                cfg.tiles[i] = atoi(q);
                q = strchr(q, ',');
                if (!q) break;
                q++;
            }
            cfg.tiles_set = 1;
        }
        else if (!strcmp(line, "last_path")) copy_str(cfg.last_path, sizeof cfg.last_path, v);
        else if (!strcmp(line, "autopayload")) copy_str(cfg.autopayload, sizeof cfg.autopayload, v);
        else if (!strcmp(line, "bookmark") && *v && cfg.nbookmarks < MAX_BOOKMARKS)
            copy_str(cfg.bookmarks[cfg.nbookmarks++], sizeof cfg.bookmarks[0], v);
    }
    fclose(f);
}

static pthread_mutex_t save_mtx = PTHREAD_MUTEX_INITIALIZER;   /* TV and web both save */

static int config_save_locked(void);

int config_save(void)
{
    pthread_mutex_lock(&save_mtx);
    int r = config_save_locked();
    pthread_mutex_unlock(&save_mtx);
    return r;
}

/* A value with a line break in it (a folder made over FTP or on a PC can have one) would start a new
 * key in settings.ini: write it as empty instead. */
static const char *one_line(const char *v)
{
    return strpbrk(v, "\r\n") ? "" : v;
}

/* Written to a temp file first and renamed over settings.ini only when every write worked,
 * so a full disk or a crash mid-save never leaves a half-written (or empty) settings file. */
static int config_save_locked(void)
{
    static const char *tmp = CONFIG_DIR "/settings.ini.tmp";
    mkdirs(CONFIG_DIR);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    int bad = fprintf(f, "web_autostart=%d\nsort_mode=%d\n"
                         "theme=%d\ntab=%d\nstart_last=%d\nlast_path=%s\nautopayload=%s\n",
                      cfg.web_autostart, cfg.sort_mode, cfg.theme, cfg.last_tab,
                      cfg.start_last, one_line(cfg.last_path), one_line(cfg.autopayload)) < 0;
    if (cfg.tiles_set)
        bad |= fprintf(f, "tiles=%d,%d,%d,%d,%d,%d,%d,%d\n", cfg.tiles[0], cfg.tiles[1], cfg.tiles[2],
                       cfg.tiles[3], cfg.tiles[4], cfg.tiles[5], cfg.tiles[6], cfg.tiles[7]) < 0;
    for (int i = 0; i < cfg.nbookmarks; i++)
        if (*one_line(cfg.bookmarks[i])) bad |= fprintf(f, "bookmark=%s\n", cfg.bookmarks[i]) < 0;
    bad |= fflush(f) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, CONFIG_FILE) != 0) { remove(tmp); return -1; }
    return 0;
}

int bookmark_index(const char *path)
{
    for (int i = 0; i < cfg.nbookmarks; i++)
        if (!strcmp(cfg.bookmarks[i], path)) return i;
    return -1;
}

int bookmark_add(const char *path)
{
    if (bookmark_index(path) >= 0) return 0;
    if (cfg.nbookmarks >= MAX_BOOKMARKS) return -1;
    if (strlen(path) >= sizeof cfg.bookmarks[0] || strpbrk(path, "\r\n")) return -1;
    copy_str(cfg.bookmarks[cfg.nbookmarks++], sizeof cfg.bookmarks[0], path);
    return 0;
}

void bookmark_remove(const char *path)
{
    int i = bookmark_index(path);
    if (i < 0) return;
    for (; i < cfg.nbookmarks - 1; i++) strcpy(cfg.bookmarks[i], cfg.bookmarks[i + 1]);
    cfg.nbookmarks--;
}
