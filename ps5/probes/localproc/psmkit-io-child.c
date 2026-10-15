/* CPU-helper profile: no static SystemService or GPU import. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "psmkit-io.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;
int sceKernelGetProcessType(pid_t pid);
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                             uint32_t flags, const void *option, int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);
int sceKernelStopUnloadModule(int handle, size_t args, const void *argp,
                              uint32_t flags, const void *option, int *result);

static int within_deadline(int64_t deadline)
{
    int64_t now = io_now_ms();
    return now >= 0 && now < deadline;
}

static int available(const char *phase, int64_t pool, uint64_t *bytes)
{
    int64_t at = -1;
    size_t size = 0;
    errno = 0;
    int32_t result = sceKernelAvailableDirectMemorySize(0, pool, PS5_KERNEL_DIRECT_ALIGNMENT, &at, &size);
    int error = errno;
    io_log("psmkit-io child available phase=%s result=%#x errno=%d at=%#llx largest_bytes=%zu",
           phase, (unsigned)result, error, (unsigned long long)at, size);
    if (!result) *bytes = size;
    return result;
}

static void memory_test(struct io_reply *reply, int64_t deadline)
{
    const int64_t pool = sceKernelGetDirectMemorySize();
    void *address = NULL;
    void *reservation = NULL;
    int64_t physical = -1;
    int reserved = 0, allocated = 0, mapped = 0, valid = 1;
    reply->memory_status = 1;
    reply->before = reply->during = reply->after = UINT64_MAX;
    io_log("psmkit-io child memory pool_bytes=%lld requested_bytes=%zu alignment=%zu page=%zu",
           (long long)pool, IO_BYTES, PS5_KERNEL_DIRECT_ALIGNMENT, PS5_KERNEL_PAGE_SIZE);
    if (pool < (int64_t)IO_BYTES || !within_deadline(deadline)) return;
    if (available("before", pool, &reply->before)) return;
    errno = 0;
    int32_t result = sceKernelReserveVirtualRange(&address, IO_BYTES, 0, PS5_KERNEL_DIRECT_ALIGNMENT);
    io_log("psmkit-io child reserve result=%#x errno=%d address=%p bytes=%zu",
           (unsigned)result, errno, address, IO_BYTES);
    if (result) return;
    reserved = 1;
    reservation = address;
    if (!within_deadline(deadline)) goto cleanup;
    errno = 0;
    result = sceKernelAllocateDirectMemory(0, pool, IO_BYTES, PS5_KERNEL_DIRECT_ALIGNMENT,
                                           PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
    io_log("psmkit-io child allocate result=%#x errno=%d physical=%#llx bytes=%zu",
           (unsigned)result, errno, (unsigned long long)physical, IO_BYTES);
    if (result) goto cleanup;
    allocated = 1;
    if (!within_deadline(deadline)) goto cleanup;
    errno = 0;
    result = sceKernelMapDirectMemory(&address, IO_BYTES,
                                      PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                      PS5_KERNEL_MAP_FIXED, physical, PS5_KERNEL_DIRECT_ALIGNMENT);
    io_log("psmkit-io child map result=%#x errno=%d address=%p reservation=%p",
           (unsigned)result, errno, address, reservation);
    if (result) goto cleanup;
    mapped = 1;
    if (address != reservation || !within_deadline(deadline)) goto cleanup;
    volatile unsigned char *pages = address;
    for (size_t page = 0; page < IO_PAGES; page++)
        pages[page * PS5_KERNEL_PAGE_SIZE] = io_page_byte(page);
    reply->checksum = UINT32_C(2166136261);
    for (size_t page = 0; page < IO_PAGES; page++)
    {
        unsigned char observed = pages[page * PS5_KERNEL_PAGE_SIZE];
        if (observed != io_page_byte(page)) valid = 0;
        reply->checksum = (reply->checksum ^ observed) * UINT32_C(16777619);
        reply->pages++;
    }
    io_log("psmkit-io child touch pages=%u checksum=%#x expected=%#x bytes_valid=%d",
           reply->pages, reply->checksum, io_expected_checksum(), valid);
    if (available("during", pool, &reply->during)) valid = 0;
cleanup:
    /* Cleanup is unconditional, even if the deadline expired. A synchronous
     * firmware syscall cannot be preempted here; the external runner owns it. */
    if (mapped)
    {
        errno = 0;
        result = sceKernelMunmap(address, IO_BYTES);
        io_log("psmkit-io child unmap result=%#x errno=%d", (unsigned)result, errno);
        if (result) valid = 0;
        if (address == reservation && !result) reserved = 0;
    }
    if (reserved)
    {
        errno = 0;
        result = sceKernelMunmap(reservation, IO_BYTES);
        io_log("psmkit-io child unreserve result=%#x errno=%d", (unsigned)result, errno);
        if (result) valid = 0;
    }
    if (allocated)
    {
        errno = 0;
        result = sceKernelReleaseDirectMemory(physical, IO_BYTES);
        io_log("psmkit-io child release result=%#x errno=%d", (unsigned)result, errno);
        if (result) valid = 0;
    }
    if (available("after", pool, &reply->after)) valid = 0;
    if (valid && mapped && reply->pages == IO_PAGES &&
        reply->checksum == io_expected_checksum() && reply->before >= IO_BYTES &&
        reply->during == reply->before - IO_BYTES && reply->after == reply->before &&
        within_deadline(deadline)) reply->memory_status = 0;
}

