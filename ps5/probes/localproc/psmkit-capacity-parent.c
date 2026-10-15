#include <errno.h>
#include <stddef.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "probe.h"
#include "psmkit-capacity.h"

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[], const struct local_process_param *param);
_Static_assert(sizeof(struct local_process_param) == 0x90, "PsmKit parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "PsmKit socket offset");
static int64_t pool;
static struct cap_result result;

static int make_pair(int pair[2]) { return socketpair(AF_UNIX, SOCK_STREAM, 0, pair); }
static int stop_write(int fd) { return shutdown(fd, SHUT_WR); }
static int32_t launch(int child_fd)
{
    static const char path[] = "/app0/psmkit-capacity-child.bin";
    const char *const argv[] = {path, NULL};
    const struct local_process_param param = {.size = sizeof(param), .socket_fd = child_fd,
        .field_8 = 1, .field_c = -1, .field_10 = 1};
    return sceSystemServiceAddLocalProcessForPsmKit(path, argv, &param);
}
static int live(int fd)
{
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    int status = poll(&descriptor, 1, 0);
    if (status < 0) return status;
    if (status) { errno = ECONNRESET; return descriptor.revents; }
    return 0;
}
static int stats(const char *phase, unsigned ready, int64_t deadline)
{
    int64_t before = io_now_ms(), physical = -1;
    size_t available = 0, flexible = 0;
    errno = 0;
    int32_t direct_status = sceKernelAvailableDirectMemorySize(0, pool, 0x10000, &physical, &available);
    int direct_error = errno;
    int64_t after = io_now_ms();
    io_log("psmkit-capacity parent stats phase=%s live_READY=%u direct_status=%#x errno=%d largest_available_direct=%llu physical=%#llx pool=%lld total_free_claim=0",
           phase, ready, (unsigned)direct_status, direct_error, (unsigned long long)available,
           (unsigned long long)physical, (long long)pool);
    if (direct_status || before < 0 || after < before || after - before > IO_OPERATION_MS || after >= deadline) {
        errno = direct_status ? direct_error : ETIMEDOUT;
        return direct_status ? direct_status : -1;
    }
    before = after;
    errno = 0;
    int32_t flexible_status = sceKernelAvailableFlexibleMemorySize(&flexible);
    int flexible_error = errno;
    after = io_now_ms();
    io_log("psmkit-capacity parent stats phase=%s available_flexible=%llu status=%#x errno=%d thread_stats=UNSUPPORTED_NO_GROUNDED_ABI resource_stats=UNSUPPORTED_NO_GROUNDED_ABI explicit_probe_threads=0",
           phase, (unsigned long long)flexible, (unsigned)flexible_status, flexible_error);
    if (flexible_status || after < before || after - before > IO_OPERATION_MS || after >= deadline) {
        errno = flexible_status ? flexible_error : ETIMEDOUT;
        return flexible_status ? flexible_status : -1;
    }
    return 0;
}
int main(void)
{
    int64_t start = io_now_ms();
    struct app_status identity = {0};
    int identity_status = get_app_status(&identity);
    print_identity("psmkit-capacity parent", &identity);
    pool = sceKernelGetDirectMemorySize();
    io_log("psmkit-capacity parent main pid=%d ceiling=64 launch_ms=80000 protocol_ms=105000 hold_ms=15000 overall_ms=120000 io_ms=2000 native_main_threads_per_child=1 CRT_profile=EXISTING_LOCAL_HELPER_UNCHANGED",
           (int)getpid());
    if (start >= 0 && identity_status >= 0 && identity.fields[0] > 0 && pool > 0 &&
        io_now_ms() - start <= IO_OPERATION_MS) {
        const struct cap_ops ops = {.now = io_now_ms, .pair = make_pair, .launch = launch,
            .transfer = io_transfer, .shutdown_write = stop_write, .close_fd = close,
            .stats = stats, .live = live};
        uint64_t seed = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)start;
        cap_run(&result, &ops, seed, (uint32_t)getpid(), (uint32_t)identity.fields[0], start);
    } else {
        io_log("psmkit-capacity parent first-failure index=0 category=resource identity_status=%#x pool=%lld calls=0", (unsigned)identity_status, (long long)pool);
        io_log("psmkit-capacity parent complete calls=0 highest_fully_READY=0 reached64_ceiling=0 cleanup=FAIL_OR_INCONCLUSIVE capture_hold_ms=15000 control_close=REQUIRED");
    }
    int64_t hold_end = io_now_ms() + CAP_HOLD_MS;
    if (start >= 0 && hold_end > start + CAP_PROTOCOL_MS + CAP_HOLD_MS)
        hold_end = start + CAP_PROTOCOL_MS + CAP_HOLD_MS;
    for (;;) {
        int64_t now = io_now_ms();
        if (now < 0 || now >= hold_end) break;
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    /* Runner owns closure during hold; never take the BigApp CRT return path. */
    _exit(result.cleanup_ok ? 0 : 1);
}
