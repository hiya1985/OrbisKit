#pragma once
/* "Find leftovers": .pkg files whose game, update or DLC is already installed
 * (same or newer version), so the file only takes up space. Scans /data and USB drives. */
#include <stdint.h>

#define LO_MAX 500

typedef struct {
    char path[1024];
    char title_id[10];
    char label[17];          /* DLC label (content ID chars 20..35) */
    int  type;               /* 0x1A game, 0x1B/0x1C DLC, 0x1E update */
    char pkg_ver[8];         /* "01.05", or "" */
    char inst_ver[8];
    uint64_t size;
    int  marked;
} Leftover;

extern Leftover lo_items[LO_MAX];
extern volatile int lo_count;

int  lo_start(void);              /* starts the scan thread; -1 if one is running */
int  lo_running(void);
void lo_cancel(void);
void lo_progress(int *checked, int *pkgs, char *cur, int n);
const char *lo_type_name(int type);
/* Deletes the marked files (each re-checked first). Returns how many were deleted;
 * *freed gets the bytes, err the last error. Removes them from the list. */
int  lo_delete_marked(uint64_t *freed, char *err, int errn);
/* Reads a .pkg header: content ID (37 bytes), content type, APP_VER ("01.05", 8 bytes) and,
 * if title is not NULL, the TITLE from its param.sfo. 0 if it is a package we understand. */
int  pkg_header(const char *path, char *cid, int *type, char *ver, char *title, int titlen);
