#pragma once

void sysinfo_firmware(char *out, int n);
int  sysinfo_cpu_temp(void);       /* deg C, -1 if unavailable */
int  sysinfo_soc_temp(void);       /* deg C, -1 if unavailable */
void sysinfo_ip(char *out, int n);

/* Temperature history for the dashboards: the UI records one sample every TEMP_STEP_S seconds,
   the last TEMP_HIST samples are kept (10 minutes). Thread-safe (the web server reads it). */
#define TEMP_HIST   60
#define TEMP_STEP_S 10
void sysinfo_temp_record(int cpu, int soc);
/* Copies the history oldest-first into cpu[]/soc[] (-1 = no reading); returns the count.
   peak_cpu/peak_soc (may be NULL) get the highest readings since the app started. */
int  sysinfo_temp_history(int *cpu, int *soc, int max, int *peak_cpu, int *peak_soc);
/* Total and free bytes of the filesystem containing path. 0 on success. */
int  disk_space(const char *path, unsigned long long *total, unsigned long long *avail);
