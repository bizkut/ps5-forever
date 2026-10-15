#ifndef PS5_PSMKIT_TWO_H
#define PS5_PSMKIT_TWO_H

#include "psmkit-io.h"

#define TWO_BYTES ((size_t)16 << 20)
#define TWO_PAGE ((size_t)0x4000)
#define TWO_PAGES (TWO_BYTES / TWO_PAGE)
#define TWO_GUARD ((size_t)0x10000)
#define TWO_SPAN (TWO_BYTES + TWO_GUARD)
#define TWO_MAGIC UINT32_C(0x50325457)
#define TWO_VERSION 1
#define TWO_OVERALL_MS 30000
#define TWO_SOCKET 3

enum two_kind { TWO_REQUEST = 1, TWO_READY = 2, TWO_GO = 3, TWO_DONE = 4 };

/* Fixed-width, padding-free native little-endian frames; no pointers on wire. */
struct two_request
{
    uint32_t magic, version, kind, index;
    uint64_t nonce;
    unsigned char cookie[16];
    int64_t physical_start;
};

struct two_reply
{
    struct two_request echo;
    int64_t pool, physical;
    uint64_t address, before, during, after;
    uint32_t pid, checksum, pages;
    int32_t status, unmap_result, release_result;
};

_Static_assert(sizeof(struct two_request) == 48, "request/GO wire size");
_Static_assert(sizeof(struct two_reply) == 120, "READY/DONE wire size");

uint64_t two_nonce(unsigned index);
void two_cookie(unsigned index, unsigned char cookie[16]);
int two_request_valid(const struct two_request *request, unsigned kind);
uint32_t two_checksum(unsigned index);
int two_within(int64_t deadline);
int two_operation(int64_t start, int64_t overall);
int two_available(const char *role, const char *phase, int64_t from, int64_t to,
                  uint64_t *bytes, int64_t *at, int64_t deadline);

static inline unsigned char two_page_byte(unsigned index, size_t page)
{
    return (unsigned char)(page * 37 + 0x5a + index * 0x63);
}

#endif
