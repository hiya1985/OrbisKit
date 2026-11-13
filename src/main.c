/* OrbisKit - all-in-one homebrew utility for jailbroken consoles.
 * Tabs: Home | Files | Apps | Network | Payloads | System | Tools | Settings
 * L1/R1 switch tabs. See README.md for controls. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "apps.h"
#include "browser.h"
#include "compat.h"
#include "config.h"
#include "dlog.h"
#include "editor.h"
#include "osk.h"
#include "viewer.h"
#include "fsops.h"
#include "gfx.h"
#include "imgview.h"
#include "jb.h"
#include "notify.h"
#include "pad.h"
#include "payload.h"
#include "sysinfo.h"
#include "web.h"

#ifndef APP_VERSION          /* set by build.ps1 from $VERSION */
#define APP_VERSION "dev"
#endif

/* ---------- colours (ARGB) ---------- */
typedef struct { const char *name; uint32_t bg, panel, line, accent, sel; } Theme;
static const Theme themes[] = {
    { "Ocean",  0xFF0F1318, 0xFF1A2129, 0xFF2A333D, 0xFF3A7BD5, 0xFF27466B },
    { "Violet", 0xFF120F1A, 0xFF1E1829, 0xFF2E2640, 0xFF8A5CF6, 0xFF3D2A66 },
    { "Forest", 0xFF0E1511, 0xFF17221B, 0xFF26362B, 0xFF3FAE6A, 0xFF1F4A30 },
    { "Ember",  0xFF16110E, 0xFF241A15, 0xFF3A2A22, 0xFFE07A3A, 0xFF5A3018 },
};
#define THEME_COUNT (int)(sizeof themes / sizeof themes[0])
#define C_BG     (themes[cfg.theme].bg)
#define C_PANEL  (themes[cfg.theme].panel)
#define C_LINE   (themes[cfg.theme].line)
#define C_ACCENT (themes[cfg.theme].accent)
#define C_SEL    (themes[cfg.theme].sel)
#define C_TEXT   0xFFE8ECEF
#define C_DIM    0xFF8995A1
#define C_MARK   0xFFFFC857
#define C_RED    0xFFE5534B
#define C_GREEN  0xFF57C17B

/* ---------- layout ---------- */
#define MX        80
#define TABS_Y    118
#define PATH_Y    196
#define LIST_Y    246
#define ROW_H     44
#define LIST_ROWS 16
#define FOOTER_Y  1000

#define GOLDHEN_FTP_PORT 2121  /* the app has no FTP server of its own: GoldHEN's is used */

enum { TAB_HOME, TAB_FILES, TAB_APPS, TAB_NET, TAB_PAYLOADS, TAB_SYSTEM, TAB_TOOLS, TAB_SETTINGS, TAB_COUNT };
static const char *tab_names[TAB_COUNT] = { "Home", "Files", "Apps", "Network", "Payloads", "System", "Tools", "Settings" };

enum {
    ACT_NONE, ACT_COPY, ACT_CUT, ACT_PASTE, ACT_DELETE, ACT_MKDIR, ACT_MARKALL,
    ACT_UNMARK, ACT_PROPS, ACT_SENDPAYLOAD, ACT_CLEARCLIP, ACT_RENAME, ACT_VIEW,
    ACT_VIEWIMG, ACT_EXTRACT, ACT_SEARCH, ACT_SIZE, ACT_SORT, ACT_BOOKMARK, ACT_UNBOOKMARK,
    ACT_ZIP, ACT_EDIT, ACT_HASH, ACT_BIGFILES,
    ACT_APPPATH, ACT_APPPATH_LAST = ACT_APPPATH + 4,   /* open app_paths[n] in Files */
    ACT_TILE_RESET,
    ACT_TILE_SET, ACT_TILE_SET_LAST = ACT_TILE_SET + 63, /* put quick action n on the selected tile */
    /* must stay last: ACT_GOTO + n opens goto_paths[n], then bookmarks after those */
    ACT_GOTO,
};

enum { OSK_RENAME = 1, OSK_MKDIR, OSK_SEARCH, OSK_ZIP, OSK_EDITLINE };

enum {
    /* Tools page (actions) */
    TOOL_SYNC_CAPTURES, TOOL_BACKUP_DB, TOOL_BACKUP_USER, TOOL_BIGSCAN, TOOL_ESCAPE,
    TOOL_OPEN_USB,
    /* Settings page (saved options) */
    TOOL_START_HOME, TOOL_WEB_AUTO, TOOL_THEME,
    TOOL_COUNT
};

/* Each list page is just an ordered set of item ids: add an id here to add a row. */
static const int tools_items[] = {
    TOOL_SYNC_CAPTURES, TOOL_BACKUP_DB, TOOL_BACKUP_USER, TOOL_BIGSCAN, TOOL_OPEN_USB, TOOL_ESCAPE,
};
static const int settings_items[] = {
    TOOL_START_HOME, TOOL_WEB_AUTO, TOOL_THEME,
};
#define NITEMS(a) (int)(sizeof a / sizeof a[0])
#define LIST_VIS 10            /* rows visible on a list page before it scrolls */
enum { PAGE_TOOLS, PAGE_SETTINGS };
static int page_sel[2], page_scroll[2];

/* Home dashboard tiles */
/* Home quick actions: every action that can be put on a tile. The ids are saved in
 * settings.ini (tiles=...), so only ever ADD new ones at the end - never reorder. */
enum {
    QA_NONE, QA_RETIRED_1 /* was the app's FTP server (removed: GoldHEN has one) */, QA_WEB, QA_USB, QA_PAYLOADS, QA_BIGSCAN, QA_CAPTURES, QA_BACKUP_DB,
    QA_SYSTEM, QA_BACKUP_USER, QA_FILES, QA_APPS, QA_TOOLS, QA_SETTINGS, QA_INTERNAL,
    QA_DATA, QA_AUTOPAYLOAD, QA_ESCAPE, QA_THEME,
    QA_RETIRED_19,   /* was "Load GoldHEN" (removed): keeps old settings.ini tiles from turning into a new action */
    QA_COUNT
};
#define HT_COUNT 8             /* tiles on the dashboard (2 rows of 4) */
#define HT_COLS 4
static const int default_tiles[HT_COUNT] = {
    QA_WEB, QA_FILES, QA_USB, QA_PAYLOADS, QA_BIGSCAN, QA_CAPTURES, QA_BACKUP_DB, QA_SYSTEM,
};
#define QA_RETIRED(a) ((a) == QA_RETIRED_1 || (a) == QA_RETIRED_19)
static int home_sel;

/* ---------- state ---------- */
static int tab;
static int frame;
static Browser br;
static char **clip;
static int nclip, clip_cut;
static char payloads[PAYLOAD_MAX][512];
static int npayloads, psel;
static int has_root;
static char fw[96];
static char ip[32] = "0.0.0.0";
static int cpu_t = -1, soc_t = -1;
static char free_here[48];   /* free space of the folder shown in Files */

typedef struct { const char *label, *path; char text[64]; int ok; unsigned long long total, avail; } Disk;
static Disk disks[] = {
    { "Internal storage", "/user", "", 0 },
    { "USB drive 0", "/mnt/usb0", "", 0 },
    { "USB drive 1", "/mnt/usb1", "", 0 },
};
#define DISK_COUNT (int)(sizeof disks / sizeof disks[0])

static char toast_msg[256];
static int toast_t;
static uint32_t toast_col;
static int toast_icons;     /* the format string (not a file name in it) has a button token */


/* Apps tab */
#define APP_ROW_H  86
#define APP_ROWS   8
static int ap_sel, ap_scroll, ap_scanned;
static char app_paths[5][256];

/* search results view (Files tab) */
static int sr_active, sr_sel, sr_scroll;
static int sr_big;          /* results are "largest files" rather than a name search */
static char sr_query[64], sr_root[1024];

#define MENU_MAX 32
#define MENU_VISIBLE 15
static struct {
    int active, sel, n, scroll;
    char title[64];
    char items[MENU_MAX][64];
    int codes[MENU_MAX];
} menu;

static struct { int active, action; char msg[256]; } confirm;
static struct { int active, n; char lines[10][200]; } info;

static const char *goto_paths[] = {
    "/", "/data", "/mnt/usb0", "/mnt/usb1", "/user", "/user/app", "/user/home",
    "/system", "/system_ex", "/system_data", "/mnt",
};
#define GOTO_COUNT (int)(sizeof goto_paths / sizeof goto_paths[0])

/* ---------- helpers ---------- */
static void toast(uint32_t col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_msg, sizeof toast_msg, fmt, ap);
    va_end(ap);
    toast_icons = strchr(fmt, '[') != NULL;
    toast_col = col;
    toast_t = 240;
}

/* Starts a background job from the TV; tells the user if one is already running
 * (e.g. a web download being zipped) instead of silently doing nothing. */
static int tv_job(JobType type, char **src, int n, const char *dst)
{
    int r = job_start(type, src, n, dst);
    if (r != 0)
        toast(C_MARK, "Busy - another job is running%s. Try again in a moment.",
              g_job.quiet ? " (a web download)" : "");
    return r;
}

static void menu_open(const char *title)
{
    memset(&menu, 0, sizeof menu);
    menu.active = 1;
    snprintf(menu.title, sizeof menu.title, "%s", title);
}

static void menu_add(const char *label, int code)
{
    if (menu.n >= MENU_MAX) return;
    snprintf(menu.items[menu.n], sizeof menu.items[0], "%s", label);
    menu.codes[menu.n++] = code;
}

static void ask(int action, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(confirm.msg, sizeof confirm.msg, fmt, ap);
    va_end(ap);
    confirm.action = action;
    confirm.active = 1;
}

static void free_list(char **l, int n)
{
    for (int i = 0; i < n; i++) free(l[i]);
    free(l);
}

static void free_clip(void)
{
    if (clip) free_list(clip, nclip);
    clip = NULL;
    nclip = 0;
}

/* Marked entries, or the highlighted one when nothing is marked. */
static int collect(char ***out)
{
    int marked = browser_marked_count(&br);
    int n = marked ? marked : (br.count ? 1 : 0);
    *out = NULL;
    if (!n) return 0;
    char **l = calloc(n, sizeof *l);
    if (!l) { toast(C_RED, "Out of memory"); return 0; }
    int k = 0;
    char p[1024];
    for (int i = 0; i < br.count && k < n; i++) {
        if (marked ? br.e[i].marked : i == br.sel) {
            /* a truncated path is a different file: never hand it to delete/move/zip */
            if (path_join(p, sizeof p, br.path, br.e[i].name)) continue;
            if ((l[k] = strdup(p))) k++;
        }
    }
    if (!k) { free(l); l = NULL; }
    *out = l;
    return k;
}

static void fix_scroll(int *sel, int *scroll, int count, int rows)
{
    if (*sel >= count) *sel = count ? count - 1 : 0;
    if (*sel < 0) *sel = 0;
    if (*sel < *scroll) *scroll = *sel;
    if (*sel >= *scroll + rows) *scroll = *sel - rows + 1;
    if (*scroll < 0) *scroll = 0;
}

static int list_nav(int *sel, int count)
{
    int old = *sel;
    if (pad_pressed(BTN_UP)) (*sel)--;
    if (pad_pressed(BTN_DOWN)) (*sel)++;
    if (pad_pressed(BTN_LEFT)) *sel -= LIST_ROWS;
    if (pad_pressed(BTN_RIGHT)) *sel += LIST_ROWS;
    if (*sel < 0) *sel = (old == 0 && count) ? count - 1 : 0;   /* wrap from top */
    if (*sel >= count) *sel = (old == count - 1) ? 0 : count - 1; /* wrap from bottom */
    return *sel != old;
}

/* First USB slot with a drive actually mounted. The /mnt/usbN folders exist even when empty,
 * so "is a folder" isn't enough - backups would otherwise land on the system filesystem. */
static const char *usb_root(void)
{
    static const char *slots[] = { "/mnt/usb0", "/mnt/usb1" };
    unsigned long long t, f;
    for (int i = 0; i < 2; i++)
        if (disk_space(slots[i], &t, &f) == 0) return slots[i];
    return NULL;
}

static void timestamp(char *out, int n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y%m%d_%H%M%S", &tm);
}

