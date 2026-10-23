/* Synthetic ELF layouts only: never execute a fixture or contact elfldr. */
#include <assert.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "addon-worker-elf.h"
#include "../../addon-worker/protocol.h"

#define FIXTURE_BYTES 1024u
#define SECTION_TABLE 128u
#define DATA_END 416u

static void put16(unsigned char *at, uint16_t value)
{
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *at, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++) at[index] = (unsigned char)(value >> (index * 8));
}

static void put64(unsigned char *at, uint64_t value)
{
    for (unsigned index = 0; index < 8; index++) at[index] = (unsigned char)(value >> (index * 8));
}

static void fixture(unsigned char image[FIXTURE_BYTES])
{
    memset(image, 0, FIXTURE_BYTES);
    memcpy(image, "\177ELF", 4);
    image[EI_CLASS] = ELFCLASS64;
    image[EI_DATA] = ELFDATA2LSB;
    image[EI_VERSION] = EV_CURRENT;
    put16(image + 16, ET_DYN);
    put16(image + 18, EM_X86_64);
    put32(image + 20, EV_CURRENT);
    put64(image + 32, 64);
    put64(image + 40, SECTION_TABLE);
    put16(image + 52, 64);
    put16(image + 54, 56);
    put16(image + 56, 1);
    put16(image + 58, 64);
    put16(image + 60, 3);
    put32(image + 64, PT_LOAD);
    put64(image + 64 + 32, DATA_END);
    put64(image + 64 + 40, 4096);
    put32(image + SECTION_TABLE + 64 + 4, SHT_PROGBITS);
    put64(image + SECTION_TABLE + 64 + 24, 384);
    put64(image + SECTION_TABLE + 64 + 32, 32);
    put32(image + SECTION_TABLE + 128 + 4, SHT_NOBITS);
    put64(image + SECTION_TABLE + 128 + 24, UINT64_MAX - 3);
    put64(image + SECTION_TABLE + 128 + 32, UINT64_MAX);
    memset(image + 384, 0x5a, 32);
    memset(image + DATA_END, 0xa5, FIXTURE_BYTES - DATA_END);
}

static int fixture_fd(const unsigned char *image, size_t size)
{
    char name[] = "/tmp/addon-worker-elf-XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);
    assert(!unlink(name));
    assert(write(fd, image, size) == (ssize_t)size);
    return fd;
}

static void exact_read(int fd, void *buffer, size_t size)
{
    unsigned char *bytes = buffer;
    size_t used = 0;
    while (used < size)
    {
        ssize_t count = read(fd, bytes + used, size - used);
        assert(count > 0);
        used += (size_t)count;
    }
}

/* Independent consumer model derived from the upstream elfldr_read framing:
 * receive Ehdr, receive through the section table, then through non-NOBITS
 * file data. Use native ELF structs here, not the production byte decoders. */
static size_t consume_elf(int fd)
{
    unsigned char received[FIXTURE_BYTES];
    Elf64_Ehdr header;
    size_t consumed, end;
    exact_read(fd, &header, sizeof(header));
    assert(sizeof(Elf64_Ehdr) == 64 && sizeof(Elf64_Shdr) == 64);
    consumed = header.e_shoff + header.e_shnum * sizeof(Elf64_Ehdr);
    assert(consumed >= sizeof(header) && consumed <= sizeof(received));
    memcpy(received, &header, sizeof(header));
    exact_read(fd, received + sizeof(header), consumed - sizeof(header));
    end = consumed;
    for (unsigned index = 0; index < header.e_shnum; index++)
    {
        Elf64_Shdr section;
        memcpy(&section, received + header.e_shoff + index * sizeof(section), sizeof(section));
        if (section.sh_type != SHT_NOBITS && section.sh_offset + section.sh_size > end)
            end = section.sh_offset + section.sh_size;
    }
    assert(end <= sizeof(received));
    exact_read(fd, received + consumed, end - consumed);
    return end;
}

static void startup_alignment(const unsigned char *image, size_t file_size,
                              uint64_t expected, int send_padding)
{
    unsigned char startup[AW_STARTUP_BYTES] = {0}, actual[AW_STARTUP_BYTES];
    unsigned char sent[FIXTURE_BYTES];
    uint64_t extent = UINT64_MAX;
    int sockets[2], fd = fixture_fd(image, file_size);
    assert(!pw_addon_worker_elf_extent(fd, file_size, &extent));
    assert(extent == expected);
    assert(lseek(fd, 0, SEEK_CUR) == 0);
    put32(startup, AW_MAGIC);
    put32(startup + 4, AW_VERSION);
    memset(startup + 8, 'k', AW_TOKEN_BYTES);
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    size_t size = send_padding ? file_size : (size_t)extent;
    exact_read(fd, sent, size);
    assert(write(sockets[0], sent, size) == (ssize_t)size);
    assert(write(sockets[0], startup, sizeof(startup)) == (ssize_t)sizeof(startup));
    assert(!shutdown(sockets[0], SHUT_WR));
    assert(consume_elf(sockets[1]) == expected);
    exact_read(sockets[1], actual, sizeof(actual));
    if (send_padding) assert(memcmp(actual, startup, sizeof(actual)));
    else
    {
        assert(!memcmp(actual, startup, sizeof(actual)));
        assert(!read(sockets[1], actual, 1));
    }
    assert(!close(sockets[0]));
    assert(!close(sockets[1]));
    assert(!close(fd));
}

