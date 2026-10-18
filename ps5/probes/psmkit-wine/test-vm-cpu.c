#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int BOOL;
typedef int32_t NTSTATUS;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
static int enabled, valid = 1;
static _Thread_local uint64_t cpu_ns = 100, wall_ns = 1000;
static char report[2048];
static int title_profile_is_enabled(void) { return enabled; }
static uint64_t ps5_title_profile_now_ns(void) { return wall_ns; }
static BOOL ps5_title_profile_read_cpu(uint64_t *cpu, uint64_t *wall)
{
    if (!valid) { errno = EIO; return FALSE; }
    *cpu = cpu_ns; *wall = wall_ns;
    return TRUE;
}
#define GetCurrentProcessId() 0x90u
static void sceKernelDebugOutText(int channel, const char *line)
{
    (void)channel;
    assert(strlen(report) + strlen(line) < sizeof(report));
    strcat(report, line);
}
#include "wine-vm-cpu.inc"

static void *concurrent(void *unused)
{
    unsigned i;
    (void)unused;
    for (i = 0; i < 10000; i++)
    {
        struct title_vm_cpu_scope scope = title_vm_cpu_begin();
        cpu_ns++; wall_ns += 2;
        title_vm_cpu_end(TITLE_VM_ALLOCATE, scope, 4096, STATUS_SUCCESS);
    }
    return NULL;
}
int main(void)
{
    struct title_vm_cpu_scope outer, inner;
    pthread_t threads[4];
    unsigned i;
    outer = title_vm_cpu_begin();
    assert(!outer.wall);
    title_vm_cpu_end(TITLE_VM_ALLOCATE, outer, 1, STATUS_SUCCESS);
    assert(!title_vm_cpu_stats[TITLE_VM_ALLOCATE].calls);
    enabled = 1;
    errno = EBUSY;
    outer = title_vm_cpu_begin();
    assert(errno == EBUSY);
    cpu_ns = 160; wall_ns = 1200;
    title_vm_cpu_end(TITLE_VM_ALLOCATE, outer, 32u << 20, STATUS_SUCCESS);
    assert(errno == EBUSY);
    assert(title_vm_cpu_stats[TITLE_VM_ALLOCATE].cpu_ns == 60);
    assert(title_vm_cpu_stats[TITLE_VM_ALLOCATE].wall_ns == 200);
    cpu_ns = 300; wall_ns = 2000;
    outer = title_vm_cpu_begin();
    cpu_ns = 310; wall_ns = 2010;
    inner = title_vm_cpu_begin();
    cpu_ns = 350; wall_ns = 2050;
    title_vm_cpu_end(TITLE_VM_CACHE_ZERO, inner, 32u << 20, STATUS_SUCCESS);
    cpu_ns = 400; wall_ns = 2100;
    title_vm_cpu_end(TITLE_VM_ALLOCATE, outer, 32u << 20, STATUS_SUCCESS);
    assert(title_vm_cpu_stats[TITLE_VM_ALLOCATE].cpu_ns == 160);
    assert(title_vm_cpu_stats[TITLE_VM_CACHE_ZERO].cpu_ns == 40);
    valid = 0; wall_ns = 3000;
    errno = EAGAIN;
    outer = title_vm_cpu_begin();
    wall_ns = 3300;
    title_vm_cpu_end(TITLE_VM_FREE, outer, 0, (NTSTATUS)-1);
    assert(errno == EAGAIN);
    assert(title_vm_cpu_stats[TITLE_VM_FREE].calls == 1 && !title_vm_cpu_stats[TITLE_VM_FREE].cpu_calls);
    assert(title_vm_cpu_stats[TITLE_VM_FREE].failures == 1);
    assert(title_vm_cpu_stats[TITLE_VM_FREE].wall_ns == 300);
    valid = 1;
    for (i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, concurrent, NULL));
    for (i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(title_vm_cpu_stats[TITLE_VM_ALLOCATE].calls == 40002);
    assert(title_vm_cpu_stats[TITLE_VM_ALLOCATE].cpu_calls == 40002);
    assert(title_vm_cpu_stats[TITLE_VM_ALLOCATE].cpu_ns == 40160);
    assert(title_vm_cpu_stats[TITLE_VM_ALLOCATE].wall_ns == 80300);
    virtual_title_vm_cpu_report(5000);
    assert(strstr(report, "pid=0090 kind=allocate cumulative=1 now_ns=5000 calls=40002"));
    assert(strstr(report, "cpu_ns=40160 wall_ns=80300"));
    assert(strstr(report, "kind=cache-zero"));
    puts("VM_CPU_HOST_PASS completed_calls=1 exact_cpu=1 zero_is_subset=1 unavailable_clock=1 errno=1 concurrent=1");
    return 0;
}