static void open_path(const char *p)
{
    if (browser_load(&br, p) != 0)
        toast(C_RED, "Can't open %s: %s", p, strerror(br.error));
    else
        tab = TAB_FILES;
}

/* ---------- actions ---------- */
static void do_action(int act)
{
    int n;
    switch (act) {
    case ACT_COPY:
    case ACT_CUT:
        free_clip();
        nclip = collect(&clip);
        clip_cut = (act == ACT_CUT);
        for (int i = 0; i < br.count; i++) br.e[i].marked = 0;
        toast(C_TEXT, "%d item(s) ready to %s. Go to the target folder and choose Paste.",
              nclip, clip_cut ? "move" : "copy");
        break;
    case ACT_PASTE:
        if (!nclip) { toast(C_RED, "Clipboard is empty"); break; }
        if (tv_job(clip_cut ? JOB_MOVE : JOB_COPY, clip, nclip, br.path) == 0 && clip_cut)
            free_clip();
        break;
    case ACT_CLEARCLIP:
        free_clip();
        toast(C_TEXT, "Clipboard cleared");
        break;
    case ACT_DELETE:
        n = browser_marked_count(&br);
        if (!br.count) break;
        if (n) ask(ACT_DELETE, "Delete %d marked item(s)? This can't be undone.", n);
        else ask(ACT_DELETE, "Delete \"%s\"? This can't be undone.", br.e[br.sel].name);
        break;
    case ACT_MKDIR:
        osk_open("New folder name", "New Folder", OSK_MKDIR);
        break;
    case ACT_RENAME:
        if (br.count) osk_open("Rename to", br.e[br.sel].name, OSK_RENAME);
        break;
    case ACT_VIEW: {
        if (!br.count) break;
        char p[1024], err[128];
        path_join(p, sizeof p, br.path, br.e[br.sel].name);
        if (viewer_open(p, err, sizeof err) != 0) toast(C_RED, "%s", err);
        break;
    }
    case ACT_VIEWIMG: {
        if (!br.count) break;
        char p[1024], err[160];
        path_join(p, sizeof p, br.path, br.e[br.sel].name);
        if (imgview_open(p, err, sizeof err) != 0) toast(C_RED, "%s", err);
        break;
    }
    case ACT_EXTRACT: {
        char **l;
        int cnt = collect(&l);
        int zips = 0;
        for (int i = 0; i < cnt; i++) {
            size_t ln = strlen(l[i]);
            if (ln > 4 && !strcasecmp(l[i] + ln - 4, ".zip")) l[zips++] = l[i];
            else free(l[i]);
        }
        if (zips) tv_job(JOB_EXTRACT, l, zips, br.path);
        else toast(C_RED, "No .zip files selected");
        free_list(l, zips);
        break;
    }
    case ACT_SEARCH:
        osk_open("Search this folder and subfolders for", sr_query, OSK_SEARCH);
        break;
    case ACT_SIZE: {
        char **l;
        int cnt = collect(&l);
        if (cnt) tv_job(JOB_SIZE, l, cnt, NULL);
        free_list(l, cnt);
        break;
    }
    case ACT_SORT: {
        /* the reload rebuilds the entries (marks cleared): carry the marks over by name */
        int nm = browser_marked_count(&br), k = 0;
        char **names = nm ? calloc(nm, sizeof *names) : NULL;
        for (int i = 0; names && i < br.count && k < nm; i++)
            if (br.e[i].marked && (names[k] = strdup(br.e[i].name))) k++;
        browser_sort = cfg.sort_mode = (browser_sort + 1) % SORT_COUNT;
        browser_reload(&br);
        for (int j = 0; j < k; j++)
            for (int i = 0; i < br.count; i++)
                if (!strcmp(br.e[i].name, names[j])) { br.e[i].marked = 1; break; }
        if (names) free_list(names, k);
        config_save();
        toast(C_TEXT, "Sorted by %s", sort_names[browser_sort]);
        break;
    }
    case ACT_BOOKMARK:
        if (cfg.nbookmarks >= MAX_BOOKMARKS) toast(C_RED, "Bookmark list is full (%d max)", MAX_BOOKMARKS);
        else if (bookmark_add(br.path) != 0) toast(C_RED, "Can't bookmark this folder (path too long)");
        else if (config_save() != 0) toast(C_RED, "Couldn't save settings to " CONFIG_FILE);
        else toast(C_GREEN, "Bookmarked %s", br.path);
        break;
    case ACT_UNBOOKMARK:
        bookmark_remove(br.path);
        config_save();
        toast(C_TEXT, "Bookmark removed");
        break;
    case ACT_ZIP: {
        if (!br.count) break;
        int m = browser_marked_count(&br);
        char def[256];
        if (m) snprintf(def, sizeof def, "Archive");
        else {
            snprintf(def, sizeof def, "%s", br.e[br.sel].name);
            char *dot = strrchr(def, '.');
            if (dot && dot != def && !br.e[br.sel].is_dir) *dot = 0;
        }
        osk_open("Zip file name (.zip is added)", def, OSK_ZIP);
        break;
    }
    case ACT_EDIT: {
        if (!br.count) break;
        char p[1024], err[128];
        if (path_join(p, sizeof p, br.path, br.e[br.sel].name)) { toast(C_RED, "Path too long"); break; }
        if (editor_open(p, err, sizeof err) != 0) toast(C_RED, "%s", err);
        break;
    }
    case ACT_HASH: {
        if (!br.count || br.e[br.sel].is_dir) break;
        char p[1024];
        path_join(p, sizeof p, br.path, br.e[br.sel].name);
        char *one[1] = { p };
        tv_job(JOB_HASH, one, 1, NULL);
        break;
    }
    case ACT_BIGFILES: {
        snprintf(sr_root, sizeof sr_root, "%s", br.path);
        char *root[1] = { sr_root };
        sr_active = 0;   /* the job frees the old results: stop drawing them first */
        tv_job(JOB_BIGFILES, root, 1, NULL);
        break;
    }
    case ACT_APPPATH: case ACT_APPPATH + 1: case ACT_APPPATH + 2: case ACT_APPPATH + 3: case ACT_APPPATH + 4:
        sr_active = 0;
        open_path(app_paths[act - ACT_APPPATH]);
        break;
    case ACT_MARKALL:
        for (int i = 0; i < br.count; i++) br.e[i].marked = 1;
        break;
    case ACT_UNMARK:
        for (int i = 0; i < br.count; i++) br.e[i].marked = 0;
        break;
    case ACT_PROPS: {
        if (!br.count) break;
        Entry *e = &br.e[br.sel];
        char p[1024], sz[32], tb[64];
        path_join(p, sizeof p, br.path, e->name);
        fmt_size(sz, sizeof sz, e->size);
        struct tm tm;
        localtime_r(&e->mtime, &tm);
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
        memset(&info, 0, sizeof info);
        info.active = 1;
        snprintf(info.lines[info.n++], 200, "Name:     %s", e->name);
        snprintf(info.lines[info.n++], 200, "Path:     %s", p);
        snprintf(info.lines[info.n++], 200, "Type:     %s", e->is_dir ? "Folder" : "File");
        if (!e->is_dir) snprintf(info.lines[info.n++], 200, "Size:     %s (%llu bytes)", sz, (unsigned long long)e->size);
        snprintf(info.lines[info.n++], 200, "Modified: %s", tb);
        snprintf(info.lines[info.n++], 200, "Mode:     %o", e->mode & 07777);
        break;
    }
    case ACT_SENDPAYLOAD: {
        if (!br.count) break;
        char p[1024], err[160];
        path_join(p, sizeof p, br.path, br.e[br.sel].name);
        if (payload_send(p, err, sizeof err) == 0) toast(C_GREEN, "Payload sent: %s", br.e[br.sel].name);
        else toast(C_RED, "%s", err);
        break;
    }
    default:
        break;
    }
}

static void confirmed(int act)
{
    if (act == ACT_DELETE) {
        char **l;
        int n = collect(&l);
        if (n) tv_job(JOB_DELETE, l, n, NULL);
        free_list(l, n);
    }
}

static void select_name(const char *name)
{
    for (int i = 0; i < br.count; i++)
        if (!strcmp(br.e[i].name, name)) { br.sel = i; return; }
}

/* Called when the on-screen keyboard closes with Done. */
static void osk_finished(int purpose, const char *text)
{
    if (purpose == OSK_EDITLINE) {          /* keep the line exactly as typed */
        editor_apply(text);
        return;
    }
    char name[256];
    snprintf(name, sizeof name, "%s", text);
    /* trim spaces at both ends */
    char *s = name;
    while (*s == ' ') s++;
    for (char *e = s + strlen(s); e > s && e[-1] == ' '; ) *--e = 0;

    if (purpose == OSK_SEARCH) {
        if (!*s) return;
        sr_active = 0;   /* the job frees the old results: stop drawing them first */
        snprintf(sr_query, sizeof sr_query, "%s", s);
        snprintf(sr_root, sizeof sr_root, "%s", br.path);
        char *root[1] = { sr_root };
        tv_job(JOB_SEARCH, root, 1, sr_query);
        return;
    }

    if (!*s || !strcmp(s, ".") || !strcmp(s, "..") || strchr(s, '/')) {
        toast(C_RED, "Invalid name");
        return;
    }
    if (purpose == OSK_ZIP) {
        char zname[300], out[1024];
        size_t l = strlen(s);
        snprintf(zname, sizeof zname, (l > 4 && !strcasecmp(s + l - 4, ".zip")) ? "%s" : "%s.zip", s);
        if (path_join(out, sizeof out, br.path, zname)) { toast(C_RED, "Path too long"); return; }
        struct kstat zs;
        if (k_lstat(out, &zs) == 0) { toast(C_RED, "\"%s\" already exists", zname); return; }
        char **l2;
        int cnt = collect(&l2);
        if (cnt) tv_job(JOB_ZIP, l2, cnt, out);
        free_list(l2, cnt);
        return;
    }
    char p[1024];
    if (path_join(p, sizeof p, br.path, s)) { toast(C_RED, "Path too long"); return; }
    struct kstat st;
    if (purpose == OSK_MKDIR) {
        if (mkdir(p, 0777) == 0) {
            browser_reload(&br);
            select_name(s);
            toast(C_GREEN, "Created folder \"%s\"", s);
        } else {
            toast(C_RED, "Create folder failed: %s", strerror(errno));
        }
    } else if (purpose == OSK_RENAME && br.count) {
        char old[1024];
        if (path_join(old, sizeof old, br.path, br.e[br.sel].name)) { toast(C_RED, "Path too long"); return; }
        if (!strcmp(old, p)) return;
        if (k_lstat(p, &st) == 0) { toast(C_RED, "\"%s\" already exists", s); return; }
        if (rename(old, p) == 0) {
            browser_reload(&br);
            select_name(s);
            toast(C_GREEN, "Renamed to \"%s\"", s);
        } else {
            toast(C_RED, "Rename failed: %s", strerror(errno));
        }
    }
}

static void open_file_menu(void)
{
    menu_open("Actions");
    int marked = browser_marked_count(&br);
    if (br.count && !br.e[br.sel].is_dir && !marked) {
        const char *nm = br.e[br.sel].name;
        if (is_image_name(nm)) menu_add("View picture", ACT_VIEWIMG);
        menu_add(is_image_name(nm) ? "View as hex" : "View", ACT_VIEW);
        if (!is_image_name(nm)) menu_add("Edit as text", ACT_EDIT);
        menu_add("Checksum (CRC32 / SHA-256)", ACT_HASH);
    }
    if (br.count) {
        const char *nm = br.e[br.sel].name;
        size_t l = strlen(nm);
        if (marked || (!br.e[br.sel].is_dir && l > 4 && !strcasecmp(nm + l - 4, ".zip")))
            menu_add(marked ? "Extract marked zips here" : "Extract here", ACT_EXTRACT);
    }
    if (br.count) {
        menu_add(marked ? "Copy marked" : "Copy", ACT_COPY);
        menu_add(marked ? "Move marked" : "Move", ACT_CUT);
    }
    if (nclip) {
        char lbl[64];
        snprintf(lbl, sizeof lbl, "Paste here (%d item%s)", nclip, nclip == 1 ? "" : "s");
        menu_add(lbl, ACT_PASTE);
    }
    if (br.count && !marked) menu_add("Rename", ACT_RENAME);
    if (br.count) menu_add(marked ? "Delete marked" : "Delete", ACT_DELETE);
    menu_add("New folder", ACT_MKDIR);
    if (br.count) {
        menu_add(marked ? "Compress marked to zip" : "Compress to zip", ACT_ZIP);
        menu_add(marked ? "Size of marked items" : "Calculate size", ACT_SIZE);
        menu_add("Mark all", ACT_MARKALL);
        if (marked) menu_add("Unmark all", ACT_UNMARK);
        menu_add("Properties", ACT_PROPS);
        const char *nm = br.e[br.sel].name;
        size_t l = strlen(nm);
        if (!br.e[br.sel].is_dir && l > 4 && !strcasecmp(nm + l - 4, ".bin"))
            menu_add("Send as payload", ACT_SENDPAYLOAD);
    }
    if (nclip) menu_add("Clear clipboard", ACT_CLEARCLIP);
}

