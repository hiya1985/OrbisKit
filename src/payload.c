#include "payload.h"
#include "compat.h"
#include "fsops.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *dirs[] = {
    "/mnt/usb0", "/mnt/usb0/payloads", "/mnt/usb1", "/mnt/usb1/payloads",
    "/data/payloads", "/data/GoldHEN/payloads",
};

int payload_scan(char paths[][512], int max)
{
    int n = 0;
    for (unsigned d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) && n < max) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcasecmp(e->d_name + l - 4, ".bin")) continue;
            path_join(paths[n], 512, dirs[d], e->d_name);
            if (!is_dir(paths[n])) n++;
        }
        closedir(dir);
    }
    return n;
}

int payload_send(const char *path, char *err, int errn)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "Can't open file"); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        fclose(f);
        snprintf(err, errn, "Bad payload size");
        return -1;
    }
    char *buf = malloc(size);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        snprintf(err, errn, "Read failed");
        return -1;
    }
    fclose(f);
    int r = payload_send_buf(buf, size, err, errn);
    free(buf);
    return r;
}

/* Sends a payload to a local loader on 127.0.0.1:<port>. */
static int send_to_port(const void *data, long size, int port, char *err, int errn)
{
    const char *buf = data;
    if (size <= 0 || size > 64 * 1024 * 1024) { snprintf(err, errn, "Bad payload size"); return -1; }
    int s = sceNetSocket("tbox_payload", NET_AF_INET, NET_SOCK_STREAM, 0);
    NetSockaddrIn a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = NET_AF_INET;
    a.sin_port = sceNetHtons((uint16_t)port);
    a.sin_addr = sceNetHtonl(0x7F000001);   /* 127.0.0.1 */
    if (s < 0 || sceNetConnect(s, &a, sizeof a) < 0) {
        if (s >= 0) sceNetSocketClose(s);
        snprintf(err, errn, "Loader not reachable on port %d.", port);
        return -1;
    }
    /* a loader that accepts but stops reading would block the screen forever: give up after 10 s
       (FreeBSD takes a struct timeval; an int in microseconds is only the fallback) */
    struct { long sec, usec; } tv = { 10, 0 };
    int tmo = 10 * 1000000;
    if (sceNetSetsockopt(s, NET_SOL_SOCKET, 0x1005 /* SO_SNDTIMEO */, &tv, sizeof tv) < 0)
        sceNetSetsockopt(s, NET_SOL_SOCKET, 0x1005, &tmo, sizeof tmo);
    long off = 0;
    while (off < size) {
        int w = sceNetSend(s, buf + off, size - off, 0);
        if (w <= 0) break;
        off += w;
    }
    sceNetSocketClose(s);
    if (off != size) { snprintf(err, errn, "Send interrupted"); return -1; }
    return 0;
}

int payload_send_buf(const void *data, long size, char *err, int errn)
{
    int r = send_to_port(data, size, BINLOADER_PORT, err, errn);
    /* keep "Send interrupted" / "Bad payload size": only a failed connect means BinLoader is off */
    if (r != 0 && !strncmp(err, "Loader not reachable", 20))
        snprintf(err, errn, "BinLoader not reachable on port %d. Enable it in GoldHEN settings.", BINLOADER_PORT);
    return r;
}
