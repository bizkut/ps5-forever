#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int starts, create_error;
static int test_create(pthread_t *thread, const pthread_attr_t *attr,
                       void *(*entry)(void *), void *argument)
{
    (void)thread; (void)attr; (void)entry; (void)argument;
    starts++;
    return create_error;
}
void io_log(const char *format, ...)
{
    (void)format;
}
#define pthread_create test_create
#include "runtime-profile.h"
#undef pthread_create
static unsigned samples;
static void sample(void) { samples++; }

int main(void)
{
    void (*set_profile)(void (*)(void)) = pw_title_profile_proc("wineSetThreadProfile");
    int (*cpu_time)(uint64_t *) = pw_title_profile_proc("wineGetCurrentThreadCpuTime");
    uint64_t cpu;
    assert(set_profile && cpu_time);
    assert(pw_title_profile_proc("wineGetThreadCpuClockId"));
    assert(!pw_title_profile_proc("vkGetInstanceProcAddr"));
    assert(!pw_title_profile_proc("wineGetGamepad"));
    assert(!cpu_time(&cpu));
    unsetenv("WINEPS5_PROFILE");
    set_profile(sample);
    assert(!starts && !atomic_load(&title_profile_sample));
    setenv("WINEPS5_PROFILE", "1", 1);
    create_error = EAGAIN;
    set_profile(sample);
    assert(starts == 1 && !atomic_load(&title_profile_sample) && !atomic_load(&title_profile_started));
    create_error = 0;
    set_profile(sample);
    assert(starts == 2 && atomic_load(&title_profile_started));
    ((title_profile_sample_fn)atomic_load(&title_profile_sample))();
    assert(samples == 1);
    set_profile(sample);
    assert(starts == 2);
    setenv("WINEPS5_PROFILE", "0", 1);
    set_profile(sample);
    assert(!atomic_load(&title_profile_sample));
    set_profile(NULL);
    assert(starts == 2);
    puts("RUNTIME_PROFILE_CALLBACKS_HOST_PASS cpu_only=1 gpu_procs_absent=1 disabled=1 start_retry=1");
    return 0;
}
