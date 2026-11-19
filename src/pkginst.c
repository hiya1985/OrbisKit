/* Package installer through BGFT (the PS4's background download/install service).
 * The same calls flatz's Remote Package Installer and PKGi use:
 *   load internal sysmodules BGFT (0x8000002A) and AppInstUtil (0x80000014),
 *   sceAppInstUtilInitialize + sceBgftServiceIntInit,
 *   sceBgftServiceIntDownloadRegisterTaskByStorageEx with a local path as contentUrl,
 *   sceBgftServiceDownloadStartTask.
 * Package type for BGFT from the pkg content type: 0x1A PS4GD (game/app), 0x1B PS4AC (DLC),
 * 0x1C PS4AL (DLC licence), 0x1E PS4DP (update). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "compat.h"
#include "dlog.h"
#include "jb.h"
#include "kstat.h"
#include "leftovers.h"
#include "pkginst.h"

int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceAppInstUtilInitialize(void);

typedef struct {
    void  *heap;
    size_t heapSize;
} BgftInit;

typedef struct {
    int32_t     userId;
    int32_t     entitlementType;
    const char *id;
    const char *contentUrl;
    const char *contentExUrl;
    const char *contentName;
    const char *iconPath;
    const char *skuId;
    int32_t     option;
    const char *playgoScenarioId;
    const char *releaseDate;
    const char *packageType;
    const char *packageSubType;
    uint64_t    packageSize;
} BgftParam;

typedef struct {
    BgftParam params;
    uint32_t  slot;
} BgftParamEx;

int sceBgftServiceIntInit(BgftInit *p);
int sceBgftServiceIntDownloadRegisterTaskByStorageEx(BgftParamEx *p, int *taskId);
int sceBgftServiceDownloadStartTask(int taskId);
int sceBgftServiceDownloadGetProgress(int taskId, void *progress);

#define SYSMODULE_INTERNAL_BGFT          0x8000002A
#define SYSMODULE_INTERNAL_APPINSTUTIL   0x80000014
#define BGFT_OPT_DISABLE_CDN_QUERY_PARAM 0x10000
#define BGFT_ERR_ALREADY_REGISTERED      0x80990088
#define BGFT_HEAP_SIZE                   (1024 * 1024)

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int inited;          /* 1 ok, -1 failed (msg in init_err) */
static char init_err[96];
static PkgTask tasks[PKGINST_MAX];
static int ntasks;

int pkginst_is_pkg_name(const char *name)
{
    size_t l = strlen(name);
    return l > 4 && !strcasecmp(name + l - 4, ".pkg");
}

static int init_locked(void)
{
    if (inited) return inited;
    int r;
    if ((r = sceSysmoduleLoadModuleInternal(SYSMODULE_INTERNAL_BGFT)) < 0) {
        snprintf(init_err, sizeof init_err, "Can't load the installer module (0x%08X)", (unsigned)r);
    } else if ((r = sceSysmoduleLoadModuleInternal(SYSMODULE_INTERNAL_APPINSTUTIL)) < 0) {
        snprintf(init_err, sizeof init_err, "Can't load AppInstUtil (0x%08X)", (unsigned)r);
    } else if ((r = sceAppInstUtilInitialize()) < 0) {
        snprintf(init_err, sizeof init_err, "AppInstUtil init failed (0x%08X)", (unsigned)r);
    } else {
        static BgftInit ip;
        ip.heap = malloc(BGFT_HEAP_SIZE);
        ip.heapSize = BGFT_HEAP_SIZE;
        if (!ip.heap) snprintf(init_err, sizeof init_err, "Out of memory");
        else if ((r = sceBgftServiceIntInit(&ip)) < 0)
            snprintf(init_err, sizeof init_err, "Installer service init failed (0x%08X)", (unsigned)r);
        else { inited = 1; dlog("pkginst: BGFT ready"); return 1; }
    }
    dlog("pkginst: %s", init_err);
    inited = -1;
    return -1;
}

static const char *bgft_type(int t)
{
    switch (t) {
    case 0x1A: return "PS4GD";
    case 0x1B: return "PS4AC";
    case 0x1C: return "PS4AL";
    case 0x1E: return "PS4DP";
    }
    return NULL;
}