static void open_goto_menu(void)
{
    char lbl[64];
    menu_open("Folder");
    menu_add("Search here...", ACT_SEARCH);
    menu_add("Find largest files here", ACT_BIGFILES);
    snprintf(lbl, sizeof lbl, "Sort by: %s", sort_names[browser_sort]);
    menu_add(lbl, ACT_SORT);
    if (bookmark_index(br.path) >= 0) menu_add("Remove bookmark", ACT_UNBOOKMARK);
    else menu_add("Bookmark this folder", ACT_BOOKMARK);
    for (int i = 0; i < cfg.nbookmarks; i++) {
        snprintf(lbl, sizeof lbl, "* %s", cfg.bookmarks[i]);
        menu_add(lbl, ACT_GOTO + GOTO_COUNT + i);
    }
    for (int i = 0; i < GOTO_COUNT; i++) {
        snprintf(lbl, sizeof lbl, "Go to %s", goto_paths[i]);
        menu_add(lbl, ACT_GOTO + i);
    }
}

static void backup_to_usb(int user)
{
    const char *usb = usb_root();
    if (!usb) { toast(C_RED, "No USB drive found (plug one into a front port)"); return; }
    char ts[32], dst[512];
    timestamp(ts, sizeof ts);
    if (user) {
        snprintf(dst, sizeof dst, "%s/OrbisKit/backup/userdata_%s", usb, ts);
        char *src[] = { "/user/home" };
        if (!is_dir(src[0])) { toast(C_RED, "/user/home not accessible (sandboxed?)"); return; }
        tv_job(JOB_COPY, src, 1, dst);
    } else {
        static const char *dbs[] = {
            "/system_data/priv/mms/app.db", "/system_data/priv/mms/addcont.db",
            "/system_data/priv/mms/av_content_bg.db",
        };
        char *src[3];
        int n = 0;
        struct kstat st;
        for (int i = 0; i < 3; i++)
            if (k_stat(dbs[i], &st) == 0) src[n++] = (char *)dbs[i];
        if (!n) { toast(C_RED, "Databases not accessible (sandboxed?)"); return; }
        snprintf(dst, sizeof dst, "%s/OrbisKit/backup/db_%s", usb, ts);
        tv_job(JOB_COPY, src, n, dst);
    }
}

static void sync_captures(void)
{
    const char *usb = usb_root();
    if (!usb) { toast(C_RED, "No USB drive found (plug one into a front port)"); return; }
    static const char *dirs[] = { "/user/av_contents/photo", "/user/av_contents/video" };
    char *src[2];
    int n = 0;
    for (int i = 0; i < 2; i++)
        if (is_dir(dirs[i])) src[n++] = (char *)dirs[i];
    if (!n) { toast(C_RED, "Capture folders not accessible (sandboxed?)"); return; }
    char dst[512];
    snprintf(dst, sizeof dst, "%s/OrbisKit/captures", usb);
    tv_job(JOB_SYNC, src, n, dst);
}

/* Opens the folder containing a search result and selects it. */
static void open_result(const char *full)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", full);
    char *sl = strrchr(dir, '/');
    if (!sl) return;
    if (sl == dir) dir[1] = 0; else *sl = 0;
    if (browser_load(&br, dir) != 0) { toast(C_RED, "Can't open %s", dir); return; }
    select_name(path_base(full));
    fix_scroll(&br.sel, &br.scroll, br.count, LIST_ROWS);
    sr_active = 0;
}

/* ---------- input ---------- */
static void input_results(void)
{
    if (search_count) list_nav(&sr_sel, search_count);
    fix_scroll(&sr_sel, &sr_scroll, search_count, LIST_ROWS);
    if (pad_pressed(BTN_CROSS) && search_count) {
        if (search_results[sr_sel]) open_result(search_results[sr_sel]);   /* NULL if strdup failed */
    }
    else if (pad_pressed(BTN_CIRCLE)) sr_active = 0;
    else if (!sr_big && (pad_pressed(BTN_OPTIONS) || pad_pressed(BTN_TRIANGLE))) do_action(ACT_SEARCH);
}

static void input_files(void)
{
    if (sr_active) { input_results(); return; }
    if (br.count) list_nav(&br.sel, br.count);
    if (pad_pressed(BTN_CROSS) && br.count) {
        Entry *e = &br.e[br.sel];
        if (e->is_dir) {
            char p[1024];
            path_join(p, sizeof p, br.path, e->name);
            if (browser_load(&br, p) != 0) toast(C_RED, "Can't open folder: %s", strerror(br.error));
        } else if (is_image_name(e->name)) {
            do_action(ACT_VIEWIMG);
        } else {
            open_file_menu();
        }
    }
    if (pad_pressed(BTN_CIRCLE)) browser_up(&br);
    if (pad_pressed(BTN_SQUARE) && br.count) {
        br.e[br.sel].marked ^= 1;
        if (br.sel < br.count - 1) br.sel++;
    }
    if (pad_pressed(BTN_TRIANGLE)) open_file_menu();
    if (pad_pressed(BTN_OPTIONS)) open_goto_menu();
    if (pad_pressed(BTN_R2) || pad_pressed(BTN_L2)) { browser_reload(&br); toast(C_TEXT, "Refreshed"); }
    fix_scroll(&br.sel, &br.scroll, br.count, LIST_ROWS);
}

static void input_web_toggle(void)
{
    if (web_running()) { web_stop(); toast(C_TEXT, "Web server stopped"); }
    else if (web_start() == 0) {
        sysinfo_ip(ip, sizeof ip);
        toast(C_GREEN, "Open http://%s:%d in your browser", ip, WEB_PORT);
        notify("Web file manager running\nhttp://%s:%d", ip, WEB_PORT);
    } else toast(C_RED, "Couldn't start the web server (port %d busy?)", WEB_PORT);
}

static void input_net(void)
{
    if (pad_pressed(BTN_CROSS)) input_web_toggle();
}

/* Moves to the previous/next picture in the current folder and opens it. */
static void image_step(int dir)
{
    for (int i = br.sel + dir; i >= 0 && i < br.count; i += dir) {
        if (br.e[i].is_dir || !is_image_name(br.e[i].name)) continue;
        char p[1024], err[160];
        path_join(p, sizeof p, br.path, br.e[i].name);
        br.sel = i;
        fix_scroll(&br.sel, &br.scroll, br.count, LIST_ROWS);
        if (imgview_open(p, err, sizeof err) != 0) toast(C_RED, "%s", err);
        return;
    }
}

static void image_position(char *out, int n)
{
    int idx = 0, total = 0;
    for (int i = 0; i < br.count; i++) {
        if (br.e[i].is_dir || !is_image_name(br.e[i].name)) continue;
        total++;
        if (i == br.sel) idx = total;
    }
    snprintf(out, n, "%d / %d", idx, total);
}

static void open_app_menu(void)
{
    App *a = apps_get(ap_sel);
    if (!a) return;
    menu_open(a->title);
    int k = 0;
    snprintf(app_paths[k], sizeof app_paths[0], "/user/app/%s", a->id);
    menu_add("Open game folder", ACT_APPPATH + k++);
    if (a->patch_size) {
        snprintf(app_paths[k], sizeof app_paths[0], "/user/patch/%s", a->id);
        menu_add("Open update folder", ACT_APPPATH + k++);
    }
    if (a->dlc) {
        snprintf(app_paths[k], sizeof app_paths[0], "/user/addcont/%s", a->id);
        menu_add("Open DLC folder", ACT_APPPATH + k++);
    }
    snprintf(app_paths[k], sizeof app_paths[0], "/user/appmeta/%s", a->id);
    menu_add("Open icon/artwork folder", ACT_APPPATH + k++);
    snprintf(app_paths[k], sizeof app_paths[0], "/system_data/priv/appmeta/%s", a->id);
    menu_add("Open param.sfo folder", ACT_APPPATH + k++);
}

static void input_apps(void)
{
    if (!ap_scanned) { apps_scan(); ap_scanned = 1; }
    int n = apps_count();
    if (n) list_nav(&ap_sel, n);
    fix_scroll(&ap_sel, &ap_scroll, n, APP_ROWS);
    if (pad_pressed(BTN_CROSS) && n) {
        App *a = apps_get(ap_sel);
        char p[64];
        snprintf(p, sizeof p, "/user/app/%s", a->id);
        sr_active = 0;
        open_path(p);
    }
    if (pad_pressed(BTN_TRIANGLE) && n) open_app_menu();
    if (pad_pressed(BTN_SQUARE)) {
        apps_scan();
        toast(C_TEXT, "Found %d installed app(s)", apps_count());
    }
}

static int pscroll;
static void input_payloads(void)
{
    if (npayloads) list_nav(&psel, npayloads);
    fix_scroll(&psel, &pscroll, npayloads, LIST_ROWS - 3);
    if (pad_pressed(BTN_TRIANGLE)) {
        npayloads = payload_scan(payloads, PAYLOAD_MAX);
        toast(C_TEXT, "Found %d payload(s)", npayloads);
    }
    if (pad_pressed(BTN_CROSS) && npayloads) {
        char err[160];
        if (payload_send(payloads[psel], err, sizeof err) == 0)
            toast(C_GREEN, "Payload sent: %s", path_base(payloads[psel]));
        else
            toast(C_RED, "%s", err);
    }
    if (pad_pressed(BTN_SQUARE) && npayloads) {
        if (!strcmp(cfg.autopayload, payloads[psel])) {
            cfg.autopayload[0] = 0;
            toast(C_TEXT, "Auto-send turned off");
        } else {
            snprintf(cfg.autopayload, sizeof cfg.autopayload, "%s", payloads[psel]);
            toast(C_GREEN, "%s will be sent every time OrbisKit starts", path_base(payloads[psel]));
        }
        config_save();
    }
}

/* ---------- list pages (Tools / Settings) ---------- */
static const char *item_name(int id)
{
    switch (id) {
    case TOOL_SYNC_CAPTURES: return "Copy screenshots and videos to USB";
    case TOOL_BACKUP_DB:     return "Back up system databases to USB";
    case TOOL_BACKUP_USER:   return "Back up user data to USB";
    case TOOL_BIGSCAN:       return "Find the largest files on internal storage";
    case TOOL_ESCAPE:        return "Retry sandbox escape (GoldHEN)";
    case TOOL_OPEN_USB:      return "Open USB drive in file manager";
    case TOOL_START_HOME:    return "When OrbisKit opens, show";
    case TOOL_WEB_AUTO:      return "Start web file manager on launch";
    case TOOL_THEME:         return "Theme";
    }
    return "?";
}

static const char *item_desc(int id)
{
    switch (id) {
    case TOOL_SYNC_CAPTURES: return "Copies new captures to USB:/OrbisKit/captures. Files already copied are skipped.";
    case TOOL_BACKUP_DB:     return "Copies app.db, addcont.db and av_content_bg.db to USB:/OrbisKit/backup/";
    case TOOL_BACKUP_USER:   return "Copies /user/home (saves, profile data) to USB:/OrbisKit/backup/";
    case TOOL_BIGSCAN:       return "Scans /user and lists the 200 biggest files, to see what fills the drive.";
    case TOOL_ESCAPE:        return "Asks GoldHEN to lift the app sandbox for full filesystem access.";
    case TOOL_OPEN_USB:      return "Jumps to /mnt/usb0 (or usb1) in the Files tab.";
    case TOOL_START_HOME:    return "The Home dashboard, or the tab and folder you used last.";
    case TOOL_WEB_AUTO:      return "Starts the browser file manager (port 8080) every time OrbisKit starts.";
    case TOOL_THEME:         return "Colour scheme of the app.";
    }
    return "";
}

