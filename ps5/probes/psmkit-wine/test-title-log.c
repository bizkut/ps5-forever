/* Host tests for title-log.c: order, wrap, overflow counting, producers never
 * waiting for a stalled klog, exit flush, environment and live switch. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "title-log.h"

#define SLOTS 512u /* title-log.c's PW_TITLE_LOG_SLOTS */

struct record
{
    char *text;
    pthread_t thread;
};

static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sink_cond = PTHREAD_COND_INITIALIZER;
static struct record *records;
static size_t record_count, record_capacity;
static const char *stall_prefix; /* the sink blocks on this line until released */
static int stall_entered, stall_released;
static unsigned int sink_delay_us;

/* The slow klog: records each line and the thread that wrote it. */
int sceKernelDebugOutText(int channel, const char *text)
{
    assert(!channel && text);
    if (sink_delay_us) usleep(sink_delay_us);
    pthread_mutex_lock(&sink_lock);
    if (record_count == record_capacity)
    {
        record_capacity = record_capacity ? record_capacity * 2 : 1024;
        records = realloc(records, record_capacity * sizeof(*records));
        assert(records);
    }
    records[record_count].text = strdup(text);
    records[record_count].thread = pthread_self();
    record_count++;
    if (stall_prefix && !strncmp(text, stall_prefix, strlen(stall_prefix)))
    {
        stall_entered = 1;
        pthread_cond_broadcast(&sink_cond);
        while (!stall_released) pthread_cond_wait(&sink_cond, &sink_lock);
    }
    pthread_mutex_unlock(&sink_lock);
    errno = EIO; /* the real call may clobber errno */
    return 0;
}

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void reset(void)
{
    pthread_mutex_lock(&sink_lock);
    for (size_t i = 0; i < record_count; i++) free(records[i].text);
    record_count = 0;
    stall_prefix = NULL;
    stall_entered = stall_released = 0;
    sink_delay_us = 0;
    pthread_mutex_unlock(&sink_lock);
}

static size_t count_prefix(const char *prefix)
{
    size_t count = 0;
    pthread_mutex_lock(&sink_lock);
    for (size_t i = 0; i < record_count; i++)
        if (!strncmp(records[i].text, prefix, strlen(prefix))) count++;
    pthread_mutex_unlock(&sink_lock);
    return count;
}

static void wait_prefix(const char *prefix, size_t count)
{
    int64_t deadline = now_ms() + 5000;
    while (count_prefix(prefix) < count)
    {
        assert(now_ms() < deadline);
        usleep(1000);
    }
}

static const struct record *last_record(void)
{
    assert(record_count);
    return &records[record_count - 1];
}

static unsigned long long dropped_total(void)
{
    unsigned long long total = 0;
    for (size_t i = 0; i < record_count; i++)
    {
        unsigned long long dropped;
        if (sscanf(records[i].text, "wine: title log dropped=%llu ", &dropped) == 1) total += dropped;
    }
    return total;
}

static void __attribute__((format(printf, 1, 2))) log_line(const char *format, ...)
{
    char line[64];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    pw_title_klog_text(0, line);
}

static void test_synchronous_before_start(void)
{
    reset();
    pw_title_klog_text(0, "early\n");
    assert(record_count == 1 && !strcmp(records[0].text, "early\n"));
    assert(pthread_equal(records[0].thread, pthread_self()));
    pw_title_log_shutdown(); /* not running: nothing to do */
}

/* 2048 lines through 512 slots, never more than half full: exact order, no
 * drop, every write on the writer thread and none on the caller's. */
static void test_order_and_wrap(void)
{
    unsigned int line = 0, index = 0;

    reset();
    pw_title_log_start(NULL);
    for (unsigned int round = 0; round < 8; round++)
    {
        for (unsigned int i = 0; i < SLOTS / 2; i++) log_line("wrap %06u\n", line++);
        wait_prefix("wrap ", line);
    }
    pw_title_log_shutdown();
    for (size_t i = 0; i < record_count; i++)
    {
        unsigned int value;
        if (sscanf(records[i].text, "wrap %u", &value) != 1) continue;
        assert(value == index++);
        assert(!pthread_equal(records[i].thread, pthread_self()));
    }
    assert(index == line && !dropped_total());
}

#define PRODUCERS 4
#define PRODUCER_LINES 20000u

static void *producer(void *arg)
{
    unsigned int id = (unsigned int)(uintptr_t)arg;
    for (unsigned int i = 0; i < PRODUCER_LINES; i++) log_line("producer %u %06u\n", id, i);
    return NULL;
}

/* Concurrent producers outrunning the writer: whatever is delivered keeps each
 * thread's order, and delivered + reported drops is exactly what was logged. */
static void test_producers_order_and_drops(void)
{
    pthread_t threads[PRODUCERS];
    unsigned int next[PRODUCERS] = {0};
    size_t delivered = 0;

    reset();
    pw_title_log_start(NULL);
    for (uintptr_t i = 0; i < PRODUCERS; i++)
        assert(!pthread_create(&threads[i], NULL, producer, (void *)i));
    for (unsigned int i = 0; i < PRODUCERS; i++) pthread_join(threads[i], NULL);
    pw_title_log_shutdown();
    for (size_t i = 0; i < record_count; i++)
    {
        unsigned int id, value;
        if (sscanf(records[i].text, "producer %u %u", &id, &value) != 2) continue;
        assert(id < PRODUCERS && value >= next[id]);
        next[id] = value + 1;
        delivered++;
    }
    assert(delivered + dropped_total() == PRODUCERS * PRODUCER_LINES);
    printf("title-log producers: delivered=%zu dropped=%llu\n", delivered, dropped_total());
}

