#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

typedef int BOOL;
typedef int32_t NTSTATUS;
typedef unsigned char BYTE;
typedef uintptr_t UINT_PTR;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225)
#define STATUS_NO_MEMORY ((NTSTATUS)0xc0000017)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define VPROT_READ 1
#define VPROT_WRITE 2
#define VPROT_EXEC 4
#define VPROT_GUARD 16
#define VPROT_COMMITTED 32
#define VPROT_WRITEWATCH 64
#define VPROT_SYSTEM 0x200
#define VPROT_ARM64EC 0x100
#define VPROT_PS5_DIRECT 0x2000
#define SEC_FILE 0x800000
#define RW (VPROT_PS5_DIRECT | VPROT_READ | VPROT_WRITE | VPROT_COMMITTED)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define ROUND_SIZE(addr,size,mask) (((uintptr_t)(addr) + (size) + (mask)) / ((mask) + 1) * ((mask) + 1) - (uintptr_t)(addr))
static const size_t host_page_mask = 16383, granularity_mask = 65535;
static int force_exec_prot, use_kernel_writewatch;
struct ps5_direct_view { void *base; size_t size; unsigned refs; int whole, released; };
struct file_view
{
    void *base;
    size_t size;
    unsigned protect, map_profile;
    struct ps5_direct_view *direct;
    struct file_view *cache_next;
    int registered, freed;
    BYTE page_flags;
    size_t uniform_size;
};
static struct file_view *all[32];
static unsigned all_count, protects, releases, legacy_frees, register_calls;
static int cache_on = 1, pressure, fail_protect, fail_release;
static int fail_none, fail_open_after_change, fatal_armed, fatal_seen;
static jmp_buf fatal_jump;
static unsigned fatal_log_lines;
static int test_fail_fast_kill(pthread_t thread, int sig)
{
    assert(fatal_armed && sig == SIGKILL && pthread_equal(thread, pthread_self()));
    assert(fatal_log_lines == 1);
    fatal_seen++;
    longjmp(fatal_jump, 1);
}
static _Noreturn void abort_process(int status)
{
    (void)status;
    assert(!"fatal cache rollback must not invoke orderly title shutdown");
    _Exit(99);
}
static int ps5_title_radv_ranges;
static uintptr_t excluded_low, excluded_high;
static BOOL wine_ps5_title_radv_overlap(uintptr_t base, size_t size, uint64_t *low, uint64_t *high)
{
    (void)low; (void)high;
    return base < excluded_high && excluded_low < base + size;
}
static uint64_t now_ns = UINT64_C(2000000000);
static int ps5_direct_cache_enabled(void) { return cache_on; }
static int ps5_direct_cache_pressure(void) { return pressure; }
static uint64_t ps5_title_profile_now_ns(void) { return now_ns; }
static int title_profile_is_enabled(void) { return 0; }
static BOOL ps5_title_profile_read_cpu(uint64_t *cpu, uint64_t *wall)
{ (void)cpu; (void)wall; assert(!"disabled profiler called"); return FALSE; }
#define GetCurrentProcessId() 0x90u
static void title_map_unmapped(struct file_view *v) { (void)v; }
static struct file_view *containing(const void *base)
{
    unsigned i;
    for (i = 0; i < all_count; i++)
        if ((uintptr_t)base >= (uintptr_t)all[i]->base &&
            (uintptr_t)base < (uintptr_t)all[i]->base + all[i]->direct->size) return all[i];
    assert(!"unknown test view");
    return NULL;
}
static size_t get_vprot_range_size(char *base, size_t size, BYTE mask, BYTE *vprot)
{
    struct file_view *v = containing(base);
    assert(mask == 0xff);
    *vprot = v->page_flags;
    return min(size, v->uniform_size);
}
static void set_page_vprot(const void *base, size_t size, int protect)
{
    struct file_view *v = containing(base);
    assert(size == v->size);
    v->page_flags = protect;
    v->uniform_size = size;
}
static BOOL ps5_direct_is_whole_backed(const struct ps5_direct_view *d, void *base, size_t size)
{
    return d->refs == 1 && d->whole && !d->released && d->base == base && d->size == size;
}
static int ps5_direct_protect(struct ps5_direct_view *d, void *base, size_t size, int prot)
{
    assert(!d->released && (uintptr_t)base >= (uintptr_t)d->base &&
           (uintptr_t)base + size <= (uintptr_t)d->base + d->size);
    assert(!(prot & PROT_EXEC));
    protects++;
    if (fail_protect && protects == (unsigned)fail_protect) return -1;
    if (!prot && fail_none) return -1;
    if ((prot & PROT_WRITE) && fail_open_after_change)
    {
        assert(!mprotect(base, size, prot)); /* A failed range change can expose a prefix. */
        return -1;
    }
    return mprotect(base, size, prot);
}
static BOOL ps5_direct_release_whole(struct ps5_direct_view *d)
{
    releases++;
    if (fail_release) return FALSE;
    if (!d->released) assert(!munmap(d->base, d->size));
    d->released = 1;
    return TRUE;
}
static void unregister_view(struct file_view *v) { assert(v->registered); v->registered = 0; }
static void register_view(struct file_view *v) { assert(!v->registered && !v->freed); v->registered = 1; register_calls++; }
static void free_view(struct file_view *v) { assert(v->direct->released); v->freed = 1; }
static void kernel_writewatch_register_range(struct file_view *v, void *base, size_t size)
{ (void)v; (void)base; (void)size; assert(!use_kernel_writewatch); }
static void sceKernelDebugOutText(int channel, const char *text)
{
    assert(!channel && strstr(text, "SIGKILL"));
    fatal_log_lines++;
}
static NTSTATUS ps5_free_pages(struct file_view *v, char *base, size_t size)
{
    assert(base == v->base && size == v->size);
    legacy_frees++;
    return ps5_direct_release_whole(v->direct) ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
static void unmap_area(void *base, size_t size) { (void)base; (void)size; assert(!"non-direct test path"); }
static void clear_arm64ec_range(void *base, size_t size) { (void)base; (void)size; assert(!"ARM64EC test path"); }
#include "wine-vm-cpu.inc"
#define pthread_kill test_fail_fast_kill
#include "wine-direct-fail-fast.inc"
#undef pthread_kill
#include "wine-direct-cache.inc"
#include "wine-delete-view.inc"

static struct file_view *make_view(size_t size)
{
    struct file_view *v = calloc(1, sizeof(*v));
    size_t native_size = ROUND_SIZE(0, size, granularity_mask);
    assert(all_count < sizeof(all) / sizeof(all[0]));
    assert(v && (v->direct = calloc(1, sizeof(*v->direct))));
    v->base = mmap(NULL, native_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(v->base != MAP_FAILED);
    /* Wine's native regions are 64-KiB aligned. Reserve extra and align here. */
    if ((uintptr_t)v->base & granularity_mask)
    {
        void *raw;
        size_t total = native_size + granularity_mask + 1;
        assert(!munmap(v->base, native_size));
        raw = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(raw != MAP_FAILED);
        v->base = (void *)(((uintptr_t)raw + granularity_mask) & ~(uintptr_t)granularity_mask);
        if (v->base != raw) assert(!munmap(raw, (char *)v->base - (char *)raw));
        if ((char *)v->base + native_size != (char *)raw + total)
            assert(!munmap((char *)v->base + native_size, (char *)raw + total - ((char *)v->base + native_size)));
    }
    v->size = size; v->protect = RW; v->registered = 1;
    v->page_flags = VPROT_READ | VPROT_WRITE | VPROT_COMMITTED; v->uniform_size = size;
    *v->direct = (struct ps5_direct_view){v->base, native_size, 1, 1, 0};
    memset(v->base, 0xa5, native_size);
    all[all_count++] = v;
    return v;
}
static void assert_inaccessible(void *base)
{
    pid_t child = fork();
    int status;
    assert(child >= 0);
    if (!child) { volatile unsigned char value = *(volatile unsigned char *)base; (void)value; _exit(0); }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}
static void reset(void)
{
    unsigned i;
    fail_release = fail_protect = pressure = force_exec_prot = ps5_title_radv_ranges = 0; cache_on = 1;
    fail_none = fail_open_after_change = fatal_armed = fatal_seen = 0;
    fatal_log_lines = 0;
    assert(virtual_title_direct_cache_trim() || !title_direct_cache);
    for (i = 0; i < all_count; i++)
    {
        if (!all[i]->direct->released) assert(ps5_direct_release_whole(all[i]->direct));
        free(all[i]->direct); free(all[i]);
    }
    all_count = protects = releases = legacy_frees = register_calls = 0;
    now_ns += UINT64_C(2000000000);
}
static NTSTATUS take(struct file_view **out, void *base, size_t size, uintptr_t low, uintptr_t high, uintptr_t align)
{
    return title_direct_cache_take(out, base, size, RW, low, high, align, FALSE);
}
int main(void)
{
    struct file_view *a, *b, *out = NULL;
    unsigned i, old_protects;
    struct rlimit core_limit = {0, 0};
    assert(!setrlimit(RLIMIT_CORE, &core_limit));
    a = make_view(2 << 20);
    assert(!delete_view(a));
    assert(!a->registered && !a->page_flags && !a->direct->released && !legacy_frees);
    assert_inaccessible(a->base);
    assert(!take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask));
    assert(out == a && a->registered && a->page_flags == (VPROT_READ | VPROT_WRITE | VPROT_COMMITTED));
    for (i = 0; i < a->size; i++) assert(!((unsigned char *)a->base)[i]);
    assert(releases == 0 && protects == 2); /* One close, one reopen; no map/allocate/release. */
    {
        FILE *file = tmpfile();
        static const char value[] = "kernel I/O writes into already-backed reused memory";
        assert(file && fwrite(value, 1, sizeof(value), file) == sizeof(value) && !fflush(file));
        assert(pread(fileno(file), a->base, sizeof(value), 0) == sizeof(value));
        assert(!memcmp(a->base, value, sizeof(value)));
        assert(!fclose(file));
    }
    reset();
    a = make_view((2 << 20) + 4096); assert(!delete_view(a));
    assert(!take(&out, a->base, a->size, (uintptr_t)a->base, (uintptr_t)a->base + a->direct->size, granularity_mask));
    assert(out == a); assert_inaccessible((char *)a->base + ROUND_SIZE(0, a->size, host_page_mask));
    reset();
    a = make_view(2 << 20); a->page_flags |= VPROT_GUARD; assert(!delete_view(a)); assert(a->freed && legacy_frees == 1);
    reset();
    a = make_view(2 << 20); a->uniform_size -= 4096; assert(!delete_view(a)); assert(a->freed);
    reset();
    a = make_view(2 << 20); a->direct->refs = 2; assert(!delete_view(a)); assert(a->freed);
    reset();
    a = make_view(2 << 20); a->protect |= SEC_FILE; assert(!delete_view(a)); assert(a->freed);
    reset();
    a = make_view(2 << 20); assert(!delete_view(a));
    assert(take(&out, NULL, a->size + 4096, 0, UINTPTR_MAX, granularity_mask) == STATUS_NOT_FOUND);
    assert(take(&out, NULL, a->size, (uintptr_t)a->base + 65536, UINTPTR_MAX, granularity_mask) == STATUS_NOT_FOUND);
    assert(take(&out, NULL, a->size, 0, (uintptr_t)a->base + a->size - 1, granularity_mask) == STATUS_NOT_FOUND);
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, UINTPTR_MAX) == STATUS_NOT_FOUND);
    ps5_title_radv_ranges = 1;
    excluded_low = (uintptr_t)a->base; excluded_high = excluded_low + a->direct->size;
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask) == STATUS_NOT_FOUND);
    assert(title_direct_cache_evict_range((char *)a->base + 65536, 4096)); assert(a->freed && !title_direct_cache);
    reset();
    a = make_view(2 << 20); assert(!delete_view(a));
    /* A low cached address must not bypass the normal highest-free search. */
    assert(title_direct_cache_take(&out, NULL, a->size, RW, 0, UINTPTR_MAX,
                                    granularity_mask, TRUE) == STATUS_NOT_FOUND);
    assert(title_direct_cache == a && !a->registered && !a->page_flags);
    reset();
    a = make_view(32 << 20); assert(!delete_view(a));
    b = make_view(40 << 20); assert(!delete_view(b)); assert(a->freed); assert(title_direct_cache_bytes <= (64u << 20));
    reset();
    a = make_view((32 << 20) + 4096); assert(!delete_view(a));
    b = make_view((32 << 20) + 4096); assert(!delete_view(b));
    assert(!a->freed && !b->freed && title_direct_cache_count == 2);
    assert(title_direct_cache_bytes == TITLE_DIRECT_CACHE_LIMIT);
    reset();
    for (i = 0; i < 5; i++) { out = make_view(2 << 20); assert(!delete_view(out)); }
    assert(title_direct_cache_count == 4 && all[0]->freed);
    reset();
    a = make_view((64 << 20) + 4096); assert(!delete_view(a)); assert(a->freed);
    reset();
    a = make_view(2 << 20); force_exec_prot = 1;
    assert(!delete_view(a)); assert(a->freed);
    reset();
    a = make_view(2 << 20); a->page_flags |= VPROT_WRITEWATCH;
    assert(!delete_view(a)); assert(a->freed);
    reset();
    a = make_view(2 << 20); assert(!delete_view(a)); cache_on = 0;
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask) == STATUS_NOT_FOUND);
    assert(a->freed && !title_direct_cache);
    reset();
    a = make_view(2 << 20); assert(!delete_view(a)); pressure = 1; now_ns += UINT64_C(2000000000);
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask) == STATUS_NOT_FOUND); assert(a->freed);
    reset();
    a = make_view(2 << 20); assert(!delete_view(a)); fail_release = 1;
    assert(!title_direct_cache_evict_range(a->base, a->size)); assert(!a->freed && title_direct_cache == a);
    fail_release = 0; assert(title_direct_cache_evict_range(a->base, a->size)); assert(a->freed);
    reset();
    a = make_view(2 << 20); assert(!delete_view(a)); old_protects = protects; fail_protect = old_protects + 1;
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask) == STATUS_ACCESS_DENIED);
    assert(!a->registered && !a->page_flags && title_direct_cache == a);
    fail_protect = 0; assert_inaccessible(a->base);
    reset();
    a = make_view((2 << 20) + 4096); assert(!delete_view(a)); fail_protect = protects + 2;
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask) == STATUS_ACCESS_DENIED);
    assert(!a->registered && !a->page_flags && title_direct_cache == a);
    fail_protect = 0; assert_inaccessible(a->base);
    reset();
    a = make_view(2 << 20); fail_protect = 1;
    assert(!delete_view(a)); assert(a->freed && !title_direct_cache && legacy_frees == 1);
    reset();
    a = make_view((2 << 20) + 4096); assert(!delete_view(a)); fail_none = 1;
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask) == STATUS_ACCESS_DENIED);
    assert(a->freed && !title_direct_cache); /* Failed tail and whole close must evict. */
    assert_inaccessible(a->base);
    reset();
    a = make_view(2 << 20); assert(!delete_view(a)); fail_none = fail_open_after_change = 1;
    assert(take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask) == STATUS_ACCESS_DENIED);
    assert(a->freed && !title_direct_cache);
    assert_inaccessible(a->base);
    reset();
    a = make_view((2 << 20) + 4096); assert(!delete_view(a));
    fail_none = fail_release = fatal_armed = 1;
    if (!setjmp(fatal_jump))
    {
        take(&out, NULL, a->size, 0, UINTPTR_MAX, granularity_mask);
        assert(!"accessible free entry returned instead of failing closed");
    }
    assert(fatal_seen == 1 && title_direct_cache == a && !a->freed);
    reset();
    puts("DIRECT_CACHE_HOST_PASS zero=1 noaccess_free=1 query_metadata=1 guard_split_section_reject=1 limits_overlap=1 bounded_pressure=1 kill_switch=1 failure_ownership=1");
    return 0;
}