static void rejected(const unsigned char *image, size_t actual_size, uint64_t declared_size)
{
    uint64_t extent = 0x1122334455667788ull;
    int fd = fixture_fd(image, actual_size);
    assert(pw_addon_worker_elf_extent(fd, declared_size, &extent) == -ENOEXEC);
    assert(extent == 0x1122334455667788ull);
    assert(!close(fd));
}

int main(void)
{
    unsigned char image[FIXTURE_BYTES];
    uint64_t extent = 0;
    fixture(image);
    startup_alignment(image, DATA_END, DATA_END, 0); /* ordinary section data after table */
    startup_alignment(image, sizeof(image), DATA_END, 0); /* trailing padding omitted */
    startup_alignment(image, sizeof(image), DATA_END, 1); /* old whole-file send corrupts startup */
    put16(image + 16, ET_EXEC);
    put64(image + SECTION_TABLE + 64 + 24, 120);
    put64(image + SECTION_TABLE + 64 + 32, 8);
    put64(image + 64 + 32, 320);
    startup_alignment(image, sizeof(image), 320, 0); /* table dominates, huge NOBITS ignored */

    /* Identity, version and exact record sizes. */
    const unsigned offsets[] = {0, EI_CLASS, EI_DATA, EI_VERSION, 16, 18, 20, 52, 54, 58};
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++)
    {
        fixture(image);
        image[offsets[index]] = 0;
        rejected(image, sizeof(image), sizeof(image));
    }
    fixture(image);
    rejected(image, 63, 63);
    rejected(image, 64, sizeof(image)); /* physical truncation despite supplied file size */
    rejected(image, sizeof(image), DATA_END - 1);
    rejected(image, sizeof(image), UINT64_MAX);

    /* Unsupported extended/absent tables and string-table indices. */
    const unsigned counts[] = {56, 60};
    for (size_t index = 0; index < sizeof(counts) / sizeof(counts[0]); index++)
    {
        fixture(image);
        put16(image + counts[index], 0);
        rejected(image, sizeof(image), sizeof(image));
        put16(image + counts[index], 0xffff);
        rejected(image, sizeof(image), sizeof(image));
    }
    fixture(image);
    put16(image + 60, 0xff00);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put16(image + 62, 3);
    rejected(image, sizeof(image), sizeof(image));
    put16(image + 62, SHN_XINDEX);
    rejected(image, sizeof(image), sizeof(image));

    /* Out-of-bounds, overflowing, header-overlapping and table-overlapping layouts. */
    const uint64_t bad_offsets[] = {0, 32, FIXTURE_BYTES - 1, UINT64_MAX - 31};
    for (size_t index = 0; index < sizeof(bad_offsets) / sizeof(bad_offsets[0]); index++)
    {
        fixture(image);
        put64(image + 40, bad_offsets[index]);
        rejected(image, sizeof(image), sizeof(image));
        fixture(image);
        put64(image + 32, bad_offsets[index]);
        rejected(image, sizeof(image), sizeof(image));
    }
    fixture(image);
    put64(image + 32, SECTION_TABLE);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + SECTION_TABLE + 64 + 24, UINT64_MAX - 15);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + SECTION_TABLE + 64 + 32, UINT64_MAX);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + SECTION_TABLE + 64 + 24, sizeof(image) + 1);
    put64(image + SECTION_TABLE + 64 + 32, 0);
    rejected(image, sizeof(image), sizeof(image));

    /* Program tables/segments must fit the prefix, not just the padded file. */
    fixture(image);
    memcpy(image + 512, image + 64, 56);
    put64(image + 32, 512);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + 64 + 32, DATA_END + 1);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + 64 + 8, UINT64_MAX - 15);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + 64 + 32, UINT64_MAX);
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + 64 + 40, DATA_END - 1); /* PT_LOAD filesz > memsz */
    rejected(image, sizeof(image), sizeof(image));
    fixture(image);
    put64(image + 64 + 8, DATA_END + 1);
    put64(image + 64 + 32, 0);
    rejected(image, sizeof(image), sizeof(image));

    assert(pw_addon_worker_elf_extent(-1, sizeof(image), &extent) == -EBADF);
    int pipes[2];
    assert(!pipe(pipes));
    assert(pw_addon_worker_elf_extent(pipes[0], sizeof(image), &extent) == -ESPIPE);
    assert(!close(pipes[0]));
    assert(!close(pipes[1]));
    assert(pw_addon_worker_elf_extent(-1, sizeof(image), NULL) == -EINVAL);
    puts("addon worker ELF: exact prefix/startup72 alignment, NOBITS, malformed layouts and I/O errors PASS");
    return 0;
}
