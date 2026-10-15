#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "probe.h"
#include "psmkit-capacity.h"
#include "../mixedproc/mixed-protocol.h"

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[], const struct local_process_param *param);
_Static_assert(sizeof(struct local_process_param) == 0x90, "PsmKit parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "PsmKit socket offset");

static int64_t pool;
static struct cap_result result;
static int mixed_pass, payload_cleanup;
static uint32_t payload_parent_pid, payload_worker_pid;

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
    io_log("psmkit-mixed parent stats phase=%s live_READY=%u direct_status=%#x errno=%d largest_available_direct=%llu physical=%#llx total_free_claim=0",
           phase, ready, (unsigned)direct_status, direct_error,
           (unsigned long long)available, (unsigned long long)physical);
    if (direct_status || before < 0 || after < before || after - before > IO_OPERATION_MS || after >= deadline) {
        errno = direct_status ? direct_error : ETIMEDOUT;
        return direct_status ? direct_status : -1;
    }
    before = after;
    errno = 0;
    int32_t flexible_status = sceKernelAvailableFlexibleMemorySize(&flexible);
    int flexible_error = errno;
    after = io_now_ms();
    io_log("psmkit-mixed parent stats phase=%s available_flexible=%llu status=%#x errno=%d payload_direct_allocations=0",
           phase, (unsigned long long)flexible, (unsigned)flexible_status, flexible_error);
    if (flexible_status || after < before || after - before > IO_OPERATION_MS || after >= deadline) {
        errno = flexible_status ? flexible_error : ETIMEDOUT;
        return flexible_status ? flexible_status : -1;
    }
    return 0;
}

static int connect_payload(uint16_t port, int64_t deadline)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int64_t before = io_now_ms();
    if (before < 0 || before >= deadline) {
        close(fd); errno = ETIMEDOUT; return -1;
    }
    /* Firmware 5.50 title sockets reject F_SETFL(O_NONBLOCK) with EACCES.
     * The qualified D8 route uses a blocking connect to an already-listening
     * loopback payload. Measure the call and retain the external run watchdog. */
    errno = 0;
    int status = connect(fd, (struct sockaddr *)&address, sizeof(address));
    int error = errno;
    int64_t after = io_now_ms();
    if (status || after < before || after - before > IO_OPERATION_MS ||
        after >= deadline) {
        if (!status) error = ETIMEDOUT;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int request_payload(uint16_t port, uint32_t role, uint32_t command,
                           uint64_t nonce, struct mixed_reply *reply,
                           int64_t deadline)
{
    errno = 0;
    int fd = connect_payload(port, deadline);
    if (fd < 0) {
        io_log("psmkit-mixed payload-request role=%u command=%u port=%u connect=-1 errno=%d",
               role, command, port, errno);
        return -1;
    }
    int status = 0, error = 0;
    struct mixed_request request = {
        .magic = MIXED_MAGIC,
        .version = MIXED_VERSION,
        .command = command,
        .nonce = nonce,
    };
    status = io_transfer(fd, &request, sizeof(request), 1, deadline);
    if (status) error = errno;
    if (!status) {
        status = io_transfer(fd, reply, sizeof(*reply), 0, deadline);
        if (status) error = errno;
    }
    if (!status && !mixed_reply_valid(reply, &request, role)) {
        status = -1;
        error = EPROTO;
    }
    errno = 0;
    int close_status = close(fd), close_error = errno;
    if (!status && close_status) {
        status = -1;
        error = close_error;
    }
    io_log("psmkit-mixed payload-request role=%u command=%u port=%u status=%d errno=%d close=%d nonce=%#llx pid=%u ppid=%u peer_pid=%u exact=%d",
           role, command, port, status, status ? error : 0, close_status,
           (unsigned long long)nonce, reply->pid, reply->ppid, reply->peer_pid,
           !status);
    if (status) errno = error;
    return status ? -1 : 0;
}

static int require_port_closed(uint16_t port, int64_t deadline)
{
    for (;;) {
        int64_t now = io_now_ms();
        if (now < 0 || now >= deadline) { errno = ETIMEDOUT; return -1; }
        int64_t attempt_deadline = now + 250;
        if (attempt_deadline > deadline) attempt_deadline = deadline;
        int fd = connect_payload(port, attempt_deadline);
        if (fd < 0 && errno == ECONNREFUSED) {
            io_log("psmkit-mixed payload-port-closed port=%u exact=1", port);
            return 0;
        }
        if (fd >= 0) close(fd);
        struct timespec pause = {.tv_nsec = 50000000};
        nanosleep(&pause, NULL);
    }
}

static int stop_payloads(void)
{
    int ok = 1;
    int64_t deadline = io_now_ms() + 8000;
    struct mixed_reply worker = {0}, parent = {0};
    uint64_t nonce = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)io_now_ms();
    if (request_payload(MIXED_WORKER_PORT, MIXED_WORKER, MIXED_STOP, nonce,
                        &worker, deadline) ||
        (payload_worker_pid && worker.pid != payload_worker_pid) ||
        require_port_closed(MIXED_WORKER_PORT, deadline)) ok = 0;
    if (request_payload(MIXED_PARENT_PORT, MIXED_PARENT, MIXED_STOP,
                        nonce ^ UINT64_C(0x4000000000000000), &parent,
                        deadline) ||
        (payload_parent_pid && parent.pid != payload_parent_pid) ||
        require_port_closed(MIXED_PARENT_PORT, deadline)) ok = 0;
    io_log("psmkit-mixed payload-stop worker_pid=%u parent_pid=%u expected_worker=%u expected_parent=%u worker_first=1 ports_closed=%d",
           worker.pid, parent.pid, payload_worker_pid, payload_parent_pid, ok);
    payload_cleanup = ok;
    return ok ? 0 : -1;
}

