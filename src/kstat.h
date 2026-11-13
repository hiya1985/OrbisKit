/* struct stat as the PS4 (FreeBSD 9) kernel actually fills it.
 *
 * The OpenOrbis toolchain's <bits/stat.h> uses a 4-byte mode_t, but the kernel's is
 * 2 bytes, so every field after st_mode is read from the wrong offset (st_size comes
 * back as the block count and timestamps are garbage). Use these wrappers instead of
 * stat()/lstat()/fstat() with struct stat. Verified against a PS4 on 11.00. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>   /* S_IS* macros and the stat()/lstat()/fstat() prototypes */

struct kts { int64_t tv_sec; int64_t tv_nsec; };

struct kstat {
    uint32_t   st_dev;
    uint32_t   st_ino;
    uint16_t   st_mode;
    uint16_t   st_nlink;
    uint32_t   st_uid;
    uint32_t   st_gid;
    uint32_t   st_rdev;
    struct kts st_atim;
    struct kts st_mtim;        /* st_mtime macro from <sys/stat.h> maps to st_mtim.tv_sec */
    struct kts st_ctim;
    int64_t    st_size;
    int64_t    st_blocks;
    uint32_t   st_blksize;
    uint32_t   st_flags;
    uint32_t   st_gen;
    int32_t    st_lspare;
    struct kts st_birthtim;
    uint8_t    pad[16];        /* room for anything the libc wrapper writes past the kernel struct */
};

_Static_assert(offsetof(struct kstat, st_mode) == 8, "kstat layout");
_Static_assert(offsetof(struct kstat, st_mtim) == 40, "kstat layout");
_Static_assert(offsetof(struct kstat, st_size) == 72, "kstat layout");
_Static_assert(sizeof(struct kstat) >= sizeof(struct stat), "kstat must be at least as big as struct stat");

static inline int k_stat(const char *p, struct kstat *s) { return stat(p, (struct stat *)(void *)s); }
static inline int k_lstat(const char *p, struct kstat *s) { return lstat(p, (struct stat *)(void *)s); }
static inline int k_fstat(int fd, struct kstat *s) { return fstat(fd, (struct stat *)(void *)s); }
