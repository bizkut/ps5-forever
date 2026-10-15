#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "probe.h"
#include "psmkit-nest.h"

_Static_assert(sizeof(struct local_process_param) == 0x90, "PsmKit parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "PsmKit socket offset");
void nest_log_hex(const char *role, const void *bytes, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t *raw = bytes;
    for (size_t offset = 0; offset < size; offset += 16) {
        char text[33];
        size_t count = size - offset < 16 ? size - offset : 16;
        for (size_t i = 0; i < count; i++) {
            text[2*i] = hex[raw[offset+i] >> 4];
            text[2*i+1] = hex[raw[offset+i] & 15];
        }
        text[2*count] = 0;
        io_log("psmkit-nest resolver raw role=%s offset=%#zx bytes=%s", role, offset, text);
    }
}
void nest_log_module(const char *role, const struct nest_module *module)
{
    const char *gate = nest_module_gate(module);
    io_log("psmkit-nest resolver layout role=%s size_field=%#zx caller_buffer_bytes=%#zx segment_count=%u name=%.*s terminated=%d module_gate=%s",
           role, module->size, sizeof(*module), module->segment_count,
           (int)sizeof(module->name), module->name,
           memchr(module->name, 0, sizeof(module->name)) != NULL, gate ? gate : "OK");
    for (unsigned i = 0; i < 4; i++) {
        const struct nest_segment *segment = &module->segments[i];
        uintptr_t base = (uintptr_t)segment->address;
        io_log("psmkit-nest resolver layout role=%s segment=%u address=%#llx size=%#x protection=%#x executable=%d base_size_overflow=%d",
               role, i, (unsigned long long)base, segment->size, (unsigned)segment->protection,
               !!(segment->protection & NEST_EXEC), base > UINTPTR_MAX - segment->size);
    }
    nest_log_hex(role, module, sizeof(*module));
}
void nest_init(struct nest_slot *slot)
{
    memset(slot, 0, sizeof(*slot));
    slot->cap.fd = slot->cap.child_fd = slot->queue = -1;
    slot->cap.handle = -1;
}
int nest_wait(int fd, int64_t deadline)
{
    for (;;) {
        int64_t now = io_now_ms();
        if (now < 0 || now >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd item = {.fd = fd, .events = POLLIN};
        int remaining = (int)(deadline - now);
        int status = poll(&item, 1, remaining < IO_OPERATION_MS ? remaining : IO_OPERATION_MS);
        if (status < 0 && errno == EINTR) continue;
        if (status) return status < 0 ? -1 : 0;
    }
}
int nest_add(struct nest_slot *slot, const char *path, const char *phase,
             unsigned index, uint64_t seed, uint32_t app, int64_t deadline,
             nest_launch_fn launch, const struct nest_export *descriptor)
{
    nest_init(slot);
    if (!launch) { errno = ENOSYS; return -1; }
    if (io_now_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
    int pair[2] = {-1, -1};
    errno = 0;
    int status = socketpair(AF_UNIX, SOCK_STREAM, 0, pair), error = errno;
    slot->cap.fd = pair[0]; slot->cap.child_fd = pair[1];
    io_log("psmkit-nest socketpair phase=%s caller=%u index=%u status=%d errno=%d endpoints=%d,%d", phase, (unsigned)getpid(), index, status, error, pair[0], pair[1]);
    if (status) return -1;
    cap_request(&slot->cap.request, index, seed, (uint32_t)getpid(), app);
    const char *const argv[] = {path, NULL};
    const struct local_process_param param = {.size = sizeof(param), .socket_fd = pair[1], .field_8 = 1, .field_c = -1, .field_10 = 1};
    slot->called = 1;
    int64_t before = io_now_ms();
    errno = 0;
    slot->cap.handle = launch(path, argv, &param);
    error = errno;
    slot->cap.started = slot->cap.handle >= 0;
    io_log("psmkit-nest add phase=%s caller=%u index=%u result=%#x errno=%d handle=%#x EXEC_pid=%s elapsed_ms=%lld path=%s", phase, (unsigned)getpid(), index, (unsigned)slot->cap.handle, error, (unsigned)slot->cap.handle, slot->cap.started ? "PENDING_READY" : "NONE", (long long)(io_now_ms()-before), path);
    errno = 0;
    status = close(pair[1]);
    io_log("psmkit-nest close-child-end phase=%s index=%u status=%d errno=%d", phase, index, status, errno);
    slot->cap.child_fd = -1;
    if (!slot->cap.started) { errno = error; return 1; }
    uint8_t wire[NEST_FRAME_BYTES];
    nest_frame_encode(wire, &slot->cap.request, descriptor);
    if (status || io_transfer(slot->cap.fd, wire, sizeof(wire), 1, deadline)) return -1;
    struct cap_frame ready = {0};
    if (nest_wait(slot->cap.fd, deadline) || io_transfer(slot->cap.fd, &ready, sizeof(ready), 0, deadline)) return -1;
    slot->cap.pid = ready.pid;
    int valid = cap_echo(&ready, &slot->cap.request, CAP_READY);
    io_log("psmkit-nest READY phase=%s caller=%u index=%u handle=%#x EXEC_pid=%u nonce=%#llx exact=%d", phase, (unsigned)getpid(), index, (unsigned)slot->cap.handle, ready.pid, (unsigned long long)ready.nonce, valid);
    if (!valid) { errno = EPROTO; return -1; }
    slot->cap.ready = 1;
    errno = 0;
    slot->queue = kqueue();
    io_log("psmkit-nest kqueue phase=%s index=%u result=%d errno=%d", phase, index, slot->queue, errno);
    if (slot->queue < 0) return -1;
    struct kevent change;
    EV_SET(&change, ready.pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    errno = 0;
    status = kevent(slot->queue, &change, 1, NULL, 0, NULL);
    error = errno;
    io_log("psmkit-nest NOTE_EXIT-register phase=%s index=%u EXEC_pid=%u status=%d errno=%d", phase, index, ready.pid, status, error);
    return status ? -1 : 0;
}
int nest_finish(struct nest_slot *slot, const char *phase, int64_t deadline)
{
    int ok = 1;
    if (slot->cap.started && slot->cap.fd >= 0) {
        struct cap_frame stop = slot->cap.request, done = {0};
        stop.kind = CAP_STOP;
        errno = 0;
        int status = io_transfer(slot->cap.fd, &stop, sizeof(stop), 1, deadline), error = errno;
        io_log("psmkit-nest STOP phase=%s index=%u status=%d errno=%d", phase, stop.index, status, error);
        if (status || shutdown(slot->cap.fd, SHUT_WR)) ok = 0;
        errno = 0;
        status = nest_wait(slot->cap.fd, deadline);
        if (!status) status = io_transfer(slot->cap.fd, &done, sizeof(done), 0, deadline);
        error = errno;
        int exact = !status && cap_echo(&done, &slot->cap.request, CAP_DONE) && (!slot->cap.ready || done.pid == slot->cap.pid);
        io_log("psmkit-nest DONE phase=%s index=%u EXEC_pid=%u status=%d errno=%d exact=%d", phase, stop.index, done.pid, status, error, exact);
        if (!exact) ok = 0;
        unsigned char extra;
        errno = 0;
        status = io_transfer(slot->cap.fd, &extra, 1, 0, deadline); error = errno;
        io_log("psmkit-nest EOF phase=%s index=%u status=%d errno=%d", phase, stop.index, status, error);
        if (status != 1 || error) ok = 0;
        if (slot->queue >= 0) {
            int64_t remaining = deadline - io_now_ms();
            if (remaining < 0) remaining = 0;
            if (remaining > IO_OPERATION_MS) remaining = IO_OPERATION_MS;
            struct timespec timeout = {.tv_sec = remaining / 1000, .tv_nsec = (remaining % 1000) * 1000000};
            struct kevent event = {0};
            errno = 0;
            status = kevent(slot->queue, NULL, 0, &event, 1, &timeout); error = errno;
            exact = status == 1 && event.ident == slot->cap.pid && event.filter == EVFILT_PROC && (event.fflags & NOTE_EXIT) && !(event.flags & EV_ERROR);
            io_log("psmkit-nest NOTE_EXIT phase=%s index=%u EXEC_pid=%u status=%d errno=%d flags=%#x fflags=%#x exit_data=%lld exact=%d at_ms=%lld", phase, stop.index, slot->cap.pid, status, error, event.flags, event.fflags, (long long)event.data, exact, (long long)io_now_ms());
            if (!exact || event.data) ok = 0;
        } else ok = 0;
    }
    int *fds[] = {&slot->cap.fd, &slot->cap.child_fd, &slot->queue};
    for (unsigned i = 0; i < 3; i++) if (*fds[i] >= 0) {
        errno = 0;
        int status = close(*fds[i]), error = errno;
        io_log("psmkit-nest close phase=%s index=%u fd=%d status=%d errno=%d", phase, slot->cap.request.index, *fds[i], status, error);
        if (status) ok = 0;
        *fds[i] = -1;
    }
    slot->cap.started = slot->cap.ready = 0;
    return ok ? 0 : -1;
}
int nest_command(int fd, const struct cap_frame *request, unsigned kind,
                 struct nest_report *report, int64_t deadline)
{
    struct cap_frame command = *request;
    command.kind = kind;
    if (io_transfer(fd, &command, sizeof(command), 1, deadline) || nest_wait(fd, deadline) ||
        io_transfer(fd, report, sizeof(*report), 0, deadline)) return -1;
    struct cap_frame expected = *request;
    expected.kind = NEST_REPORT; expected.pid = report->echo.pid;
    if (!expected.pid || memcmp(&expected, &report->echo, sizeof(expected))) { errno = EPROTO; return -1; }
    return 0;
}
