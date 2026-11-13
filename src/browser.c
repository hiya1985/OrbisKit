#include "browser.h"
#include "fsops.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

int browser_sort = SORT_NAME;
const char *sort_names[SORT_COUNT] = { "Name", "Size (largest first)", "Date (newest first)" };

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    if (browser_sort == SORT_SIZE && x->size != y->size) return x->size < y->size ? 1 : -1;
    if (browser_sort == SORT_DATE && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

int browser_load(Browser *b, const char *path)
{
    DIR *d = opendir(path);
    if (!d) { b->error = errno; return -1; }
    b->error = 0;
    b->count = 0;
    snprintf(b->path, sizeof b->path, "%s", path);
    struct dirent *de;
    char full[1024];
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (b->count == b->cap) {
            int nc = b->cap ? b->cap * 2 : 256;
            Entry *ne = realloc(b->e, nc * sizeof(Entry));
            if (!ne) break;
            b->e = ne;
            b->cap = nc;
        }
        Entry *e = &b->e[b->count++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        path_join(full, sizeof full, path, de->d_name);
        struct kstat st;
        if (k_stat(full, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
            e->mode = st.st_mode;
        } else {
            e->is_dir = (de->d_type == DT_DIR);
        }
    }
    closedir(d);
    qsort(b->e, b->count, sizeof(Entry), cmp_entry);
    b->sel = 0;
    b->scroll = 0;
    return 0;
}

void browser_reload(Browser *b)
{
    char keep[256] = "";
    int sel = b->sel;
    if (b->count && sel < b->count) snprintf(keep, sizeof keep, "%s", b->e[sel].name);
    char path[1024];
    snprintf(path, sizeof path, "%s", b->path);
    if (browser_load(b, path) != 0) return;
    for (int i = 0; i < b->count; i++)
        if (!strcmp(b->e[i].name, keep)) { b->sel = i; return; }
    b->sel = sel < b->count ? sel : (b->count ? b->count - 1 : 0);
}

int browser_up(Browser *b)
{
    if (!strcmp(b->path, "/")) return -1;
    char parent[1024], came[256];
    snprintf(parent, sizeof parent, "%s", b->path);
    char *s = strrchr(parent, '/');
    if (!s) return -1;   /* empty or relative path (failed start-up load, settings.ini) */
    snprintf(came, sizeof came, "%s", s + 1);
    if (s == parent) parent[1] = 0; else *s = 0;
    if (browser_load(b, parent) != 0) return -1;
    for (int i = 0; i < b->count; i++)
        if (!strcmp(b->e[i].name, came)) { b->sel = i; break; }
    return 0;
}

int browser_marked_count(const Browser *b)
{
    int n = 0;
    for (int i = 0; i < b->count; i++) n += b->e[i].marked;
    return n;
}
