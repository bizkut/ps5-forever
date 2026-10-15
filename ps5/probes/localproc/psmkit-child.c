/* PsmKit local-process child: use only modules the CPU-helper class actually
 * preloads. The first probe imported libSceSystemService and faulted through
 * its null GetAppStatus slot before logging main; this variant has no
 * SystemService import. Kernel EXEC lines establish app id/vm#/dmem#.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;

int sceKernelDebugOutText(int channel, const char *text);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelGetProcessType(pid_t pid);

static void log_line(const char *format, ...)
{
    char line[512];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    strcat(line, "\n");
    sceKernelDebugOutText(0, line);
}

int main(void)
{
    pid_t pid = getpid();
    int type = sceKernelGetProcessType(pid);
    int64_t direct = sceKernelGetDirectMemorySize();

    /* This public process type is diagnostic only; it is not the private
     * libSceSysmodule process-info flag 0x40. */
    log_line("psmkit child main pid=%d proc_type=%#x direct=%lld MiB works=1",
             (int)pid, (unsigned)type, (long long)(direct >> 20));
    sleep(15);
    log_line("psmkit child complete pid=%d", (int)pid);
    return 0;
}
