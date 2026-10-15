#include <errno.h>
#include <string.h>
#include <ps5platform/kernel.h>
#include "psmkit-two.h"

uint64_t two_nonce(unsigned index)
{
    return UINT64_C(0x50322d4e4f4e4300) + index;
}

void two_cookie(unsigned index, unsigned char cookie[16])
{
    static const unsigned char base[16] = "P1-PsmKit-Two-00";
    memcpy(cookie, base, 16);
    cookie[14] = (unsigned char)('0' + index);
}

int two_request_valid(const struct two_request *request, unsigned kind)
{
    unsigned char cookie[16];
    if (request->index > 1) return 0;
    two_cookie(request->index, cookie);
    return request->magic == TWO_MAGIC && request->version == TWO_VERSION &&
           request->kind == kind && request->nonce == two_nonce(request->index) &&
           !memcmp(request->cookie, cookie, sizeof(cookie));
}

uint32_t two_checksum(unsigned index)
{
    uint32_t checksum = UINT32_C(2166136261);
    for (size_t page = 0; page < TWO_PAGES; page++)
        checksum = (checksum ^ two_page_byte(index, page)) * UINT32_C(16777619);
    return checksum;
}

int two_within(int64_t deadline)
{
    int64_t now = io_now_ms();
    if (now < 0) return 0;
    if (now < deadline) return 1;
    errno = ETIMEDOUT;
    return 0;
}

int two_operation(int64_t start, int64_t overall)
{
    return start >= 0 && two_within(start + IO_OPERATION_MS) && two_within(overall);
}

int two_available(const char *role, const char *phase, int64_t from, int64_t to,
                  uint64_t *bytes, int64_t *at, int64_t deadline)
{
    int64_t start = io_now_ms(), physical = -1;
    size_t size = 0;
    if (!two_within(deadline)) return -1;
    errno = 0;
    int32_t result = sceKernelAvailableDirectMemorySize(from, to, TWO_GUARD, &physical, &size);
    int error = errno;
    io_log("psmkit-two %s available phase=%s result=%#x errno=%d from=%#llx to=%#llx at=%#llx largest_bytes=%zu",
           role, phase, (unsigned)result, error, (unsigned long long)from,
           (unsigned long long)to, (unsigned long long)physical, size);
    if (result || !two_operation(start, deadline)) return -1;
    *bytes = size;
    if (at) *at = physical;
    return 0;
}
