#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "probe.h"
#include "psmkit-fd3.h"

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[], const struct local_process_param *param);
int sceKernelGetProcessType(pid_t pid);

_Static_assert(sizeof(struct local_process_param) == 0x90, "parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "socket parameter offset");

int main(void)
{
    static const char path[] = "/app0/psmkit-fd3-child.bin";
    const char *const argv[] = {path, NULL};
    int64_t start = io_now_ms();
    int64_t deadline = start + IO_OVERALL_MS;
    int calls = 0, passed = 0, pair[2] = {-1, -1};
    io_log("psmkit-fd3 parent main pid=%d proc_type=%#x source=FIRMWARE_GETTER_CONSTANT operation_ms=%d overall_ms=%d",
           (int)getpid(), (unsigned)sceKernelGetProcessType(getpid()), IO_OPERATION_MS, IO_OVERALL_MS);
    fd3_log_firmware("parent");
    if (start < 0 || !fd3_within_deadline(deadline)) goto complete;
    struct app_status status;
    int status_result = get_app_status(&status);
    io_log("psmkit-fd3 parent app-status result=%#x", (unsigned)status_result);
    print_identity("psmkit-fd3 parent", &status);
    if (status_result < 0 || status.fields[0] <= 0 || !fd3_within_deadline(deadline)) goto complete;
    errno = 0;
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    io_log("psmkit-fd3 parent socketpair result=%d errno=%d parent_fd=%d child_endpoint=%d",
           result, errno, pair[0], pair[1]);
    if (result) goto complete;
    const struct local_process_param param = {
        .size = sizeof(param), .socket_fd = pair[1],
        .field_8 = 1, .field_c = -1, .field_10 = 1,
    };
    if (!fd3_within_deadline(deadline)) goto complete;
    io_log("psmkit-fd3 parent calling once path=%s param_size=%#x socket_fd=%d",
           path, param.size, param.socket_fd);
    calls = 1;
    errno = 0;
    int32_t handle = sceSystemServiceAddLocalProcessForPsmKit(path, argv, &param);
    int error = errno;
    /* Launch copies the endpoint. Drop our copy even on rejection so it
     * cannot hide EOF; never interpret this local-process handle as a PID. */
    errno = 0;
    int child_close = close(pair[1]);
    int close_error = errno;
    pair[1] = -1;
    io_log("psmkit-fd3 parent add result=%#x errno=%d calls=1 handle_not_pid=1", (unsigned)handle, error);
    io_log("psmkit-fd3 parent close-child-endpoint result=%d errno=%d", child_close, close_error);
    if (handle < 0 || child_close || !fd3_within_deadline(deadline)) goto complete;
    struct io_request request = {.magic = IO_MAGIC, .version = IO_VERSION, .cookie = IO_COOKIE};
    errno = 0;
    result = io_transfer(pair[0], &request, sizeof(request), 1, deadline);
    io_log("psmkit-fd3 parent send result=%d errno=%d bytes=%zu", result, errno, sizeof(request));
    if (result) goto complete;
    errno = 0;
    result = shutdown(pair[0], SHUT_WR);
    io_log("psmkit-fd3 parent shutdown-write result=%d errno=%d", result, errno);
    if (result || !fd3_within_deadline(deadline)) goto complete;
    struct socket_reply reply = {0};
    errno = 0;
    result = io_transfer(pair[0], &reply, sizeof(reply), 0, deadline);
    io_log("psmkit-fd3 parent receive result=%d errno=%d bytes=%zu", result, errno, sizeof(reply));
    if (result) goto complete;
    int cookie_valid = reply.echo.magic == IO_MAGIC && reply.echo.version == IO_VERSION &&
                       !memcmp(reply.echo.cookie, IO_COOKIE, IO_COOKIE_BYTES);
    io_log("psmkit-fd3 parent verify cookie=%d socket_status=%d", cookie_valid, reply.socket_status);
    unsigned char extra = 0;
    errno = 0;
    result = io_transfer(pair[0], &extra, 1, 0, deadline);
    error = errno;
    int exact_frame = result == 1 && error == 0;
    io_log("psmkit-fd3 parent end-of-stream result=%d errno=%d exact_frame=%d", result, error, exact_frame);
    passed = cookie_valid && !reply.socket_status && exact_frame && fd3_within_deadline(deadline);
complete:
    for (int i = 0; i < 2; i++)
    {
        if (pair[i] < 0) continue;
        errno = 0;
        int close_result = close(pair[i]);
        io_log("psmkit-fd3 parent close endpoint=%d fd=%d result=%d errno=%d", i, pair[i], close_result, errno);
        if (close_result) passed = 0;
    }
    passed = passed && fd3_within_deadline(deadline);
    /* EOF and an intended exit log are not actual child exit evidence.
     * Prior parent's CRT exit faulted: control must close this survivor. */
    io_log("psmkit-fd3 parent complete calls=%d result=%s clean_child_exit=EXTERNAL_GATE identity=EXTERNAL_GATE fatal_gpu_va=EXTERNAL_GATE capture_hold_ms=15000 control_close=REQUIRED",
           calls, passed ? "PROTOCOL_PASS" : "FAIL_OR_INCONCLUSIVE");
    int64_t hold_start = io_now_ms();
    int64_t hold_deadline = hold_start + 15000;
    if (hold_start >= 0)
    {
        while (fd3_within_deadline(hold_deadline))
        {
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
            nanosleep(&pause, NULL);
        }
    }
    else
    {
        /* Preserve a bounded control-close opportunity even on clock failure. */
        sleep(15);
    }
    return passed ? 0 : 1;
}
