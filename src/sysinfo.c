#include "sysinfo.h"
#include "compat.h"
#include "fsops.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>

typedef struct {
    size_t   size;
    char     version[0x1C];
    uint32_t hex;
} SwVersion;

void sysinfo_firmware(char *out, int n)
{
    SwVersion v;
    memset(&v, 0, sizeof v);
    v.size = sizeof v;
    if (sceKernelGetSystemSwVersion(&v) == 0 && v.version[0]) {
        /* "09.000.001" -> "9.00" */
        unsigned maj = (v.hex >> 24) & 0xFF, min = (v.hex >> 16) & 0xFF;
        snprintf(out, n, "%x.%02x  (%.*s)", maj, min, (int)sizeof v.version, v.version);
    } else {
        snprintf(out, n, "unknown");
    }
}

int sysinfo_cpu_temp(void)
{
    int t = 0;
    return sceKernelGetCpuTemperature(&t) == 0 ? t : -1;
}

int sysinfo_soc_temp(void)
{
    int t = 0;
    return sceKernelGetSocSensorTemperature(0, &t) == 0 ? t : -1;
}

static pthread_mutex_t th_mtx = PTHREAD_MUTEX_INITIALIZER;
static int th_cpu[TEMP_HIST], th_soc[TEMP_HIST], th_head, th_count, th_peak_cpu = -1, th_peak_soc = -1;

void sysinfo_temp_record(int cpu, int soc)
{
    pthread_mutex_lock(&th_mtx);
    th_cpu[th_head] = cpu;
    th_soc[th_head] = soc;
    th_head = (th_head + 1) % TEMP_HIST;
    if (th_count < TEMP_HIST) th_count++;
    if (cpu > th_peak_cpu) th_peak_cpu = cpu;
    if (soc > th_peak_soc) th_peak_soc = soc;
    pthread_mutex_unlock(&th_mtx);
}

int sysinfo_temp_history(int *cpu, int *soc, int max, int *peak_cpu, int *peak_soc)
{
    pthread_mutex_lock(&th_mtx);
    int n = th_count < max ? th_count : max;
    for (int i = 0; i < n; i++) {
        int idx = (th_head - n + i + TEMP_HIST) % TEMP_HIST;
        cpu[i] = th_cpu[idx];
        soc[i] = th_soc[idx];
    }
    if (peak_cpu) *peak_cpu = th_peak_cpu;
    if (peak_soc) *peak_soc = th_peak_soc;
    pthread_mutex_unlock(&th_mtx);
    return n;
}

void sysinfo_ip(char *out, int n)
{
    static int inited;
    if (!inited) { sceNetCtlInit(); inited = 1; }
    char info[256];
    memset(info, 0, sizeof info);
    if (sceNetCtlGetInfo(NETCTL_INFO_IP_ADDRESS, info) == 0 && info[0])
        snprintf(out, n, "%.16s", info);
    else
        snprintf(out, n, "0.0.0.0");
}

/* The toolchain's libc statfs() uses Linux syscall numbers and always fails on the PS4.
 * libkernel exports the real FreeBSD call as _fstatfs (struct layout = <bits/statfs.h>). */
int _fstatfs(int fd, struct statfs *buf);

int disk_space(const char *path, unsigned long long *total, unsigned long long *avail)
{
    struct statfs st;
    memset(&st, 0, sizeof st);
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int r = _fstatfs(fd, &st);
    close(fd);
    if (r != 0 || !st.f_bsize || !st.f_blocks) return -1;
    /* /mnt/usb0..7 exist as empty folders even with nothing plugged in; statfs then reports
       the small filesystem they sit on. Only count a USB slot when a drive is mounted on it.
       Checked by device number (not f_mntonname: the toolchain's struct statfs has a 4-byte
       f_fsid where the PS4 has 8, so every field after it is read 4 bytes off). */
    if (!strncmp(path, "/mnt/usb", 8) && strchr(path + 8, '/') == NULL && !is_mount_point(path))
        return -1;
    *total = (unsigned long long)st.f_blocks * st.f_bsize;
    *avail = st.f_bavail > 0 ? (unsigned long long)st.f_bavail * st.f_bsize : 0;
    return 0;
}
