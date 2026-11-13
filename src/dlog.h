#pragma once
/* Debug log: /data/OrbisKit/log.txt (previous run kept as log_prev.txt). */

#define DLOG_FILE "/data/OrbisKit/log.txt"

void dlog_init(void);
void dlog(const char *fmt, ...);
