/* HOST SIMULATION ONLY: no native launch, process capacity or console evidence. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "psmkit-capacity.h"

enum fault { NONE, REFUSE, SOCKET_REFUSE, DUP_PID, DUP_NONCE, DUP_HANDLE, TIMEOUT, DISCONNECT, RESOURCE, DONE_BAD, EOF_EXTRA, STOP_FAIL, HELD_FAIL };
static enum fault fault;
static unsigned at, calls, pairs, stopped, started, closed, checkpoints, held_calls;
static int64_t clock_ms;
static unsigned open_fds[CAP_LIMIT * 2], stop_seen[CAP_LIMIT], read_step[CAP_LIMIT];
static struct cap_frame requests[CAP_LIMIT];
static struct cap_result result;
void io_log(const char *format, ...) { (void)format; }
static int64_t now(void) { return clock_ms; }
static int pair(int descriptors[2]) {
    unsigned i = pairs++;
    if (fault == SOCKET_REFUSE && i == at) { errno = EMFILE; return -1; }
    descriptors[0] = 100 + (int)i * 2; descriptors[1] = descriptors[0] + 1;
    open_fds[i * 2] = open_fds[i * 2 + 1] = 1;
    return 0;
}
static int32_t launch(int fd) {
    unsigned i = calls++;
    assert(fd == 101 + (int)i * 2);
    if (fault == REFUSE && i == at) { errno = ENOMEM; return (int32_t)UINT32_C(0x8002000c); }
    started++;
    return fault == DUP_HANDLE && i == at ? 1000 : 1000 + (int32_t)i;
}
static int close_fd(int fd) {
    unsigned n = (unsigned)(fd - 100);
    assert(n < CAP_LIMIT * 2 && open_fds[n]);
    open_fds[n] = 0; closed++; return 0;
}
static int shutdown_write(int fd) { assert(fd >= 100); return 0; }
static int live(int fd) {
    unsigned i = (unsigned)(fd - 100) / 2;
    if (fault == DISCONNECT && calls == at && i == 0) { errno = ECONNRESET; return 16; }
    return 0;
}
static int stats(const char *phase, unsigned ready, int64_t deadline) {
    assert(deadline >= clock_ms);
    if (!strcmp(phase, "checkpoint-held")) {
        assert(ready == 4 || ready == 8 || ready == 16 || ready == 32 || ready == 64);
        checkpoints++;
        if (fault == RESOURCE && ready == at) { errno = ENOMEM; return -12; }
    }
    return 0;
}
static int held_gate(const struct cap_result *capacity, int64_t deadline) {
    assert(deadline >= clock_ms && stopped == 0);
    for (unsigned i = 0; i < capacity->highest_ready; i++)
        assert(capacity->slots[i].ready && open_fds[i * 2]);
    held_calls++;
    if (fault == HELD_FAIL) { errno = EIO; return -1; }
    return 0;
}
static int transfer(int fd, void *buffer, size_t size, int writing, int64_t deadline) {
    unsigned i = (unsigned)(fd - 100) / 2;
    assert(i < CAP_LIMIT && open_fds[i * 2]);
    assert(deadline > clock_ms);
    if (writing) {
        assert(size == sizeof(struct cap_frame));
        struct cap_frame *frame = buffer;
        if (frame->kind == CAP_REQUEST) {
            assert(i == calls - 1 && i == started - 1);
            requests[i] = *frame;
            assert(cap_request_valid(frame));
        } else {
            assert(frame->kind == CAP_STOP && !stop_seen[i]);
            stop_seen[i] = 1; stopped++;
            if (fault == STOP_FAIL && i == at) { errno = EPIPE; return -1; }
        }
        return 0;
    }
    if (!stop_seen[i]) {
        if (fault == TIMEOUT && i == at) { clock_ms += IO_OPERATION_MS; errno = ETIMEDOUT; return -1; }
        struct cap_frame *frame = buffer;
        assert(size == sizeof(*frame));
        *frame = requests[i]; frame->kind = CAP_READY; frame->pid = 2000 + i;
        if (fault == DUP_PID && i == at) frame->pid = 2000;
        if (fault == DUP_NONCE && i == at) frame->nonce = requests[0].nonce;
        return 0;
    }
    /* Every started child must receive STOP before ANY terminal wait. */
    assert(stopped == started);
    if (read_step[i]++ == 0) {
        assert(size == sizeof(struct cap_frame));
        struct cap_frame *frame = buffer;
        *frame = requests[i]; frame->kind = CAP_DONE;
        frame->pid = fault == DUP_PID && i == at ? 2000 : 2000 + i;
        if (fault == DONE_BAD && i == at) frame->nonce++;
        if (fault == DISCONNECT && i == 0) { errno = 0; return 1; }
        return 0;
    }
    assert(size == 1);
    errno = 0;
    return fault == EOF_EXTRA && i == at ? 0 : 1;
}
static void run(enum fault mode, unsigned index) {
    fault = mode; at = index; calls = pairs = stopped = started = closed = checkpoints = held_calls = 0; clock_ms = 100;
    memset(open_fds, 0, sizeof(open_fds)); memset(stop_seen, 0, sizeof(stop_seen));
    memset(read_step, 0, sizeof(read_step)); memset(requests, 0, sizeof(requests));
    const struct cap_ops ops = {.now = now, .pair = pair, .launch = launch, .transfer = transfer,
        .shutdown_write = shutdown_write, .close_fd = close_fd, .stats = stats, .live = live,
        .held = held_gate};
    cap_run(&result, &ops, UINT64_C(0x12340000), 77, 123, clock_ms);
    assert(result.calls == calls && result.started == started && stopped == started);
    for (unsigned i = 0; i < CAP_LIMIT * 2; i++) assert(!open_fds[i]);
    for (unsigned i = 0; i < CAP_LIMIT; i++) assert(result.slots[i].fd == -1 && result.slots[i].child_fd == -1);
}
int main(void) {
    const unsigned refusals[] = {0, 1, 4, 63};
    for (unsigned j = 0; j < sizeof(refusals) / sizeof(refusals[0]); j++) {
        unsigned i = refusals[j]; run(REFUSE, i);
        assert(calls == i + 1 && result.highest_ready == i && result.failed_index == (int)i);
        assert(result.category == CAP_LAUNCH && result.status == (int32_t)UINT32_C(0x8002000c) && result.error == ENOMEM);
        assert(result.cleanup_ok && !result.ceiling && held_calls == 1);
    }
    run(NONE, 0); assert(calls == 64 && result.highest_ready == 64 && result.ceiling && result.cleanup_ok && checkpoints == 5 && held_calls == 1);
    run(SOCKET_REFUSE, 4); assert(calls == 4 && result.highest_ready == 4 && result.category == CAP_SOCKETPAIR && result.error == EMFILE);
    run(DUP_PID, 4); assert(calls == 5 && result.highest_ready == 4 && result.category == CAP_READY_ERROR && result.cleanup_ok);
    run(DUP_NONCE, 4); assert(calls == 5 && result.highest_ready == 4 && result.category == CAP_READY_ERROR && result.cleanup_ok);
    run(DUP_HANDLE, 4); assert(calls == 5 && result.highest_ready == 4 && result.category == CAP_READY_ERROR && result.cleanup_ok);
    run(TIMEOUT, 4); assert(calls == 5 && result.highest_ready == 4 && result.category == CAP_TIME && result.cleanup_ok);
    run(DISCONNECT, 4); assert(calls == 4 && result.highest_ready == 3 && result.category == CAP_READY_ERROR && !result.cleanup_ok && held_calls == 0);
    run(RESOURCE, 4); assert(calls == 4 && result.highest_ready == 4 && result.category == CAP_RESOURCE && result.status == -12 && result.cleanup_ok && held_calls == 1);
    run(DONE_BAD, 4); assert(calls == 64 && !result.cleanup_ok && held_calls == 1);
    run(EOF_EXTRA, 4); assert(calls == 64 && !result.cleanup_ok && held_calls == 1);
    run(STOP_FAIL, 4); assert(calls == 64 && !result.cleanup_ok && held_calls == 1);
    run(HELD_FAIL, 0); assert(calls == 64 && !result.cleanup_ok && held_calls == 1);
    struct cap_frame frame;
    cap_request(&frame, 0, 999, 77, 123); assert(cap_request_valid(&frame));
    frame.cookie[5] ^= 1; assert(!cap_request_valid(&frame));
    puts("HOST_SIMULATION_PASS refusal0/1/4/63 socketpair duplicatePID/nonces/handles timeout disconnect resource held-gate-before-cleanup STOP/DONE/EOF cleanup ceiling64; NOT_NATIVE_EVIDENCE");
    return 0;
}
