#pragma once
#include <stdint.h>
#include "kstat.h"   /* use struct kstat / k_stat(): the toolchain's struct stat layout is wrong */

typedef enum {
    JOB_COPY, JOB_MOVE, JOB_DELETE, JOB_EXTRACT,
    JOB_SIZE,     /* measure: result in bytes_total / files_total */
    JOB_SEARCH,   /* src[0] = root folder, dst = query; results in search_results */
    JOB_SYNC,     /* like copy into dst, merging folders and skipping files already there */
    JOB_ZIP,      /* dst = output .zip path */
    JOB_HASH,     /* src[0] = file; CRC32 / SHA-256 hex in hash_crc / hash_sha */
    JOB_BIGFILES, /* src[0] = root; the largest files go in search_results / search_sizes */
} JobType;

#define SEARCH_MAX 1000
#define BIGFILES_MAX 200
extern char *search_results[SEARCH_MAX];
extern uint64_t search_sizes[SEARCH_MAX];   /* filled by JOB_BIGFILES */
extern int search_count;

typedef struct {
    volatile int running;       /* worker thread active */
    volatile int finished;      /* set when done; UI clears it after reading */
    volatile int cancel;
    volatile int quiet;         /* started by the web server: no toast on the TV */
    uint64_t start_us;          /* for speed / time-left display */
    JobType type;
    volatile uint64_t bytes_done, bytes_total;
    volatile int files_done, files_total;
    char current[512];
    int  errors;
    char err[256];
    char hash_crc[12], hash_sha[68];
} Job;

extern Job g_job;

/* Starts a background job. src paths are copied. For JOB_DELETE dst is ignored. */
int  job_start(JobType type, char **src, int nsrc, const char *dst);
/* Same, but "quiet": started by the web server, so the TV shows no result toast. */
int  job_start_quiet(JobType type, char **src, int nsrc, const char *dst);

/* Starts a thread with an explicit stack size (the default is small and the folder-walking
 * code is recursive). detach: don't keep the thread joinable. 0 on success. */
#define STACK_JOB (2 * 1024 * 1024)
#define STACK_NET (512 * 1024)
int thread_start(void *(*fn)(void *), void *arg, size_t stack, int detach);

/* Helpers (synchronous) */
/* 0 on success, -1 if the result didn't fit (out then holds a truncated, wrong path). */
int  path_join(char *out, int n, const char *dir, const char *name);
const char *path_base(const char *p);
int  mkdirs(const char *path);
int  is_dir(const char *path);
int  is_mount_point(const char *path);   /* a drive mounted here (e.g. /mnt/usb0) */
void fmt_size(char *out, int n, uint64_t bytes);
