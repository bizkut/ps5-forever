#define _GNU_SOURCE 1
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>

typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define DIRECT_UNIT_SIZE ((size_t)65536)
#define DIRECT_RW 3
#define PS5_KERNEL_DIRECT_TYPE_CPU 3
#define PS5_KERNEL_MAP_FIXED 1
#define PS5_KERNEL_PAGE_SIZE 16384
struct ps5_direct_view
{
    void *base;
    size_t units;
    unsigned int refs;
    int64_t physical[8];
    unsigned char bits[3][8];
    size_t failed_unit;
    void *unexpected_mapping;
    void *unexpected_reservation;
};
static int64_t direct_memory_size = 16 << 20;
static unsigned allocate_calls, map_calls, hide_calls, drops;
static unsigned fail_allocate, fail_map, fail_hide;
static unsigned trim_calls;
static int trim_result;
static int fail_drop = -1;
static int host_protect[8], fail_all_hides, fatal_armed, fatal_seen;
static jmp_buf fatal_jump;
static unsigned fatal_log_lines;
static int test_fail_fast_kill(pthread_t thread, int sig)
{
    assert(fatal_armed && sig == SIGKILL && pthread_equal(thread, pthread_self()));
    assert(fatal_log_lines == 1);
    fatal_seen++;
    longjmp(fatal_jump, 1);
}
static void sceKernelDebugOutText(int channel, const char *text)
{
    assert(!channel && strstr(text, "SIGKILL"));
    fatal_log_lines++;
}
static _Noreturn void abort_process(int status)
{
    (void)status;
    assert(!"fatal direct memory must not invoke orderly title shutdown");
    _Exit(99);
}
static BOOL virtual_title_direct_cache_trim(void) { trim_calls++; return trim_result; }
static struct { size_t first, units; } hides[8];
static struct ps5_direct_view view;
static unsigned char *bytes;
static struct
{
    uint64_t allocate_ns, map_ns, zero_ns, hide_ns, units, backing_ns, hide_calls;
} direct_profile;
static int title_profile_is_enabled(void) { return 0; }
static uint64_t fake_now = UINT64_C(2000000000);
static int is_title = 1;
static BOOL ps5_title_agent_process;
static char tune_root[] = "/tmp/direct-commit-tune-XXXXXX";
static uint64_t ps5_title_profile_now_ns(void) { return fake_now; }
static int __wine_ps5_is_title(void) { return is_title; }
static const char *ps5_title_drive(char drive) { assert(drive == 'c'); return tune_root; }
static void direct_profile_report(uint64_t now) { (void)now; }
#include "wine-direct-tune.inc"
static size_t direct_unit_index(const struct ps5_direct_view *v, const void *address)
{
    return ((const char *)address - (const char *)v->base) / DIRECT_UNIT_SIZE;
}
static int direct_bit(const struct ps5_direct_view *v, unsigned bitmap, size_t index)
{
    assert(index < v->units);
    return v->bits[bitmap][index];
}
static void direct_set_bit(struct ps5_direct_view *v, unsigned bitmap, size_t index, BOOL value)
{
    assert(index < v->units);
    v->bits[bitmap][index] = value;
}
static BOOL ps5_direct_drop_unit(struct ps5_direct_view *v, void *address, BOOL reserve)
{
    size_t unit = direct_unit_index(v, address);
    if ((int)unit == fail_drop) return FALSE;
    drops++;
    v->physical[unit] = -1;
    v->bits[0][unit] = reserve;
    v->bits[1][unit] = v->bits[2][unit] = 0;
    host_protect[unit] = 0;
    return TRUE;
}
static int32_t direct_error(const char *stage, const char *operation, int32_t error)
{
    (void)stage; (void)operation;
    return error;
}
static int32_t direct_wrong_address(const char *stage, const char *operation, void *expected, void *actual)
{
    (void)stage; (void)operation; (void)expected; (void)actual;
    return -1;
}
static int32_t sceKernelAllocateDirectMemory(int64_t low, int64_t high, size_t size,
                                            size_t alignment, int type, int64_t *physical)
{
    assert(!low && high == direct_memory_size && size == DIRECT_UNIT_SIZE);
    assert(alignment == DIRECT_UNIT_SIZE && type == PS5_KERNEL_DIRECT_TYPE_CPU);
    allocate_calls++;
    if (allocate_calls == fail_allocate) return -12;
    *physical = allocate_calls * DIRECT_UNIT_SIZE;
    return 0;
}
static int32_t sceKernelMapDirectMemory(void **placed, size_t size, int prot, int flags,
                                       int64_t physical, size_t alignment)
{
    assert(*placed >= view.base && (char *)*placed < (char *)view.base + view.units * DIRECT_UNIT_SIZE);
    assert(size == DIRECT_UNIT_SIZE && prot == DIRECT_RW && flags == PS5_KERNEL_MAP_FIXED);
    assert(physical >= 0 && alignment == PS5_KERNEL_PAGE_SIZE);
    if (++map_calls == fail_map) return -12;
    host_protect[direct_unit_index(&view, *placed)] = DIRECT_RW;
    return 0;
}
static int32_t sceKernelMprotect(void *address, size_t size, int prot)
{
    size_t first = direct_unit_index(&view, address), i;
    assert(!prot && size && !(size % DIRECT_UNIT_SIZE));
    assert(hide_calls < sizeof(hides) / sizeof(hides[0]));
    hides[hide_calls].first = first;
    hides[hide_calls].units = size / DIRECT_UNIT_SIZE;
    for (i = first; i < first + size / DIRECT_UNIT_SIZE; i++)
        assert(view.bits[1][i] && view.bits[2][i]); /* Never hide an old committed unit. */
    if (++hide_calls == fail_hide || fail_all_hides) return -13;
    for (i = first; i < first + size / DIRECT_UNIT_SIZE; i++) host_protect[i] = 0;
    return 0;
}
#define pthread_kill test_fail_fast_kill
#include "wine-direct-fail-fast.inc"
#undef pthread_kill
#include "wine-direct-commit.inc"
#include "wine-direct-whole.inc"