int pkginst_install(const char *path, char *msg, int msgn)
{
    char cid[40], ver[8], title[128];
    int type;
    struct kstat st;
    if (k_stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(msg, msgn, "Can't open the file"); return -1; }
    if (pkg_header(path, cid, &type, ver, title, sizeof title) != 0 || strlen(cid) < 36) {
        snprintf(msg, msgn, "Not a PS4 package (bad header)");
        return -1;
    }
    const char *ptype = bgft_type(type);
    if (!ptype) { snprintf(msg, msgn, "Unsupported package type 0x%X", type); return -1; }
    if (!jb_has_root()) { snprintf(msg, msgn, "Installing needs the sandbox escape (GoldHEN)"); return -1; }
    if (!title[0]) {
        const char *b = strrchr(path, '/');
        snprintf(title, sizeof title, "%s", b ? b + 1 : path);
    }

    pthread_mutex_lock(&mtx);
    if (init_locked() < 0) {
        snprintf(msg, msgn, "%s", init_err);
        pthread_mutex_unlock(&mtx);
        return -1;
    }
    int uid = -1;
    sceUserServiceGetInitialUser(&uid);

    BgftParamEx p;
    memset(&p, 0, sizeof p);
    p.params.userId = uid;
    p.params.entitlementType = 5;
    p.params.id = cid;
    p.params.contentUrl = path;
    p.params.contentName = title;
    p.params.iconPath = "";
    p.params.option = BGFT_OPT_DISABLE_CDN_QUERY_PARAM;
    p.params.playgoScenarioId = "0";
    p.params.releaseDate = "";
    p.params.packageType = ptype;
    p.params.packageSubType = "";
    p.params.packageSize = (uint64_t)st.st_size;
    p.slot = 0;

    int task = -1;
    int r = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&p, &task);
    dlog("pkginst: register %s (%s %s v%s) -> 0x%08X task %d", path, ptype, cid, ver, (unsigned)r, task);
    if ((unsigned)r == BGFT_ERR_ALREADY_REGISTERED) {
        snprintf(msg, msgn, "%s is already installed or queued", title);
        pthread_mutex_unlock(&mtx);
        return -1;
    }
    if (r < 0 || task < 0) {
        snprintf(msg, msgn, "The installer refused the package (0x%08X)", (unsigned)r);
        pthread_mutex_unlock(&mtx);
        return -1;
    }
    r = sceBgftServiceDownloadStartTask(task);
    dlog("pkginst: start task %d -> 0x%08X", task, (unsigned)r);
    if (r < 0) {
        snprintf(msg, msgn, "Couldn't start the install (0x%08X)", (unsigned)r);
        pthread_mutex_unlock(&mtx);
        return -1;
    }
    if (ntasks == PKGINST_MAX) { memmove(tasks, tasks + 1, sizeof tasks[0] * (PKGINST_MAX - 1)); ntasks--; }
    PkgTask *t = &tasks[ntasks++];
    memset(t, 0, sizeof *t);
    snprintf(t->title, sizeof t->title, "%s", title);
    t->task = task;
    t->total = (unsigned long long)st.st_size;
    pthread_mutex_unlock(&mtx);

    snprintf(msg, msgn, "Installing %s%s%s - progress is in Notifications > Downloads",
             title, ver[0] ? " v" : "", ver);
    return 0;
}

/* Progress layout from flatz's reversing (64-bit lengths); read into a padded buffer so a
 * firmware with a bigger struct can't overflow it. */
static void refresh(PkgTask *t)
{
    union { unsigned char b[256]; uint64_t align; } u;
    memset(&u, 0, sizeof u);
    int r = sceBgftServiceDownloadGetProgress(t->task, u.b);
    if (r < 0) { t->finished = 1; t->error = r; return; }
    int32_t err;
    uint64_t len, done;
    memcpy(&err, u.b + 4, 4);
    memcpy(&len, u.b + 8, 8);
    memcpy(&done, u.b + 16, 8);
    if (len && done <= len) { t->total = len; t->done = done; }
    if (err) { t->error = err; t->finished = 1; }
    else if (len && done >= len) t->finished = 1;
}

int pkginst_tasks(PkgTask *out, int max)
{
    pthread_mutex_lock(&mtx);
    int n = 0;
    for (int i = 0; i < ntasks && n < max; i++) {
        if (!tasks[i].finished) refresh(&tasks[i]);
        out[n++] = tasks[i];
    }
    pthread_mutex_unlock(&mtx);
    return n;
}