/* The klog never returns: callers still return at once, the ring holds
 * exactly SLOTS lines, the rest are counted and reported after the stall. */
static void test_stalled_sink_never_blocks(void)
{
    int64_t start, elapsed;
    unsigned int index = 0;

    reset();
    stall_prefix = "stall";
    pw_title_log_start(NULL);
    pw_title_klog_text(0, "stall here\n");
    pthread_mutex_lock(&sink_lock);
    while (!stall_entered) pthread_cond_wait(&sink_cond, &sink_lock);
    pthread_mutex_unlock(&sink_lock);
    start = now_ms();
    for (unsigned int i = 0; i < SLOTS + 100; i++) log_line("held %06u\n", i);
    elapsed = now_ms() - start;
    assert(elapsed < 500);
    assert(count_prefix("held ") == 0);
    pthread_mutex_lock(&sink_lock);
    stall_released = 1;
    pthread_cond_broadcast(&sink_cond);
    pthread_mutex_unlock(&sink_lock);
    wait_prefix("wine: title log dropped=", 1);
    pw_title_log_shutdown();
    for (size_t i = 0; i < record_count; i++)
    {
        unsigned int value;
        if (sscanf(records[i].text, "held %u", &value) == 1) assert(value == index++);
    }
    assert(index == SLOTS);
    assert(dropped_total() == 100);
    assert(count_prefix("wine: title log dropped=100 total=100\n") == 1);
    printf("title-log stall: %u calls in %lld ms while the sink was blocked\n",
           SLOTS + 100, (long long)elapsed);
}

/* Exit: shutdown writes every queued line, in order, before returning; later
 * lines are synchronous. Asynchronous calls preserve errno. */
static void test_shutdown_flush(void)
{
    unsigned int index = 0;

    reset();
    sink_delay_us = 2000;
    pw_title_log_start(NULL);
    errno = 1234;
    for (unsigned int i = 0; i < 300; i++) log_line("flush %06u\n", i);
    assert(errno == 1234);
    pw_title_log_shutdown();
    assert(count_prefix("flush ") == 300);
    for (size_t i = 0; i < record_count; i++)
    {
        unsigned int value;
        if (sscanf(records[i].text, "flush %u", &value) == 1) assert(value == index++);
    }
    pw_title_klog_text(0, "after exit\n");
    assert(!strcmp(last_record()->text, "after exit\n"));
    assert(pthread_equal(last_record()->thread, pthread_self()));
}

static void write_tune(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(text, file);
    fclose(file);
}

/* log_async=off|on in the tuning file switches live; other keys are ignored. */
static void test_tune_switch(void)
{
    char path[] = "/tmp/title-log-tune-XXXXXX";
    int fd = mkstemp(path);

    assert(fd >= 0);
    close(fd);
    reset();
    write_tune(path, "unwind_cache=on\r\nlog_async=off\r\nqpc=tsc\r\n");
    pw_title_log_start(path);
    assert(count_prefix("wine: title log async=on source=default") == 1);
    wait_prefix("wine: title log async=off source=C:\\wineps5-tune.txt", 1);
    pw_title_klog_text(0, "sync line\n");
    assert(!strcmp(last_record()->text, "sync line\n"));
    assert(pthread_equal(last_record()->thread, pthread_self()));
    write_tune(path, "log_async=on\n");
    wait_prefix("wine: title log async=on source=C:\\wineps5-tune.txt", 1);
    pw_title_klog_text(0, "async line\n");
    wait_prefix("async line", 1);
    assert(!pthread_equal(last_record()->thread, pthread_self()));
    unlink(path); /* a missing file keeps the current mode */
    usleep(100000);
    assert(count_prefix("wine: title log async=") == 3);
    pw_title_log_shutdown();
}

static void test_environment_off(void)
{
    reset();
    setenv("WINEPS5_LOG_ASYNC", "0", 1);
    pw_title_log_start(NULL);
    assert(count_prefix("wine: title log async=off source=environment") == 1);
    pw_title_klog_text(0, "direct\n");
    assert(!strcmp(last_record()->text, "direct\n"));
    assert(pthread_equal(last_record()->thread, pthread_self()));
    pw_title_log_shutdown();
    unsetenv("WINEPS5_LOG_ASYNC");
}

/* A line longer than a slot leaves as consecutive slot-sized pieces. */
static void test_long_line(void)
{
    char line[2500];

    reset();
    memset(line, 'x', sizeof(line) - 2);
    line[sizeof(line) - 2] = '\n';
    line[sizeof(line) - 1] = 0;
    pw_title_log_start(NULL);
    pw_title_klog_text(0, line);
    pw_title_log_shutdown();
    assert(count_prefix("x") == 3);
    assert(strlen(records[1].text) == 1023 && strlen(records[2].text) == 1023);
    assert(strlen(records[3].text) == sizeof(line) - 1 - 2 * 1023);
}

int main(void)
{
    unsetenv("WINEPS5_LOG_ASYNC");
    test_synchronous_before_start();
    test_order_and_wrap();
    test_producers_order_and_drops();
    test_stalled_sink_never_blocks();
    test_shutdown_flush();
    test_tune_switch();
    test_environment_off();
    test_long_line();
    reset();
    puts("title-log tests passed");
    return 0;
}