/* Value shown on the right of a row; NULL for plain actions. */
static const char *item_value(int id, char *buf, int n, uint32_t *col)
{
    *col = C_DIM;
    switch (id) {
    case TOOL_THEME:      snprintf(buf, n, "< %s >", themes[cfg.theme].name); *col = C_TEXT; return buf;
    case TOOL_START_HOME: snprintf(buf, n, "< %s >", cfg.start_last ? "Last place" : "Home"); *col = C_TEXT; return buf;
    case TOOL_WEB_AUTO:   if (cfg.web_autostart) *col = C_GREEN; return cfg.web_autostart ? "ON" : "OFF";
    }
    return NULL;
}

/* Items changed with LEFT/RIGHT rather than run with X. */
static int item_adjust(int id, int d)
{
    switch (id) {
    case TOOL_THEME:
        cfg.theme = (cfg.theme + d + THEME_COUNT) % THEME_COUNT;
        config_save();
        return 1;
    case TOOL_START_HOME:
        cfg.start_last ^= 1;
        config_save();
        return 1;
    }
    return 0;
}

static void qa_run(int a);   /* defined in the Home dashboard section below */

static void run_item(int id)
{
    switch (id) {
    case TOOL_THEME:
    case TOOL_START_HOME:
        item_adjust(id, 1);
        break;
    case TOOL_BIGSCAN:
        if (browser_load(&br, "/user") != 0) { toast(C_RED, "Can't open /user (sandboxed?)"); break; }
        tab = TAB_FILES;
        sr_active = 0;
        do_action(ACT_BIGFILES);
        break;
    case TOOL_WEB_AUTO:
        cfg.web_autostart ^= 1;
        if (config_save() == 0) toast(C_GREEN, "Setting saved");
        else toast(C_RED, "Couldn't save settings to " CONFIG_FILE);
        break;
    case TOOL_SYNC_CAPTURES: sync_captures(); break;
    case TOOL_BACKUP_DB:   backup_to_usb(0); break;
    case TOOL_BACKUP_USER: backup_to_usb(1); break;
    case TOOL_ESCAPE:
        if (!jb_built_with_goldhen()) { toast(C_RED, "This build has no GoldHEN SDK. Rebuild with GOLDHEN_SDK set."); break; }
        jb_escape();
        has_root = jb_has_root();
        toast(has_root ? C_GREEN : C_RED, has_root ? "Full filesystem access enabled" : "Sandbox escape failed (is GoldHEN running?)");
        break;
    case TOOL_OPEN_USB: {
        const char *u = usb_root();
        if (u) { sr_active = 0; open_path(u); } else toast(C_RED, "No USB drive found");
        break;
    }
    }
}

static void input_list(const int *items, int n, int page)
{
    int *sel = &page_sel[page];
    if (pad_pressed(BTN_UP)) *sel = (*sel + n - 1) % n;
    if (pad_pressed(BTN_DOWN)) *sel = (*sel + 1) % n;
    fix_scroll(sel, &page_scroll[page], n, LIST_VIS);
    int id = items[*sel];
    if (pad_pressed(BTN_LEFT)) item_adjust(id, -1);
    if (pad_pressed(BTN_RIGHT)) item_adjust(id, 1);
    if (pad_pressed(BTN_CROSS)) run_item(id);
}

/* ---------- Home dashboard: quick actions ---------- */
static const char *qa_title(int a)
{
    switch (a) {
    case QA_WEB:         return "Web file manager";
    case QA_USB:         return "Open USB drive";
    case QA_PAYLOADS:    return "Payloads";
    case QA_BIGSCAN:     return "Find largest files";
    case QA_CAPTURES:    return "Sync captures";
    case QA_BACKUP_DB:   return "Back up databases";
    case QA_SYSTEM:      return "System";
    case QA_BACKUP_USER: return "Back up user data";
    case QA_FILES:       return "Files";
    case QA_APPS:        return "Apps";
    case QA_TOOLS:       return "Tools";
    case QA_SETTINGS:    return "Settings";
    case QA_INTERNAL:    return "Internal storage";
    case QA_DATA:        return "/data folder";
    case QA_AUTOPAYLOAD: return "Send auto payload";
    case QA_ESCAPE:      return "Sandbox escape";
    case QA_THEME:       return "Next theme";
    }
    return "Empty";
}

/* The action on tile t (customised in settings.ini, or the default; never out of range). */
static int tile_action(int t)
{
    int a = cfg.tiles_set ? cfg.tiles[t] : default_tiles[t];
    return (a > QA_NONE && a < QA_COUNT && !QA_RETIRED(a)) ? a : QA_NONE;
}

static void qa_run(int a)
{
    switch (a) {
    case QA_WEB:         input_web_toggle(); break;
    case QA_USB:         run_item(TOOL_OPEN_USB); break;
    case QA_PAYLOADS:    tab = TAB_PAYLOADS; break;
    case QA_BIGSCAN:     run_item(TOOL_BIGSCAN); break;
    case QA_CAPTURES:    run_item(TOOL_SYNC_CAPTURES); break;
    case QA_BACKUP_DB:   run_item(TOOL_BACKUP_DB); break;
    case QA_BACKUP_USER: run_item(TOOL_BACKUP_USER); break;
    case QA_SYSTEM:      tab = TAB_SYSTEM; break;
    case QA_FILES:       tab = TAB_FILES; break;
    case QA_APPS:        tab = TAB_APPS; break;
    case QA_TOOLS:       tab = TAB_TOOLS; break;
    case QA_SETTINGS:    tab = TAB_SETTINGS; break;
    case QA_INTERNAL:    sr_active = 0; open_path("/user"); break;
    case QA_DATA:        sr_active = 0; open_path("/data"); break;
    case QA_ESCAPE:      run_item(TOOL_ESCAPE); break;
    case QA_THEME:
        cfg.theme = (cfg.theme + 1) % THEME_COUNT;
        config_save();
        toast(C_TEXT, "Theme: %s", themes[cfg.theme].name);
        break;
    case QA_AUTOPAYLOAD: {
        if (!cfg.autopayload[0]) { toast(C_MARK, "No auto payload set - pick one in Payloads with [SQUARE]"); break; }
        char err[160];
        if (payload_send(cfg.autopayload, err, sizeof err) == 0) toast(C_GREEN, "Payload sent: %s", path_base(cfg.autopayload));
        else toast(C_RED, "%s", err);
        break;
    }
    default:
        toast(C_DIM, "Empty tile - press [TRIANGLE] to choose an action for it");
        break;
    }
}

/* TRIANGLE on a tile: choose what it does. */
static void open_tile_menu(void)
{
    char t[64];
    snprintf(t, sizeof t, "Tile %d: choose an action", home_sel + 1);
    menu_open(t);
    int cur = tile_action(home_sel);
    for (int a = QA_NONE + 1; a < QA_COUNT; a++) {
        if (QA_RETIRED(a)) continue;
        char lbl[64];
        snprintf(lbl, sizeof lbl, "%s%s", a == cur ? "> " : "  ", qa_title(a));
        menu_add(lbl, ACT_TILE_SET + a);
    }
    menu_add("  (empty)", ACT_TILE_SET + QA_NONE);
    menu_add("  Reset all tiles to default", ACT_TILE_RESET);
    menu.sel = cur > QA_NONE ? cur - 1 : 0;      /* start on the current choice */
    fix_scroll(&menu.sel, &menu.scroll, menu.n, MENU_VISIBLE);
}

static void tile_set(int a)
{
    if (!cfg.tiles_set) {                      /* first customisation: start from the defaults */
        memcpy(cfg.tiles, default_tiles, sizeof cfg.tiles);
        cfg.tiles_set = 1;
    }
    cfg.tiles[home_sel] = a;
    config_save();
    toast(C_GREEN, "Tile %d: %s", home_sel + 1, a == QA_NONE ? "empty" : qa_title(a));
}

static void home_activate(int t)
{
    qa_run(tile_action(t));
}

static void input_home(void)
{
    if (!ap_scanned) { apps_scan(); ap_scanned = 1; }   /* for the Apps card */
    int rows = (HT_COUNT + HT_COLS - 1) / HT_COLS;
    int r = home_sel / HT_COLS, c = home_sel % HT_COLS;
    if (pad_pressed(BTN_LEFT)) c = (c + HT_COLS - 1) % HT_COLS;
    if (pad_pressed(BTN_RIGHT)) c = (c + 1) % HT_COLS;
    if (pad_pressed(BTN_UP)) r = (r + rows - 1) % rows;
    if (pad_pressed(BTN_DOWN)) r = (r + 1) % rows;
    home_sel = r * HT_COLS + c;
    if (home_sel >= HT_COUNT) home_sel = HT_COUNT - 1;
    if (pad_pressed(BTN_CROSS)) home_activate(home_sel);
    if (pad_pressed(BTN_TRIANGLE)) open_tile_menu();
}

static void handle_input(void)
{
    /* a job started from the web (quiet) runs invisibly: the TV stays usable and can't cancel it */
    if (g_job.running && !g_job.quiet) {
        if (pad_pressed(BTN_CIRCLE)) g_job.cancel = 1;
        return;
    }
    if (osk_active()) {
        if (osk_update() && osk_accepted()) osk_finished(osk_purpose(), osk_text());
        return;
    }
    if (editor_active()) {
        const char *init = NULL;
        if (editor_input(&init) && init) osk_open("Edit line", init, OSK_EDITLINE);
        const char *m = editor_message();
        if (m) toast(strncmp(m, "Saved", 5) ? C_RED : C_GREEN, "%s", m);
        return;
    }
    if (imgview_active()) {
        if (pad_pressed(BTN_CIRCLE)) imgview_close();
        else if (pad_pressed(BTN_LEFT) || pad_pressed(BTN_L1)) image_step(-1);
        else if (pad_pressed(BTN_RIGHT) || pad_pressed(BTN_R1)) image_step(1);
        else if (pad_pressed(BTN_TRIANGLE)) imgview_toggle_info();
        return;
    }
    if (viewer_active()) {
        viewer_input();
        return;
    }
    if (info.active) {
        if (pad_pressed(BTN_CROSS) || pad_pressed(BTN_CIRCLE)) info.active = 0;
        return;
    }
    if (confirm.active) {
        if (pad_pressed(BTN_CROSS)) { confirm.active = 0; confirmed(confirm.action); }
        else if (pad_pressed(BTN_CIRCLE)) confirm.active = 0;
        return;
    }
    if (menu.active) {
        if (pad_pressed(BTN_UP)) menu.sel = (menu.sel + menu.n - 1) % menu.n;
        if (pad_pressed(BTN_DOWN)) menu.sel = (menu.sel + 1) % menu.n;
        fix_scroll(&menu.sel, &menu.scroll, menu.n, MENU_VISIBLE);
        if (pad_pressed(BTN_CIRCLE) || pad_pressed(BTN_TRIANGLE) || pad_pressed(BTN_OPTIONS)) menu.active = 0;
        else if (pad_pressed(BTN_CROSS)) {
            int code = menu.codes[menu.sel];
            menu.active = 0;
            if (code >= ACT_GOTO + GOTO_COUNT) {
                sr_active = 0;
                open_path(cfg.bookmarks[code - ACT_GOTO - GOTO_COUNT]);
            } else if (code >= ACT_GOTO) {
                sr_active = 0;
                open_path(goto_paths[code - ACT_GOTO]);
            } else if (code >= ACT_TILE_SET && code <= ACT_TILE_SET_LAST) {
                tile_set(code - ACT_TILE_SET);
            } else if (code == ACT_TILE_RESET) {
                cfg.tiles_set = 0;
                config_save();
                toast(C_GREEN, "Quick actions reset to default");
            } else {
                do_action(code);
            }
        }
        return;
    }
    if (pad_pressed(BTN_L1)) tab = (tab + TAB_COUNT - 1) % TAB_COUNT;
    if (pad_pressed(BTN_R1)) tab = (tab + 1) % TAB_COUNT;

    switch (tab) {
    case TAB_HOME:     input_home(); break;
    case TAB_FILES:    input_files(); break;
    case TAB_APPS:     input_apps(); break;
    case TAB_NET:      input_net(); break;
    case TAB_PAYLOADS: input_payloads(); break;
    case TAB_TOOLS:    input_list(tools_items, NITEMS(tools_items), PAGE_TOOLS); break;
    case TAB_SETTINGS: input_list(settings_items, NITEMS(settings_items), PAGE_SETTINGS); break;
    case TAB_SYSTEM:
        if (pad_pressed(BTN_CROSS)) {
            char err[128];
            if (viewer_open(DLOG_FILE, err, sizeof err) != 0) toast(C_RED, "No log yet: %s", err);
        }
        break;
    default: break;
    }
}

