#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "probe.h"
#include "psmkit-two.h"

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[], const struct local_process_param *param);

_Static_assert(sizeof(struct local_process_param) == 0x90, "parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "socket parameter offset");

static int32_t launch(unsigned index, int pair[2], int *calls, int *valid, int64_t deadline)
{
    static const char path[] = "/app0/psmkit-two-child.bin";
    const char *const argv[] = {path, NULL};
    const struct local_process_param param = {
        .size = sizeof(param), .socket_fd = pair[1],
        .field_8 = 1, .field_c = -1, .field_10 = 1,
    };
    if (!two_within(deadline)) { *valid = 0; return -1; }
    io_log("psmkit-two parent launch index=%u path=%s param_size=%#x socket_fd=%d calls_next=%d",
           index, path, param.size, param.socket_fd, *calls + 1);
    int64_t start = io_now_ms();
    (*calls)++;
    errno = 0;
    int32_t handle = sceSystemServiceAddLocalProcessForPsmKit(path, argv, &param);
    int error = errno;
    int timely = two_operation(start, deadline);
    /* Drop the copied endpoint even on rejection; do not mask child EOF. */
    start = io_now_ms();
    errno = 0;
    int closed = close(pair[1]);
    int close_error = errno;
    pair[1] = -1;
    io_log("psmkit-two parent add index=%u handle=%#x errno=%d calls=%d handle_not_pid=1 timely=%d",
           index, (unsigned)handle, error, *calls, timely);
    io_log("psmkit-two parent close-child-endpoint index=%u result=%d errno=%d", index, closed, close_error);
    if (handle < 0 || closed || !timely || !two_operation(start, deadline)) *valid = 0;
    return handle;
}

static int reply_valid(const struct two_reply *reply, const struct two_request *request,
                       unsigned kind, int64_t pool)
{
    struct two_request expected = *request;
    expected.kind = kind;
    return !memcmp(&reply->echo, &expected, sizeof(expected)) && !reply->status &&
           reply->pool == pool && reply->physical == request->physical_start && reply->address &&
           reply->pid && reply->pid != (uint32_t)getpid() &&
           reply->before == TWO_SPAN && reply->during == TWO_GUARD &&
           reply->pages == TWO_PAGES && reply->checksum == two_checksum(request->index) &&
           (kind == TWO_READY ? reply->after == UINT64_MAX && reply->unmap_result == -1 && reply->release_result == -1
                             : reply->after == reply->before && !reply->unmap_result && !reply->release_result);
}

int main(void)
{
    int64_t start = io_now_ms(), deadline = start + TWO_OVERALL_MS;
    int pairs[2][2] = {{-1, -1}, {-1, -1}}, calls = 0, passed = 0, launches_valid = 1;
    int32_t handles[2] = {-1, -1};
    struct two_request requests[2] = {0};
    struct two_reply ready[2] = {0}, done[2] = {0};
    uint64_t before = 0, held = 0, after = 0;
    int64_t first = -1, pool = -1;
    int64_t ready_at[2] = {-1, -1}, first_go = -1;
    io_log("psmkit-two parent main pid=%d operation_ms=%d overall_ms=%d child_calls_limit=2 source=FIRMWARE_GETTER_CONSTANT",
           (int)getpid(), IO_OPERATION_MS, TWO_OVERALL_MS);
    if (start < 0 || !two_within(deadline)) goto complete;
    struct app_status status;
    start = io_now_ms();
    int result = get_app_status(&status);
    print_identity("psmkit-two parent", &status);
    if (result < 0 || status.fields[0] <= 0 || !two_operation(start, deadline)) goto complete;
    start = io_now_ms();
    pool = sceKernelGetDirectMemorySize();
    if (pool < (int64_t)(2 * TWO_SPAN) || !two_operation(start, deadline)) goto complete;
    if (two_available("parent", "before-launches", 0, pool, &before, &first, deadline) ||
        before < 2 * TWO_SPAN || first < 0 || first % TWO_GUARD ||
        first > pool - (int64_t)(2 * TWO_SPAN)) goto complete;
    for (unsigned i = 0; i < 2; i++)
    {
        requests[i] = (struct two_request){.magic = TWO_MAGIC, .version = TWO_VERSION,
            .kind = TWO_REQUEST, .index = i, .nonce = two_nonce(i),
            .physical_start = first + (int64_t)(i * TWO_SPAN)};
        two_cookie(i, requests[i].cookie);
        start = io_now_ms();
        errno = 0;
        result = socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]);
        io_log("psmkit-two parent socketpair index=%u result=%d errno=%d parent_fd=%d child_endpoint=%d nonce=%#llx cookie=%.*s",
               i, result, errno, pairs[i][0], pairs[i][1],
               (unsigned long long)requests[i].nonce, 16, requests[i].cookie);
        if (result || !two_operation(start, deadline)) goto complete;
    }
    /* Exactly these two launch invocations, in order, never a retry loop.
     * Even rejection of child0 does not cause an extra call or substitution. */
    handles[0] = launch(0, pairs[0], &calls, &launches_valid, deadline);
    handles[1] = launch(1, pairs[1], &calls, &launches_valid, deadline);
    if (!launches_valid || calls != 2 || handles[0] == handles[1]) goto complete;
    for (unsigned i = 0; i < 2; i++)
    {
        errno = 0;
        result = io_transfer(pairs[i][0], &requests[i], sizeof(requests[i]), 1, deadline);
        io_log("psmkit-two parent REQUEST index=%u result=%d errno=%d", i, result, errno);
        if (result) goto complete;
    }
    for (unsigned i = 0; i < 2; i++)
    {
        errno = 0;
        result = io_transfer(pairs[i][0], &ready[i], sizeof(ready[i]), 0, deadline);
        int valid = !result && reply_valid(&ready[i], &requests[i], TWO_READY, pool);
        ready_at[i] = io_now_ms();
        io_log("psmkit-two parent READY index=%u fd=%d handle=%#x pid=%u nonce=%#llx checksum=%#x result=%d errno=%d valid=%d held=1 at_ms=%lld",
               i, pairs[i][0], (unsigned)handles[i], ready[i].pid,
               (unsigned long long)ready[i].echo.nonce, ready[i].checksum,
               result, errno, valid, (long long)ready_at[i]);
        if (!valid || !two_within(deadline)) goto complete;
    }
    if (ready[0].pid == ready[1].pid || ready[0].echo.nonce == ready[1].echo.nonce ||
        !memcmp(ready[0].echo.cookie, ready[1].echo.cookie, 16) || pairs[0][0] == pairs[1][0]) goto complete;
    if (two_available("parent", "both-READY-held", 0, pool, &held, NULL, deadline)) goto complete;
    first_go = io_now_ms();
    if (first_go < 0 || first_go < ready_at[0] || first_go < ready_at[1]) goto complete;
    io_log("psmkit-two parent overlap both_READY=1 pids=%u,%u handles=%#x,%#x ready_ms=%lld,%lld first_go_ms=%lld largest_before=%llu held=%llu delta_ge_32MiB=%d",
           ready[0].pid, ready[1].pid, (unsigned)handles[0], (unsigned)handles[1],
           (long long)ready_at[0], (long long)ready_at[1], (long long)first_go,
           (unsigned long long)before, (unsigned long long)held,
           before >= held && before - held >= 2 * TWO_BYTES);
    for (unsigned i = 0; i < 2; i++)
    {
        struct two_request go = requests[i];
        go.kind = TWO_GO;
        errno = 0;
        result = io_transfer(pairs[i][0], &go, sizeof(go), 1, deadline);
        io_log("psmkit-two parent GO index=%u result=%d errno=%d both_READY=1", i, result, errno);
        if (result) goto complete;
        start = io_now_ms();
        errno = 0;
        result = shutdown(pairs[i][0], SHUT_WR);
        io_log("psmkit-two parent shutdown-write index=%u result=%d errno=%d", i, result, errno);
        if (result || !two_operation(start, deadline)) goto complete;
    }
    for (unsigned i = 0; i < 2; i++)
    {
        errno = 0;
        result = io_transfer(pairs[i][0], &done[i], sizeof(done[i]), 0, deadline);
        int valid = !result && reply_valid(&done[i], &requests[i], TWO_DONE, pool) &&
                    done[i].pid == ready[i].pid && done[i].address == ready[i].address;
        io_log("psmkit-two parent DONE index=%u pid=%u result=%d errno=%d checksum=%#x valid=%d before=%llu during=%llu after=%llu unmap=%#x release=%#x",
               i, done[i].pid, result, errno, done[i].checksum, valid,
               (unsigned long long)done[i].before, (unsigned long long)done[i].during,
               (unsigned long long)done[i].after, (unsigned)done[i].unmap_result, (unsigned)done[i].release_result);
        if (!valid) goto complete;
        unsigned char extra;
        errno = 0;
        result = io_transfer(pairs[i][0], &extra, 1, 0, deadline);
        int exact = result == 1 && !errno;
        io_log("psmkit-two parent EOF index=%u result=%d errno=%d exact=%d", i, result, errno, exact);
        if (!exact) goto complete;
    }
    if (two_available("parent", "after-both-DONE", 0, pool, &after, NULL, deadline)) goto complete;
    int global = before >= held && before - held >= 2 * TWO_BYTES && after == before;
    /* Largest free block is not total free memory. Independent bounded
     * physical-slot queries prove exact debits/recovery when CRT allocations
     * or ordering change that statistic. Every READY/DONE gate above is required. */
    io_log("psmkit-two parent memory global_exact=%d scoped_exact_16MiB_each=1 overlap=1 before=%llu held=%llu after=%llu proof=%s",
           global, (unsigned long long)before, (unsigned long long)held,
           (unsigned long long)after, global ? "GLOBAL_AND_SCOPED" : "SCOPED_OVERLAP");
    passed = two_within(deadline);
