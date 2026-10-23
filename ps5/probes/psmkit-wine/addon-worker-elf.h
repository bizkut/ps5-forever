#ifndef PW_ADDON_WORKER_ELF_H
#define PW_ADDON_WORKER_ELF_H

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

static inline uint16_t pw_addon_worker_elf_u16(const unsigned char *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static inline uint32_t pw_addon_worker_elf_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static inline uint64_t pw_addon_worker_elf_u64(const unsigned char *bytes)
{
    return (uint64_t)pw_addon_worker_elf_u32(bytes) |
           (uint64_t)pw_addon_worker_elf_u32(bytes + 4) << 32;
}

static inline int pw_addon_worker_elf_read(int fd, uint64_t offset,
                                          unsigned char *bytes, size_t size)
{
    off_t position = (off_t)offset;
    size_t used = 0;
    if (position < 0 || (uint64_t)position != offset) return -ENOEXEC;
    if (lseek(fd, position, SEEK_SET) < 0) return -errno;
    while (used < size)
    {
        ssize_t count = read(fd, bytes + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -errno;
        if (!count) return -ENOEXEC;
        used += (size_t)count;
    }
    return 0;
}

/* elfldr_read consumes the section table and all non-NOBITS section data,
 * not necessarily the whole file. Send exactly this prefix before startup72:
 * trailing ELF padding would otherwise become the worker's stdin startup.
 * Framing facts: https://github.com/ps5-payload-dev/elfldr/blob/master/elfldr.c
 * (elfldr_read uses sizeof(Elf64_Ehdr), also 64, for its section-table extent).
 * The sender separately caps file_size at 2 MiB. No allocation or pread import.
 * On success, rewind fd for the sender; on failure, leave *extent untouched.
 * This validates file layout for framing, not executable code or relocations. */
static inline int pw_addon_worker_elf_extent(int fd, uint64_t file_size,
                                            uint64_t *extent)
{
    unsigned char bytes[64];
    uint64_t shoff, phoff, shend, phend, consumed;
    uint16_t shnum, phnum;
    int result;
    if (!extent) return -EINVAL;
    if (file_size < sizeof(bytes) || file_size > INT64_MAX) return -ENOEXEC;
    if ((result = pw_addon_worker_elf_read(fd, 0, bytes, sizeof(bytes)))) return result;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F' ||
        bytes[4] != 2 || bytes[5] != 1 || bytes[6] != 1 ||
        (pw_addon_worker_elf_u16(bytes + 16) != 2 &&
         pw_addon_worker_elf_u16(bytes + 16) != 3) ||
        pw_addon_worker_elf_u16(bytes + 18) != 62 ||
        pw_addon_worker_elf_u32(bytes + 20) != 1 ||
        pw_addon_worker_elf_u16(bytes + 52) != 64 ||
        pw_addon_worker_elf_u16(bytes + 54) != 56 ||
        pw_addon_worker_elf_u16(bytes + 58) != 64) return -ENOEXEC;
    phoff = pw_addon_worker_elf_u64(bytes + 32);
    shoff = pw_addon_worker_elf_u64(bytes + 40);
    phnum = pw_addon_worker_elf_u16(bytes + 56);
    shnum = pw_addon_worker_elf_u16(bytes + 60);
    /* Extended counts/indices need section-zero interpretation that elfldr
     * does not implement. Require ordinary executable header tables. */
    if (!phnum || phnum == 0xffff || !shnum || shnum >= 0xff00 ||
        pw_addon_worker_elf_u16(bytes + 62) >= shnum ||
        phoff < 64 || phoff > file_size ||
        (uint64_t)phnum * 56 > file_size - phoff ||
        shoff < 64 || shoff > file_size ||
        (uint64_t)shnum * 64 > file_size - shoff) return -ENOEXEC;
    phend = phoff + (uint64_t)phnum * 56;
    shend = shoff + (uint64_t)shnum * 64;
    if (phoff < shend && shoff < phend) return -ENOEXEC;
    consumed = shend;
    for (uint16_t index = 0; index < shnum; index++)
    {
        uint64_t offset, size;
        if ((result = pw_addon_worker_elf_read(fd, shoff + (uint64_t)index * 64,
                                              bytes, sizeof(bytes)))) return result;
        if (pw_addon_worker_elf_u32(bytes + 4) == 8) continue; /* SHT_NOBITS */
        offset = pw_addon_worker_elf_u64(bytes + 24);
        size = pw_addon_worker_elf_u64(bytes + 32);
        if (offset > file_size || size > file_size - offset) return -ENOEXEC;
        if (offset + size > consumed) consumed = offset + size;
    }
    /* Program headers do not increase elfldr's read extent. Refuse layouts
     * whose headers or segment data would be absent from its received prefix. */
    if (phend > consumed) return -ENOEXEC;
    for (uint16_t index = 0; index < phnum; index++)
    {
        uint64_t offset, size;
        if ((result = pw_addon_worker_elf_read(fd, phoff + (uint64_t)index * 56,
                                              bytes, 56))) return result;
        offset = pw_addon_worker_elf_u64(bytes + 8);
        size = pw_addon_worker_elf_u64(bytes + 32);
        if (offset > consumed || size > consumed - offset) return -ENOEXEC;
        if (pw_addon_worker_elf_u32(bytes) == 1 &&
            size > pw_addon_worker_elf_u64(bytes + 40)) return -ENOEXEC;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) return -errno;
    *extent = consumed;
    return 0;
}

#endif
