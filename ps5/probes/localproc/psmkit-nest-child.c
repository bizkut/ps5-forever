#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <unistd.h>
#include "psmkit-nest.h"
extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const libc_requirement = &Need_sceLibcInternal;
static struct nest_slot grandchildren[NEST_GRANDCHILDREN];
static nest_launch_fn resolve_export(const struct nest_export *descriptor)
{
    int32_t handle = -1;
    errno = 0;
    int status = sceSysmoduleLoadModuleInternal(NEST_MODULE_ID), error = errno;
    io_log("psmkit-nest resolver child load id=%#x status=%#x errno=%d", NEST_MODULE_ID, (unsigned)status, error);
    if (status) { io_log("psmkit-nest resolver child gate=MODULE_LOAD_CALL"); return NULL; }
    errno = 0;
    status = sceSysmoduleGetModuleHandleInternal(NEST_MODULE_ID, &handle); error = errno;
    io_log("psmkit-nest resolver child handle status=%#x errno=%d handle=%#x", (unsigned)status, error, (unsigned)handle);
    if (status || handle < 0) { io_log("psmkit-nest resolver child gate=%s", status ? "MODULE_HANDLE_CALL" : "MODULE_HANDLE_NEGATIVE"); return NULL; }
    struct nest_module module = {.size = sizeof(module)};
    errno = 0;
    status = sceKernelGetModuleInfo(handle, &module); error = errno;
    nest_log_module("child-internal", &module);
    uintptr_t address = 0;
    struct nest_validation validation = {.reason = "MODULE_INFO_CALL", .segment = UINT32_MAX};
    int valid = !status && nest_export_resolve(descriptor, &module, &address, &validation);
    const char *descriptor_gate = nest_descriptor_gate(descriptor);
    io_log("psmkit-nest resolver child gate=%s failed_segment=%u containment_matches=%u descriptor_gate=%s",
           validation.reason ? validation.reason : "OK", validation.segment, validation.matches,
           descriptor_gate ? descriptor_gate : "OK");
    io_log("psmkit-nest resolver child descriptor count=%u index=%u size=%#x protection=%#x offset=%#llx base=%#llx reserved=%#x name=%.*s",
           descriptor->segment_count, descriptor->segment_index, descriptor->segment_size,
           (unsigned)descriptor->segment_protection, (unsigned long long)descriptor->offset,
           (unsigned long long)descriptor->parent_base, descriptor->reserved,
           (int)sizeof(descriptor->name), descriptor->name);
    char fingerprint[41];
    nest_fingerprint(fingerprint, module.fingerprint);
    int fingerprint_match = !memcmp(descriptor->fingerprint, module.fingerprint, sizeof(module.fingerprint));
    uintptr_t base = descriptor->segment_index < 4 ? (uintptr_t)module.segments[descriptor->segment_index].address : 0;
    io_log("psmkit-nest resolver child info status=%#x errno=%d name=%.*s segments=%u fingerprint=%s fingerprint_match=%d",
           (unsigned)status, error, (int)sizeof(module.name), module.name, module.segment_count, fingerprint, fingerprint_match);
    io_log("psmkit-nest resolver child segment=%u parent_base=%#llx child_base=%#llx offset=%#llx target=%#llx exact=%d nesting_skipped=%d",
           descriptor->segment_index, (unsigned long long)descriptor->parent_base,
           (unsigned long long)base, (unsigned long long)descriptor->offset,
           (unsigned long long)address, valid, !valid);
    return valid ? (nest_launch_fn)address : NULL;
}
int main(void)
{
    int64_t deadline = io_now_ms() + NEST_MS;
    struct cap_frame request = {0}, reply = {0};
    struct nest_export descriptor = {0};
    uint8_t wire[NEST_FRAME_BYTES];
    nest_launch_fn launch = NULL;
    int protocol = 0, cleanup = 1, stage = 0;
    for (unsigned i = 0; i < NEST_GRANDCHILDREN; i++) nest_init(&grandchildren[i]);
    if (io_transfer(NEST_SOCKET, wire, sizeof(wire), 0, deadline) ||
        !nest_frame_decode(&request, &descriptor, wire)) {
        io_log("psmkit-nest child frame rejected=1 nesting_skipped=1 errno=%d", errno);
        goto complete;
    }
    if (request.index == 0) launch = resolve_export(&descriptor);
    reply = request; reply.kind = CAP_READY; reply.pid = (uint32_t)getpid();
    if (io_transfer(NEST_SOCKET, &reply, sizeof(reply), 1, deadline)) goto complete;
    io_log("psmkit-nest child READY index=%u EXEC_pid=%u parent_pid=%u nonce=%#llx main_threads=1", request.index, reply.pid, request.parent_pid, (unsigned long long)request.nonce);
    for (;;) {
        struct cap_frame command = {0}, expected = request;
        if (nest_wait(NEST_SOCKET, deadline) || io_transfer(NEST_SOCKET, &command, sizeof(command), 0, deadline)) break;
        expected.kind = command.kind;
        if (memcmp(&expected, &command, sizeof(command))) break;
        if (command.kind == CAP_STOP) {
            unsigned char extra;
            int eof = io_transfer(NEST_SOCKET, &extra, 1, 0, deadline);
            if (eof != 1 || errno) break;
            protocol = 1; break;
        }
        if (request.index != 0) break;
        struct nest_report report = {.echo = reply, .cleanup = 1};
        report.echo.kind = NEST_REPORT;
        if ((command.kind == NEST_A && stage == 0) || (command.kind == NEST_B && stage == 1)) {
            unsigned limit = command.kind == NEST_A ? 1 : NEST_GRANDCHILDREN;
            if (!launch) {
                report.status = 1; report.error = ENOEXEC; stage = -1;
                io_log("psmkit-nest resolver child rejected=1 phase-A/B/C/D_skipped=1 add_calls=0");
            }
            const char *phase = command.kind == NEST_A ? "A-grandchild" : "B-grandchild";
            for (unsigned i = 0; launch && i < limit; i++) {
                for (unsigned j = 0; j < report.ready; j++) {
                    struct pollfd fd = {.fd = grandchildren[j].cap.fd, .events = POLLIN};
                    errno = 0;
                    int live = poll(&fd, 1, 0);
                    if (live) {
                        io_log("psmkit-nest lost-held-grandchild phase=%s index=%u EXEC_pid=%u status=%d errno=%d revents=%#x",
                               phase, j, grandchildren[j].cap.pid, live, errno, fd.revents);
                        report.status = -1; report.error = errno ? errno : EPIPE;
                        break;
                    }
                }
                if (report.status < 0) break;
                errno = 0;
                int status = nest_add(&grandchildren[i], "/app0/psmkit-nest-grandchild.bin", phase, 16 + i,
                                      request.nonce ^ ((uint64_t)getpid() << 32) ^ (uint64_t)command.kind, request.app_id, deadline,
                                      launch, NULL);
                report.calls += grandchildren[i].called;
                report.status = status; report.error = errno;
                if (status) break;
                report.ready++;
                report.peak = report.ready;
            }
            if (command.kind == NEST_A) {
                for (unsigned i = 0; i < NEST_GRANDCHILDREN; i++) if (nest_finish(&grandchildren[i], "A-release", deadline)) report.cleanup = 0;
                stage = report.status || !report.cleanup ? -1 : 1;
            } else stage = 2;
        } else if (command.kind == NEST_RELEASE && stage == 2) {
            for (unsigned i = 0; i < NEST_GRANDCHILDREN; i++) if (nest_finish(&grandchildren[i], "B-release", deadline)) report.cleanup = 0;
            stage = 3;
        } else break;
        if (!report.cleanup) cleanup = 0;
        io_log("psmkit-nest child phase-report command=%u calls=%u READY=%u peak=%u status=%d errno=%d cleanup=%d", command.kind, report.calls, report.ready, report.peak, report.status, report.error, report.cleanup);
        if (io_transfer(NEST_SOCKET, &report, sizeof(report), 1, deadline)) break;
    }
complete:
    for (unsigned i = 0; i < NEST_GRANDCHILDREN; i++) if (nest_finish(&grandchildren[i], "child-cleanup", deadline)) cleanup = 0;
    if (protocol) {
        reply.kind = CAP_DONE;
        if (io_transfer(NEST_SOCKET, &reply, sizeof(reply), 1, deadline)) protocol = 0;
    }
    shutdown(NEST_SOCKET, SHUT_WR); close(NEST_SOCKET);
    io_log("psmkit-nest child complete index=%u EXEC_pid=%u protocol=%d cleanup=%d exit=0 actual_exit=EXTERNAL_GATE", request.index, (unsigned)getpid(), protocol, cleanup);
    return 0;
}