/* ---------- drawing ---------- */
static void draw_box(int x, int y, int w, int h, uint32_t border)
{
    gfx_rect(x - 3, y - 3, w + 6, h + 6, border);
    gfx_rect(x, y, w, h, C_PANEL);
}

static void draw_header(void)
{
    gfx_rect(0, 0, SCREEN_W, 96, C_PANEL);
    gfx_text(MX, 30, "ORBISKIT", C_TEXT, 0);
    gfx_text(MX + 200, 30, "v" APP_VERSION, C_DIM, 0);
    char right[160], cpu[16], fwshort[16] = "?", clock[16];
    if (cpu_t < 0) snprintf(cpu, sizeof cpu, "--");
    else snprintf(cpu, sizeof cpu, "%d C", cpu_t);
    sscanf(fw, "%15s", fwshort);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(clock, sizeof clock, "%H:%M", &tm);
    snprintf(right, sizeof right, "FW %s   CPU %s   %s", fwshort, cpu, has_root ? "ROOT" : "SANDBOXED");
    int w = gfx_text_width(right);
    gfx_text(SCREEN_W - MX - w, 30, right, has_root ? C_GREEN : C_MARK, 0);
    int cw = gfx_text_width(clock);
    gfx_text(SCREEN_W - MX - w - 40 - cw, 30, clock, C_TEXT, 0);

    /* server badges: shows at a glance that the console is reachable over the network */
    int bx = SCREEN_W - MX - w - 40 - cw - 30;
    const char *badges[1] = { web_running() ? "WEB" : NULL };
    for (int i = 0; i < 1; i++) {
        if (!badges[i]) continue;
        int bw = gfx_text_width(badges[i]) + 20;
        bx -= bw;
        gfx_rect(bx, 26, bw, 42, C_GREEN);
        gfx_text(bx + 10, 30, badges[i], 0xFF0F1318, 0);
        bx -= 10;
    }

    int x = MX;
    gfx_hint(x, TABS_Y + 10, "[L1]", C_DIM, 0);
    x += 56;
    for (int i = 0; i < TAB_COUNT; i++) {
        int tw = gfx_text_width(tab_names[i]) + 48;
        if (i == tab) gfx_rect(x, TABS_Y, tw, 54, C_ACCENT);
        else gfx_rect(x, TABS_Y, tw, 54, C_PANEL);
        gfx_text(x + 24, TABS_Y + 10, tab_names[i], i == tab ? 0xFFFFFFFF : C_DIM, 0);
        x += tw + 8;
    }
    gfx_hint(x + 16, TABS_Y + 10, "[R1]", C_DIM, 0);
}

static void draw_footer(const char *hints)
{
    gfx_rect(0, FOOTER_Y - 8, SCREEN_W, SCREEN_H - FOOTER_Y + 8, C_PANEL);
    gfx_hint(MX, FOOTER_Y + 14, hints, C_DIM, SCREEN_W - 2 * MX);
}

static void draw_scrollbar(int top, int rows, int scroll, int count)
{
    if (count <= rows) return;
    int h = rows * ROW_H;
    int bh = h * rows / count;
    if (bh < 30) bh = 30;
    int by = top + (h - bh) * scroll / (count - rows);
    gfx_rect(SCREEN_W - MX + 20, top, 6, h, C_LINE);
    gfx_rect(SCREEN_W - MX + 20, by, 6, bh, C_ACCENT);
}

static void draw_results(void)
{
    char buf[300];
    if (sr_big) snprintf(buf, sizeof buf, "Largest files in %s", sr_root);
    else snprintf(buf, sizeof buf, "Search \"%s\" in %s", sr_query, sr_root);
    gfx_text(MX, PATH_Y, buf, C_ACCENT, 1300);
    snprintf(buf, sizeof buf, "%d result%s%s", search_count, search_count == 1 ? "" : "s",
             !sr_big && search_count >= SEARCH_MAX ? " (limit reached)" : "");
    gfx_text(SCREEN_W - MX - gfx_text_width(buf), PATH_Y, buf, C_MARK, 0);
    gfx_rect(MX, LIST_Y - 10, SCREEN_W - 2 * MX, 2, C_LINE);
    if (!search_count) gfx_text(MX + 20, LIST_Y + 10, "Nothing found.", C_DIM, 0);
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = sr_scroll + r;
        if (i >= search_count) break;
        int y = LIST_Y + r * ROW_H;
        if (i == sr_sel) gfx_rect(MX, y, SCREEN_W - 2 * MX, ROW_H - 4, C_SEL);
        const char *full = search_results[i];
        if (!full) continue;   /* the worker's strdup failed (out of memory) */
        size_t rl = strlen(sr_root);
        const char *rel = strncmp(full, sr_root, rl) ? full : full + rl;
        if (*rel == '/') rel++;
        if (!strcmp(sr_root, "/")) rel = full + 1;
        int w = gfx_text(MX + 20, y + 3, path_base(full), C_TEXT, 600);
        int sizew = 0;
        if (sr_big) {
            char sz[32];
            fmt_size(sz, sizeof sz, search_sizes[i]);
            sizew = gfx_text_width(sz) + 40;
            gfx_text(SCREEN_W - MX - 20 - gfx_text_width(sz), y + 3, sz, C_MARK, 0);
        }
        gfx_text(MX + 40 + (w > 600 ? 600 : w) + 20, y + 3, rel, C_DIM, SCREEN_W - 2 * MX - 700 - sizew);
    }
    draw_scrollbar(LIST_Y, LIST_ROWS, sr_scroll, search_count);
    draw_footer(sr_big ? "[X] Go to file  [O] Back to folder"
                       : "[X] Go to file  [O] Back to folder  [TRIANGLE] New search");
}

static void draw_files(void)
{
    char buf[256];
    if (sr_active) { draw_results(); return; }
    gfx_text(MX, PATH_Y, br.path, C_ACCENT, 1250);
    if (nclip) {
        snprintf(buf, sizeof buf, "Clipboard: %d to %s", nclip, clip_cut ? "move" : "copy");
        gfx_text(SCREEN_W - MX - gfx_text_width(buf), PATH_Y, buf, C_MARK, 0);
    }
    gfx_rect(MX, LIST_Y - 10, SCREEN_W - 2 * MX, 2, C_LINE);

    if (!br.count) {
        gfx_text(MX + 20, LIST_Y + 10, br.error ? strerror(br.error) : "(empty folder)", C_DIM, 0);
    }
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = br.scroll + r;
        if (i >= br.count) break;
        Entry *e = &br.e[i];
        int y = LIST_Y + r * ROW_H;
        if (i == br.sel) gfx_rect(MX, y, SCREEN_W - 2 * MX, ROW_H - 4, C_SEL);
        uint32_t col = e->marked ? C_MARK : (e->is_dir ? C_ACCENT : C_TEXT);
        if (e->marked) gfx_text(MX + 10, y + 3, "*", C_MARK, 0);
        snprintf(buf, sizeof buf, "%s%s", e->name, e->is_dir ? "/" : "");
        gfx_text(MX + 40, y + 3, buf, col, SCREEN_W - 2 * MX - 40 - 520);
        /* date column, then size right-aligned */
        if (e->mtime > 0) {
            struct tm tm;
            localtime_r(&e->mtime, &tm);
            strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
            gfx_text(SCREEN_W - MX - 480, y + 3, buf, C_DIM, 0);
        }
        if (!e->is_dir) {
            fmt_size(buf, sizeof buf, e->size);
            gfx_text(SCREEN_W - MX - 20 - gfx_text_width(buf), y + 3, buf, C_DIM, 0);
        }
    }
    draw_scrollbar(LIST_Y, LIST_ROWS, br.scroll, br.count);
    snprintf(buf, sizeof buf, "%d item(s)", br.count);
    int m = browser_marked_count(&br);
    if (m) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), ", %d marked", m);
    gfx_text(SCREEN_W - MX - gfx_text_width(buf), LIST_Y + LIST_ROWS * ROW_H + 4, buf, C_DIM, 0);
    {
        char left[128];
        snprintf(left, sizeof left, "%s%sSort: %s", free_here, free_here[0] ? "   " : "", sort_names[browser_sort]);
        gfx_text(MX, LIST_Y + LIST_ROWS * ROW_H + 4, left, C_DIM, 0);
    }

    draw_footer("[X] Open  [O] Back  [SQUARE] Mark  [TRIANGLE] Actions  [OPTIONS] Search/Sort/Go to  [L2/R2] Refresh");
}

static void draw_apps(void)
{
    int n = apps_count();
    char buf[300], a1[32], a2[32];
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += apps_get(i)->app_size + apps_get(i)->patch_size;
    fmt_size(a1, sizeof a1, total);
    snprintf(buf, sizeof buf, "%d installed   %s total (games + updates)", n, a1);
    gfx_text(MX, PATH_Y - 4, buf, C_ACCENT, 0);
    int top = PATH_Y + 40;
    gfx_rect(MX, top - 8, SCREEN_W - 2 * MX, 2, C_LINE);
    if (!n) gfx_text(MX + 20, top + 10, "No installed apps found (needs root access).", C_DIM, 0);
    for (int r = 0; r < APP_ROWS; r++) {
        int i = ap_scroll + r;
        App *a = apps_get(i);
        if (!a) break;
        int y = top + r * APP_ROW_H;
        if (i == ap_sel) gfx_rect(MX, y, SCREEN_W - 2 * MX, APP_ROW_H - 6, C_SEL);
        int ix = MX + 6, iy = y + (APP_ROW_H - 6 - APP_ICON) / 2;
        if (a->icon) gfx_blit(ix, iy, APP_ICON, APP_ICON, a->icon);
        else gfx_rect(ix, iy, APP_ICON, APP_ICON, C_LINE);
        int tx = ix + APP_ICON + 24;
        gfx_text(tx, y + 6, a->title, C_TEXT, SCREEN_W - MX - tx - 20);
        fmt_size(a1, sizeof a1, a->app_size);
        int k = snprintf(buf, sizeof buf, "%s   %s   v%s   %s", a->id, apps_category_name(a->category),
                         a->version[0] ? a->version : "?", a1);
        if (a->patch_size) {
            fmt_size(a2, sizeof a2, a->patch_size);
            k += snprintf(buf + k, sizeof buf - k, "   + update %s", a2);
        }
        if (a->dlc) snprintf(buf + k, sizeof buf - k, "   + %d DLC", a->dlc);
        gfx_text(tx, y + 44, buf, C_DIM, SCREEN_W - MX - tx - 20);
    }
    if (n > APP_ROWS) {
        int h = APP_ROWS * APP_ROW_H, bh = h * APP_ROWS / n;
        if (bh < 30) bh = 30;
        int by = top + (h - bh) * ap_scroll / (n - APP_ROWS);
        gfx_rect(SCREEN_W - MX + 20, top, 6, h, C_LINE);
        gfx_rect(SCREEN_W - MX + 20, by, 6, bh, C_ACCENT);
    }
    draw_footer("[X] Open game folder   [TRIANGLE] More folders   [SQUARE] Refresh");
}

static void draw_kv(int y, const char *k, const char *v, uint32_t vc)
{
    gfx_text(MX + 20, y, k, C_DIM, 0);
    gfx_text(MX + 420, y, v, vc, 540);   /* stays left of the System tab's temperature graph */
}

