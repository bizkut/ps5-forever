#ifndef PS5_PSMKIT_DMEMSHARE_H
#define PS5_PSMKIT_DMEMSHARE_H

/* psmkit-dmemshare: can a PsmKit child map direct memory its title parent
 * allocated without spending flexible memory? The parent passes both the
 * physical offset and its already-open /dev/dmem1 descriptor over SCM_RIGHTS.
 * The child compares sceKernelMapDirectMemory, a child-opened dmem descriptor,
 * and the parent-opened descriptor, then writes through any successful view.
 * The parent verifies coherence through its original direct mapping. */
#include <stdint.h>
#include <stddef.h>

#define DS_BYTES ((size_t)64 << 20)
#define DS_PAGE ((size_t)0x4000)
#define DS_PAGES (DS_BYTES / DS_PAGE)
#define DS_MAGIC UINT32_C(0x44534831)

struct ds_request
{
    uint32_t magic, pages;
    int64_t physical;
};

struct ds_reply
{
    uint32_t magic;
    int32_t map_result;          /* sceKernelMapDirectMemory of the parent's offset */
    int32_t own_dmem_errno;      /* child-opened /dev/dmem1 mmap: 0 or errno */
    int32_t passed_dmem_errno;   /* parent-opened SCM_RIGHTS fd mmap: 0 or errno */
    uint32_t map_matches;
    uint32_t own_dmem_matches;
    uint32_t passed_dmem_matches;
    uint32_t written;
    int64_t flexible_before, flexible_mapped, flexible_after;
};

_Static_assert(sizeof(struct ds_request) == 16, "request wire size");
_Static_assert(sizeof(struct ds_reply) == 56, "reply wire size");

static inline unsigned char ds_parent_byte(size_t page) { return (unsigned char)(page * 37 + 0x5a); }
static inline unsigned char ds_child_byte(size_t page) { return (unsigned char)(page * 11 + 0xc3); }

#endif
