/* CPU helper: static libkernel/libc/Sysmodule only; public loader APIs. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "psmkit-socket.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;
int sceKernelGetProcessType(pid_t pid);
int sceSysmoduleLoadModuleInternal(unsigned int module_id);

/* OpenOrbis public SDK OrbisKernelModuleInfo layout, corroborated by the
 * local prospero-win parser and firmware 5.50 public libkernel wrappers.
 * Not ModuleInfoEx/FromAddr (0x1a8), and not a private dynlib object. */
struct module_segment
{
    void *address;
    uint32_t size;
    int32_t protection;
};
struct module_info
{
    size_t size;
    char name[256];
    struct module_segment segments[4];
    uint32_t segment_count;
    uint8_t fingerprint[20];
};
_Static_assert(sizeof(struct module_segment) == 0x10, "module segment size");
_Static_assert(sizeof(struct module_info) == 0x160, "public module info size");
_Static_assert(offsetof(struct module_info, name) == 8, "public module name offset");
_Static_assert(offsetof(struct module_info, segments) == 0x108, "public segments offset");
_Static_assert(offsetof(struct module_info, segment_count) == 0x148, "public segment count offset");
int sceKernelGetModuleList(int32_t *handles, size_t capacity, size_t *count);
int sceKernelGetModuleInfo(int32_t handle, struct module_info *info);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);

static int within_deadline(int64_t deadline)
{
    int64_t now = io_now_ms();
    return now >= 0 && now < deadline;
}