/* The web server panel on the Network tab. */
static int draw_server(int x, int y, int w, const char *title, int run, const char *addr,
                       const char *how, const char *stat, int (*logline)(int, char *, int))
{
    char buf[160];
    draw_box(x, y, w, 690, run ? C_GREEN : C_LINE);
    gfx_text(x + 30, y + 20, title, C_ACCENT, 0);
    const char *st = run ? "RUNNING" : "STOPPED";
    gfx_text(x + w - 30 - gfx_text_width(st), y + 20, st, run ? C_GREEN : C_RED, 0);
    gfx_text(x + 30, y + 80, addr, run ? 0xFFFFFFFF : C_DIM, w - 60);
    gfx_text(x + 30, y + 130, how, C_DIM, w - 60);
    gfx_text(x + 30, y + 175, stat, C_TEXT, w - 60);
    gfx_rect(x + 20, y + 230, w - 40, 2, C_LINE);
    for (int i = 0; i < 9; i++) {
        if (!logline(i, buf, sizeof buf)) break;
        gfx_text(x + 30, y + 250 + i * 44, buf, i ? C_DIM : C_TEXT, w - 60);
    }
    return 0;
}

static void draw_net(void)
{
    char addr[96], stat[96];
    int w = (SCREEN_W - 2 * MX - 40) / 2;
    int y = PATH_Y + 4;

    snprintf(addr, sizeof addr, "http://%s:%d", ip, WEB_PORT);
    snprintf(stat, sizeof stat, "Requests served: %d", web_requests());
    draw_server(MX, y, w, "Web file manager", web_running(), addr,
                "Open in any browser on your PC or phone", stat, web_log_line);

    /* FTP: GoldHEN already runs one, so the app doesn't have its own */
    int x = MX + w + 40;
    draw_box(x, y, w, 690, C_LINE);
    gfx_text(x + 30, y + 20, "FTP (GoldHEN)", C_ACCENT, 0);
    snprintf(addr, sizeof addr, "ftp://%s:%d", ip, GOLDHEN_FTP_PORT);
    gfx_text(x + 30, y + 80, addr, 0xFFFFFFFF, w - 60);
    gfx_text(x + 30, y + 130, "GoldHEN has its own FTP server, so OrbisKit", C_DIM, w - 60);
    gfx_text(x + 30, y + 175, "doesn't need one. Turn it on in GoldHEN's", C_DIM, w - 60);
    gfx_text(x + 30, y + 220, "settings, then connect with FileZilla / WinSCP.", C_DIM, w - 60);

    draw_footer("[X] Start/stop the web file manager   It can reach the whole console");
}

static void draw_payloads(void)
{
    gfx_text(MX, PATH_Y, "Sent to GoldHEN BinLoader (127.0.0.1:9090)", C_ACCENT, 0);
    gfx_text(MX, PATH_Y + 40, "Searched: USB root, USB/payloads, /data/payloads, /data/GoldHEN/payloads", C_DIM, 0);
    int top = LIST_Y + 50;
    gfx_rect(MX, top - 10, SCREEN_W - 2 * MX, 2, C_LINE);
    if (!npayloads) gfx_hint(MX + 20, top + 10, "No .bin payloads found. Copy some to a USB drive and press [TRIANGLE].", C_DIM, 0);
    int rows = LIST_ROWS - 3;
    for (int r = 0; r < rows; r++) {
        int i = pscroll + r;
        if (i >= npayloads) break;
        int y = top + r * ROW_H;
        if (i == psel) gfx_rect(MX, y, SCREEN_W - 2 * MX, ROW_H - 4, C_SEL);
        int is_auto = !strcmp(cfg.autopayload, payloads[i]);
        int w = gfx_text(MX + 20, y + 3, path_base(payloads[i]), C_TEXT, 600);
        if (is_auto) gfx_text(MX + 40 + w, y + 3, "[AUTO]", C_GREEN, 0);
        gfx_text(MX + 760, y + 3, payloads[i], C_DIM, SCREEN_W - 2 * MX - 780);
    }
    draw_scrollbar(top, rows, pscroll, npayloads);
    draw_footer("[X] Send payload   [SQUARE] Send automatically at launch (on/off)   [TRIANGLE] Rescan");
}

#define C_SOC 0xFFB07CFF   /* SoC line in the temperature graph (CPU uses the accent colour) */

/* Temperature history graph (last 10 minutes), 30-90 C scale with 65/75 C guide lines. */
static void draw_temp_graph(int x, int y, int w, int h)
{
    int cpu[TEMP_HIST], soc[TEMP_HIST], pc, ps;
    int n = sysinfo_temp_history(cpu, soc, TEMP_HIST, &pc, &ps);
    char buf[96];
    gfx_rect(x, y, w, h, C_PANEL);
    gfx_rect(x, y, w, 4, C_ACCENT);
    gfx_text(x + 24, y + 18, "Temperature - last 10 minutes", C_ACCENT, w - 48);
    int gx = x + 90, gy = y + 80, gw = w - 120, gh = h - 150;
    const int lo = 30, hi = 90;
#define TY(t) (gy + gh - ((t) < lo ? 0 : (t) > hi ? gh : ((t) - lo) * gh / (hi - lo)))
    static const int guides[] = { 50, 65, 75 };
    for (int i = 0; i < 3; i++) {
        int t = guides[i], yy = TY(t);
        gfx_rect(gx, yy, gw, 1, t == 75 ? C_RED : t == 65 ? C_MARK : C_LINE);
        snprintf(buf, sizeof buf, "%d", t);
        gfx_text(x + 24, yy - 14, buf, C_DIM, 0);
    }
    if (n < 2) {
        gfx_text(gx + 20, gy + gh / 2 - 14, "Collecting readings (one every 10 s)...", C_DIM, 0);
    } else {
        for (int s = 0; s < 2; s++) {
            const int *v = s ? soc : cpu;
            uint32_t col = s ? C_SOC : C_ACCENT;
            for (int i = 0; i < n; i++) {
                if (v[i] < 0) continue;
                /* newest sample at the right edge; older ones step left */
                int px = gx + gw - (n - 1 - i) * gw / (TEMP_HIST - 1), py = TY(v[i]);
                gfx_rect(px - 3, py - 3, 7, 7, col);
                if (i > 0 && v[i - 1] >= 0) {   /* connect to the previous point */
                    int qx = gx + gw - (n - i) * gw / (TEMP_HIST - 1), qy = TY(v[i - 1]);
                    int steps = px - qx;
                    for (int k = 1; k < steps; k++) gfx_rect(qx + k - 1, qy + (py - qy) * k / steps - 1, 3, 3, col);
                }
            }
        }
    }
#undef TY
    int ly = y + h - 52;
    gfx_rect(x + 24, ly + 10, 26, 8, C_ACCENT);
    if (cpu_t >= 0) snprintf(buf, sizeof buf, "CPU  now %d C  peak %d C", cpu_t, pc);
    else snprintf(buf, sizeof buf, "CPU  n/a");
    gfx_text(x + 62, ly, buf, C_TEXT, w / 2 - 70);
    gfx_rect(x + w / 2, ly + 10, 26, 8, C_SOC);
    if (soc_t >= 0) snprintf(buf, sizeof buf, "SoC  now %d C  peak %d C", soc_t, ps);
    else snprintf(buf, sizeof buf, "SoC  n/a");
    gfx_text(x + w / 2 + 38, ly, buf, C_TEXT, w / 2 - 60);
}

static void draw_system(void)
{
    char buf[160];
    draw_temp_graph(1060, PATH_Y + 4, SCREEN_W - MX - 1060, 560);
    int y = PATH_Y + 10;
    draw_kv(y, "Firmware", fw, C_TEXT);                                    y += ROW_H + 6;
    if (cpu_t >= 0) snprintf(buf, sizeof buf, "%d C", cpu_t); else snprintf(buf, sizeof buf, "n/a");
    draw_kv(y, "CPU temperature", buf, cpu_t >= 75 ? C_RED : cpu_t >= 65 ? C_MARK : C_GREEN); y += ROW_H + 6;
    if (soc_t >= 0) snprintf(buf, sizeof buf, "%d C", soc_t); else snprintf(buf, sizeof buf, "n/a");
    draw_kv(y, "SoC temperature", buf, soc_t >= 75 ? C_RED : soc_t >= 65 ? C_MARK : C_GREEN); y += ROW_H + 6;
    draw_kv(y, "IP address", ip, C_TEXT);                                  y += ROW_H + 6;
    draw_kv(y, "Filesystem access", has_root ? "Full (sandbox escaped)" : "Sandboxed", has_root ? C_GREEN : C_MARK); y += ROW_H + 6;
    {
        /* root access here can only come from GoldHEN's jailbreak call */
        const char *gh = !jb_built_with_goldhen() ? "not in this build" : has_root ? "active" : "not detected";
        draw_kv(y, "GoldHEN", gh, has_root ? C_GREEN : C_DIM);
        y += ROW_H + 6;
    }
    draw_kv(y, "Web server", web_running() ? "running" : "stopped", web_running() ? C_GREEN : C_DIM); y += ROW_H + 6;
    for (int i = 0; i < DISK_COUNT; i++) {
        if (i > 0 && !disks[i].ok) continue;      /* hide USB slots with nothing plugged in */
        draw_kv(y, disks[i].label, disks[i].ok ? disks[i].text : "n/a", disks[i].ok ? C_TEXT : C_DIM);
        y += ROW_H + 6;
    }
    uint64_t s = sceKernelGetProcessTime() / 1000000ULL;
    snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600),
             (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    draw_kv(y, "App uptime", buf, C_TEXT);
    draw_footer("[X] Open debug log   (" DLOG_FILE ")");
}

#define ITEM_H 76

static void draw_list(const int *items, int n, int page, const char *title)
{
    gfx_text(MX, PATH_Y - 4, title, C_ACCENT, 0);
    int top = PATH_Y + 42;
    gfx_rect(MX, top - 10, SCREEN_W - 2 * MX, 2, C_LINE);
    int sel = page_sel[page], scroll = page_scroll[page];
    for (int r = 0; r < LIST_VIS; r++) {
        int i = scroll + r;
        if (i >= n) break;
        int id = items[i], y = top + r * ITEM_H;
        if (i == sel) gfx_rect(MX, y - 4, SCREEN_W - 2 * MX, ITEM_H - 6, C_SEL);
        gfx_text(MX + 20, y, item_name(id), i == sel ? 0xFFFFFFFF : C_TEXT, 1200);
        char vb[64];
        uint32_t vc;
        const char *v = item_value(id, vb, sizeof vb, &vc);
        if (v) gfx_text(SCREEN_W - MX - 30 - gfx_text_width(v), y, v, vc, 0);
        gfx_text(MX + 20, y + 33, item_desc(id), C_DIM, SCREEN_W - 2 * MX - 40);
    }
    if (n > LIST_VIS) {
        int h = LIST_VIS * ITEM_H, bh = h * LIST_VIS / n, by = top + (h - bh) * scroll / (n - LIST_VIS);
        gfx_rect(SCREEN_W - MX + 20, top, 6, h, C_LINE);
        gfx_rect(SCREEN_W - MX + 20, by, 6, bh, C_ACCENT);
    }
    int id = items[sel];
    draw_footer((id == TOOL_THEME || id == TOOL_START_HOME) ? "[LEFT/RIGHT] or [X] Change"
                : id == TOOL_WEB_AUTO ? "[X] Turn on/off" : "[X] Run");
}

/* ---------- Home dashboard ---------- */
#define CARD_GAP 24

static void draw_card(int x, int y, int w, int h, const char *title)
{
    gfx_rect(x, y, w, h, C_PANEL);
    gfx_rect(x, y, w, 4, C_ACCENT);
    gfx_text(x + 24, y + 18, title, C_ACCENT, w - 48);
}

/* label left, value right, inside a card */
static void card_kv(int x, int y, int w, const char *k, const char *v, uint32_t vc)
{
    int kw = gfx_text(x + 24, y, k, C_DIM, w / 2);
    int room = w - 48 - kw - 20, vw = gfx_text_width(v);
    if (vw > room) vw = room;
    gfx_text(x + w - 24 - vw, y, v, vc, vw);
}

static uint32_t temp_col(int t) { return t < 0 ? C_DIM : t >= 75 ? C_RED : t >= 65 ? C_MARK : C_GREEN; }

