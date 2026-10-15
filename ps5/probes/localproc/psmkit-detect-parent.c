#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "probe.h"
#include "psmkit-detect.h"

int32_t sceSystemServiceGetAppStatus(struct app_status *status);
/* Firmware 5.50 cltshBrDLC0 at 0x1280: no app_id argument. */
int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[], const struct local_process_param *param);
int sceKernelGetProcessType(pid_t pid);

_Static_assert(sizeof(struct local_process_param) == 0x90, "parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "socket parameter offset");
_Static_assert(sizeof(struct app_status) == 16, "app status size");

int main(void)
{
    static const char path[] = "/app0/psmkit-detect-child.bin";
    const char *const argv[] = {path, NULL};
    const struct local_process_param param = {
        .size = sizeof(param), .socket_fd = -1,
        .field_8 = 1, .field_c = -1, .field_10 = 1,
    };
    struct app_status status = {0};
    int64_t start = detect_now_ms();
    int64_t deadline = start + DETECT_PARENT_MS;
    int calls = 0, accepted = 0;
    errno = 0;
    int result = sceSystemServiceGetAppStatus(&status);
    int error = errno;
    detect_log("psmkit-detect parent app-status result=%#x errno=%d fields=%#x,%#x,%#x,%#x\n",
               (unsigned)result, error, (unsigned)status.fields[0], (unsigned)status.fields[1],
               (unsigned)status.fields[2], (unsigned)status.fields[3]);
    detect_log("psmkit-detect parent main pid=%d proc_type=%#x socket_fd=-1 child_calls_limit=1\n",
               (int)getpid(), (unsigned)sceKernelGetProcessType(getpid()));
    if (start < 0 || result < 0 || status.fields[0] <= 0 ||
        !detect_within_deadline(deadline)) goto complete;
    calls = 1;
    errno = 0;
    int32_t handle = sceSystemServiceAddLocalProcessForPsmKit(path, argv, &param);
    error = errno;
    detect_log("psmkit-detect parent add path=%s param_size=%#x result=%#x errno=%d calls=1 handle_not_pid=1\n",
               path, param.size, (unsigned)handle, error);
    accepted = handle >= 0;
    /* No socket or process-handle-as-pid wait. Give the child's 5-second
     * bounded queries a capture window, with one sleep and no EINTR retry.
     * Real child exit is a mandatory external klog gate, never inferred. */
    int64_t now = detect_now_ms();
    if (accepted && now >= 0 && now < deadline)
    {
        int64_t remaining = deadline - now;
        const struct timespec delay = {remaining / 1000, (remaining % 1000) * 1000000};
        errno = 0;
        result = nanosleep(&delay, NULL);
        error = errno;
        detect_log("psmkit-detect parent capture-window result=%d errno=%d sleep_calls=1\n", result, error);
    }
complete:
    detect_log("psmkit-detect parent complete calls=%d launch=%s child_detection=EXTERNAL_GATE clean_child_exit=EXTERNAL_GATE\n",
               calls, accepted ? "ACCEPTED" : "REJECTED_OR_SKIPPED");
    return accepted ? 0 : 1;
}
