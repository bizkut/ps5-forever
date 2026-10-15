#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "probe.h"
#include "psmkit-socket.h"

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[], const struct local_process_param *param);
int sceKernelGetProcessType(pid_t pid);

_Static_assert(sizeof(struct local_process_param) == 0x90, "parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "socket parameter offset");

int main(void)
{
    static const char path[] = "/app0/psmkit-socket-child.bin";
    const char *const argv[] = {path, NULL};
    int64_t start = io_now_ms();
    int64_t deadline = start + IO_OVERALL_MS;
    int calls = 0, passed = 0, pair[2] = {-1, -1};
    struct app_status status;
    int status_result = get_app_status(&status);
    io_log("psmkit-socket parent app-status result=%#x", (unsigned)status_result);
    print_identity("psmkit-socket parent", &status);
    io_log("psmkit-socket parent pid=%d proc_type=%#x", (int)getpid(),
           (unsigned)sceKernelGetProcessType(getpid()));
    if (start < 0 || status_result < 0 || status.fields[0] <= 0) goto complete;
    errno = 0;
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    io_log("psmkit-socket parent socketpair result=%d errno=%d parent_fd=%d child_endpoint=%d",
           result, errno, pair[0], pair[1]);
    if (result) goto complete;
    const struct local_process_param param = {
        .size = sizeof(param), .socket_fd = pair[1],
        .field_8 = 1, .field_c = -1, .field_10 = 1,
    };
    int64_t now = io_now_ms();
    if (now < 0 || now >= deadline) goto complete;
    io_log("psmkit-socket parent calling once path=%s param_size=%#x socket_fd=%d",
           path, param.size, param.socket_fd);
    calls = 1;
    errno = 0;
    int32_t handle = sceSystemServiceAddLocalProcessForPsmKit(path, argv, &param);
    int error = errno;
    /* Drop the copied child endpoint even on launch rejection; otherwise the
     * parent's retained copy can hide EOF. The launch handle is not a pid. */
    int child_close = close(pair[1]);
    int close_error = errno;
    pair[1] = -1;
    io_log("psmkit-socket parent add result=%#x errno=%d calls=1 (handle_not_pid)",
           (unsigned)handle, error);
    io_log("psmkit-socket parent close-child-endpoint result=%d errno=%d", child_close, close_error);
    if (handle < 0 || child_close) goto complete;
    struct io_request request = {.magic = IO_MAGIC, .version = IO_VERSION, .cookie = IO_COOKIE};
    errno = 0;
    result = io_transfer(pair[0], &request, sizeof(request), 1, deadline);
    io_log("psmkit-socket parent send result=%d errno=%d bytes=%zu", result, errno, sizeof(request));
    if (result) goto complete;
    errno = 0;
    result = shutdown(pair[0], SHUT_WR);
    io_log("psmkit-socket parent shutdown-write result=%d errno=%d", result, errno);
    if (result) goto complete;
    struct socket_reply reply = {0};
    errno = 0;
    result = io_transfer(pair[0], &reply, sizeof(reply), 0, deadline);
    io_log("psmkit-socket parent receive result=%d errno=%d bytes=%zu", result, errno, sizeof(reply));
    if (result) goto complete;
    int cookie_valid = reply.echo.magic == IO_MAGIC && reply.echo.version == IO_VERSION &&
                       !memcmp(reply.echo.cookie, IO_COOKIE, IO_COOKIE_BYTES);
    io_log("psmkit-socket parent verify cookie=%d socket_status=%d", cookie_valid, reply.socket_status);
    unsigned char extra = 0;
    errno = 0;
    result = io_transfer(pair[0], &extra, 1, 0, deadline);
    error = errno;
    int exact_frame = result == 1 && error == 0;
    io_log("psmkit-socket parent end-of-stream result=%d errno=%d exact_frame=%d", result, error, exact_frame);
    now = io_now_ms();
    passed = cookie_valid && !reply.socket_status && exact_frame && now >= 0 && now < deadline;
complete:
    if (pair[0] >= 0 && close(pair[0])) passed = 0;
    if (pair[1] >= 0) close(pair[1]);
    /* Clean child exit is a separate mandatory klog gate, not inferred from
     * EOF or from the public launch handle. Never waitpid(handle). */
    io_log("psmkit-socket parent complete calls=%d result=%s clean_child_exit=EXTERNAL_GATE",
           calls, passed ? "PROTOCOL_PASS" : "FAIL_OR_INCONCLUSIVE");
    return passed ? 0 : 1;
}