static void draw_home(void)
{
    char a[48], b[48];
    int cw = (SCREEN_W - 2 * MX - 3 * CARD_GAP) / 4, ch = 330, y0 = PATH_Y - 16;

    /* --- Storage --- */
    int x = MX;
    draw_card(x, y0, cw, ch, "Storage");
    int yy = y0 + 72, shown = 0;
    for (int i = 0; i < DISK_COUNT && shown < 3; i++) {
        if (!disks[i].ok || !disks[i].total) continue;
        double used = 1.0 - (double)disks[i].avail / disks[i].total;
        fmt_size(a, sizeof a, disks[i].avail);
        snprintf(b, sizeof b, "%s free", a);
        card_kv(x, yy, cw, i == 0 ? "Internal" : disks[i].path + 5, b, C_TEXT);
        gfx_rect(x + 24, yy + 40, cw - 48, 12, C_LINE);
        gfx_rect(x + 24, yy + 40, (int)((cw - 48) * used), 12, used > 0.9 ? C_RED : used > 0.75 ? C_MARK : C_ACCENT);
        yy += 82;
        shown++;
    }
    if (!shown) gfx_text(x + 24, yy, "Reading...", C_DIM, 0);

    /* --- System --- */
    x += cw + CARD_GAP;
    draw_card(x, y0, cw, ch, "System");
    char fwshort[16] = "?";
    sscanf(fw, "%15s", fwshort);
    card_kv(x, y0 + 72, cw, "Firmware", fwshort, C_TEXT);
    int pkc, pks, hc[1], hs[1];
    sysinfo_temp_history(hc, hs, 1, &pkc, &pks);
    if (cpu_t >= 0) snprintf(a, sizeof a, pkc > cpu_t ? "%d C  (peak %d)" : "%d C", cpu_t, pkc); else snprintf(a, sizeof a, "n/a");
    card_kv(x, y0 + 120, cw, "CPU", a, temp_col(cpu_t));
    if (soc_t >= 0) snprintf(a, sizeof a, pks > soc_t ? "%d C  (peak %d)" : "%d C", soc_t, pks); else snprintf(a, sizeof a, "n/a");
    card_kv(x, y0 + 168, cw, "SoC", a, temp_col(soc_t));
    card_kv(x, y0 + 216, cw, "Access", has_root ? "ROOT" : "Sandboxed", has_root ? C_GREEN : C_MARK);
    uint64_t up = sceKernelGetProcessTime() / 1000000ULL;
    snprintf(a, sizeof a, "%llu:%02llu", (unsigned long long)(up / 3600), (unsigned long long)(up / 60 % 60));
    card_kv(x, y0 + 264, cw, "App uptime", a, C_TEXT);

    /* --- Network --- */
    x += cw + CARD_GAP;
    draw_card(x, y0, cw, ch, "Network");
    card_kv(x, y0 + 72, cw, "IP", ip, C_TEXT);
    card_kv(x, y0 + 120, cw, "FTP", "GoldHEN :2121", C_DIM);
    card_kv(x, y0 + 168, cw, "Web :8080", web_running() ? "ON" : "OFF", web_running() ? C_GREEN : C_DIM);
    snprintf(a, sizeof a, "%d", web_requests());
    card_kv(x, y0 + 216, cw, "Web requests", a, web_running() ? C_TEXT : C_DIM);
    card_kv(x, y0 + 264, cw, "Login", "any (no password)", C_DIM);

    /* --- Apps & payloads --- */
    x += cw + CARD_GAP;
    draw_card(x, y0, cw, ch, "Apps & payloads");
    int n = apps_count();
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += apps_get(i)->app_size + apps_get(i)->patch_size;
    snprintf(a, sizeof a, "%d", n);
    card_kv(x, y0 + 72, cw, "Installed", a, C_TEXT);
    fmt_size(a, sizeof a, total);
    card_kv(x, y0 + 120, cw, "Using", a, C_TEXT);
    snprintf(a, sizeof a, "%d", npayloads);
    card_kv(x, y0 + 168, cw, "Payloads found", a, C_TEXT);
    card_kv(x, y0 + 216, cw, "Auto payload", cfg.autopayload[0] ? path_base(cfg.autopayload) : "none",
            cfg.autopayload[0] ? C_GREEN : C_DIM);
    const char *u = usb_root();
    card_kv(x, y0 + 264, cw, "USB", u ? u + 5 : "not plugged in", u ? C_TEXT : C_DIM);

    /* --- quick actions (each tile shows the action chosen for it) --- */
    int ty = y0 + ch + 34;
    gfx_text(MX, ty, "Quick actions", C_DIM, 0);
    gfx_hint(MX + 260, ty, "[TRIANGLE] on a tile = choose its action", C_DIM, 0);
    ty += 46;
    int th = 150;
    for (int t = 0; t < HT_COUNT; t++) {
        int tx = MX + (t % HT_COLS) * (cw + CARD_GAP), tyy = ty + (t / HT_COLS) * (th + CARD_GAP);
        int sel = (t == home_sel), act = tile_action(t);
        if (sel) gfx_rect(tx - 4, tyy - 4, cw + 8, th + 8, C_ACCENT);
        gfx_rect(tx, tyy, cw, th, sel ? C_SEL : C_PANEL);
        gfx_text(tx + 24, tyy + 26, qa_title(act), act == QA_NONE ? C_DIM : 0xFFFFFFFF, cw - 48);
        uint32_t sc = C_DIM;
        int icons = 0;   /* only the app's own hints; paths and payload names are drawn as typed */
        switch (act) {
        case QA_WEB:
            if (web_running()) { snprintf(a, sizeof a, "ON - port %d", WEB_PORT); sc = C_GREEN; }
            else { snprintf(a, sizeof a, "OFF - [X] to start"); icons = 1; }
            break;
        case QA_USB:         snprintf(a, sizeof a, "%s", u ? u : "no USB drive"); break;
        case QA_PAYLOADS:    snprintf(a, sizeof a, "%d found", npayloads); break;
        case QA_BIGSCAN:     snprintf(a, sizeof a, "on internal storage"); break;
        case QA_CAPTURES:    snprintf(a, sizeof a, "screenshots + videos to USB"); break;
        case QA_BACKUP_DB:   snprintf(a, sizeof a, "app.db etc. to USB"); break;
        case QA_BACKUP_USER: snprintf(a, sizeof a, "/user/home to USB"); break;
        case QA_FILES:       snprintf(a, sizeof a, "%s", br.path); break;
        case QA_APPS:        snprintf(a, sizeof a, "%d installed", apps_count()); break;
        case QA_TOOLS:       snprintf(a, sizeof a, "backups, scans, USB"); break;
        case QA_SETTINGS:    snprintf(a, sizeof a, "start screen, servers, theme"); break;
        case QA_INTERNAL:
            if (disks[0].ok) { fmt_size(b, sizeof b, disks[0].avail); snprintf(a, sizeof a, "/user - %s free", b); }
            else snprintf(a, sizeof a, "/user");
            break;
        case QA_DATA:        snprintf(a, sizeof a, "homebrew data and settings"); break;
        case QA_AUTOPAYLOAD:
            if (cfg.autopayload[0]) { snprintf(a, sizeof a, "%s", path_base(cfg.autopayload)); sc = C_GREEN; }
            else { snprintf(a, sizeof a, "none set (Payloads, [SQUARE])"); icons = 1; }
            break;
        case QA_ESCAPE:
            snprintf(a, sizeof a, "%s", has_root ? "active - ROOT" : "not active");
            sc = has_root ? C_GREEN : C_MARK;
            break;
        case QA_THEME:       snprintf(a, sizeof a, "now: %s", themes[cfg.theme].name); break;
        case QA_SYSTEM:
            /* live temperatures, coloured by the hotter of the two */
            if (cpu_t >= 0 || soc_t >= 0) {
                snprintf(a, sizeof a, "CPU %d C   SoC %d C", cpu_t, soc_t);
                sc = temp_col(cpu_t > soc_t ? cpu_t : soc_t);
            } else {
                snprintf(a, sizeof a, "temperatures n/a");
            }
            break;
        default:             snprintf(a, sizeof a, "[TRIANGLE] to choose"); icons = 1; break;
        }
        (icons ? gfx_hint : gfx_text)(tx + 24, tyy + 84, a, sc, cw - 48);
    }
    draw_footer("[D-PAD] Choose   [X] Run   [TRIANGLE] Change this tile   [L1/R1] Switch tabs");
}

static void draw_overlays(void)
{
    if (menu.active) {
        int vis = menu.n < MENU_VISIBLE ? menu.n : MENU_VISIBLE;
        int w = 760, h = 80 + vis * ROW_H;
        int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
        gfx_rect_blend(0, 0, SCREEN_W, SCREEN_H, 0xFF000000, 140);
        draw_box(x, y, w, h, C_ACCENT);
        int mw = 0;
        if (menu.n > vis) {
            char more[24];
            snprintf(more, sizeof more, "%d/%d", menu.sel + 1, menu.n);
            mw = gfx_text_width(more);
            gfx_text(x + w - 30 - mw, y + 18, more, C_DIM, 0);
            mw += 20;
        }
        gfx_text(x + 30, y + 18, menu.title, C_ACCENT, w - 60 - mw);   /* long game titles end in ~ */
        for (int r = 0; r < vis; r++) {
            int i = menu.scroll + r;
            int iy = y + 66 + r * ROW_H;
            if (i == menu.sel) gfx_rect(x + 10, iy - 4, w - 20, ROW_H - 2, C_SEL);
            gfx_text(x + 30, iy, menu.items[i], C_TEXT, w - 60);
        }
    }
    if (confirm.active) {
        int w = 1200, h = 200;
        int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
        gfx_rect_blend(0, 0, SCREEN_W, SCREEN_H, 0xFF000000, 140);
        draw_box(x, y, w, h, C_RED);
        gfx_text(x + 40, y + 40, confirm.msg, C_TEXT, w - 80);
        gfx_hint(x + 40, y + 120, "[X] Yes      [O] No", C_DIM, 0);
    }
    if (info.active) {
        int w = 1400, h = 120 + info.n * ROW_H;
        int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
        gfx_rect_blend(0, 0, SCREEN_W, SCREEN_H, 0xFF000000, 140);
        draw_box(x, y, w, h, C_ACCENT);
        for (int i = 0; i < info.n; i++) gfx_text(x + 40, y + 30 + i * ROW_H, info.lines[i], C_TEXT, w - 80);
        gfx_hint(x + 40, y + h - 60, "[X] Close", C_DIM, 0);
    }
    if (g_job.running && !g_job.quiet) {
        static const char *verbs[] = { "Copying", "Moving", "Deleting", "Extracting",
                                       "Calculating size", "Searching", "Syncing", "Compressing",
                                       "Checksumming", "Finding largest files" };
        int w = 1300, h = 280;
        int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
        gfx_rect_blend(0, 0, SCREEN_W, SCREEN_H, 0xFF000000, 160);
        draw_box(x, y, w, h, C_ACCENT);
        char buf[200], a[32], b[32];
        int tot = g_job.files_total;
        if (g_job.type == JOB_SIZE || g_job.type == JOB_SEARCH || g_job.type == JOB_BIGFILES) {
            /* no known total: show running counts instead of a bar */
            if (g_job.type == JOB_BIGFILES) {
                fmt_size(a, sizeof a, g_job.bytes_total);
                snprintf(buf, sizeof buf, "%s...  %d files, %s%s", verbs[g_job.type], g_job.files_done, a,
                         g_job.cancel ? "  (cancelling)" : "");
            } else if (g_job.type == JOB_SIZE) {
                fmt_size(a, sizeof a, g_job.bytes_total);
                snprintf(buf, sizeof buf, "%s...  %d item(s), %s%s", verbs[g_job.type], tot, a,
                         g_job.cancel ? "  (cancelling)" : "");
            } else {
                snprintf(buf, sizeof buf, "Searching for \"%s\"...  %d checked, %d found%s", sr_query,
                         g_job.files_done, tot, g_job.cancel ? "  (cancelling)" : "");
            }
            gfx_text(x + 40, y + 30, buf, C_TEXT, w - 80);
            gfx_text(x + 40, y + 80, g_job.current, C_DIM, w - 80);
            int pulse = (frame * 12) % (w - 280);
            gfx_rect(x + 40, y + 140, w - 80, 30, C_LINE);
            gfx_rect(x + 40 + pulse, y + 140, 200, 30, C_ACCENT);
            gfx_hint(x + w - 40 - gfx_hint_width("[O] Cancel"), y + 190, "[O] Cancel", C_DIM, 0);
            goto job_done;
        }
        snprintf(buf, sizeof buf, "%s...  %d / %d item(s)%s", verbs[g_job.type], g_job.files_done,
                 tot, g_job.cancel ? "  (cancelling)" : "");
        gfx_text(x + 40, y + 30, buf, C_TEXT, 0);
        gfx_text(x + 40, y + 80, g_job.current, C_DIM, w - 80);
        double frac;
        if (g_job.type == JOB_DELETE || !g_job.bytes_total)
            frac = tot ? (double)g_job.files_done / tot : 0;
        else
            frac = (double)g_job.bytes_done / g_job.bytes_total;
        if (frac > 1) frac = 1;
        gfx_rect(x + 40, y + 140, w - 80, 30, C_LINE);
        gfx_rect(x + 40, y + 140, (int)((w - 80) * frac), 30, C_ACCENT);
        if (g_job.type != JOB_DELETE) {
            fmt_size(a, sizeof a, g_job.bytes_done);
            fmt_size(b, sizeof b, g_job.bytes_total);
            int k = snprintf(buf, sizeof buf, "%s / %s", a, b);
            /* speed and time left, once there is enough data for a stable estimate */
            double secs = (double)(sceKernelGetProcessTime() - g_job.start_us) / 1e6;
            if (secs > 1.0 && g_job.bytes_done > 0) {
                double bps = g_job.bytes_done / secs;
                char sp[32];
                fmt_size(sp, sizeof sp, (uint64_t)bps);
                k += snprintf(buf + k, sizeof buf - k, "   %s/s", sp);
                if (g_job.bytes_total > g_job.bytes_done) {
                    long left = (long)((g_job.bytes_total - g_job.bytes_done) / bps);
                    if (left >= 3600) snprintf(buf + k, sizeof buf - k, "   %ld:%02ld:%02ld left", left / 3600, left / 60 % 60, left % 60);
                    else snprintf(buf + k, sizeof buf - k, "   %ld:%02ld left", left / 60, left % 60);
                }
            }
            gfx_text(x + 40, y + 190, buf, C_DIM, 0);
        }
        gfx_hint(x + w - 40 - gfx_hint_width("[O] Cancel"), y + 190, "[O] Cancel", C_DIM, 0);
    }
job_done:
    if (toast_t > 0) {
        int w = (toast_icons ? gfx_hint_width : gfx_text_width)(toast_msg) + 60;
        if (w > SCREEN_W - 2 * MX) w = SCREEN_W - 2 * MX;
        int x = (SCREEN_W - w) / 2, y = FOOTER_Y - 80;
        int a = toast_t > 30 ? 235 : toast_t * 235 / 30;
        gfx_rect_blend(x, y, w, 56, 0xFF000000, a);
        gfx_rect_blend(x, y, 6, 56, toast_col, a);
        if (toast_t > 10) (toast_icons ? gfx_hint : gfx_text)(x + 30, y + 11, toast_msg, toast_col, w - 50);
        toast_t--;
    }
}

