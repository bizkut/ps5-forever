#include <errno.h>
#include <unistd.h>
#include "protocol.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement = &Need_sceLibcInternal;
int sceKernelGetProcessType(pid_t pid);

int main(void)
{
    int64_t start = io_now_ms(), deadline = start + PW_OVERALL_MS;
    io_log("psmkit-wine child main pid=%d proc_type=%#x fd=3 source=FIRMWARE_GETTER_CONSTANT gate=SCM_RIGHTS_PREREQUISITE",
           (int)getpid(), (unsigned)sceKernelGetProcessType(getpid()));
    int valid = start >= 0 && pw_socket_valid(3);
    io_log("psmkit-wine child fd3 valid=%d contract=AF_UNIX_SOCK_STREAM runtime=NOT_STARTED", valid);
    int result = valid ? pw_child_session(3, deadline) : 1;
    if (valid && close(3)) result = 1;
    io_log("psmkit-wine child complete result=%s exit=%d runtime=NOT_STARTED shared_wineserver=NOT_STARTED",
           !result ? "DESCRIPTOR_ONLY" : "FAIL_OR_INCONCLUSIVE", result);
    return result;
}
