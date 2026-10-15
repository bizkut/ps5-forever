#include <errno.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "probe.h"
#include "psmkit-nest.h"
int32_t sceSystemServiceAddLocalProcessForPsmKit(const char *, const char *const [],
                                               const struct local_process_param *);
static struct nest_export export_descriptor;
static int capture_export(void)
{
    int32_t handle = -1;
    errno = 0;
    int status = sceSysmoduleLoadModuleInternal(NEST_MODULE_ID), error = errno;
    io_log("psmkit-nest resolver parent load id=%#x status=%#x errno=%d", NEST_MODULE_ID, (unsigned)status, error);
    if (status) { io_log("psmkit-nest resolver parent gate=MODULE_LOAD_CALL"); return -1; }
    errno = 0;
    status = sceSysmoduleGetModuleHandleInternal(NEST_MODULE_ID, &handle); error = errno;
    io_log("psmkit-nest resolver parent handle status=%#x errno=%d handle=%#x", (unsigned)status, error, (unsigned)handle);
    if (status || handle < 0) { io_log("psmkit-nest resolver parent gate=%s", status ? "MODULE_HANDLE_CALL" : "MODULE_HANDLE_NEGATIVE"); return -1; }
    struct nest_module module = {.size = sizeof(module)};
    errno = 0;
    status = sceKernelGetModuleInfo(handle, &module); error = errno;
    nest_log_module("parent-internal", &module);
    uintptr_t address = (uintptr_t)sceSystemServiceAddLocalProcessForPsmKit;
    uint64_t from_address[0x1a8 / sizeof(uint64_t)] = {0};
    errno = 0;
    int address_status = sceKernelGetModuleInfoFromAddr((const void *)address, 1, from_address);
    int address_error = errno;
    io_log("psmkit-nest resolver parent address-query address=%#llx flags=1 status=%#x errno=%d output_size=%#llx",
           (unsigned long long)address, (unsigned)address_status, address_error,
           (unsigned long long)from_address[0]);
    nest_log_hex("parent-from-address", from_address, sizeof(from_address));
    struct nest_validation validation = {.reason = "MODULE_INFO_CALL", .segment = UINT32_MAX};
    int valid = !status && nest_export_capture(&export_descriptor, &module, address, &validation);
    const char *descriptor_gate = nest_descriptor_gate(&export_descriptor);
    io_log("psmkit-nest resolver parent gate=%s failed_segment=%u containment_matches=%u descriptor_gate=%s",
           validation.reason ? validation.reason : "OK", validation.segment, validation.matches,
           descriptor_gate ? descriptor_gate : "OK");
    io_log("psmkit-nest resolver parent descriptor count=%u index=%u size=%#x protection=%#x offset=%#llx base=%#llx reserved=%#x name=%.*s",
           export_descriptor.segment_count, export_descriptor.segment_index, export_descriptor.segment_size,
           (unsigned)export_descriptor.segment_protection, (unsigned long long)export_descriptor.offset,
           (unsigned long long)export_descriptor.parent_base, export_descriptor.reserved,
           (int)sizeof(export_descriptor.name), export_descriptor.name);
    char fingerprint[41];
    nest_fingerprint(fingerprint, module.fingerprint);
    io_log("psmkit-nest resolver parent info status=%#x errno=%d name=%.*s segments=%u linked_address=%#llx contained_exec_exactly_once=%d fingerprint=%s",
           (unsigned)status, error, (int)sizeof(module.name), module.name, module.segment_count,
           (unsigned long long)address, valid, fingerprint);
    if (!valid) {
        io_log("psmkit-nest resolver parent rejected=1 nesting_skipped=1 no_stub_unwrapping=1 no_hardcoded_offset=1");
        return -1;
    }
    io_log("psmkit-nest resolver parent segment=%u base=%#llx size=%#x protection=%#x offset=%#llx frame_version=%u",
           export_descriptor.segment_index, (unsigned long long)export_descriptor.parent_base,
           export_descriptor.segment_size, (unsigned)export_descriptor.segment_protection,
           (unsigned long long)export_descriptor.offset, NEST_FRAME_VERSION);
    return 0;
}
static struct nest_slot children[NEST_CHILDREN + 1];
static int64_t pool;
static int stats(const char *phase, unsigned ready)
{
    int64_t physical = -1;
    size_t direct = 0, flexible = 0;
    errno = 0;
    int status = sceKernelAvailableDirectMemorySize(0, pool, 0x10000, &physical, &direct), error = errno;
    io_log("psmkit-nest stats phase=%s READY=%u direct_status=%#x errno=%d largest_available_direct=%llu physical=%#llx total_free_claim=0", phase, ready, (unsigned)status, error, (unsigned long long)direct, (unsigned long long)physical);
    errno = 0;
    int flex_status = sceKernelAvailableFlexibleMemorySize(&flexible), flex_error = errno;
    io_log("psmkit-nest stats phase=%s flexible_status=%#x errno=%d available_flexible=%llu", phase, (unsigned)flex_status, flex_error, (unsigned long long)flexible);
    return status || flex_status ? -1 : 0;
}
static int held(unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        struct pollfd fd = {.fd = children[i].cap.fd, .events = POLLIN};
        errno = 0;
        int status = poll(&fd, 1, 0);
        if (status) {
            io_log("psmkit-nest lost-held index=%u EXEC_pid=%u status=%d errno=%d revents=%#x", i, children[i].cap.pid, status, errno, fd.revents);
            return -1;
        }
    }
    return 0;
}
int main(void)
{
    int64_t start = io_now_ms(), deadline = start + NEST_WORK_MS;
    unsigned calls = 0, ready = 0, peak = 0;
    int eligible = 0, parent_refused = 0, reuse = -1, cleanup = 1;
    struct app_status app = {0};
    for (unsigned i = 0; i <= NEST_CHILDREN; i++) nest_init(&children[i]);
    int identity = get_app_status(&app);
    print_identity("psmkit-nest parent", &app);
    pool = sceKernelGetDirectMemorySize();
    uint64_t seed = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)start;
    io_log("psmkit-nest parent main EXEC_pid=%u add_cap=18 grandchild_cap=8 protocol_ms=22000 overall_ms=30000 explicit_threads_per_process=1 probe_allocations=0 control_close=REQUIRED", (unsigned)getpid());
    if (start < 0 || identity < 0 || app.fields[0] <= 0 || pool <= 0 || stats("before", 0)) { cleanup = 0; goto cleanup; }
    if (capture_export()) {
        io_log("psmkit-nest phase-A/B/C/D skipped reason=PARENT_RESOLVER_REJECTED cleanup_only=1");
        goto cleanup;
    }
    errno = 0;
    int status = nest_add(&children[0], "/app0/psmkit-nest-child.bin", "A-C0", 0, seed, (uint32_t)app.fields[0], deadline,
                          sceSystemServiceAddLocalProcessForPsmKit, &export_descriptor);
    calls += children[0].called;
    if (status) { cleanup = status == 1; goto cleanup; }
    ready = peak = 1;
    struct nest_report report = {0};
    if (nest_command(children[0].cap.fd, &children[0].cap.request, NEST_A, &report, deadline)) { cleanup = 0; goto cleanup; }
    calls += report.calls;
    peak = ready + report.peak;
    eligible = report.status == 0 && report.ready == 1 && report.cleanup;
    io_log("psmkit-nest phase-A eligible=%d status=%d errno=%d calls=%u concurrent_READY=%u cleanup=%d", eligible, report.status, report.error, calls, peak, report.cleanup);
    if (!eligible) { cleanup = report.cleanup; io_log("psmkit-nest phase-B/C/D skipped eligibility_refused_or_faulted=1 cleanup_only=1"); goto cleanup; }
    for (unsigned i = 1; i < NEST_CHILDREN; i++) {
        if (held(ready)) { cleanup = 0; goto cleanup; }
        errno = 0;
        status = nest_add(&children[i], "/app0/psmkit-nest-child.bin", "B-parent", i, seed, (uint32_t)app.fields[0], deadline,
                          sceSystemServiceAddLocalProcessForPsmKit, NULL);
        calls += children[i].called;
        if (status) { parent_refused = status == 1; if (status < 0) cleanup = 0; break; }
        ready++; if (ready > peak) peak = ready;
    }
    if (!cleanup || held(ready)) { cleanup = 0; goto cleanup; }
    if (nest_command(children[0].cap.fd, &children[0].cap.request, NEST_B, &report, deadline)) { cleanup = 0; goto cleanup; }
    calls += report.calls;
    if (ready + report.peak > peak) peak = ready + report.peak;
    io_log("psmkit-nest phase-B live_children=%u grandchild_admitted_at_seven=%d grandchildren_READY=%u concurrent_READY=%u parent_first_refusal=%d grandchild_status=%d grandchild_errno=%d total_calls=%u", ready, ready == 7 && report.ready > 0, report.ready, ready + report.ready, parent_refused, report.status, report.error, calls);
    if (report.status < 0) { cleanup = 0; goto cleanup; }
    if (nest_command(children[0].cap.fd, &children[0].cap.request, NEST_RELEASE, &report, deadline) || !report.cleanup) { cleanup = 0; goto cleanup; }
    /* One boundary call establishes the parent's first refusal at seven live.
     * A success is held, reported and stops expansion; it is not discarded. */
    if (ready == NEST_CHILDREN) {
        if (held(ready)) { cleanup = 0; goto cleanup; }
        status = nest_add(&children[7], "/app0/psmkit-nest-child.bin", "B-parent-boundary", 7, seed, (uint32_t)app.fields[0], deadline,
                          sceSystemServiceAddLocalProcessForPsmKit, NULL);
        calls += children[7].called;
        parent_refused = status == 1;
        if (status < 0) { cleanup = 0; goto cleanup; }
        if (!status) { ready++; if (ready > peak) peak = ready; }
    }
    if (ready > 1) {
        int64_t release_at = io_now_ms();
        if (nest_finish(&children[ready - 1], "C-release-one", deadline)) { cleanup = 0; goto cleanup; }
        ready--;
        int64_t note_at = io_now_ms();
        unsigned index = ready;
        status = nest_add(&children[index], "/app0/psmkit-nest-child.bin", "C-reuse-once", index, seed ^ UINT64_C(0x8000000000000000), (uint32_t)app.fields[0], deadline,
                          sceSystemServiceAddLocalProcessForPsmKit, NULL);
        calls += children[index].called;
        reuse = status == 0;
        if (status < 0) cleanup = 0;
        if (!status) ready++;
        io_log("psmkit-nest phase-C attempted=1 admitted=%d status=%d total_calls=%u release_to_NOTE_EXIT_ms=%lld NOTE_EXIT_to_READY_ms=%lld retries=0", reuse, status, calls, (long long)(note_at-release_at), (long long)(io_now_ms()-note_at));
    } else io_log("psmkit-nest phase-C attempted=0 reason=no_nonC0_child");
cleanup:
    for (unsigned i = NEST_CHILDREN + 1; i > 0; i--) if (nest_finish(&children[i-1], "D-cleanup", deadline)) cleanup = 0;
    if (stats("after-exits", 0)) cleanup = 0;
    io_log("psmkit-nest parent complete calls=%u eligibility=%d max_concurrent_READY=%u max_concurrent_processes=%u parent_refused=%d slot_reuse=%d cleanup=%d elapsed_ms=%lld parent_control_close=REQUIRED actual_exit_status=KLOG_GATE", calls, eligible, peak, peak + 1, parent_refused, reuse, cleanup, (long long)(io_now_ms()-start));
    /* Never use the unqualified BigApp CRT-exit path. External runner owns
     * the combined 30-second watchdog and control-close, including stalled APIs. */
    for (;;) { struct timespec delay = {.tv_nsec = 100000000}; nanosleep(&delay, NULL); }
}