static int held_gate(const struct cap_result *capacity, int64_t deadline)
{
    if (capacity->highest_ready != 7 || capacity->failed_index != 7 ||
        capacity->category != CAP_LAUNCH ||
        (uint32_t)capacity->status != UINT32_C(0x80aa0008) ||
        capacity->error != 0) {
        io_log("psmkit-mixed held-gate rejected ready=%u failed_index=%d category=%s status=%#x errno=%d",
               capacity->highest_ready, capacity->failed_index,
               cap_category_name(capacity->category), (unsigned)capacity->status,
               capacity->error);
        errno = EPROTO;
        return -1;
    }
    uint32_t payload_parent = 0, payload_worker = 0;
    for (unsigned round = 0; round < 2; round++) {
        for (unsigned i = 0; i < capacity->highest_ready; i++) {
            if (live(capacity->slots[i].fd)) {
                io_log("psmkit-mixed held-gate lost-local round=%u index=%u", round, i);
                errno = ECONNRESET;
                return -1;
            }
        }
        int64_t round_deadline = io_now_ms() + IO_OPERATION_MS;
        if (round_deadline > deadline) round_deadline = deadline;
        struct mixed_reply parent = {0}, worker = {0};
        uint64_t nonce = ((uint64_t)(uint32_t)getpid() << 32) ^
                         (uint64_t)io_now_ms() ^ round;
        if (request_payload(MIXED_PARENT_PORT, MIXED_PARENT, MIXED_PING,
                            nonce, &parent, round_deadline) ||
            request_payload(MIXED_WORKER_PORT, MIXED_WORKER, MIXED_PING,
                            nonce ^ UINT64_C(0x8000000000000000),
                            &worker, round_deadline)) return -1;
        int exact = parent.peer_pid == worker.pid && worker.ppid == parent.pid &&
                    worker.peer_pid == parent.pid && parent.pid != worker.pid &&
                    parent.pid != (uint32_t)getpid() && worker.pid != (uint32_t)getpid();
        for (unsigned i = 0; i < capacity->highest_ready; i++) {
            if (parent.pid == capacity->slots[i].pid || worker.pid == capacity->slots[i].pid)
                exact = 0;
        }
        if (round && (parent.pid != payload_parent || worker.pid != payload_worker)) exact = 0;
        payload_parent = parent.pid;
        payload_worker = worker.pid;
        io_log("psmkit-mixed concurrent round=%u title_parent=%u local_READY=%u payload_parent=%u payload_worker=%u total_processes=%u identities_exact=%d",
               round, (unsigned)getpid(), capacity->highest_ready, payload_parent,
               payload_worker, 1 + capacity->highest_ready + 2, exact);
        if (!exact) { errno = EPROTO; return -1; }
        if (!round) {
            struct timespec pause = {.tv_nsec = 250000000};
            nanosleep(&pause, NULL);
        }
    }
    payload_parent_pid = payload_parent;
    payload_worker_pid = payload_worker;
    mixed_pass = 1;
    return 0;
}

int main(void)
{
    int64_t start = io_now_ms();
    struct app_status identity = {0};
    int identity_status = get_app_status(&identity);
    print_identity("psmkit-mixed parent", &identity);
    pool = sceKernelGetDirectMemorySize();
    io_log("psmkit-mixed parent main pid=%d target_total_processes=10 payload_ports=%u,%u direct_allocations=0",
           (int)getpid(), MIXED_PARENT_PORT, MIXED_WORKER_PORT);
    if (start >= 0 && identity_status >= 0 && identity.fields[0] > 0 && pool > 0 &&
        io_now_ms() - start <= IO_OPERATION_MS) {
        const struct cap_ops ops = {.now = io_now_ms, .pair = make_pair, .launch = launch,
            .transfer = io_transfer, .shutdown_write = stop_write, .close_fd = close,
            .stats = stats, .live = live, .held = held_gate};
        uint64_t seed = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)start;
        cap_run(&result, &ops, seed, (uint32_t)getpid(), (uint32_t)identity.fields[0], start);
    } else {
        result.cleanup_ok = 0;
        io_log("psmkit-mixed parent setup failed identity_status=%#x pool=%lld", (unsigned)identity_status, (long long)pool);
    }
    stop_payloads();
    int passed = mixed_pass && payload_cleanup && result.cleanup_ok;
    io_log("psmkit-mixed parent complete mixed_pass=%d payload_cleanup=%d local_READY=%u total_processes=%u cleanup=%d result=%s control_close=REQUIRED",
           mixed_pass, payload_cleanup, result.highest_ready,
           mixed_pass ? 1 + result.highest_ready + 2 : 0, result.cleanup_ok,
           passed ? "MIXED_PROCESS_PASS" : "FAIL_OR_INCONCLUSIVE");
    int64_t hold_end = io_now_ms() + CAP_HOLD_MS;
    for (;;) {
        int64_t now = io_now_ms();
        if (now < 0 || now >= hold_end) break;
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    _exit(passed ? 0 : 1);
}
