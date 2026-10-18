/* CPU sampler callbacks shared by maintenance, GPU and PsmKit child roles.
 * The generic lookup deliberately exposes no Vulkan, display or input API. */
#ifndef PW_RUNTIME_PROFILE_H
#define PW_RUNTIME_PROFILE_H
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wine/ps5_suspend.h>
#include "../localproc/psmkit-io.h"

typedef void (*title_profile_sample_fn)(void);
static _Atomic uintptr_t title_profile_sample;
static _Atomic int title_profile_started;

static int64_t title_profile_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void title_profile_sleep_until(int64_t target_ns)
{
    for (;;)
    {
        int64_t remaining = target_ns - title_profile_now_ns();
        struct timespec delay;
        if (remaining <= 0) return;
        delay.tv_sec = remaining / 1000000000;
        delay.tv_nsec = remaining % 1000000000;
        if (!nanosleep(&delay, NULL) || errno != EINTR) return;
    }
}

/* Called in the sampled thread's SIGURG handler, not on the sampler thread. */
static int title_get_current_thread_cpu_time(uint64_t *time_ns)
{
    struct timespec now;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now)) return errno;
    *time_ns = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    return 0;
}

static int title_get_thread_cpu_clock_id(pthread_t thread, int *id)
{
    clockid_t clock;
    int error = pthread_getcpuclockid(thread, &clock);
    if (!error) *id = clock;
    return error;
}

static void *title_profile_sampler(void *unused)
{
    const int64_t interval_ns = INT64_C(1000000000) / WINE_PS5_PROFILE_HZ;
    int64_t next_ns = title_profile_now_ns();
    (void)unused;
    for (;;)
    {
        title_profile_sample_fn sample = (title_profile_sample_fn)
            atomic_load_explicit(&title_profile_sample, memory_order_acquire);
        int64_t now;
        if (sample) sample();
        next_ns += interval_ns;
        now = title_profile_now_ns();
        if (next_ns < now) next_ns = now + interval_ns;
        title_profile_sleep_until(next_ns);
    }
    return NULL;
}

static void title_set_thread_profile(title_profile_sample_fn sample)
{
    const char *enabled = getenv("WINEPS5_PROFILE");
    if (!enabled || strcmp(enabled, "1")) sample = NULL;
    atomic_store_explicit(&title_profile_sample, (uintptr_t)sample, memory_order_release);
    if (sample && !atomic_exchange_explicit(&title_profile_started, 1, memory_order_relaxed))
    {
        pthread_t thread;
        int error = pthread_create(&thread, NULL, title_profile_sampler, NULL);
        if (error)
        {
            atomic_store_explicit(&title_profile_started, 0, memory_order_relaxed);
            atomic_store_explicit(&title_profile_sample, 0, memory_order_release);
            io_log("title profile start failed error=%d", error);
        }
        else
            io_log("title profile enabled hz=%u window_s=10", WINE_PS5_PROFILE_HZ);
    }
}

void *pw_title_profile_proc(const char *name)
{
    if (!strcmp(name, "wineSetThreadProfile")) return (void *)title_set_thread_profile;
    if (!strcmp(name, "wineGetCurrentThreadCpuTime")) return (void *)title_get_current_thread_cpu_time;
    if (!strcmp(name, "wineGetThreadCpuClockId")) return (void *)title_get_thread_cpu_clock_id;
    return NULL;
}
#endif
