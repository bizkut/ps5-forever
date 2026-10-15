#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <time.h>
#include "psmkit-io.h"

int sceKernelDebugOutText(int channel, const char *text);

void io_log(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    strcat(line, "\n");
    sceKernelDebugOutText(0, line);
}

int64_t io_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Partial stream IO and EINTR stay within the ORIGINAL operation deadline;
 * they are not another launch/exchange attempt. Never block in send/recv. */
int io_transfer(int fd, void *buffer, size_t size, int writing, int64_t overall_deadline)
{
    int64_t now = io_now_ms();
    if (now < 0) return -1;
    int64_t deadline = now + IO_OPERATION_MS;
    if (deadline > overall_deadline) deadline = overall_deadline;
    unsigned char *bytes = buffer;
    size_t done = 0;
    while (done < size)
    {
        now = io_now_ms();
        if (now < 0) return -1;
        if (now >= deadline)
        {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd descriptor = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int result = poll(&descriptor, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (!result)
        {
            errno = ETIMEDOUT;
            return -1;
        }
        if (descriptor.revents & POLLNVAL)
        {
            errno = EBADF;
            return -1;
        }
        ssize_t count = writing ? send(fd, bytes + done, size - done, MSG_DONTWAIT | MSG_NOSIGNAL)
                                : recv(fd, bytes + done, size - done, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0) return -1;
        if (!count)
        {
            errno = 0;
            return 1; /* orderly EOF, distinct from a socket error */
        }
        done += (size_t)count;
    }
    now = io_now_ms();
    if (now < 0) return -1;
    if (now >= deadline)
    {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

uint32_t io_expected_checksum(void)
{
    uint32_t checksum = UINT32_C(2166136261);
    for (size_t page = 0; page < IO_PAGES; page++)
        checksum = (checksum ^ io_page_byte(page)) * UINT32_C(16777619);
    return checksum;
}
