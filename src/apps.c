#include "apps.h"
#include "dlog.h"
#include "fsops.h"
#include <dirent.h>
#include <stb/stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_APPS 1000

static App *apps;
static int napps;

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

int sfo_get(const char *path, const char *key, char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char buf[16384];
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (len < 20 || memcmp(buf, "\0PSF", 4)) return -1;
    /* every offset comes from the file: do the bounds math in 64 bits so nothing can wrap
       (a corrupt or crafted param.sfo must not crash the app - it's read at every launch) */
    uint64_t key_tab = rd32(buf + 8), data_tab = rd32(buf + 12), count = rd32(buf + 16);
    size_t klen = strlen(key);
    for (uint64_t i = 0; i < count && 20 + i * 16 + 16 <= len; i++) {
        const unsigned char *e = buf + 20 + i * 16;
        uint64_t koff = key_tab + rd16(e), fmt = rd16(e + 2), dlen = rd32(e + 4), doff = data_tab + rd32(e + 12);
        if (koff + klen >= len || doff >= len) continue;          /* key name + its NUL must fit */
        if (memcmp(buf + koff, key, klen) || buf[koff + klen] != 0) continue;
        if (fmt == 0x0404) {                       /* integer */
            if (doff + 4 > len) return -1;
            snprintf(out, n, "%u", rd32(buf + doff));
        } else {                                   /* utf-8 string */
            if (doff + dlen > len) dlen = len - doff;
            size_t k = dlen < (uint64_t)(n - 1) ? (size_t)dlen : (size_t)(n - 1);
            memcpy(out, buf + doff, k);
            out[k] = 0;
        }
        return 0;
    }
    return -1;
}

static uint64_t file_size(const char *path)
{
    struct kstat st;
    return k_stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static int cmp_app(const void *a, const void *b)
{
    return strcasecmp(((const App *)a)->title, ((const App *)b)->title);
}

int apps_scan(void)
{
    for (int i = 0; i < napps; i++) free(apps[i].icon);
    napps = 0;
    if (!apps && !(apps = calloc(MAX_APPS, sizeof(App)))) return 0;

    DIR *d = opendir("/user/app");
    if (!d) { dlog("apps: can't open /user/app"); return 0; }
    struct dirent *e;
    char p[512];
    while ((e = readdir(d)) && napps < MAX_APPS) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 9) continue;   /* title IDs: ABCD12345 */
        App *a = &apps[napps];
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", e->d_name);

        snprintf(p, sizeof p, "/system_data/priv/appmeta/%s/param.sfo", a->id);
        if (sfo_get(p, "TITLE", a->title, sizeof a->title)) snprintf(a->title, sizeof a->title, "%s", a->id);
        sfo_get(p, "APP_VER", a->version, sizeof a->version);
        if (!a->version[0]) sfo_get(p, "VERSION", a->version, sizeof a->version);
        sfo_get(p, "CATEGORY", a->category, sizeof a->category);
        for (char *c = a->title; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';

        snprintf(p, sizeof p, "/user/app/%s/app.pkg", a->id);
        a->app_size = file_size(p);
        snprintf(p, sizeof p, "/user/patch/%s/patch.pkg", a->id);
        a->patch_size = file_size(p);

        snprintf(p, sizeof p, "/user/addcont/%s", a->id);
        DIR *ad = opendir(p);
        if (ad) {
            struct dirent *x;
            while ((x = readdir(ad))) if (x->d_name[0] != '.') a->dlc++;
            closedir(ad);
        }
        napps++;
    }
    closedir(d);
    qsort(apps, napps, sizeof(App), cmp_app);
    dlog("apps: found %d installed", napps);
    return napps;
}

int apps_count(void) { return napps; }
App *apps_get(int i) { return (i >= 0 && i < napps) ? &apps[i] : NULL; }

void apps_load_icon_step(void)
{
    for (int i = 0; i < napps; i++) {
        App *a = &apps[i];
        if (a->icon_state) continue;
        a->icon_state = -1;
        char p[256];
        snprintf(p, sizeof p, "/user/appmeta/%s/icon0.png", a->id);
        int w, h, c;
        unsigned char *px = stbi_load(p, &w, &h, &c, 4);
        if (!px) return;
        uint32_t *ic = malloc(APP_ICON * APP_ICON * 4);
        if (ic) {
            for (int y = 0; y < APP_ICON; y++) {
                for (int x = 0; x < APP_ICON; x++) {
                    /* average a small block of source pixels */
                    int x0 = x * w / APP_ICON, x1 = (x + 1) * w / APP_ICON;
                    int y0 = y * h / APP_ICON, y1 = (y + 1) * h / APP_ICON;
                    if (x1 <= x0) x1 = x0 + 1;
                    if (y1 <= y0) y1 = y0 + 1;
                    unsigned r = 0, g = 0, b = 0, n = 0;
                    for (int yy = y0; yy < y1; yy++)
                        for (int xx = x0; xx < x1; xx++) {
                            const unsigned char *s = px + ((size_t)yy * w + xx) * 4;
                            r += s[0]; g += s[1]; b += s[2]; n++;
                        }
                    ic[y * APP_ICON + x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
                }
            }
            a->icon = ic;
            a->icon_state = 1;
        }
        stbi_image_free(px);
        return;   /* one per call */
    }
}

const char *apps_category_name(const char *cat)
{
    /* with an update installed the stored param.sfo is the patch's: gp / gpe */
    if (!strcmp(cat, "gd") || !strcmp(cat, "gp")) return "Game";
    if (!strcmp(cat, "gde") || !strcmp(cat, "gpe")) return "App";
    if (!strcmp(cat, "ac")) return "Add-on";
    return cat[0] ? cat : "?";
}
