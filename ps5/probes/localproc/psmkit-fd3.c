#include <errno.h>
#include <stddef.h>
#include <ps5platform/kernel.h>
#include "psmkit-fd3.h"

_Static_assert(sizeof(struct ps5_kernel_sw_version) == 0x28, "firmware record size");
_Static_assert(offsetof(struct ps5_kernel_sw_version, version) == 0x24, "firmware value offset");

void fd3_log_firmware(const char *role)
{
    struct ps5_kernel_sw_version version = {.size = sizeof(version)};
    errno = 0;
    int32_t result = sceKernelGetSystemSwVersion(&version);
    int error = errno;
    io_log("psmkit-fd3 %s firmware result=%#x errno=%d version=%#x policy=CAPABILITY_VALIDATION",
           role, (unsigned)result, error, version.version);
}

int fd3_within_deadline(int64_t deadline)
{
    int64_t now = io_now_ms();
    return now >= 0 && now < deadline;
}
