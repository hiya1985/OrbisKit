#include "notify.h"
#include "compat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
} NotifyRequest;

void notify(const char *fmt, ...)
{
    NotifyRequest req;
    memset(&req, 0, sizeof req);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    req.useIconImageUri = 1;
    strcpy(req.iconUri, "cxml://psnotification/tex_icon_system");
    req.targetId = -1;
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