complete:
    for (unsigned i = 0; i < 2; i++)
        for (unsigned endpoint = 0; endpoint < 2; endpoint++)
        {
            if (pairs[i][endpoint] < 0) continue;
            start = io_now_ms();
            errno = 0;
            int closed = close(pairs[i][endpoint]);
            io_log("psmkit-two parent close index=%u endpoint=%u fd=%d result=%d errno=%d",
                   i, endpoint, pairs[i][endpoint], closed, errno);
            if (closed || !two_operation(start, deadline)) passed = 0;
        }
    passed = passed && calls == 2 && two_within(deadline);
    io_log("psmkit-two parent complete calls=%d result=%s actual_child_exit0=EXTERNAL_GATE app_vm_dmem=EXTERNAL_GATE fatal_gpu_va=EXTERNAL_GATE capture_hold_ms=15000 control_close=REQUIRED",
           calls, passed ? "TWO_CHILD_PROTOCOL_PASS" : "FAIL_OR_INCONCLUSIVE");
    int64_t hold_start = io_now_ms();
    if (hold_start >= 0)
    {
        while (two_within(hold_start + 15000))
        {
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
            nanosleep(&pause, NULL);
        }
    }
    else sleep(15);
    /* Runner MUST close us during the bounded hold. Never return into CRT. */
    _exit(passed ? 0 : 1);
}
