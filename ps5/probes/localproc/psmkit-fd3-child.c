#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "psmkit-fd3.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;
int sceKernelGetProcessType(pid_t pid);

/* These are read-only descriptor metadata queries, not socket data transfer.
 * No fallback descriptor, loader route or scan exists. F_GETFD is diagnostic:
 * the console may reject that command for a valid socket, so successful
 * socket-specific queries establish open ownership. */
static int validate_parent_socket(int *open, int64_t deadline)
{
    int fd = FD3_PARENT_SOCKET;
    errno = 0;
    int flags = fcntl(fd, F_GETFD);
    int error = errno;
    *open = flags >= 0;
    io_log("psmkit-fd3 child validate F_GETFD fd=%d result=%d errno=%d diagnostic_valid=%d",
           fd, flags, error, *open);
    int type = 0;
    socklen_t type_length = sizeof(type);
    errno = 0;
    int type_result = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length);
    error = errno;
    int type_valid = !type_result && type_length == sizeof(type) && type == SOCK_STREAM;
    io_log("psmkit-fd3 child validate SO_TYPE result=%d errno=%d type=%d length=%u valid=%d",
           type_result, error, type, (unsigned)type_length, type_valid);
    struct sockaddr_un local = {0}, peer = {0};
    socklen_t local_length = sizeof(local), peer_length = sizeof(peer);
    errno = 0;
    int local_result = getsockname(fd, (struct sockaddr *)&local, &local_length);
    error = errno;
    int local_valid = !local_result && local_length >= offsetof(struct sockaddr_un, sun_path) &&
                      local_length <= sizeof(local) && local.sun_family == AF_UNIX;
    io_log("psmkit-fd3 child validate getsockname result=%d errno=%d family=%u length=%u min=%zu max=%zu valid=%d",
           local_result, error, (unsigned)local.sun_family, (unsigned)local_length,
           offsetof(struct sockaddr_un, sun_path), sizeof(local), local_valid);
    errno = 0;
    int peer_result = getpeername(fd, (struct sockaddr *)&peer, &peer_length);
    error = errno;
    int peer_valid = !peer_result && peer_length >= offsetof(struct sockaddr_un, sun_path) &&
                     peer_length <= sizeof(peer) && peer.sun_family == AF_UNIX;
    io_log("psmkit-fd3 child validate getpeername result=%d errno=%d family=%u length=%u min=%zu max=%zu valid=%d",
           peer_result, error, (unsigned)peer.sun_family, (unsigned)peer_length,
           offsetof(struct sockaddr_un, sun_path), sizeof(peer), peer_valid);
    *open = !type_result || !local_result || !peer_result;
    int valid = type_valid && local_valid && peer_valid && fd3_within_deadline(deadline);
    io_log("psmkit-fd3 child validate contract=%s source=FIRMWARE_GETTER_CONSTANT transfer_started=0",
           valid ? "AF_UNIX_SOCK_STREAM" : "REJECTED");
    return valid;
}

int main(void)
{
    int64_t start = io_now_ms();
    int64_t deadline = start + IO_OVERALL_MS;
    int passed = 0, open = 0, transfer_started = 0;
    io_log("psmkit-fd3 child main pid=%d proc_type=%#x fd=%d source=FIRMWARE_GETTER_CONSTANT operation_ms=%d overall_ms=%d",
           (int)getpid(), (unsigned)sceKernelGetProcessType(getpid()), FD3_PARENT_SOCKET,
           IO_OPERATION_MS, IO_OVERALL_MS);
    fd3_log_firmware("child");
    if (start < 0 || !fd3_within_deadline(deadline)) goto complete;
    if (!validate_parent_socket(&open, deadline)) goto complete;
    struct io_request request = {0};
    transfer_started = 1;
    errno = 0;
    int result = io_transfer(FD3_PARENT_SOCKET, &request, sizeof(request), 0, deadline);
    int cookie_valid = !result && request.magic == IO_MAGIC && request.version == IO_VERSION &&
                       !memcmp(request.cookie, IO_COOKIE, IO_COOKIE_BYTES);
    io_log("psmkit-fd3 child receive result=%d errno=%d cookie=%d bytes=%zu", result, errno, cookie_valid, sizeof(request));
    if (!cookie_valid) goto complete;
    unsigned char extra = 0;
    errno = 0;
    result = io_transfer(FD3_PARENT_SOCKET, &extra, 1, 0, deadline);
    int error = errno;
    int exact_frame = result == 1 && error == 0;
    io_log("psmkit-fd3 child end-of-request result=%d errno=%d exact_frame=%d", result, error, exact_frame);
    if (!exact_frame) goto complete;
    struct socket_reply reply = {.echo = request, .socket_status = 0};
    errno = 0;
    result = io_transfer(FD3_PARENT_SOCKET, &reply, sizeof(reply), 1, deadline);
    io_log("psmkit-fd3 child send result=%d errno=%d bytes=%zu", result, errno, sizeof(reply));
    if (result || !fd3_within_deadline(deadline)) goto complete;
    errno = 0;
    result = shutdown(FD3_PARENT_SOCKET, SHUT_WR);
    io_log("psmkit-fd3 child shutdown-write result=%d errno=%d", result, errno);
    passed = !result && fd3_within_deadline(deadline);
complete:
    if (open)
    {
        errno = 0;
        int close_result = close(FD3_PARENT_SOCKET);
        io_log("psmkit-fd3 child close fd=%d result=%d errno=%d", FD3_PARENT_SOCKET, close_result, errno);
        if (close_result) passed = 0;
    }
    passed = passed && fd3_within_deadline(deadline);
    io_log("psmkit-fd3 child complete socket=%s exit=%d transfer_started=%d source=FIRMWARE_GETTER_CONSTANT",
           passed ? "COOKIE_SENT" : "FAIL_OR_INCONCLUSIVE", passed ? 0 : 1, transfer_started);
    return passed ? 0 : 1;
}
