/* App-local AMD64 ELF loading. No SDK payload CRT or private dynlib APIs. */
#include "elf-runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include "../localproc/psmkit-io.h"
#include <ps5platform/kernel.h>
static void init_trace(const char *api, const char *phase, long result)
{
    int saved_errno = errno;
    io_log("psmkit-wine startup api=%s phase=%s result=%ld errno=%d",
           api, phase, result, saved_errno);
    errno = saved_errno;
}
#else
#define init_trace(api, phase, result) ((void)(result))
#endif

struct ehdr { unsigned char ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phsize, phnum, shsize, shnum, shstr; };
struct phdr { uint32_t type, flags; uint64_t offset, address, physical, filesz, memsz, align; };
struct dyn { int64_t tag; uint64_t value; };
struct sym { uint32_t name; unsigned char info, other; uint16_t section; uint64_t value, size; };
struct rela { uint64_t offset, info; int64_t addend; };
struct tls_block { void *allocation[PW_ELF_MODULES]; };
_Static_assert(sizeof(struct ehdr) == 64 && sizeof(struct phdr) == 56 &&
               sizeof(struct sym) == 24 && sizeof(struct rela) == 24, "ELF64 layout");

enum {
    PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PT_TLS = 7,
    PT_GNU_STACK = 0x6474e551, PT_GNU_RELRO = 0x6474e552,
    PF_X = 1, PF_W = 2, PF_R = 4
};

struct sha256_context {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
};
static uint32_t rotate_right(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32 - shift));
}
static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void store_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
}
static void sha256_transform(struct sha256_context *context, const unsigned char block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t words[64];
    for (unsigned i = 0; i < 16; ++i) words[i] = load_be32(block + i * 4);
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t x = words[i - 15], y = words[i - 2];
        uint32_t a = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
        uint32_t b = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10);
        words[i] = words[i - 16] + a + words[i - 7] + b;
    }
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2];
    uint32_t d = context->state[3], e = context->state[4], f = context->state[5];
    uint32_t g = context->state[6], h = context->state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + sum1 + choice + constants[i] + words[i];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}
