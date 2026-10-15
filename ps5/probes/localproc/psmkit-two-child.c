/* CPU-only helper: kernel/libc imports, no module APIs or GPU permissions. */
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "psmkit-two.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;
int sceKernelGetProcessType(pid_t pid);

static int validate_socket(int64_t deadline)
{
    int64_t start = io_now_ms();
    int type = 0;
    socklen_t length = sizeof(type);
    errno = 0;
    int result = getsockopt(TWO_SOCKET, SOL_SOCKET, SO_TYPE, &type, &length);
    int valid = !result && length == sizeof(type) && type == SOCK_STREAM;
    io_log("psmkit-two child validate SO_TYPE result=%d errno=%d type=%d length=%u valid=%d",
           result, errno, type, (unsigned)length, valid);
    if (!two_operation(start, deadline)) return 0;
    struct sockaddr_un local = {0}, peer = {0};
    length = sizeof(local);
    start = io_now_ms();
    errno = 0;
    result = getsockname(TWO_SOCKET, (struct sockaddr *)&local, &length);
    int local_valid = !result && length >= offsetof(struct sockaddr_un, sun_path) &&
                      length <= sizeof(local) && local.sun_family == AF_UNIX;
    io_log("psmkit-two child validate getsockname result=%d errno=%d family=%u length=%u valid=%d",
           result, errno, (unsigned)local.sun_family, (unsigned)length, local_valid);
    if (!two_operation(start, deadline)) return 0;
    length = sizeof(peer);
    start = io_now_ms();
    errno = 0;
    result = getpeername(TWO_SOCKET, (struct sockaddr *)&peer, &length);
    int peer_valid = !result && length >= offsetof(struct sockaddr_un, sun_path) &&
                     length <= sizeof(peer) && peer.sun_family == AF_UNIX;
    io_log("psmkit-two child validate getpeername result=%d errno=%d family=%u length=%u valid=%d",
           result, errno, (unsigned)peer.sun_family, (unsigned)length, peer_valid);
    return valid && local_valid && peer_valid && two_operation(start, deadline);
}