static int parent_socket(int64_t deadline)
{
    errno = 0;
    int result = sceSysmoduleLoadModuleInternal(UINT32_C(0x80000010));
    io_log("psmkit-socket child internal-load id=0x80000010 result=%#x errno=%d",
           (unsigned)result, errno);
    if (result || !within_deadline(deadline)) return -1;
    /* The SDK payload dlopen's loaded-soname route reads private kernel
     * objects. Use public enumeration instead: no path load, alias, retry,
     * descriptor scan, kernel read/write or dynamically generated stub. */
    int32_t handles[256];
    size_t count = 0;
    errno = 0;
    result = sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count);
    io_log("psmkit-socket child module-list result=%#x errno=%d count=%zu capacity=256",
           (unsigned)result, errno, count);
    if (result || count > sizeof(handles) / sizeof(handles[0])) return -1;
    int32_t module = -1;
    for (size_t i = 0; i < count; i++)
    {
        if (!within_deadline(deadline)) return -1;
        struct module_info info = {.size = sizeof(info)};
        errno = 0;
        result = sceKernelGetModuleInfo(handles[i], &info);
        int error = errno;
        int terminated = memchr(info.name, 0, sizeof(info.name)) != NULL;
        io_log("psmkit-socket child module-info handle=%#x result=%#x errno=%d name=%.*s terminated=%d",
               (unsigned)handles[i], (unsigned)result, error, (int)sizeof(info.name), info.name, terminated);
        if (result || !terminated) return -1;
        /* 5.50 DT_SONAME is .prx; the SDK sysmodtab filename is .sprx.
         * These are exact canonical names, not randomized path aliases. */
        if (!strcmp(info.name, "libSceSystemService.prx") ||
            !strcmp(info.name, "libSceSystemService.sprx"))
        {
            if (module >= 0) return -1; /* ambiguous enumeration is not a pass */
            module = handles[i];
        }
    }
    io_log("psmkit-socket child loaded-SystemService handle=%#x", (unsigned)module);
    if (module < 0 || !within_deadline(deadline)) return -1;
    void *symbol = NULL;
    errno = 0;
    result = sceKernelDlsym(module, "sceSystemServiceGetParentSocketForPsmKit", &symbol);
    io_log("psmkit-socket child resolve result=%#x errno=%d symbol=%p", (unsigned)result, errno, symbol);
    if (result || !symbol || !within_deadline(deadline)) return -1;
    /* 5.50 export 4ZYuSI8i2aM at 0x14c0 grounds int(void).
     * Call the getter exactly once; its fd is never inferred or hardcoded. */
    int (*get_socket)(void) = (int (*)(void))symbol;
    errno = 0;
    int fd = get_socket();
    io_log("psmkit-socket child getter result=%#x fd=%d errno=%d", (unsigned)fd, fd, errno);
    if (fd < 0) return -1;
    int type = 0;
    socklen_t type_length = sizeof(type);
    errno = 0;
    int type_result = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length);
    io_log("psmkit-socket child socket-type result=%d errno=%d type=%d length=%u",
           type_result, errno, type, (unsigned)type_length);
    struct sockaddr_storage local = {0}, peer = {0};
    socklen_t local_length = sizeof(local), peer_length = sizeof(peer);
    errno = 0;
    int local_result = getsockname(fd, (struct sockaddr *)&local, &local_length);
    io_log("psmkit-socket child socket-local result=%d errno=%d family=%u length=%u",
           local_result, errno, (unsigned)local.ss_family, (unsigned)local_length);
    errno = 0;
    int peer_result = getpeername(fd, (struct sockaddr *)&peer, &peer_length);
    io_log("psmkit-socket child socket-peer result=%d errno=%d family=%u length=%u",
           peer_result, errno, (unsigned)peer.ss_family, (unsigned)peer_length);
    if (type_result || type_length != sizeof(type) || type != SOCK_STREAM ||
        local_result || peer_result || local.ss_family != AF_UNIX || peer.ss_family != AF_UNIX ||
        local_length < 2 || local_length > sizeof(local) ||
        peer_length < 2 || peer_length > sizeof(peer) || !within_deadline(deadline))
    {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void)
{
    int64_t start = io_now_ms();
    int64_t deadline = start + IO_OVERALL_MS;
    int passed = 0, fd = -1;
    io_log("psmkit-socket child main pid=%d proc_type=%#x", (int)getpid(),
           (unsigned)sceKernelGetProcessType(getpid()));
    if (start < 0) goto complete;
    fd = parent_socket(deadline);
    if (fd < 0 || !within_deadline(deadline)) goto complete;
    struct io_request request = {0};
    errno = 0;
    int result = io_transfer(fd, &request, sizeof(request), 0, deadline);
    int cookie_valid = !result && request.magic == IO_MAGIC && request.version == IO_VERSION &&
                       !memcmp(request.cookie, IO_COOKIE, IO_COOKIE_BYTES);
    io_log("psmkit-socket child receive result=%d errno=%d cookie=%d bytes=%zu",
           result, errno, cookie_valid, sizeof(request));
    if (!cookie_valid) goto complete;
    unsigned char extra = 0;
    errno = 0;
    result = io_transfer(fd, &extra, 1, 0, deadline);
    int error = errno;
    int exact_frame = result == 1 && error == 0;
    io_log("psmkit-socket child end-of-request result=%d errno=%d exact_frame=%d", result, error, exact_frame);
    if (!exact_frame) goto complete;
    struct socket_reply reply = {.echo = request, .socket_status = 0};
    errno = 0;
    result = io_transfer(fd, &reply, sizeof(reply), 1, deadline);
    io_log("psmkit-socket child send result=%d errno=%d bytes=%zu", result, errno, sizeof(reply));
    passed = !result && within_deadline(deadline);
complete:
    if (fd >= 0)
    {
        errno = 0;
        int result = close(fd);
        io_log("psmkit-socket child close result=%d errno=%d", result, errno);
        if (result) passed = 0;
    }
    /* Internal-load refcount ownership/unload semantics are not proven by
     * public evidence. Process exit owns module cleanup; never StopUnload or
     * SysmoduleUnload a borrowed public-list handle. */
    passed = passed && within_deadline(deadline);
    io_log("psmkit-socket child complete socket=%s exit=%d module_cleanup=PROCESS_EXIT",
           passed ? "COOKIE_SENT" : "FAIL_OR_INCONCLUSIVE", passed ? 0 : 1);
    return passed ? 0 : 1;
}
