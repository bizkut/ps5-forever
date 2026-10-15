#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "psmkit-detect.h"

int sceKernelDebugOutText(int channel, const char *text);

void detect_log(const char *format, ...)
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

int64_t detect_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int detect_within_deadline(int64_t deadline)
{
    int64_t now = detect_now_ms();
    return now >= 0 && now < deadline;
}