int main(void)
{
    int64_t start = io_now_ms(), deadline = start + TWO_OVERALL_MS;
    int allocated = 0, mapped = 0, passed = 0, go_valid = 0, scope_valid = 0;
    void *address = NULL;
    struct two_request request = {0};
    struct two_reply reply = {.physical = -1, .before = UINT64_MAX,
        .during = UINT64_MAX, .after = UINT64_MAX, .status = 1,
        .unmap_result = -1, .release_result = -1};
    io_log("psmkit-two child main pid=%d proc_type=%#x fd=3 source=FIRMWARE_GETTER_CONSTANT operation_ms=%d overall_ms=%d",
           (int)getpid(), (unsigned)sceKernelGetProcessType(getpid()), IO_OPERATION_MS, TWO_OVERALL_MS);
    if (start < 0 || !two_within(deadline) || !validate_socket(deadline)) goto cleanup;
    errno = 0;
    int result = io_transfer(TWO_SOCKET, &request, sizeof(request), 0, deadline);
    io_log("psmkit-two child REQUEST result=%d errno=%d index=%u nonce=%#llx",
           result, errno, request.index, (unsigned long long)request.nonce);
    if (result || !two_request_valid(&request, TWO_REQUEST)) goto cleanup;
    reply.echo = request;
    reply.pid = (uint32_t)getpid();
    start = io_now_ms();
    reply.pool = sceKernelGetDirectMemorySize();
    if (!two_operation(start, deadline) || reply.pool < (int64_t)(2 * TWO_SPAN) ||
        request.physical_start < 0 || request.physical_start % TWO_GUARD ||
        request.physical_start > reply.pool - (int64_t)TWO_SPAN) goto cleanup;
    scope_valid = 1;
    int64_t at = -1;
    if (two_available("child", "before-scoped", request.physical_start,
                      request.physical_start + TWO_SPAN, &reply.before, &at, deadline) ||
        reply.before != TWO_SPAN || at != request.physical_start) goto cleanup;
    start = io_now_ms();
    errno = 0;
    result = sceKernelAllocateDirectMemory(request.physical_start,
        request.physical_start + TWO_BYTES, TWO_BYTES, TWO_GUARD,
        PS5_KERNEL_DIRECT_TYPE_CPU, &reply.physical);
    io_log("psmkit-two child allocate index=%u result=%#x errno=%d physical=%#llx bytes=%zu pool_bytes=%lld",
           request.index, (unsigned)result, errno, (unsigned long long)reply.physical,
           TWO_BYTES, (long long)reply.pool);
    if (result) goto cleanup;
    allocated = 1;
    if (reply.physical != request.physical_start || !two_operation(start, deadline)) goto cleanup;
    start = io_now_ms();
    errno = 0;
    result = sceKernelMapDirectMemory(&address, TWO_BYTES,
        PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE, 0, reply.physical, TWO_GUARD);
    io_log("psmkit-two child map index=%u result=%#x errno=%d address=%p bytes=%zu flags=0 kernel_chosen_va=1",
           request.index, (unsigned)result, errno, address, TWO_BYTES);
    if (result) goto cleanup;
    mapped = 1;
    reply.address = (uint64_t)(uintptr_t)address;
    if (!address || !two_operation(start, deadline)) goto cleanup;
    start = io_now_ms();
    volatile unsigned char *pages = address;
    for (size_t page = 0; page < TWO_PAGES; page++)
        pages[page * TWO_PAGE] = two_page_byte(request.index, page);
    reply.checksum = UINT32_C(2166136261);
    int bytes_valid = 1;
    for (size_t page = 0; page < TWO_PAGES; page++)
    {
        unsigned char observed = pages[page * TWO_PAGE];
        if (observed != two_page_byte(request.index, page)) bytes_valid = 0;
        reply.checksum = (reply.checksum ^ observed) * UINT32_C(16777619);
        reply.pages++;
    }
    io_log("psmkit-two child touch index=%u pages=%u checksum=%#x expected=%#x bytes_valid=%d",
           request.index, reply.pages, reply.checksum, two_checksum(request.index), bytes_valid);
    if (!bytes_valid || reply.checksum != two_checksum(request.index) ||
        !two_operation(start, deadline)) goto cleanup;
    if (two_available("child", "during-scoped", request.physical_start,
                      request.physical_start + TWO_SPAN, &reply.during, &at, deadline) ||
        reply.during != TWO_GUARD || at != request.physical_start + (int64_t)TWO_BYTES)
        goto cleanup;
    reply.echo.kind = TWO_READY;
    reply.status = 0;
    errno = 0;
    result = io_transfer(TWO_SOCKET, &reply, sizeof(reply), 1, deadline);
    io_log("psmkit-two child READY index=%u pid=%u nonce=%#llx result=%d errno=%d allocation_held=%d",
           request.index, reply.pid, (unsigned long long)request.nonce, result, errno, allocated);
    if (result) goto cleanup;
    struct two_request go = {0}, expected = request;
    expected.kind = TWO_GO;
    errno = 0;
    result = io_transfer(TWO_SOCKET, &go, sizeof(go), 0, deadline);
    go_valid = !result && !memcmp(&go, &expected, sizeof(go));
    io_log("psmkit-two child GO index=%u result=%d errno=%d exact=%d allocation_held=%d",
           request.index, result, errno, go_valid, allocated);
    if (!go_valid) goto cleanup;
    unsigned char extra;
    errno = 0;
    result = io_transfer(TWO_SOCKET, &extra, 1, 0, deadline);
    go_valid = result == 1 && !errno;
    io_log("psmkit-two child request-EOF index=%u result=%d errno=%d exact=%d",
           request.index, result, errno, go_valid);
cleanup:
    reply.status = 1;
    /* Never skip cleanup on deadline/error; synchronous kernel calls cannot
     * be preempted here. No retries, remaps or replacement allocation. */
    if (mapped)
    {
        start = io_now_ms();
        errno = 0;
        reply.unmap_result = sceKernelMunmap(address, TWO_BYTES);
        io_log("psmkit-two child unmap index=%u result=%#x errno=%d",
               request.index, (unsigned)reply.unmap_result, errno);
        if (!two_operation(start, deadline)) go_valid = 0;
    }
    if (allocated)
    {
        start = io_now_ms();
        errno = 0;
        reply.release_result = sceKernelReleaseDirectMemory(reply.physical, TWO_BYTES);
        io_log("psmkit-two child release index=%u result=%#x errno=%d bytes=%zu",
               request.index, (unsigned)reply.release_result, errno, TWO_BYTES);
        if (!two_operation(start, deadline)) go_valid = 0;
    }
    if (scope_valid && two_available("child", "after-scoped", request.physical_start,
        request.physical_start + TWO_SPAN, &reply.after, NULL, deadline)) go_valid = 0;
    passed = go_valid && mapped && allocated && !reply.unmap_result && !reply.release_result &&
             reply.before == TWO_SPAN && reply.during == TWO_GUARD && reply.after == reply.before &&
             reply.pages == TWO_PAGES && reply.checksum == two_checksum(request.index) && two_within(deadline);
    if (passed)
    {
        reply.echo.kind = TWO_DONE;
        reply.status = 0;
        errno = 0;
        int sent = io_transfer(TWO_SOCKET, &reply, sizeof(reply), 1, deadline);
        io_log("psmkit-two child DONE index=%u pid=%u checksum=%#x result=%d errno=%d before=%llu during=%llu after=%llu released=1",
               request.index, reply.pid, reply.checksum, sent, errno,
               (unsigned long long)reply.before, (unsigned long long)reply.during,
               (unsigned long long)reply.after);
        passed = !sent;
        start = io_now_ms();
        errno = 0;
        int shut = shutdown(TWO_SOCKET, SHUT_WR);
        io_log("psmkit-two child shutdown-write index=%u result=%d errno=%d", request.index, shut, errno);
        if (shut || !two_operation(start, deadline)) passed = 0;
    }
    start = io_now_ms();
    errno = 0;
    int closed = close(TWO_SOCKET);
    io_log("psmkit-two child close index=%u fd=3 result=%d errno=%d", request.index, closed, errno);
    passed = passed && !closed && two_operation(start, deadline);
    io_log("psmkit-two child complete index=%u pid=%d nonce=%#llx result=%s intended_exit=%d actual_exit=EXTERNAL_GATE",
           request.index, (int)getpid(), (unsigned long long)request.nonce,
           passed ? "DONE_SENT" : "FAIL_OR_INCONCLUSIVE", passed ? 0 : 1);
    return passed ? 0 : 1;
}