static void reset(void)
{
    unsigned i;
    memset(&view, 0, sizeof(view));
    view.base = bytes; view.units = view.failed_unit = 4; view.refs = 1;
    memset(host_protect, 0, sizeof(host_protect));
    fail_all_hides = fatal_armed = fatal_seen = 0;
    fatal_log_lines = 0;
    memset(bytes, 0xa5, 4 * DIRECT_UNIT_SIZE);
    for (i = 0; i < 4; i++) { view.physical[i] = -1; view.bits[0][i] = 1; }
    allocate_calls = map_calls = hide_calls = drops = 0;
    fail_allocate = fail_map = fail_hide = 0;
    fail_drop = -1;
    direct_commit_batch = 1;
    direct_commit_tune_ns = fake_now;
}
static void old_unit(unsigned index)
{
    view.physical[index] = 123;
    view.bits[1][index] = 1;
    host_protect[index] = DIRECT_RW;
}
static void assert_rollback(void)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        if (i == 1) assert(view.physical[i] == 123 && view.bits[1][i] && !view.bits[2][i]);
        else assert(view.physical[i] < 0 && !view.bits[1][i] && !view.bits[2][i]);
    assert(bytes[DIRECT_UNIT_SIZE] == 0xa5);
}
static void tune_write(const char *text)
{
    char path[PATH_MAX];
    int fd;
    snprintf(path, sizeof(path), "%s/wineps5-tune.txt", tune_root);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(!close(fd));
}
static void test_tune(void)
{
    char path[PATH_MAX];
    assert(mkdtemp(tune_root));
    assert(!ps5_direct_cache_enabled()); /* Unknown/CEF image is never admitted. */
    ps5_title_agent_process = TRUE;
    assert(title_direct_commit_batch_enabled());
    assert(ps5_direct_cache_enabled());
    tune_write("write_buffer=off\ndirect_commit_batch=off\r\ndirect_cache=off\r\n");
    fake_now += UINT64_C(1000000000);
    assert(!title_direct_commit_batch_enabled());
    assert(!ps5_direct_cache_enabled());
    tune_write("direct_commit_batch=on\ndirect_cache=on\n");
    assert(!title_direct_commit_batch_enabled()); /* Cached until one second. */
    fake_now += UINT64_C(1000000000);
    assert(title_direct_commit_batch_enabled());
    assert(ps5_direct_cache_enabled());
    ps5_title_agent_process = FALSE;
    assert(!ps5_direct_cache_enabled()); /* direct_cache=on cannot opt CEF into the cache. */
    assert(title_direct_commit_batch_enabled());
    ps5_title_agent_process = TRUE;
    tune_write("not_direct_commit_batch=off\ndirect_commit_batch=invalid\n");
    fake_now += UINT64_C(1000000000);
    assert(title_direct_commit_batch_enabled());
    is_title = 0;
    assert(!title_direct_commit_batch_enabled());
    assert(!ps5_direct_cache_enabled());
    is_title = 1;
    snprintf(path, sizeof(path), "%s/wineps5-tune.txt", tune_root);
    assert(!unlink(path) && !rmdir(tune_root));
}
int main(void)
{
    unsigned i;
    test_tune();
    assert(!posix_memalign((void **)&bytes, DIRECT_UNIT_SIZE, 4 * DIRECT_UNIT_SIZE));
    reset();
    assert(ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert(allocate_calls == 4 && map_calls == 4 && hide_calls == 1);
    assert(hides[0].first == 0 && hides[0].units == 4);
    for (i = 0; i < 4 * DIRECT_UNIT_SIZE; i++) assert(!bytes[i]);
    assert(ps5_direct_finish_commit(&view, bytes, 4 * DIRECT_UNIT_SIZE, FALSE));
    for (i = 0; i < 4; i++) assert(view.bits[1][i] && !view.bits[2][i]);
    assert(ps5_direct_is_whole_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    view.refs = 2;
    assert(!ps5_direct_is_whole_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    view.refs = 1; view.bits[2][0] = 1;
    assert(!ps5_direct_is_whole_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    view.bits[2][0] = 0; fail_drop = 2;
    assert(!ps5_direct_release_whole(&view));
    assert(view.physical[2] >= 0 && view.bits[1][2]);
    assert(!ps5_direct_is_whole_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    fail_drop = -1;
    assert(ps5_direct_release_whole(&view));
    for (i = 0; i < 4; i++) assert(view.physical[i] < 0 && !view.bits[0][i]);
    reset(); old_unit(1);
    assert(ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert(allocate_calls == 3 && hide_calls == 2 && hides[0].units == 1 && hides[1].units == 2);
    assert(bytes[DIRECT_UNIT_SIZE] == 0xa5);
    reset(); old_unit(1); direct_commit_batch = 0;
    assert(ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert(hide_calls == 3);
    reset(); old_unit(1); fail_allocate = 3;
    assert(!ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert_rollback();
    reset(); old_unit(1); fail_map = 2;
    assert(!ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert_rollback();
    reset(); old_unit(1); fail_hide = 2;
    assert(!ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert_rollback();
    reset();
    assert(ps5_direct_ensure_backed(&view, bytes + 4096, 4096));
    assert(allocate_calls == 1 && hide_calls == 1 && hides[0].units == 1);
    reset(); fail_allocate = 3; fail_drop = 0;
    assert(!ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert(view.bits[1][0] && view.bits[2][0] && view.physical[0] >= 0);
    assert(host_protect[0] == 0); /* Retained rollback ownership must be inaccessible. */
    reset(); fail_map = 3; fail_drop = 1;
    assert(!ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert(view.bits[1][1] && view.bits[2][1] && host_protect[1] == 0);
    reset(); old_unit(1); fail_allocate = 3; fail_drop = 2;
    assert(!ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert(host_protect[2] == 0 && host_protect[1] == DIRECT_RW);
    reset(); fail_allocate = 3; fail_drop = 0; fail_all_hides = 1; fatal_armed = 1;
    if (!setjmp(fatal_jump))
    {
        ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE);
        assert(!"unsafe rollback returned instead of failing closed");
    }
    assert(fatal_seen == 1);
    reset(); trim_result = 1; trim_calls = 0; fail_allocate = 1;
    assert(ps5_direct_ensure_backed(&view, bytes, 4 * DIRECT_UNIT_SIZE));
    assert(trim_calls == 1 && allocate_calls == 5 && map_calls == 4 && hide_calls == 1);
    free(bytes);
    puts("DIRECT_COMMIT_HOST_PASS batching=1 kill_switch=1 old_units_untouched=1 zeroing=1 rollback=1 subunit=1");
    return 0;
}
