#pragma once
#include <stdint.h>

/* Installed games and apps, read from the PS4's own folders:
 *   /user/app/<ID>/app.pkg                       game data
 *   /system_data/priv/appmeta/<ID>/param.sfo     title, version, category
 *   /user/appmeta/<ID>/icon0.png                 icon
 *   /user/patch/<ID>/patch.pkg                   installed update
 *   /user/addcont/<ID>/<...>                     DLC */

#define APP_ICON 72

typedef struct {
    char     id[16];
    char     title[128];
    char     version[16];
    char     category[8];
    uint64_t app_size, patch_size;
    int      dlc;
    uint32_t *icon;        /* APP_ICON x APP_ICON ARGB, NULL until loaded */
    int      icon_state;   /* 0 = not tried, 1 = loaded, -1 = failed */
} App;

int  apps_scan(void);          /* returns number of apps */
int  apps_count(void);
App *apps_get(int i);
/* Loads at most one missing icon per call (keeps the UI responsive). */
void apps_load_icon_step(void);
const char *apps_category_name(const char *cat);

/* Reads a string or integer value from a param.sfo. 0 on success. */
int  sfo_get(const char *path, const char *key, char *out, int n);
