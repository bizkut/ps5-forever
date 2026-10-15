#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "probe.h"

int32_t sceSystemServiceGetAppStatus(struct app_status *status);
int sceKernelDebugOutText(int channel, const char *text);

void probe_log(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0) return;
    line[sizeof(line) - 1] = '\0';
    sceKernelDebugOutText(0, line);
}


int get_app_status(struct app_status *status)
{
    for (unsigned int i = 0; i < sizeof(status->fields) / sizeof(status->fields[0]); i++)
        status->fields[i] = 0;
    return sceSystemServiceGetAppStatus(status);
}

void print_identity(const char *role, const struct app_status *status)
{
    probe_log("%s pid=%d app_status=%#x,%#x,%#x,%#x direct=%llu MiB\n",
              role, getpid(), (unsigned)status->fields[0], (unsigned)status->fields[1],
              (unsigned)status->fields[2], (unsigned)status->fields[3],
              (unsigned long long)(sceKernelGetDirectMemorySize() >> 20));
}