static int parent_socket(int *module)
{
    static const char path[] = "/system/common/lib/libSceSystemService.sprx";
    int status = -1;
    errno = 0;
    *module = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &status);
    io_log("psmkit-io child load path=%s handle=%#x status=%#x errno=%d",
           path, (unsigned)*module, (unsigned)status, errno);
    if (*module < 0 || status < 0) return -1;
    void *symbol = NULL;
    errno = 0;
    int result = sceKernelDlsym(*module, "sceSystemServiceGetParentSocketForPsmKit", &symbol);
    io_log("psmkit-io child resolve result=%#x errno=%d symbol=%p", (unsigned)result, errno, symbol);
    if (result || !symbol) return -1;
    /* Firmware 5.50: export 4ZYuSI8i2aM at 0x14c0 -> 0x6610 ->
     * 0x8af0 (mov eax,3; ret). No argument/output pointer; int fd return.
     * Use the public getter, not the parent's descriptor number or a guess. */
    int (*get_socket)(void) = (int (*)(void))symbol;
    errno = 0;
    int fd = get_socket();
    io_log("psmkit-io child getter fd=%d errno=%d", fd, errno);
    if (fd < 0) return -1;
    int type = 0;
    socklen_t length = sizeof(type);
    errno = 0;
    result = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length);
    io_log("psmkit-io child socket-type result=%d errno=%d type=%d length=%u",
           result, errno, type, (unsigned)length);
    struct sockaddr peer = {0};
    length = sizeof(peer);
    errno = 0;
    int peer_result = getpeername(fd, &peer, &length);
    io_log("psmkit-io child socket-peer result=%d errno=%d family=%u length=%u",
           peer_result, errno, (unsigned)peer.sa_family, (unsigned)length);
    if (result || type != SOCK_STREAM || peer_result || peer.sa_family != AF_UNIX)
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
    struct io_reply reply = {.magic = IO_MAGIC, .version = IO_VERSION, .socket_status = 1};
    /* Public process type is diagnostic, not private flag 0x40. */
    io_log("psmkit-io child main pid=%d proc_type=%#x", (int)getpid(),
           (unsigned)sceKernelGetProcessType(getpid()));
    if (start < 0) return 1;
    memory_test(&reply, deadline);
    io_log("psmkit-io child memory complete status=%d before=%llu during=%llu after=%llu checksum=%#x pages=%u",
           reply.memory_status, (unsigned long long)reply.before, (unsigned long long)reply.during,
           (unsigned long long)reply.after, reply.checksum, reply.pages);
    int module = -1, fd = -1, exchange = -1;
    if (within_deadline(deadline)) fd = parent_socket(&module);
    if (fd >= 0 && within_deadline(deadline))
    {
        struct io_request request = {0};
        errno = 0;
        int result = io_transfer(fd, &request, sizeof(request), 0, deadline);
        io_log("psmkit-io child receive result=%d errno=%d", result, errno);
        if (!result && request.magic == IO_MAGIC && request.version == IO_VERSION &&
            !memcmp(request.cookie, IO_COOKIE, IO_COOKIE_BYTES))
        {
            memcpy(reply.cookie, request.cookie, IO_COOKIE_BYTES);
            reply.socket_status = 0;
            errno = 0;
            exchange = io_transfer(fd, &reply, sizeof(reply), 1, deadline);
            io_log("psmkit-io child send result=%d errno=%d", exchange, errno);
        }
        else io_log("psmkit-io child request invalid_or_incomplete=1");
    }
    if (module >= 0)
    {
        int status = -1;
        errno = 0;
        int result = sceKernelStopUnloadModule(module, 0, NULL, 0, NULL, &status);
        io_log("psmkit-io child unload result=%#x status=%#x errno=%d",
               (unsigned)result, (unsigned)status, errno);
        if (result || status < 0) exchange = -1;
    }
    int passed = !reply.memory_status && !exchange && within_deadline(deadline);
    io_log("psmkit-io child complete memory=%s socket=%s exit=%d",
           reply.memory_status ? "FAIL" : "PASS", exchange ? "INCONCLUSIVE" : "COOKIE_SENT",
           passed ? 0 : 1);
    if (fd >= 0) close(fd);
    return passed ? 0 : 1;
}