static void sha256_init(struct sha256_context *context)
{
    static const uint32_t initial[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    memcpy(context->state, initial, sizeof(initial));
    context->length = 0; context->used = 0;
}
static void sha256_update(struct sha256_context *context, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    context->length += (uint64_t)size;
    while (size) {
        size_t available = sizeof(context->block) - context->used;
        size_t take = size < available ? size : available;
        memcpy(context->block + context->used, bytes, take);
        context->used += take; bytes += take; size -= take;
        if (context->used == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->used = 0;
        }
    }
}
static void sha256_final(struct sha256_context *context, unsigned char digest[32])
{
    uint64_t bits = context->length << 3;
    unsigned char padding[64] = {0x80};
    size_t padding_size = context->used < 56 ? 56 - context->used : 120 - context->used;
    sha256_update(context, padding, padding_size);
    unsigned char length[8];
    for (unsigned i = 0; i < 8; ++i) length[7 - i] = (unsigned char)(bits >> (i * 8));
    sha256_update(context, length, sizeof(length));
    for (unsigned i = 0; i < 8; ++i) store_be32(digest + i * 4, context->state[i]);
}
void pw_elf_sha256(const void *data, size_t size, uint8_t digest[32])
{
    struct sha256_context context;
    sha256_init(&context); sha256_update(&context, data, size); sha256_final(&context, digest);
}
static int fail(struct pw_elf_runtime *r, const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vsnprintf(r->error, sizeof(r->error), fmt, args); va_end(args);
    return -1;
}
static int range(uint64_t offset, uint64_t size, uint64_t limit)
{ return offset <= limit && size <= limit - offset; }
static int power2(uint64_t n) { return n && !(n & (n - 1)); }
static void *at(struct pw_elf_module *m, uint64_t address, uint64_t size, unsigned flags)
{
    for (unsigned i = 0; i < m->segment_count; ++i) {
        struct pw_elf_segment *s = &m->segments[i];
        if (address >= s->address && range(address - s->address, size, s->size) &&
            (s->flags & flags) == flags && address <= UINTPTR_MAX - m->bias)
            return (void *)(m->bias + address);
    }
    return NULL;
}
static const char *string_at(struct pw_elf_module *m, uint64_t offset)
{
    if (offset >= m->string_size) return NULL;
    const char *p = at(m, m->strings + offset, m->string_size - offset, 4);
    return p && memchr(p, 0, m->string_size - offset) ? p : NULL;
}
static int fetch(int fd, void *buffer, size_t size, uint64_t offset)
{
    if (offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset) return -1;
    while (size) {
        ssize_t got = pread(fd, buffer, size, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        buffer = (char *)buffer + got; size -= (size_t)got; offset += (uint64_t)got;
    }
    return 0;
}
static void copy_file_range(const unsigned char *buffer, uint64_t offset, size_t size,
                            uint64_t source, uint64_t source_size, void *destination)
{
    uint64_t end = offset + size, source_end = source + source_size;
    uint64_t first = offset > source ? offset : source;
    uint64_t last = end < source_end ? end : source_end;
    if (first < last)
        memcpy((char *)destination + (size_t)(first - source),
               buffer + (size_t)(first - offset), (size_t)(last - first));
}
static int hash_and_load(struct pw_elf_runtime *r, int fd, const struct stat *identity,
                         const struct ehdr *header, const struct phdr *headers,
                         struct pw_elf_module *module, const uint8_t expected[32],
                         uint64_t expected_size, const char *path)
{
    struct sha256_context context;
    struct ehdr authenticated_header;
    struct phdr authenticated_headers[PW_ELF_SEGMENTS];
    unsigned char buffer[4096], digest[32];
    uint64_t offset = 0;
    memset(&authenticated_header, 0, sizeof(authenticated_header));
    memset(authenticated_headers, 0, sizeof(authenticated_headers));
    sha256_init(&context);
    while (offset < expected_size) {
        size_t want = expected_size - offset < sizeof(buffer) ?
                      (size_t)(expected_size - offset) : sizeof(buffer);
        ssize_t got = pread(fd, buffer, want, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return fail(r, "SHA-256 read failed: %s", path);
        size_t bytes = (size_t)got;
        sha256_update(&context, buffer, bytes);
        copy_file_range(buffer, offset, bytes, 0, sizeof(authenticated_header),
                        &authenticated_header);
        copy_file_range(buffer, offset, bytes, header->phoff,
                        header->phnum * sizeof(authenticated_headers[0]),
                        authenticated_headers);
        for (unsigned i = 0; i < header->phnum; ++i)
            if (headers[i].type == PT_LOAD && headers[i].filesz)
                copy_file_range(buffer, offset, bytes, headers[i].offset,
                                headers[i].filesz,
                                (char *)module->mapping + headers[i].address);
        offset += bytes;
    }
    sha256_final(&context, digest);
    struct stat after;
    if (fstat(fd, &after) || !S_ISREG(after.st_mode) || after.st_size < 0 ||
        (uint64_t)after.st_size != expected_size ||
        after.st_dev != identity->st_dev || after.st_ino != identity->st_ino)
        return fail(r, "module identity changed while hashing: %s", path);
    unsigned difference = 0;
    for (unsigned i = 0; i < sizeof(digest); ++i) difference |= digest[i] ^ expected[i];
    if (difference) return fail(r, "SHA-256 mismatch: %s", path);
    if (memcmp(header, &authenticated_header, sizeof(*header)) ||
        memcmp(headers, authenticated_headers, header->phnum * sizeof(headers[0])))
        return fail(r, "ELF layout changed while hashing: %s", path);
    return 0;
}
static void tls_free(void *data)
{
    struct tls_block *b = data;
    if (!b) return;
    for (unsigned i = 0; i < PW_ELF_MODULES; ++i) free(b->allocation[i]);
    free(b);
}
int pw_elf_init(struct pw_elf_runtime *r, const struct pw_elf_import *imports, size_t count,
                const char *const *systems, size_t system_count)
{
    memset(r, 0, sizeof(*r));
    init_trace("sysconf(_SC_PAGESIZE)", "BEGIN", 0);
    long page = sysconf(_SC_PAGESIZE);
    init_trace("sysconf(_SC_PAGESIZE)", "END", page);
    if (page <= 0 || !power2((uint64_t)page))
        return fail(r, "sysconf(_SC_PAGESIZE) result=%ld errno=%d", page, errno);
    r->page_size = (size_t)page; r->imports = imports; r->import_count = count;
    r->system_modules = systems; r->system_count = system_count;
    pthread_mutexattr_t attributes;
    init_trace("pthread_mutexattr_init", "BEGIN", 0);
    int result = pthread_mutexattr_init(&attributes);
    init_trace("pthread_mutexattr_init", "END", result);
    if (result) return fail(r, "pthread_mutexattr_init result=%d", result);
    init_trace("pthread_mutexattr_settype", "BEGIN", 0);
    result = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    init_trace("pthread_mutexattr_settype", "END", result);
    const char *operation = "pthread_mutexattr_settype";
    if (!result) {
        operation = "pthread_mutex_init";
        init_trace("pthread_mutex_init", "BEGIN", 0);
        result = pthread_mutex_init(&r->lock, &attributes);
        init_trace("pthread_mutex_init", "END", result);
    }
    init_trace("pthread_mutexattr_destroy", "BEGIN", 0);
    int destroy_result = pthread_mutexattr_destroy(&attributes);
    init_trace("pthread_mutexattr_destroy", "END", destroy_result);
    if (result) return fail(r, "%s result=%d recursive_type=%d", operation, result, PTHREAD_MUTEX_RECURSIVE);
    r->lock_created = 1;
    init_trace("pthread_key_create", "BEGIN", 0);
    result = pthread_key_create(&r->tls_key, tls_free);
    init_trace("pthread_key_create", "END", result);
    if (result) {
        pthread_mutex_destroy(&r->lock); r->lock_created = 0;
        return fail(r, "pthread_key_create result=%d", result);
    }
    r->key_created = 1;
    return 0;
}
static int symbol_count(struct pw_elf_runtime *r, struct pw_elf_module *m, uint64_t hash, uint64_t gnu)
{
    if (hash) {
        uint32_t *h = at(m, hash, 8, 4);
        if ((hash & 3) || !h || !at(m, hash, 8 + ((uint64_t)h[0] + h[1]) * 4, 4))
            return fail(r, "invalid SysV symbol hash");
        m->symbol_count = h[1];
    } else if (gnu) {
        uint32_t *h = at(m, gnu, 16, 4);
        if ((gnu & 7) || !h || !h[0] || !power2(h[2])) return fail(r, "invalid GNU symbol hash");
        uint64_t off = gnu + 16;
        if (off < gnu || (uint64_t)h[2] * 8 > UINT64_MAX - off) return fail(r, "GNU hash overflow");
        off += (uint64_t)h[2] * 8;
        uint32_t *buckets = at(m, off, (uint64_t)h[0] * 4, 4);
        if (!buckets || (uint64_t)h[0] * 4 > UINT64_MAX - off) return fail(r, "invalid GNU buckets");
        uint64_t chains = off + (uint64_t)h[0] * 4, highest = h[1];
        for (uint32_t i = 0; i < h[0]; ++i) {
            if (!buckets[i]) continue;
            if (buckets[i] < h[1]) return fail(r, "invalid GNU bucket index");
            uint64_t index = buckets[i];
            for (;;) {
                uint64_t delta = (index - h[1]) * 4;
                if (delta > UINT64_MAX - chains) return fail(r, "GNU chain overflow");
                uint32_t *chain = at(m, chains + delta, 4, 4);
                if (!chain) return fail(r, "unterminated GNU chain");
                ++index;
                if (*chain & 1) break;
            }
            if (index > highest) highest = index;
        }
        m->symbol_count = highest;
    } else return fail(r, "missing symbol hash");
    if ((m->symbols & 7) || m->symbol_count > UINT64_MAX / sizeof(struct sym) ||
        !at(m, m->symbols, m->symbol_count * sizeof(struct sym), 4))
        return fail(r, "invalid symbol table");
    return 0;
}
uintptr_t pw_elf_symbol(struct pw_elf_runtime *r, struct pw_elf_module *m, const char *name)
{
    (void)r;
    struct sym *symbols = at(m, m->symbols, m->symbol_count * sizeof(*symbols), 4);
    if (!symbols) return 0;
    for (uint64_t i = 1; i < m->symbol_count; ++i) {
        struct sym *s = &symbols[i];
        const char *n = string_at(m, s->name);
        if (!n || !s->section || (s->info >> 4) == 0 || (s->other & 3) == 2 || strcmp(n, name)) continue;
        if ((s->info & 15) == 6 || (s->info & 15) == 10) return 0; /* TLS/IFUNC are not addresses. */
        if (s->section == 0xfff1) return (uintptr_t)s->value;
        if (!at(m, s->value, s->size ? s->size : 1, 0)) return 0;
        return m->bias + s->value;
    }
    return 0;
}
static int resolve(struct pw_elf_runtime *r, struct pw_elf_module *m, uint64_t index,
                   uintptr_t *value, struct sym **symbol)
{
    if (index >= m->symbol_count) return fail(r, "relocation symbol out of bounds");
    struct sym *s = at(m, m->symbols + index * sizeof(*s), sizeof(*s), 4);
    *symbol = s;
    const char *name = string_at(m, s->name);
    if (!name) return fail(r, "invalid symbol name");
    if ((s->info & 15) == 10) return fail(r, "unsupported IFUNC: %s", name);
    if (s->section) {
        if (s->section == 0xfff1) *value = (uintptr_t)s->value;
        else if ((s->info & 15) == 6) *value = (uintptr_t)s->value;
        else {
            if (!at(m, s->value, s->size ? s->size : 1, 0)) return fail(r, "symbol outside image: %s", name);
            *value = m->bias + s->value;
        }
        return 0;
    }
    for (size_t i = 0; i < r->import_count; ++i)
        if (!strcmp(name, r->imports[i].name)) { *value = r->imports[i].address; return 0; }
    for (unsigned i = 0; i < r->count; ++i) {
        uintptr_t found = pw_elf_symbol(r, &r->modules[i], name);
        if (found) { *value = found; return 0; }
    }
    if ((s->info >> 4) == 2) { *value = 0; return 0; }
    return fail(r, "unresolved import: %s (%s)", name, m->path);
}
static int signed_add(uintptr_t value, int64_t addend, uint64_t *result)
{
    if (addend >= 0) {
        if ((uint64_t)addend > UINTPTR_MAX - value) return -1;
        *result = value + (uint64_t)addend;
    } else {
        uint64_t magnitude = (uint64_t)(-(addend + 1)) + 1;
        if (magnitude > value) return -1;
        *result = value - magnitude;
    }
    return 0;
}
static int relocate(struct pw_elf_runtime *r, struct pw_elf_module *m, uint64_t address, uint64_t size)
{
    if (!size) return 0;
    struct rela *relocations = at(m, address, size, 4);
    if ((address & 7) || !relocations || size % sizeof(*relocations)) return fail(r, "invalid RELA table");
    for (uint64_t i = 0; i < size / sizeof(*relocations); ++i) {
        struct rela rel = relocations[i];
        unsigned type = (uint32_t)rel.info;
        if (!type) continue;
        void *target = at(m, rel.offset, 8, 2);
        uint64_t value;
        if (!target) return fail(r, "relocation target outside writable segment");
        uintptr_t resolved = 0;
        struct sym *s = NULL;
        if (type == 8) {
            if ((rel.info >> 32) || signed_add(m->bias, rel.addend, &value))
                return fail(r, "invalid RELATIVE relocation");
            if (!at(m, value - m->bias, 0, 0)) return fail(r, "RELATIVE pointer outside image");
        } else if (type == 1 || type == 6 || type == 7 || type == 16 || type == 17) {
            if ((rel.info >> 32) || (type != 16 && type != 17))
                if (resolve(r, m, rel.info >> 32, &resolved, &s)) return -1;
            if (type == 16 || type == 17) {
                if (!m->tls_id || ((rel.info >> 32) && (!s->section || (s->info & 15) != 6)))
                    return fail(r, "external TLS ABI unsupported");
                if (type == 16) value = m->tls_id;
                else if (signed_add(resolved, rel.addend, &value) || value >= m->tls_size)
                    return fail(r, "TLS offset outside template");
            } else if (signed_add(resolved, rel.addend, &value)) return fail(r, "relocation arithmetic overflow");
        } else return fail(r, "unsupported AMD64 relocation: %u", type);
        /* memcpy also permits legal unaligned ELF relocation locations. */
        memcpy(target, &value, sizeof(value));
    }
    return 0;
}
static struct pw_elf_module *load(struct pw_elf_runtime *r, const char *path);
static struct pw_elf_module *loaded_module(struct pw_elf_runtime *r, const char *path)
{
    for (unsigned i = 0; i < r->count; ++i)
        if (!strcmp(path, r->modules[i].path)) return &r->modules[i];
    return NULL;
}
static unsigned page_flags(const struct pw_elf_module *m, uint64_t page, size_t page_size)
{
    unsigned flags = 0;
    for (unsigned i = 0; i < m->segment_count; ++i) {
        const struct pw_elf_segment *s = &m->segments[i];
        if (s->address < page + page_size && page < s->address + s->size) flags |= s->flags;
    }
    return flags;
}
static int allocate_module_mapping(struct pw_elf_runtime *r, struct pw_elf_module *m, size_t alignment)
{
#ifdef __PROSPERO__
    static int logged;
    const size_t unit = PS5_KERNEL_DIRECT_ALIGNMENT;
    size_t direct_alignment = alignment > unit ? alignment : unit;
    int64_t pool = sceKernelGetDirectMemorySize(), physical = -1;
    void *mapping = NULL;
    int32_t result;

    m->allocation_size = (m->mapping_size + unit - 1) & ~(unit - 1);
    if (pool <= 0) {
        fail(r, "direct memory pool unavailable");
        return -1;
    }
    result = sceKernelAllocateDirectMemory(0, pool, m->allocation_size, direct_alignment,
                                           PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
    if (result) {
        fail(r, "direct allocate: 0x%08x", (unsigned)result);
        return -1;
    }
    result = sceKernelMapDirectMemory(&mapping, m->allocation_size,
                                      PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                      0, physical, direct_alignment);
    if (result) {
        sceKernelReleaseDirectMemory(physical, m->allocation_size);
        fail(r, "direct map: 0x%08x", (unsigned)result);
        return -1;
    }
    m->mapping = mapping;
    m->bias = (uintptr_t)mapping;
    m->direct_start = physical;
    m->direct = 1;
    memset(mapping, 0, m->allocation_size);
    if (!logged++) io_log("psmkit-wine public-loader backing=direct pool_bytes=%lld",
                          (long long)pool);
    return 0;
#else
    size_t allocation_size = m->mapping_size + alignment - r->page_size;
    int map_flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__) && defined(MAP_JIT)
    /* macOS ARM permits executable anonymous memory only in its public JIT
     * mapping class. This is host-only; PS5 uses per-process direct memory. */
    for (unsigned i = 0; i < m->segment_count; ++i)
        if (m->segments[i].flags & PF_X) { map_flags |= MAP_JIT; break; }
#endif
    void *allocation = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE, map_flags, -1, 0);
    if (allocation == MAP_FAILED) {
        fail(r, "public mmap: %s", strerror(errno));
        return -1;
    }
    uintptr_t base = ((uintptr_t)allocation + alignment - 1) & ~(uintptr_t)(alignment - 1);
    size_t prefix = base - (uintptr_t)allocation;
    size_t suffix = allocation_size - prefix - m->mapping_size;
    if (prefix) munmap(allocation, prefix);
    if (suffix) munmap((void *)(base + m->mapping_size), suffix);
    m->mapping = (void *)base;
    m->allocation_size = m->mapping_size;
    m->bias = base;
    return 0;
#endif
}

static void release_module_mapping(struct pw_elf_module *m)
{
    if (!m->mapping) return;
#ifdef __PROSPERO__
    if (m->direct) {
        int32_t unmap = sceKernelMunmap(m->mapping, m->allocation_size);
        int32_t release = unmap ? 0 : sceKernelReleaseDirectMemory(m->direct_start,
                                                                   m->allocation_size);
        if (unmap || release)
            io_log("psmkit-wine public-loader direct release failure unmap=%#x release=%#x",
                   (unsigned)unmap, (unsigned)release);
        m->mapping = NULL;
        return;
    }
#endif
    munmap(m->mapping, m->allocation_size);
    m->mapping = NULL;
}

static struct pw_elf_module *load_fd(struct pw_elf_runtime *r, const char *path, int fd,
                                     const struct stat *st, int verified,
                                     const uint8_t expected_sha256[32], uint64_t expected_size)
{
    struct ehdr h;
    struct phdr ph[PW_ELF_SEGMENTS];
    if (st->st_size < 0 || fetch(fd, &h, sizeof(h), 0) ||
        memcmp(h.ident, "\177ELF\2\1\1", 7) || h.type != 3 || h.machine != 62 || h.version != 1 ||
        h.ehsize != sizeof(h) || h.phsize != sizeof(ph[0]) || !h.phnum || h.phnum > PW_ELF_SEGMENTS ||
        !range(h.phoff, h.phnum * sizeof(ph[0]), (uint64_t)st->st_size) ||
        fetch(fd, ph, h.phnum * sizeof(ph[0]), h.phoff)) {
        fail(r, "invalid AMD64 ELF header: %s", path); close(fd); return NULL;
    }
    uint64_t low = UINT64_MAX, high = 0, dynamic = 0, dynamic_size = 0;
    uint64_t relro_address = 0, relro_size = 0, alignment = r->page_size;
    unsigned gnu_stack_count = 0, relro_count = 0;
    struct pw_elf_module *m = &r->modules[r->count];
    strcpy(m->path, path);
    for (unsigned i = 0; i < h.phnum; ++i) {
        struct phdr *p = &ph[i];
        if (p->type == PT_INTERP) { fail(r, "PT_INTERP not supported"); goto error; }
        if (p->type == PT_GNU_STACK) {
            if (++gnu_stack_count != 1) { fail(r, "duplicate PT_GNU_STACK"); goto error; }
            if (p->flags & PF_X) { fail(r, "executable PT_GNU_STACK"); goto error; }
            continue;
        }
        if (p->type == PT_GNU_RELRO) {
            if (++relro_count != 1 || p->flags != PF_R || !p->memsz ||
                p->memsz > UINT64_MAX - p->address ||
                p->filesz > p->memsz || !range(p->offset, p->filesz, (uint64_t)st->st_size)) {
                fail(r, "invalid/writable PT_GNU_RELRO"); goto error;
            }
            uint64_t end = p->address + p->memsz;
            if (end & (r->page_size - 1)) {
                fail(r, "PT_GNU_RELRO end is not page-aligned"); goto error;
            }
            relro_address = p->address; relro_size = p->memsz;
            m->relro_address = p->address & ~(uint64_t)(r->page_size - 1);
            m->relro_size = end - m->relro_address;
            continue;
        }
        if (p->type != PT_LOAD && p->type != PT_DYNAMIC && p->type != PT_TLS) continue;
        if (p->filesz > p->memsz || !range(p->offset, p->filesz, (uint64_t)st->st_size) ||
            p->memsz > UINT64_MAX - p->address ||
            (p->align > 1 && (!power2(p->align) || ((p->address - p->offset) & (p->align - 1))))) {
            fail(r, "invalid ELF segment extent/alignment"); goto error;
        }
        if (p->type == PT_DYNAMIC) {
            if ((p->address & 7) || dynamic_size || !p->filesz || p->filesz % sizeof(struct dyn)) {
                fail(r, "invalid PT_DYNAMIC"); goto error;
            }
            dynamic = p->address; dynamic_size = p->filesz;
        } else if (p->type == PT_TLS) {
            if (m->tls_id || !p->memsz || p->memsz > 256 * 1024 * 1024 || p->align > r->page_size) {
                fail(r, "invalid TLS template/alignment"); goto error;
            }
            m->tls_id = r->count + 1; m->tls_address = p->address;
            m->tls_size = p->memsz; m->tls_file_size = p->filesz; m->tls_align = p->align ? p->align : 1;
        } else {
            if ((p->flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
                fail(r, "writable/executable PT_LOAD"); goto error;
            }
            if (!p->memsz) continue;
            if (p->align > 256 * 1024 * 1024) { fail(r, "unbounded segment alignment"); goto error; }
            if (p->align > alignment) alignment = p->align;
            uint64_t end = p->address + p->memsz;
            if (end > UINT64_MAX - (r->page_size - 1)) { fail(r, "segment page overflow"); goto error; }
            for (unsigned j = 0; j < m->segment_count; ++j) {
                struct pw_elf_segment *s = &m->segments[j];
                if (p->address < s->address + s->size && s->address < end) {
                    fail(r, "overlapping PT_LOAD segments"); goto error;
                }
            }
            m->segments[m->segment_count++] = (struct pw_elf_segment){p->address, p->memsz, p->flags};
            uint64_t start = p->address & ~(uint64_t)(r->page_size - 1);
            end = (end + r->page_size - 1) & ~(uint64_t)(r->page_size - 1);
            if (start < low) low = start;
            if (end > high) high = end;
        }
    }
    if (low != 0 || high <= low || high > SIZE_MAX || high > 256 * 1024 * 1024 || !dynamic_size) {
        fail(r, "invalid/unbounded module image"); goto error;
    }
    if (verified && (!gnu_stack_count || !m->relro_size)) {
        fail(r, "verified module requires non-executable stack and PT_GNU_RELRO"); goto error;
    }
    if (m->relro_size && !range(m->relro_address, m->relro_size, high)) {
        fail(r, "PT_GNU_RELRO outside module image"); goto error;
    }
    if (m->relro_size) {
        int backed = 0;
        for (unsigned i = 0; i < m->segment_count; ++i) {
            struct pw_elf_segment *s = &m->segments[i];
            if ((s->flags & (PF_R | PF_W)) == (PF_R | PF_W) &&
                relro_address >= s->address &&
                range(relro_address - s->address, relro_size, s->size)) backed = 1;
        }
        if (!backed) { fail(r, "PT_GNU_RELRO outside writable PT_LOAD"); goto error; }
    }
    for (uint64_t page = 0; page < high; page += r->page_size) {
        unsigned flags = page_flags(m, page, r->page_size);
        if ((flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
            fail(r, "target page permission union is writable/executable"); goto error;
        }
        if (m->relro_size && page >= m->relro_address &&
            page - m->relro_address < m->relro_size &&
            (flags & (PF_R | PF_W)) != (PF_R | PF_W)) {
            fail(r, "PT_GNU_RELRO not backed by readable/writable PT_LOAD pages"); goto error;
        }
    }
    m->mapping_size = (size_t)high;
    m->verified = verified;
    if (verified) {
        m->verified_size = expected_size;
        memcpy(m->verified_sha256, expected_sha256, sizeof(m->verified_sha256));
    }
    if (allocate_module_mapping(r, m, (size_t)alignment)) goto error;
    /* Verified modules hash every file byte while the same stream populates
     * PT_LOAD, so no pathname reopen or post-hash file read can change code. */
    if (verified) {
        if (hash_and_load(r, fd, st, &h, ph, m, expected_sha256, expected_size, path))
            goto error;
    } else {
        for (unsigned i = 0; i < h.phnum; ++i)
            if (ph[i].type == PT_LOAD && ph[i].filesz &&
                fetch(fd, (char *)m->mapping + ph[i].address, ph[i].filesz, ph[i].offset)) {
                fail(r, "short PT_LOAD read"); goto error;
            }
    }
    close(fd); fd = -1;
    struct dyn *d = at(m, dynamic, dynamic_size, PF_R);
    if (!d || (m->tls_id && !at(m, m->tls_address, m->tls_file_size, PF_R))) {
        fail(r, "dynamic/TLS outside readable image"); goto error;
    }
    uint64_t hash = 0, gnu = 0, needed[PW_ELF_MODULES], needed_count = 0;
    unsigned lifecycle_tags = 0;
    uint64_t preinit_array = 0, preinit_size = 0;
    int terminated = 0;
    for (uint64_t i = 0; i < dynamic_size / sizeof(*d); ++i) {
        uint64_t v = d[i].value;
        if (!d[i].tag) { terminated = 1; break; }
        switch (d[i].tag) {
        case 1: if (needed_count == PW_ELF_MODULES) { fail(r, "too many dependencies"); goto error; }
                needed[needed_count++] = v; break;
        case 4: hash = v; break;
        case 5: m->strings = v; break;
        case 6: m->symbols = v; break;
        case 7: m->rela = v; break;
        case 8: m->rela_size = v; break;
        case 9: if (v != sizeof(struct rela)) { fail(r, "invalid RELAENT"); goto error; } break;
        case 10: m->string_size = v; break;
        case 11: if (v != sizeof(struct sym)) { fail(r, "invalid SYMENT"); goto error; } break;
        case 12: if (lifecycle_tags & 1u) { fail(r, "duplicate DT_INIT"); goto error; }
                 lifecycle_tags |= 1u; m->init = v; break;
        case 13: if (lifecycle_tags & 2u) { fail(r, "duplicate DT_FINI"); goto error; }
                 lifecycle_tags |= 2u; m->fini = v; break;
        case 2: m->plt_size = v; break;
        case 20: if (v != 7) { fail(r, "non-RELA PLT"); goto error; } break;
        case 23: m->plt = v; break;
        case 25: if (lifecycle_tags & 4u) { fail(r, "duplicate DT_INIT_ARRAY"); goto error; }
                 lifecycle_tags |= 4u; m->init_array = v; break;
        case 26: if (lifecycle_tags & 8u) { fail(r, "duplicate DT_FINI_ARRAY"); goto error; }
                 lifecycle_tags |= 8u; m->fini_array = v; break;
        case 27: if (lifecycle_tags & 16u) { fail(r, "duplicate DT_INIT_ARRAYSZ"); goto error; }
                 lifecycle_tags |= 16u; m->init_size = v; break;
        case 28: if (lifecycle_tags & 32u) { fail(r, "duplicate DT_FINI_ARRAYSZ"); goto error; }
                 lifecycle_tags |= 32u; m->fini_size = v; break;
        case 32: if (lifecycle_tags & 64u) { fail(r, "duplicate DT_PREINIT_ARRAY"); goto error; }
                 lifecycle_tags |= 64u; preinit_array = v; break;
        case 33: if (lifecycle_tags & 128u) { fail(r, "duplicate DT_PREINIT_ARRAYSZ"); goto error; }
                 lifecycle_tags |= 128u; preinit_size = v; break;
        case 17: case 18: case 19: case 22: case 35: case 36: case 37:
            fail(r, "unsupported dynamic relocation tag: %lld", (long long)d[i].tag); goto error;
        case 0x6ffffef5: gnu = v; break;
        default: break;
        }
    }
    if (!terminated || !m->string_size || !at(m, m->strings, m->string_size, PF_R) ||
        symbol_count(r, m, hash, gnu)) {
        if (!r->error[0]) fail(r, "invalid dynamic tables");
        goto error;
    }
    if (preinit_array || preinit_size ||
        (m->init_size && !m->init_array) || (m->fini_size && !m->fini_array)) {
        fail(r, "unsupported constructor/destructor layout"); goto error;
    }
    if (verified && (m->init || m->fini || m->init_array || m->init_size ||
                     m->fini_array || m->fini_size)) {
        fail(r, "verified module constructors/destructors unsupported"); goto error;
    }
    ++r->count; m->state = 1;
    for (uint64_t i = 0; i < needed_count; ++i) {
        const char *name = string_at(m, needed[i]);
        if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
            fail(r, "non-local dependency name"); return NULL;
        }
        size_t j;
        for (j = 0; j < r->system_count; ++j) if (!strcmp(name, r->system_modules[j])) break;
        if (j < r->system_count) continue; /* Imports still require explicit resolver entries. */
        if (verified) { fail(r, "verified module dependency unsupported: %s", name); return NULL; }
        char dep[PW_ELF_PATH];
        const char *slash = strrchr(path, '/');
        size_t dir = slash ? (size_t)(slash - path + 1) : 0;
        if (dir + strlen(name) >= sizeof(dep)) { fail(r, "dependency path overflow"); return NULL; }
        memcpy(dep, path, dir); strcpy(dep + dir, name);
        struct pw_elf_module *dependency = load(r, dep);
        if (!dependency) return NULL;
        m->dependencies[m->dependency_count++] = (unsigned)(dependency - r->modules);
    }
    if (relocate(r, m, m->rela, m->rela_size) || relocate(r, m, m->plt, m->plt_size)) return NULL;
    /* Relocations see the initial writable image; final W^X and RELRO are
     * established before this module can be returned for construction. */
    for (size_t page = 0; page < m->mapping_size; page += r->page_size) {
        unsigned flags = page_flags(m, page, r->page_size);
        if (m->relro_size && page >= m->relro_address &&
            page - m->relro_address < m->relro_size) flags &= ~PF_W;
        int prot = ((flags & PF_R) ? PROT_READ : 0) |
                   ((flags & PF_W) ? PROT_WRITE : 0) |
                   ((flags & PF_X) ? PROT_EXEC : 0);
        if (mprotect((char *)m->mapping + page, r->page_size, prot)) {
            fail(r, "public mprotect: %s", strerror(errno)); return NULL;
        }
    }
#ifdef __PROSPERO__
    if (m->allocation_size > m->mapping_size &&
        mprotect((char *)m->mapping + m->mapping_size,
                 m->allocation_size - m->mapping_size, PROT_NONE)) {
        fail(r, "public tail mprotect: %s", strerror(errno)); return NULL;
    }
#endif
    m->state = 2;
    return m;
error:
    if (fd >= 0) close(fd);
    release_module_mapping(m);
    memset(m, 0, sizeof(*m));
    return NULL;
}
static struct pw_elf_module *load(struct pw_elf_runtime *r, const char *path)
{
    if (!path) { fail(r, "null module path"); return NULL; }
    struct pw_elf_module *existing = loaded_module(r, path);
    if (existing) return existing;
    if (r->count == PW_ELF_MODULES || strlen(path) >= PW_ELF_PATH) {
        fail(r, "module/path capacity exceeded"); return NULL;
    }
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0) { fail(r, "open %s: %s", path, strerror(errno)); return NULL; }
    if (fstat(fd, &st)) {
        fail(r, "fstat %s: %s", path, strerror(errno)); close(fd); return NULL;
    }
    return load_fd(r, path, fd, &st, 0, NULL, 0);
}
static void rollback_load(struct pw_elf_runtime *r, unsigned before)
{
    /* Roll back only this graph, never unmap an already-running Wine image. */
    while (r->count > before) {
        struct pw_elf_module *failed = &r->modules[--r->count];
        release_module_mapping(failed);
        memset(failed, 0, sizeof(*failed));
    }
}
struct pw_elf_module *pw_elf_load(struct pw_elf_runtime *r, const char *path)
{
    if (r->finalizing) { fail(r, "module load during finalization"); return NULL; }
    r->error[0] = 0;
    unsigned before = r->count;
    struct pw_elf_module *m = load(r, path);
    if (!m) rollback_load(r, before);
    return m;
}
struct pw_elf_module *pw_elf_load_verified(struct pw_elf_runtime *r, const char *path,
                                           const uint8_t expected_sha256[32],
                                           uint64_t expected_size)
{
    if (r->finalizing) { fail(r, "module load during finalization"); return NULL; }
    r->error[0] = 0;
    if (!path || !expected_sha256 || expected_size > INT64_MAX) {
        fail(r, "invalid verified module identity"); return NULL;
    }
    struct pw_elf_module *existing = loaded_module(r, path);
    if (existing) {
        if (!existing->verified || existing->verified_size != expected_size ||
            memcmp(existing->verified_sha256, expected_sha256, sizeof(existing->verified_sha256))) {
            fail(r, "loaded module identity does not match verification request"); return NULL;
        }
        return existing;
    }
    if (r->count == PW_ELF_MODULES || strlen(path) >= PW_ELF_PATH) {
        fail(r, "module/path capacity exceeded"); return NULL;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { fail(r, "verified open %s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size != expected_size) {
        fail(r, "verified module must be a regular file of exact size: %s", path);
        close(fd); return NULL;
    }
    unsigned before = r->count;
    struct pw_elf_module *m = load_fd(r, path, fd, &st, 1, expected_sha256, expected_size);
    if (!m) rollback_load(r, before);
    return m;
}
static int executable_function(struct pw_elf_runtime *r, uintptr_t function)
{
    for (unsigned i = 0; i < r->count; ++i)
        if (function >= r->modules[i].bias &&
            at(&r->modules[i], function - r->modules[i].bias, 1, PF_X)) return 1;
    return 0;
}
static int validate_array(struct pw_elf_runtime *r, struct pw_elf_module *m,
                          uint64_t address, uint64_t size)
{
    if (!size) return 0;
    uint64_t *array = at(m, address, size, PF_R);
    if ((address & 7) || !array || size % 8)
        return fail(r, "invalid constructor/destructor array");
    for (uint64_t i = 0; i < size / 8; ++i)
        if (array[i] && array[i] != UINTPTR_MAX && !executable_function(r, array[i]))
            return fail(r, "constructor/destructor outside executable image");
    return 0;
}
static int validate_lifecycle(struct pw_elf_runtime *r, struct pw_elf_module *m)
{
    if (m->init && !at(m, m->init, 1, PF_X)) return fail(r, "DT_INIT outside executable image");
    if (m->fini && !at(m, m->fini, 1, PF_X)) return fail(r, "DT_FINI outside executable image");
    if (validate_array(r, m, m->init_array, m->init_size) ||
        validate_array(r, m, m->fini_array, m->fini_size)) return -1;
    return 0;
}
static int call_array(struct pw_elf_runtime *r, struct pw_elf_module *m,
                      uint64_t address, uint64_t size, int reverse)
{
    if (!size) return 0;
    uint64_t *array = at(m, address, size, PF_R);
    if ((address & 7) || !array || size % 8) return fail(r, "invalid constructor/destructor array");
    for (uint64_t i = 0; i < size / 8; ++i) {
        uintptr_t function = array[reverse ? size / 8 - i - 1 : i];
        if (!function || function == UINTPTR_MAX) continue;
        if (!executable_function(r, function))
            return fail(r, "constructor/destructor outside executable image");
        ((void (*)(void))function)();
    }
    return 0;
}
static int start_module(struct pw_elf_runtime *r, struct pw_elf_module *m)
{
    if (m->state == 3 || m->state == 4) return 0;
    if (m->state != 2) return fail(r, "incomplete module graph");
    m->state = 4; /* Visiting: dependency cycles initialize each image once. */
    for (unsigned i = 0; i < m->dependency_count; ++i)
        if (start_module(r, &r->modules[m->dependencies[i]])) goto failed;
    if (m->init) {
        void *f = at(m, m->init, 1, 1);
        if (!f) { fail(r, "DT_INIT outside executable image"); goto failed; }
        ((void (*)(void))f)();
    }
    if (call_array(r, m, m->init_array, m->init_size, 0)) goto failed;
    m->state = 3;
    r->initialized[r->initialized_count++] = (unsigned)(m - r->modules);
    return 0;
    failed:
    m->state = 5; /* A partial/invalid constructor graph can never be retried. */
    return -1;
}
int pw_elf_start(struct pw_elf_runtime *r)
{
    if (r->finalizing) return fail(r, "module initialization during finalization");
    /* Validate the complete graph before allowing its first constructor to run. */
    for (unsigned i = 0; i < r->count; ++i)
        if (r->modules[i].state == 2 && validate_lifecycle(r, &r->modules[i])) return -1;
    for (unsigned i = 0; i < r->count; ++i)
        if (start_module(r, &r->modules[i])) return -1;
    return 0;
}
void *pw_elf_tls(struct pw_elf_runtime *r, size_t module, size_t offset)
{
    if (!r->key_created || !module || module > r->count) { fail(r, "invalid TLS module"); return NULL; }
    struct pw_elf_module *m = &r->modules[module - 1];
    if (!m->tls_id || offset >= m->tls_size) { fail(r, "invalid TLS offset"); return NULL; }
    struct tls_block *b = pthread_getspecific(r->tls_key);
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b || pthread_setspecific(r->tls_key, b)) { free(b); fail(r, "TLS thread allocation failed"); return NULL; }
    }
    if (!b->allocation[module - 1]) {
        if (m->tls_size > SIZE_MAX - m->tls_align + 1) { fail(r, "TLS allocation overflow"); return NULL; }
        void *p = calloc(1, (size_t)(m->tls_size + m->tls_align - 1));
        if (!p) { fail(r, "TLS allocation failed"); return NULL; }
        b->allocation[module - 1] = p;
        uintptr_t aligned = ((uintptr_t)p + m->tls_align - 1) & ~(uintptr_t)(m->tls_align - 1);
        memcpy((void *)aligned, at(m, m->tls_address, m->tls_file_size, 4), m->tls_file_size);
    }
    uintptr_t aligned = ((uintptr_t)b->allocation[module - 1] + m->tls_align - 1) & ~(uintptr_t)(m->tls_align - 1);
    return (void *)(aligned + offset);
}
void pw_elf_destroy(struct pw_elf_runtime *r)
{
    if (r->finalizing) return;
    if (r->lock_created) pthread_mutex_lock(&r->lock);
    r->finalizing = 1;
    /* Reverse successful initialization, not insertion order. Keep every
     * image and current-thread TLS live throughout the entire fini pass. */
    for (unsigned i = r->initialized_count; i; --i) {
        struct pw_elf_module *m = &r->modules[r->initialized[i - 1]];
        if (m->state != 3) continue;
        m->state = 6; /* Finalized/visiting: recursive destroy cannot repeat it. */
        call_array(r, m, m->fini_array, m->fini_size, 1);
        if (m->fini && at(m, m->fini, 1, 1)) ((void (*)(void))(m->bias + m->fini))();
    }
    /* Callers must join other TLS users before destroy. Finalizers may have
     * accessed or allocated the current thread's TLS above. */
    if (r->key_created) {
        tls_free(pthread_getspecific(r->tls_key));
        pthread_setspecific(r->tls_key, NULL); pthread_key_delete(r->tls_key); r->key_created = 0;
    }
    for (unsigned i = 0; i < r->count; ++i) {
        struct pw_elf_module *m = &r->modules[i];
        release_module_mapping(m);
        m->mapping = NULL; m->state = 0;
    }
    r->initialized_count = 0;
    if (r->lock_created) {
        pthread_mutex_unlock(&r->lock); pthread_mutex_destroy(&r->lock); r->lock_created = 0;
    }
    r->count = 0;
}
