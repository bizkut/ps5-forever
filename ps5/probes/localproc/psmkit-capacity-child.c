#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "psmkit-capacity.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement = &Need_sceLibcInternal;

static int socket_valid(void)
{
    int type = 0;
    socklen_t size = sizeof(type);
    if (getsockopt(CAP_SOCKET, SOL_SOCKET, SO_TYPE, &type, &size) ||
        size != sizeof(type) || type != SOCK_STREAM) return 0;
    struct sockaddr_un address = {0};
    size = sizeof(address);
    if (getsockname(CAP_SOCKET, (struct sockaddr *)&address, &size) ||
        size < offsetof(struct sockaddr_un, sun_path) || size > sizeof(address) || address.sun_family != AF_UNIX) return 0;
    size = sizeof(address);
    return !getpeername(CAP_SOCKET, (struct sockaddr *)&address, &size) &&
           size >= offsetof(struct sockaddr_un, sun_path) && size <= sizeof(address) && address.sun_family == AF_UNIX;
}

int main(void)
{
    int64_t start = io_now_ms(), deadline = start + CAP_CHILD_MS;
    struct cap_frame request = {0}, stop = {0};
    int protocol = 0, error = 0;
    unsigned index = CAP_LIMIT;
    if (start < 0 || !socket_valid()) goto complete;
    errno = 0;
    int status = io_transfer(CAP_SOCKET, &request, sizeof(request), 0, deadline);
    error = errno;
    if (status || !cap_request_valid(&request)) goto complete;
    index = request.index;
    struct cap_frame reply = request;
    reply.kind = CAP_READY;
    reply.pid = (uint32_t)getpid();
    if (!reply.pid || reply.pid == request.parent_pid) goto complete;
    errno = 0;
    status = io_transfer(CAP_SOCKET, &reply, sizeof(reply), 1, deadline);
    error = errno;
    if (status) goto complete;
    io_log("psmkit-capacity child READY index=%u pid=%u parent_pid=%u app_id=%u nonce=%#llx held=1 main_threads=1 probe_allocations=0",
           index, reply.pid, request.parent_pid, request.app_id, (unsigned long long)request.nonce);
    /* Idle STOP wait spans the COMPLETE batch, not a two-second IO timeout.
     * Each individual poll and the eventual frame transfer remains <=2s.
     * EOF/control-close also releases the child, so failure cannot orphan it. */
    for (;;) {
        int64_t now = io_now_ms();
        if (now < 0 || now >= deadline) { error = ETIMEDOUT; goto complete; }
        int remaining = (int)(deadline - now);
        struct pollfd descriptor = {.fd = CAP_SOCKET, .events = POLLIN};
        errno = 0;
        status = poll(&descriptor, 1, remaining < IO_OPERATION_MS ? remaining : IO_OPERATION_MS);
        if (status < 0 && errno == EINTR) continue;
        if (status < 0) { error = errno; goto complete; }
        if (status) break;
    }
    errno = 0;
    status = io_transfer(CAP_SOCKET, &stop, sizeof(stop), 0, deadline);
    error = errno;
    struct cap_frame expected = request;
    expected.kind = CAP_STOP;
    if (status || memcmp(&stop, &expected, sizeof(stop))) goto complete;
    unsigned char extra;
    errno = 0;
    status = io_transfer(CAP_SOCKET, &extra, 1, 0, deadline);
    error = errno;
    if (status != 1 || error) goto complete;
    reply.kind = CAP_DONE;
    errno = 0;
    status = io_transfer(CAP_SOCKET, &reply, sizeof(reply), 1, deadline);
    error = errno;
    if (status) goto complete;
    protocol = 1;
complete:
    errno = 0;
    int shut = shutdown(CAP_SOCKET, SHUT_WR), shut_error = errno;
    errno = 0;
    int closed = close(CAP_SOCKET), close_error = errno;
    io_log("psmkit-capacity child complete index=%u pid=%u protocol=%s errno=%d shutdown=%d shutdown_errno=%d close=%d close_errno=%d exit=0 actual_exit=EXTERNAL_GATE",
           index, (unsigned)getpid(), protocol ? "DONE" : "ABORTED", error, shut, shut_error, closed, close_error);
    /* This CPU-only helper has no resource to retain; all aborts exit cleanly.
     * Exit0 alone does not pass protocol, whose exact DONE/EOF is mandatory. */
    return 0;
}
