#ifndef PW_ELF_RUNTIME_H
#define PW_ELF_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* B1 GPU's admitted Wine/XInput graph plus room for its dependencies. */
#define PW_ELF_MODULES 32
#define PW_ELF_SEGMENTS 16
#define PW_ELF_PATH 512
struct pw_elf_import { const char *name; uintptr_t address; };
struct pw_elf_segment { uint64_t address, size; unsigned flags; };
struct pw_elf_module {
    char path[PW_ELF_PATH];
    void *mapping;
    size_t mapping_size, allocation_size;
    uintptr_t bias;
    int64_t direct_start;
    int direct;
    struct pw_elf_segment segments[PW_ELF_SEGMENTS];
    unsigned dependencies[PW_ELF_MODULES], dependency_count;
    unsigned segment_count, state, tls_id;
    uint64_t symbols, strings, string_size, symbol_count;
    uint64_t rela, rela_size, plt, plt_size, init, init_array, init_size;
    uint64_t fini, fini_array, fini_size, tls_address, tls_size, tls_file_size, tls_align;
    uint64_t relro_address, relro_size, verified_size;
    uint8_t verified_sha256[32];
    int verified;
};
struct pw_elf_runtime {
    struct pw_elf_module modules[PW_ELF_MODULES];
    unsigned count;
    unsigned initialized[PW_ELF_MODULES], initialized_count;
    /* Native consumers serialize APIs with this recursive callback-safe lock. */
    pthread_mutex_t lock;
    int lock_created, finalizing;
    const struct pw_elf_import *imports;
    size_t import_count;
    const char *const *system_modules;
    size_t system_count;
    size_t page_size;
    pthread_key_t tls_key;
    int key_created;
    char error[256];
};
int pw_elf_init(struct pw_elf_runtime *, const struct pw_elf_import *, size_t,
                const char *const *, size_t);
/* Allocation-free one-shot SHA-256, also used by host identity-vector tests. */
void pw_elf_sha256(const void *, size_t, uint8_t digest[32]);
/* Both loaders relocate/protect only; constructors are explicitly entered later.
 * The verified form hashes and loads one O_NOFOLLOW regular-file descriptor. */
struct pw_elf_module *pw_elf_load(struct pw_elf_runtime *, const char *);
struct pw_elf_module *pw_elf_load_verified(struct pw_elf_runtime *, const char *,
                                           const uint8_t sha256[32], uint64_t size);
uintptr_t pw_elf_symbol(struct pw_elf_runtime *, struct pw_elf_module *, const char *);
int pw_elf_start(struct pw_elf_runtime *);
void *pw_elf_tls(struct pw_elf_runtime *, size_t module, size_t offset);
void pw_elf_destroy(struct pw_elf_runtime *);
#endif
