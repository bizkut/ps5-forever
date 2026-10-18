#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int log_fd;
static pthread_mutex_t vm_mutex;
static atomic_int waiter_entered;
static void sceKernelDebugOutText(int channel, const char *text)
{
    size_t size = strlen(text);
    assert(!channel && write(log_fd, text, size) == (ssize_t)size);
}
/* This is the actual production consumer, with real pthread_kill: no terminal
 * longjmp/mock and no Wine orderly-exit callback is linked into this test. */
#include "wine-direct-fail-fast.inc"

static void *waiting_client(void *unused)
{
    (void)unused;
    atomic_store_explicit(&waiter_entered, 1, memory_order_release);
    assert(!pthread_mutex_lock(&vm_mutex));
    assert(!pthread_mutex_unlock(&vm_mutex));
    return NULL;
}
int main(void)
{
    int logs[2], status, watchdog_fired = 0;
    pid_t child;
    char text[512];
    ssize_t size;
    unsigned newlines = 0;
    struct timespec started, now, delay = {.tv_nsec = 10000000};
    assert(!pipe(logs));
    child = fork();
    assert(child >= 0);
    if (!child)
    {
        pthread_mutexattr_t attr;
        pthread_t client;
        sigset_t blocked;
        close(logs[0]); log_fd = logs[1];
        alarm(2); /* A regression which waits on the client fails boundedly. */
        sigfillset(&blocked);
        sigdelset(&blocked, SIGALRM);
        assert(!pthread_sigmask(SIG_SETMASK, &blocked, NULL));
        assert(!pthread_mutexattr_init(&attr));
        assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
        assert(!pthread_mutex_init(&vm_mutex, &attr));
        assert(!pthread_mutexattr_destroy(&attr));
        assert(!pthread_mutex_lock(&vm_mutex));
        assert(!pthread_mutex_lock(&vm_mutex));
        assert(!pthread_create(&client, NULL, waiting_client, NULL));
        while (!atomic_load_explicit(&waiter_entered, memory_order_acquire)) sched_yield();
        ps5_direct_fail_fast();
        _Exit(99);
    }
    close(logs[1]);
    assert(!clock_gettime(CLOCK_MONOTONIC, &started));
    for (;;)
    {
        pid_t ready = waitpid(child, &status, WNOHANG);
        assert(ready == child || ready == 0);
        if (ready == child) break;
        assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        if ((now.tv_sec - started.tv_sec) * 1000000000LL + now.tv_nsec - started.tv_nsec >= 2000000000LL)
        {
            watchdog_fired = 1;
            assert(!kill(child, SIGKILL));
            assert(waitpid(child, &status, 0) == child);
            break;
        }
        nanosleep(&delay, NULL);
    }
    assert(!watchdog_fired); /* A watchdog SIGKILL is cleanup, never consumer success. */
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    size = read(logs[0], text, sizeof(text) - 1);
    assert(size > 0);
    text[size] = 0;
    for (ssize_t i = 0; i < size; i++) if (text[i] == '\n') newlines++;
    assert(newlines == 1 && strstr(text, "SIGKILL"));
    close(logs[0]);
    puts("DIRECT_FAIL_FAST_HOST_PASS actual_sigkill=1 process_wide=1 recursive_vm_lock=2 blocked_client=1 masked_signals=1 single_klog_line=1");
    return 0;
}
