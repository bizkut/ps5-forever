#include <errno.h>
#include <string.h>
#include "psmkit-capacity.h"

const char *cap_category_name(enum cap_category category)
{
    static const char *const names[] = {"none", "launch", "socketpair", "READY", "time", "resource"};
    return names[category];
}

static int timely(const struct cap_ops *ops, int64_t before, int64_t deadline)
{
    int64_t now = ops->now();
    return before >= 0 && now >= before && now - before <= IO_OPERATION_MS && now < deadline;
}

static void refusal(struct cap_result *result, unsigned index, enum cap_category category,
                    int status, int error)
{
    if (result->failed_index >= 0) return;
    result->failed_index = (int)index;
    result->category = category;
    result->status = status;
    result->error = error;
    io_log("psmkit-capacity parent first-failure index=%u category=%s status=%#x errno=%d launch_again=0",
           index, cap_category_name(category), (unsigned)status, error);
}

void cap_run(struct cap_result *result, const struct cap_ops *ops, uint64_t seed,
             uint32_t parent_pid, uint32_t app_id, int64_t start)
{
    memset(result, 0, sizeof(*result));
    result->failed_index = -1;
    result->cleanup_ok = 1;
    for (unsigned i = 0; i < CAP_LIMIT; i++) {
        result->slots[i].fd = result->slots[i].child_fd = -1;
        result->slots[i].handle = -1;
    }
    int64_t launch_deadline = start + CAP_LAUNCH_MS;
    int64_t deadline = start + CAP_PROTOCOL_MS;
    errno = 0;
    int status = ops->stats("before", 0, launch_deadline), error = errno;
    if (start < 0 || status) {
        refusal(result, 0, start < 0 ? CAP_TIME : CAP_RESOURCE, status, error);
        goto cleanup;
    }
    for (unsigned i = 0; i < CAP_LIMIT; i++) {
        struct cap_slot *slot = &result->slots[i];
        int64_t before = ops->now();
        if (before < 0 || before >= launch_deadline) {
            refusal(result, i, CAP_TIME, -1, ETIMEDOUT); break;
        }
        for (unsigned j = 0; j < i; j++) {
            errno = 0;
            status = ops->live(result->slots[j].fd); error = errno;
            if (status) {
                refusal(result, i, CAP_READY_ERROR, status, error);
                io_log("psmkit-capacity parent lost-held-child failed_next_index=%u lost_index=%u status=%d errno=%d",
                       i, j, status, error);
                goto cleanup;
            }
        }
        cap_request(&slot->request, i, seed, parent_pid, app_id);
        int pair[2] = {-1, -1};
        errno = 0;
        status = ops->pair(pair); error = errno;
        slot->fd = pair[0]; slot->child_fd = pair[1];
        io_log("psmkit-capacity parent socketpair index=%u status=%d errno=%d parent_fd=%d child_fd=%d",
               i, status, error, pair[0], pair[1]);
        if (status || !timely(ops, before, launch_deadline)) {
            refusal(result, i, status ? CAP_SOCKETPAIR : CAP_TIME, status, error); break;
        }
        before = ops->now();
        errno = 0;
        result->calls++;
        slot->handle = ops->launch(slot->child_fd); error = errno;
        int launch_timely = timely(ops, before, launch_deadline);
        slot->started = slot->handle >= 0;
        result->started += slot->started;
        io_log("psmkit-capacity parent launch index=%u handle=%#x errno=%d calls=%u handle_not_pid=1 timely=%d",
               i, (unsigned)slot->handle, error, result->calls, launch_timely);
        before = ops->now();
        errno = 0;
        status = ops->close_fd(slot->child_fd); int close_error = errno;
        slot->child_fd = -1;
        io_log("psmkit-capacity parent close-child-endpoint index=%u status=%d errno=%d", i, status, close_error);
        if (slot->handle < 0 || !launch_timely || status || !timely(ops, before, launch_deadline)) {
            refusal(result, i, slot->handle < 0 ? CAP_LAUNCH : (!launch_timely ? CAP_TIME : CAP_RESOURCE),
                    slot->handle < 0 ? slot->handle : status, slot->handle < 0 ? error : close_error); break;
        }
        errno = 0;
        status = ops->transfer(slot->fd, &slot->request, sizeof(slot->request), 1, launch_deadline); error = errno;
        io_log("psmkit-capacity parent REQUEST index=%u nonce=%#llx parent_pid=%u app_id=%u status=%d errno=%d",
               i, (unsigned long long)slot->request.nonce, parent_pid, app_id, status, error);
        if (status) { refusal(result, i, error == ETIMEDOUT ? CAP_TIME : CAP_READY_ERROR, status, error); break; }
        struct cap_frame ready = {0};
        errno = 0;
        status = ops->transfer(slot->fd, &ready, sizeof(ready), 0, launch_deadline); error = errno;
        slot->pid = ready.pid;
        int valid = !status && cap_echo(&ready, &slot->request, CAP_READY);
        for (unsigned j = 0; j < i; j++) {
            const struct cap_slot *previous = &result->slots[j];
            if (slot->pid == previous->pid || slot->handle == previous->handle || slot->fd == previous->fd ||
                ready.nonce == previous->request.nonce || !memcmp(ready.cookie, previous->request.cookie, 16)) valid = 0;
        }
        io_log("psmkit-capacity parent READY index=%u handle=%#x pid=%u nonce=%#llx status=%d errno=%d child_status=%#x exact_distinct=%d held=1",
               i, (unsigned)slot->handle, slot->pid, (unsigned long long)ready.nonce,
               status, error, (unsigned)ready.status, valid);
        if (!valid) { refusal(result, i, error == ETIMEDOUT ? CAP_TIME : CAP_READY_ERROR,
                             status ? status : (ready.status ? ready.status : -1), error); break; }
        for (unsigned j = 0; j <= i; j++) {
            errno = 0;
            status = ops->live(result->slots[j].fd); error = errno;
            if (status) {
                refusal(result, i, CAP_READY_ERROR, status, error);
                io_log("psmkit-capacity parent lost-held-child failed_index=%u lost_index=%u status=%d errno=%d",
                       i, j, status, error);
                goto cleanup;
            }
        }
        int64_t ready_at = ops->now();
        if (ready_at < 0 || ready_at >= launch_deadline) {
            refusal(result, i, CAP_TIME, -1, ETIMEDOUT); break;
        }
        slot->ready = 1;
        result->highest_ready++;
        if (result->highest_ready == 4 || result->highest_ready == 8 || result->highest_ready == 16 ||
            result->highest_ready == 32 || result->highest_ready == 64) {
            errno = 0;
            status = ops->stats("checkpoint-held", result->highest_ready, launch_deadline); error = errno;
            io_log("psmkit-capacity parent checkpoint live_READY=%u distinct_handles=%u distinct_pids=%u status=%#x errno=%d",
                   result->highest_ready, result->highest_ready, result->highest_ready, (unsigned)status, error);
            if (status) { refusal(result, i, CAP_RESOURCE, status, error); break; }
        }
    }
    result->ceiling = result->highest_ready == CAP_LIMIT && result->failed_index < 0;
    if (ops->held) {
        errno = 0;
        status = ops->held(result, deadline); error = errno;
        io_log("psmkit-capacity parent held-gate live_READY=%u status=%d errno=%d before_cleanup=1",
               result->highest_ready, status, error);
        if (status) result->cleanup_ok = 0;
    }
cleanup:
    /* Broadcast STOP to EVERY started child before waiting on any DONE. */
    for (unsigned i = 0; i < CAP_LIMIT; i++) {
        struct cap_slot *slot = &result->slots[i];
        if (!slot->started || slot->fd < 0) continue;
        struct cap_frame stop = slot->request;
        stop.kind = CAP_STOP;
        errno = 0;
        status = ops->transfer(slot->fd, &stop, sizeof(stop), 1, deadline); error = errno;
        slot->stop = !status;
        io_log("psmkit-capacity parent STOP index=%u started=1 READY=%d status=%d errno=%d", i, slot->ready, status, error);
        errno = 0;
        int shut = ops->shutdown_write(slot->fd), shut_error = errno;
        io_log("psmkit-capacity parent shutdown-write index=%u status=%d errno=%d", i, shut, shut_error);
        if (status || shut) result->cleanup_ok = 0;
    }
    for (unsigned i = 0; i < CAP_LIMIT; i++) {
        struct cap_slot *slot = &result->slots[i];
        if (slot->started && slot->fd >= 0) {
            struct cap_frame done = {0};
            errno = 0;
            status = ops->transfer(slot->fd, &done, sizeof(done), 0, deadline); error = errno;
            slot->done = !status && cap_echo(&done, &slot->request, CAP_DONE) &&
                         (!slot->ready || done.pid == slot->pid);
            io_log("psmkit-capacity parent DONE index=%u pid=%u status=%d errno=%d child_status=%#x exact=%d",
                   i, done.pid, status, error, (unsigned)done.status, slot->done);
            unsigned char extra;
            errno = 0;
            status = ops->transfer(slot->fd, &extra, 1, 0, deadline); error = errno;
            slot->eof = status == 1 && !error;
            io_log("psmkit-capacity parent EOF index=%u status=%d errno=%d exact=%d", i, status, error, slot->eof);
            if (!slot->done || !slot->eof) result->cleanup_ok = 0;
        }
        int *fds[] = {&slot->fd, &slot->child_fd};
        for (unsigned j = 0; j < 2; j++) {
            if (*fds[j] < 0) continue;
            errno = 0;
            status = ops->close_fd(*fds[j]); error = errno;
            io_log("psmkit-capacity parent close index=%u endpoint=%u fd=%d status=%d errno=%d", i, j, *fds[j], status, error);
            if (status) result->cleanup_ok = 0;
            *fds[j] = -1;
        }
    }
    errno = 0;
    status = ops->stats("after-channels-close", 0, deadline); error = errno;
    if (status) result->cleanup_ok = 0;
    int64_t now = ops->now();
    if (now < 0 || now >= deadline) result->cleanup_ok = 0;
    io_log("psmkit-capacity parent external-gates actual_exit0=REQUIRED_EVERY_STARTED_PID final_app_count0=REQUIRED memory_recovered_after_control_close=REQUIRED fatal_gpu=NONE_REQUIRED capture_hold_ms=15000 control_close=REQUIRED");
    io_log("psmkit-capacity parent complete calls=%u started=%u highest_fully_READY=%u first_failed_index=%d category=%s status=%#x errno=%d reached64_ceiling=%d conclusion=%s cleanup=%s elapsed_ms=%lld",
           result->calls, result->started, result->highest_ready, result->failed_index,
           cap_category_name(result->category), (unsigned)result->status, result->error, result->ceiling,
           result->ceiling ? "LOWER_BOUND_NOT_MAXIMUM" : "FIRST_REFUSAL_NOT_GLOBAL_MAXIMUM",
           result->cleanup_ok ? "DONE_EOF_CLOSED" : "FAIL_OR_INCONCLUSIVE", (long long)(now - start));
}
