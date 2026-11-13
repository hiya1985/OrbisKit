#pragma once
#include <stdint.h>
#include <time.h>

typedef struct {
    char     name[256];
    int      is_dir;
    uint64_t size;
    time_t   mtime;
    int      mode;
    int      marked;
} Entry;

typedef struct {
    char   path[1024];
    Entry *e;
    int    count, cap;
    int    sel, scroll;
    int    error;     /* errno from last load, 0 if fine */
} Browser;

enum { SORT_NAME, SORT_SIZE, SORT_DATE, SORT_COUNT };
extern int browser_sort;                  /* one of SORT_*; folders always come first */
extern const char *sort_names[SORT_COUNT];

int  browser_load(Browser *b, const char *path);
void browser_reload(Browser *b);          /* keeps selection where possible */
int  browser_up(Browser *b);              /* go to parent; selects folder we came from */
int  browser_marked_count(const Browser *b);
