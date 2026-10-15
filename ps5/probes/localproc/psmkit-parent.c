#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>
#include "probe.h"

/* Firmware 5.50 libSceSystemService, NID cltshBrDLC0, wrapper at 0x1280.
 * Unlike the ordinary PS5 wrapper, this takes no app_id argument. */
int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[],
    const struct local_process_param *param);

/* Firmware 5.50 +g+UP8Pyfmo at 0x275d0 forwards pid to syscall 610. */
int sceKernelGetProcessType(pid_t pid);

_Static_assert(sizeof(struct local_process_param) == 0x90, "local process parameter size");
_Static_assert(sizeof(struct app_status) == 16, "app status size");

static void print_process_type(const char *role, pid_t pid)
{
    errno = 0;
    int result = sceKernelGetProcessType(pid);
    int error = errno;
    probe_log("psmkit %s pid=%d proc_type=%#x errno=%d\n",
              role, (int)pid, (unsigned)result, error);
}

static void observe_children(const char *phase, int seconds)
{
    /* No heap allocation. Walk the console's larger records by ki_structsize,
     * as the SDK ps sample and the existing local process-list probes do. */
    _Alignas(struct kinfo_proc) static unsigned char records[512 * 1024];
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = sizeof(records);
    errno = 0;
    int result = sysctl(mib, 4, records, &size, NULL, 0);
    int error = errno;
    probe_log("psmkit observe phase=%s seconds=%d result=%d errno=%d bytes=%zu\n",
              phase, seconds, result, error, size);
    if (result || size > sizeof(records)) return;

    int candidates = 0;
    for (size_t at = 0; at + sizeof(struct kinfo_proc) <= size;)
    {
        const struct kinfo_proc *info = (const struct kinfo_proc *)(records + at);
        if (info->ki_structsize < (int)sizeof(*info) ||
            (size_t)info->ki_structsize > size - at ||
            (size_t)info->ki_structsize % _Alignof(struct kinfo_proc))
        {
            probe_log("psmkit observe malformed record offset=%zu size=%d\n",
                      at, info->ki_structsize);
            return;
        }
        at += (size_t)info->ki_structsize;
        if (strncmp(info->ki_comm, "localproc-child.bin", sizeof(info->ki_comm))) continue;
        candidates++;
        probe_log("psmkit child candidate phase=%s seconds=%d pid=%d ppid=%d status=%u comm=%.*s\n",
                  phase, seconds, (int)info->ki_pid, (int)info->ki_ppid,
                  (unsigned char)info->ki_stat, (int)sizeof(info->ki_comm), info->ki_comm);
        print_process_type("child candidate", info->ki_pid);
    }
    probe_log("psmkit observe phase=%s seconds=%d child_candidates=%d\n",
              phase, seconds, candidates);
}

static int wait_for_child(void)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start))
    {
        probe_log("psmkit observation clock failed errno=%d\n", errno);
        return 1;
    }
    for (int seconds = 0; seconds <= 20; seconds++)
    {
        /* Absolute deadlines prevent interrupted sleeps or listing time from
         * stretching the observation window into another launch attempt. */
        struct timespec target = start;
        target.tv_sec += seconds;
        for (;;)
        {
            if (clock_gettime(CLOCK_MONOTONIC, &now))
            {
                probe_log("psmkit observation clock failed errno=%d\n", errno);
                return 1;
            }
            struct timespec delay = {target.tv_sec - now.tv_sec,
                                     target.tv_nsec - now.tv_nsec};
            if (delay.tv_nsec < 0)
            {
                delay.tv_sec--;
                delay.tv_nsec += 1000000000;
            }
            if (delay.tv_sec < 0 || (!delay.tv_sec && !delay.tv_nsec)) break;
            if (nanosleep(&delay, NULL) && errno != EINTR)
            {
                probe_log("psmkit observation sleep failed errno=%d\n", errno);
                return 1;
            }
        }
        if (seconds == 20) break;
        observe_children("after", seconds);
    }
    return 0;
}

int main(void)
{
    static const char path[] = "/app0/localproc-child.bin";
    const char *const argv[] = {path, "identity", NULL};
    const struct local_process_param param = {
        .size = sizeof(param),
        .socket_fd = -1,
        .field_8 = 1,
        .field_c = -1,
        .field_10 = 1,
    };
    struct app_status status;
    int status_result = get_app_status(&status);
    probe_log("psmkit parent get app status result=%#x\n", (unsigned)status_result);
    print_identity("psmkit parent", &status);
    print_process_type("parent", getpid());
    if (status_result < 0 || status.fields[0] <= 0)
    {
        probe_log("psmkit parent refusing launch without a valid app id\n");
        probe_log("psmkit parent complete calls=0\n");
        return 1;
    }
    observe_children("before", 0);
    probe_log("psmkit parent calling once path=%s param_size=%#x\n", path, param.size);
    errno = 0;
    int32_t result = sceSystemServiceAddLocalProcessForPsmKit(path, argv, &param);
    int error = errno;
    /* A nonnegative return is a local-process handle, not the child PID. */
    probe_log("psmkit parent add local process path=%s result=%#x errno=%d calls=1\n",
              path, (unsigned)result, error);
    int wait_result = wait_for_child();
    probe_log("psmkit parent complete calls=1 observation_result=%d\n", wait_result);
    return result < 0 || wait_result ? 1 : 0;
}
