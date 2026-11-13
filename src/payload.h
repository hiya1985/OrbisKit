#pragma once

#define PAYLOAD_MAX  64
#define BINLOADER_PORT 9090

/* Finds .bin files in USB roots, their payloads folders and /data/payloads. */
int payload_scan(char paths[][512], int max);
/* Sends a payload to the local BinLoader (GoldHEN). 0 on success. */
int payload_send(const char *path, char *err, int errn);
/* Same, from memory (used by the web page's "Send payload"). */
int payload_send_buf(const void *buf, long size, char *err, int errn);
