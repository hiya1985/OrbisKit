#pragma once

#define CONFIG_DIR  "/data/OrbisKit"
#define CONFIG_FILE CONFIG_DIR "/settings.ini"
#define MAX_BOOKMARKS 8

typedef struct {
    int  web_autostart;          /* start web file manager on launch */
    int  sort_mode;              /* browser sort order */
    int  theme;                  /* colour scheme index */
    int  last_tab;               /* reopened on launch when start_last is set */
    int  start_last;             /* 0 = open on Home dashboard, 1 = last tab and folder */
    int  tiles[8];               /* Home quick-action ids per tile (QA_* in main.c) */
    int  tiles_set;              /* 0 = never customised: use the defaults */
    char last_path[1024];        /* Files folder reopened on launch */
    char autopayload[1024];      /* payload sent on launch, empty = none */
    char bookmarks[MAX_BOOKMARKS][1024];   /* full path length: a cut path is a different folder */
    int  nbookmarks;
} Config;

extern Config cfg;

void config_load(void);
int  config_save(void);   /* 0 on success */
int  config_set_last(int tab, const char *path);   /* remember tab + folder, thread-safe */
int  bookmark_index(const char *path);   /* -1 if not bookmarked */
int  bookmark_add(const char *path);     /* 0 ok, -1 full or path can't be stored */
void bookmark_remove(const char *path);