static void draw(void)
{
    if (imgview_active()) {
        char pos[32];
        image_position(pos, sizeof pos);
        imgview_draw(pos);
        if (toast_t > 0) draw_overlays();   /* e.g. "can't decode next image" */
        return;
    }
    gfx_clear(C_BG);
    draw_header();
    switch (tab) {
    case TAB_HOME:     draw_home(); break;
    case TAB_FILES:    draw_files(); break;
    case TAB_APPS:     draw_apps(); break;
    case TAB_NET:      draw_net(); break;
    case TAB_PAYLOADS: draw_payloads(); break;
    case TAB_SYSTEM:   draw_system(); break;
    case TAB_TOOLS:    draw_list(tools_items, NITEMS(tools_items), PAGE_TOOLS, "Tools"); break;
    case TAB_SETTINGS: draw_list(settings_items, NITEMS(settings_items), PAGE_SETTINGS, "Settings - saved automatically"); break;
    }
    if (editor_active()) {
        editor_draw();
        if (!editor_confirming())
            draw_footer("[X] Edit line  [TRIANGLE] New line  [SQUARE] Delete line  [OPTIONS] Save  [L2/R2] Top/End  [O] Close");
    } else if (viewer_active()) {
        viewer_draw();
        draw_footer("[UP/DOWN] Scroll  [LEFT/RIGHT] Page  [L2/R2] Top/End  [SQUARE] Text/Hex  [O] Close");
    }
    draw_overlays();
    osk_draw();
}

/* Shows the result of the job that just ended. g_job.finished stays set while this reads
 * g_job, so no new job (e.g. a web download) can start and overwrite it meanwhile. */
static void job_collect(void)
{
    /* web download zip: nothing to show on the TV, and no reload - reloading would clear
       the user's marked files (e.g. while a delete confirmation is open) */
    if (g_job.quiet) return;
    if (g_job.type == JOB_BIGFILES) {
        sr_sel = sr_scroll = 0;
        sr_active = sr_big = 1;
        char a[32];
        fmt_size(a, sizeof a, g_job.bytes_total);
        toast(C_GREEN, "%d files (%s) checked - showing the %d largest", g_job.files_done, a, search_count);
        return;
    }
    if (g_job.type == JOB_SEARCH) {
        sr_sel = sr_scroll = 0;
        sr_active = 1;
        sr_big = 0;
        if (g_job.cancel) toast(C_MARK, "Search stopped: %d result(s) so far", search_count);
        else toast(search_count ? C_GREEN : C_MARK, "%d result(s) for \"%s\"", search_count, sr_query);
        return;
    }
    if (g_job.type == JOB_HASH) {
        if (g_job.cancel) { toast(C_MARK, "Cancelled"); return; }
        if (g_job.errors) { toast(C_RED, "%s", g_job.err); return; }
        char a[32];
        fmt_size(a, sizeof a, g_job.bytes_done);
        memset(&info, 0, sizeof info);
        info.active = 1;
        snprintf(info.lines[info.n++], 200, "%s  (%s)", path_base(g_job.current), a);
        snprintf(info.lines[info.n++], 200, "CRC32:   %s", g_job.hash_crc);
        snprintf(info.lines[info.n++], 200, "SHA-256: %.32s", g_job.hash_sha);
        snprintf(info.lines[info.n++], 200, "         %s", g_job.hash_sha + 32);
        dlog("checksum %s crc32=%s sha256=%s", g_job.current, g_job.hash_crc, g_job.hash_sha);
        return;
    }
    if (g_job.type == JOB_SIZE) {
        if (g_job.cancel) { toast(C_MARK, "Cancelled"); return; }
        char a[32];
        fmt_size(a, sizeof a, g_job.bytes_total);
        memset(&info, 0, sizeof info);
        info.active = 1;
        if (browser_marked_count(&br)) snprintf(info.lines[info.n++], 200, "Marked items");
        else if (br.count) snprintf(info.lines[info.n++], 200, "%s", br.e[br.sel].name);
        snprintf(info.lines[info.n++], 200, "Total size: %s (%llu bytes)", a, (unsigned long long)g_job.bytes_total);
        snprintf(info.lines[info.n++], 200, "Contains:   %d item(s)", g_job.files_total);
        return;
    }
    browser_reload(&br);
    npayloads = payload_scan(payloads, PAYLOAD_MAX);
    if (g_job.cancel) toast(C_MARK, "Cancelled");
    else if (g_job.errors) toast(C_RED, "%d error(s). Last: %s", g_job.errors, g_job.err);
    else if (g_job.type == JOB_ZIP) toast(C_GREEN, "Zip created: %d item(s)", g_job.files_done);
    else if (g_job.type == JOB_SYNC) toast(C_GREEN, "Captures synced to USB:/OrbisKit/captures");
    else toast(C_GREEN, "Done: %d item(s)", g_job.files_done);
}

static void poll_status(void)
{
    if (tab == TAB_APPS && ap_scanned) apps_load_icon_step();
    if (frame % 60 == 0) {
        cpu_t = sysinfo_cpu_temp();
        soc_t = sysinfo_soc_temp();
        if (frame % (60 * TEMP_STEP_S) == 0) sysinfo_temp_record(cpu_t, soc_t);
    }
    if (frame % 300 == 0) sysinfo_ip(ip, sizeof ip);

    /* storage: refresh every ~3 s, and right away when the Files folder changes */
    static char last_path[1024];
    int changed = strcmp(last_path, br.path) != 0;
    if (frame % 180 == 0 || changed) {
        unsigned long long tot, av;
        char a[32], b[32];
        if (frame % 180 == 0) {
            for (int i = 0; i < DISK_COUNT; i++) {
                disks[i].ok = (i == 0 || is_dir(disks[i].path)) && disk_space(disks[i].path, &tot, &av) == 0;
                if (disks[i].ok) {
                    disks[i].total = tot;
                    disks[i].avail = av;
                    fmt_size(a, sizeof a, av);
                    fmt_size(b, sizeof b, tot);
                    snprintf(disks[i].text, sizeof disks[i].text, "%s free of %s", a, b);
                }
            }
        }
        if (disk_space(br.path, &tot, &av) == 0) {
            fmt_size(a, sizeof a, av);
            snprintf(free_here, sizeof free_here, "Free here: %s", a);
        } else {
            free_here[0] = 0;
        }
        snprintf(last_path, sizeof last_path, "%s", br.path);
    }

    /* remember tab and folder for next launch */
    if (frame % 120 == 60 && (cfg.last_tab != tab || strcmp(cfg.last_path, br.path))) {
        cfg.last_tab = tab;
        snprintf(cfg.last_path, sizeof cfg.last_path, "%s", br.path);
        config_save();
    }

    if (g_job.finished) {
        job_collect();
        g_job.finished = 0;   /* only now may a new job reset g_job */
    }
}

int main(void)
{
    dlog_init();
    dlog("OrbisKit " APP_VERSION " starting");
    sceSystemServiceHideSplashScreen();
    int r = gfx_init();
    dlog("gfx_init: %d (0x%08X)", r, r);
    if (r != 0) {
        notify("OrbisKit: video init failed (0x%08X)\nSee " DLOG_FILE, r);
        for (;;) sceKernelUsleep(1000000);
    }
    r = pad_init();
    dlog("pad_init: handle %d", r);
    r = sceNetInit();
    dlog("sceNetInit: 0x%08X", r);

    r = jb_escape();
    has_root = jb_has_root();
    dlog("sandbox escape: %s (GoldHEN SDK build: %s), root access: %s",
         r ? "ok" : "not done", jb_built_with_goldhen() ? "yes" : "no", has_root ? "yes" : "no");
    sysinfo_firmware(fw, sizeof fw);
    sysinfo_ip(ip, sizeof ip);
    dlog("firmware: %s, ip: %s", fw, ip);
    config_load();
    browser_sort = cfg.sort_mode;
    if (cfg.theme < 0 || cfg.theme >= THEME_COUNT) cfg.theme = 0;
    if (!(cfg.last_path[0] && browser_load(&br, cfg.last_path) == 0))
        if (browser_load(&br, has_root ? "/" : "/data") != 0) browser_load(&br, "/");
    tab = (cfg.start_last && cfg.last_tab >= 0 && cfg.last_tab < TAB_COUNT) ? cfg.last_tab : TAB_HOME;
    apps_scan();          /* the Home dashboard shows the installed-app summary */
    ap_scanned = 1;
    dlog("start folder: %s (%d entries, error %d)", br.path, br.count, br.error);
    npayloads = payload_scan(payloads, PAYLOAD_MAX);
    dlog("payloads found: %d", npayloads);

    if (cfg.autopayload[0]) {
        char err[160];
        if (payload_send(cfg.autopayload, err, sizeof err) == 0) {
            notify("Auto-sent payload\n%s", path_base(cfg.autopayload));
            dlog("auto payload sent: %s", cfg.autopayload);
        } else {
            notify("Auto payload failed\n%s", err);
            dlog("auto payload failed: %s: %s", cfg.autopayload, err);
        }
    }
    if (cfg.web_autostart) {
        r = web_start();
        dlog("web autostart: %d", r);
        if (r == 0) notify("Web file manager running\nhttp://%s:%d", ip, WEB_PORT);
    }

    dlog("startup complete");
    notify("OrbisKit " APP_VERSION " loaded\n%s", has_root ? "Full filesystem access" : "Sandboxed mode");
    if (!has_root) toast(C_MARK, "Running sandboxed: some folders and tools need a GoldHEN SDK build");

    for (;;) {
        pad_update();
        handle_input();
        poll_status();
        draw();
        gfx_present();
        frame++;
    }
    return 0;
}
