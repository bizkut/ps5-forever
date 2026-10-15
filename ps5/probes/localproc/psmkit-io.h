#ifndef PS5_PSMKIT_IO_H
#define PS5_PSMKIT_IO_H

#include <stdint.h>
#include <stddef.h>

#define IO_BYTES ((size_t)64 << 20)
#define IO_PAGES (IO_BYTES / 0x4000)
#define IO_MAGIC UINT32_C(0x50494f31)
#define IO_VERSION 1
#define IO_COOKIE "P1-PsmKit-IO-550"
#define IO_COOKIE_BYTES 16
#define IO_OPERATION_MS 2000
#define IO_OVERALL_MS 20000

struct io_request
{
    uint32_t magic, version;
    unsigned char cookie[IO_COOKIE_BYTES];
};

struct io_reply
{
    uint32_t magic, version;
    unsigned char cookie[IO_COOKIE_BYTES];
    uint64_t before, during, after;
    uint32_t checksum, pages;
    int32_t memory_status, socket_status;
};

_Static_assert(sizeof(struct io_request) == 24, "request wire size");
_Static_assert(sizeof(struct io_reply) == 64, "reply wire size");
_Static_assert(sizeof(IO_COOKIE) - 1 == IO_COOKIE_BYTES, "cookie wire size");

void io_log(const char *format, ...);
int64_t io_now_ms(void);
int io_transfer(int fd, void *buffer, size_t size, int writing, int64_t overall_deadline);
uint32_t io_expected_checksum(void);

static inline unsigned char io_page_byte(size_t page)
{
    return (unsigned char)(page * 37 + 0x5a);
}

#endif
