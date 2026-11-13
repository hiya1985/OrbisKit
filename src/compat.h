/* Minimal declarations for the PS4 system libraries we import.
 * Declared here (instead of the OpenOrbis orbis headers) so the code
 * doesn't depend on exact header struct layouts. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- libkernel ---- */
typedef void *SceKernelEqueue;
int    sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t len,
                                     size_t alignment, int memoryType, int64_t *physAddrOut);
int    sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
                                int64_t directMemoryStart, size_t alignment);
size_t sceKernelGetDirectMemorySize(void);
int    sceKernelCreateEqueue(SceKernelEqueue *eq, const char *name);
int    sceKernelWaitEqueue(SceKernelEqueue eq, void *ev, int num, int *out, void *timeout);
int    sceKernelUsleep(unsigned int usec);
int    sceKernelGetCpuTemperature(int *temp);
int    sceKernelGetSocSensorTemperature(int sensor, int *temp);
int    sceKernelGetSystemSwVersion(void *ver);
int    sceKernelSendNotificationRequest(int device, void *req, size_t size, int blocking);
uint64_t sceKernelGetProcessTime(void);
int    ioctl(int fd, unsigned long request, ...);

/* ---- VideoOut ---- */
int sceVideoOutOpen(int userId, int busType, int index, const void *param);
int sceVideoOutSetBufferAttribute(void *attr, uint32_t pixelFormat, uint32_t tilingMode,
                                  uint32_t aspectRatio, uint32_t width, uint32_t height,
                                  uint32_t pitchInPixel);
int sceVideoOutRegisterBuffers(int handle, int startIndex, void *const *addresses,
                               int bufferNum, const void *attr);
int sceVideoOutSubmitFlip(int handle, int bufferIndex, int flipMode, int64_t flipArg);
int sceVideoOutAddFlipEvent(SceKernelEqueue eq, int handle, void *udata);
int sceVideoOutSetFlipRate(int handle, int rate);

/* ---- UserService / Pad / SystemService ---- */
int sceUserServiceInitialize(void *params);
int sceUserServiceGetInitialUser(int *userId);
int scePadInit(void);
int scePadOpen(int userId, int type, int index, const void *param);
int scePadReadState(int handle, void *data);
int sceSystemServiceHideSplashScreen(void);

/* ---- Net / NetCtl ---- */
typedef struct {
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} NetSockaddrIn;

#define NET_AF_INET       2
#define NET_SOCK_STREAM   1
#define NET_SOL_SOCKET    0xffff
#define NET_SO_REUSEADDR  0x0004

int      sceNetInit(void);
int      sceNetSocket(const char *name, int family, int type, int protocol);
int      sceNetSocketClose(int s);
int      sceNetSocketAbort(int s, int flags);
int      sceNetBind(int s, const void *addr, unsigned int addrlen);
int      sceNetListen(int s, int backlog);
int      sceNetAccept(int s, void *addr, unsigned int *addrlen);
int      sceNetConnect(int s, const void *addr, unsigned int addrlen);
int      sceNetRecv(int s, void *buf, size_t len, int flags);
int      sceNetSend(int s, const void *buf, size_t len, int flags);
int      sceNetSetsockopt(int s, int level, int optname, const void *optval, unsigned int optlen);
int      sceNetGetsockname(int s, void *addr, unsigned int *addrlen);
uint16_t sceNetHtons(uint16_t v);
uint32_t sceNetHtonl(uint32_t v);
uint16_t sceNetNtohs(uint16_t v);

int sceNetCtlInit(void);
int sceNetCtlGetInfo(int code, void *info);
#define NETCTL_INFO_IP_ADDRESS 14
