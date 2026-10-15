/* Asynchronous klog for B1 title processes; see title-log.h.
 *
 * Runs 165-167 (WoW, 2026-10-13): each klog line cost its thread 4-29 ms
 * (vkd3d's census under its census lock, ntdll's unwind report on the main or
 * a presenting thread, the hitch report on the presenting thread), and every
 * 10-s report started a burst of ~96 ms frames. Callers now only copy the
 * line; this file's writer thread pays for the klog write. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "title-log.h"

int sceKernelDebugOutText(int channel, const char *text);

/* 512 lines: several 10-s report bursts of the profiling build (~150 lines). */
#ifndef PW_TITLE_LOG_SLOTS
#define PW_TITLE_LOG_SLOTS 512u
#endif
#ifndef PW_TITLE_LOG_TUNE_POLL_MS
#define PW_TITLE_LOG_TUNE_POLL_MS 1000
#endif
#define PW_TITLE_LOG_LINE 1024u    /* ntdll's debug_write already splits at 1023 */
#define PW_TITLE_LOG_IDLE_US 4000
#define PW_TITLE_LOG_BATCH 16u     /* lines between stop and tuning checks */
#define PW_TITLE_LOG_SETTLE_MS 200 /* exit: producers between claim and publish */

_Static_assert(!(PW_TITLE_LOG_SLOTS & (PW_TITLE_LOG_SLOTS - 1)), "slot count must be a power of two");

/* Bounded ring (Vyukov's queue, one consumer): a slot's sequence is the
 * ticket a producer may claim, ticket + 1 once its line is published, and
 * ticket + SLOTS once the consumer has copied the line out. Tickets are
 * claimed in order, so each thread's lines leave in the order it wrote them.
 * Producers never wait: a slot not yet freed means full, and the line is
 * dropped and counted. No lock, no allocation: signal handlers log too. */
struct title_log_slot
{
    _Atomic uint64_t sequence;
    int channel;
    char text[PW_TITLE_LOG_LINE];
};

static struct title_log_slot title_log_slots[PW_TITLE_LOG_SLOTS];
static _Atomic uint64_t title_log_tail;
static atomic_int title_log_running;   /* producers may queue */
static atomic_int title_log_async = 1; /* log_async=on|off */
static atomic_int title_log_inflight;  /* producers past the running check */
static atomic_int title_log_stop;
static _Atomic uint64_t title_log_dropped;
static pthread_t title_log_writer;
static const char *title_log_tune_path;
/* Consumer state: the writer's, then (after joining it) the exiting thread's. */
static uint64_t title_log_head, title_log_dropped_reported;
static char title_log_line[PW_TITLE_LOG_LINE];

static int64_t title_log_now_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int title_log_enqueue(int channel, const char *text, size_t length)
{
    uint64_t ticket = atomic_load_explicit(&title_log_tail, memory_order_relaxed);

    for (;;)
    {
        struct title_log_slot *slot = &title_log_slots[ticket & (PW_TITLE_LOG_SLOTS - 1)];
        int64_t distance = (int64_t)(atomic_load_explicit(&slot->sequence, memory_order_acquire) - ticket);

        if (distance < 0) return 0;
        if (distance > 0)
        {
            ticket = atomic_load_explicit(&title_log_tail, memory_order_relaxed);
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&title_log_tail, &ticket, ticket + 1,
                                                  memory_order_relaxed, memory_order_relaxed))
        {
            memcpy(slot->text, text, length);
            slot->text[length] = 0;
            slot->channel = channel;
            atomic_store_explicit(&slot->sequence, ticket + 1, memory_order_release);
            return 1;
        }
    }
}

/* 1: wrote a line; 0: empty; -1: the next line is claimed but not published. */
static int title_log_write_one(void)
{
    struct title_log_slot *slot = &title_log_slots[title_log_head & (PW_TITLE_LOG_SLOTS - 1)];
    int channel;

    if (atomic_load_explicit(&slot->sequence, memory_order_acquire) != title_log_head + 1)
        return atomic_load_explicit(&title_log_tail, memory_order_relaxed) != title_log_head ? -1 : 0;
    channel = slot->channel;
    memcpy(title_log_line, slot->text, strlen(slot->text) + 1);
    atomic_store_explicit(&slot->sequence, title_log_head + PW_TITLE_LOG_SLOTS, memory_order_release);
    title_log_head++;
    sceKernelDebugOutText(channel, title_log_line);
    return 1;
}

static void title_log_report_drops(void)
{
    uint64_t dropped = atomic_exchange_explicit(&title_log_dropped, 0, memory_order_relaxed);
    char line[96];

    if (!dropped) return;
    title_log_dropped_reported += dropped;
    snprintf(line, sizeof(line), "wine: title log dropped=%llu total=%llu\n",
             (unsigned long long)dropped, (unsigned long long)title_log_dropped_reported);
    sceKernelDebugOutText(0, line);
}

static void title_log_format_note(char *line, size_t size, int async, const char *source)
{
    snprintf(line, size, "wine: title log async=%s source=%s slots=%u slot_bytes=%u\n",
             async ? "on" : "off", source, PW_TITLE_LOG_SLOTS, PW_TITLE_LOG_LINE);
}

/* The same file and line syntax as ntdll's unwind_cache/qpc switches. */
static void title_log_poll_tune(void)
{
    char buffer[1024], note[160], *line, *end;
    ssize_t length;
    int fd, value = -1;

    if (!title_log_tune_path || (fd = open(title_log_tune_path, O_RDONLY | O_CLOEXEC)) < 0) return;
    length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) return;
    buffer[length] = 0;
    for (line = buffer; *line; line = end)
    {
        size_t size;

        if ((end = strchr(line, '\n'))) *end++ = 0;
        else end = line + strlen(line);
        size = strlen(line);
        if (size && line[size - 1] == '\r') line[size - 1] = 0;
        if (!strcmp(line, "log_async=on")) value = 1;
        else if (!strcmp(line, "log_async=off")) value = 0;
    }
    if (value < 0 || atomic_exchange(&title_log_async, value) == value) return;
    title_log_format_note(note, sizeof(note), value, "C:\\wineps5-tune.txt");
    pw_title_klog_text(0, note);
}

static void *title_log_writer_main(void *arg)
{
    int64_t next_poll = 0;

    (void)arg;
    while (!atomic_load_explicit(&title_log_stop, memory_order_acquire))
    {
        unsigned int written = 0;
        int64_t now = title_log_now_ms();

        if (now >= next_poll)
        {
            title_log_poll_tune();
            next_poll = now + PW_TITLE_LOG_TUNE_POLL_MS;
        }
        while (written < PW_TITLE_LOG_BATCH && title_log_write_one() > 0) written++;
        title_log_report_drops();
        if (!written) usleep(PW_TITLE_LOG_IDLE_US);
    }
    return NULL;
}

int pw_title_klog_text(int channel, const char *text)
{
    int saved_errno = errno;
    size_t length;

    atomic_fetch_add(&title_log_inflight, 1);
    if (!text || !atomic_load(&title_log_running) ||
        !atomic_load_explicit(&title_log_async, memory_order_relaxed))
    {
        atomic_fetch_sub(&title_log_inflight, 1);
        return sceKernelDebugOutText(channel, text);
    }
    for (length = strlen(text); length;)
    {
        size_t chunk = length < PW_TITLE_LOG_LINE - 1 ? length : PW_TITLE_LOG_LINE - 1;

        if (!title_log_enqueue(channel, text, chunk))
        {
            atomic_fetch_add_explicit(&title_log_dropped, 1, memory_order_relaxed);
            break;
        }
        text += chunk;
        length -= chunk;
    }
    atomic_fetch_sub(&title_log_inflight, 1);
    errno = saved_errno;
    return 0;
}

void pw_title_log_start(const char *tune_path)
{
    const char *env = getenv("WINEPS5_LOG_ASYNC");
    sigset_t all, previous;
    char line[160];
    unsigned int i;
    int error;

    if (atomic_load(&title_log_running)) return;
    for (i = 0; i < PW_TITLE_LOG_SLOTS; i++)
        atomic_store_explicit(&title_log_slots[i].sequence, i, memory_order_relaxed);
    atomic_store(&title_log_tail, 0);
    atomic_store(&title_log_dropped, 0);
    atomic_store(&title_log_stop, 0);
    title_log_head = title_log_dropped_reported = 0;
    title_log_tune_path = tune_path;
    atomic_store(&title_log_async, !(env && (!strcmp(env, "0") || !strcmp(env, "off"))));
    /* Before the writer exists, so its first tuning poll can only follow. */
    title_log_format_note(line, sizeof(line), atomic_load(&title_log_async), env ? "environment" : "default");
    sceKernelDebugOutText(0, line);
    /* The writer has no Wine TEB: no process-directed signal may run there. */
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    error = pthread_create(&title_log_writer, NULL, title_log_writer_main, NULL);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (error)
    {
        snprintf(line, sizeof(line), "wine: title log async=off source=writer_error error=%d\n", error);
        sceKernelDebugOutText(0, line);
        return;
    }
    atomic_store(&title_log_running, 1);
}

void pw_title_log_shutdown(void)
{
    unsigned int backlog;
    int64_t deadline;
    int result;

    if (!atomic_load(&title_log_running)) return;
    atomic_store_explicit(&title_log_stop, 1, memory_order_release);
    pthread_join(title_log_writer, NULL);
    /* Queued lines first, so the exit lines that follow stay in order; bounded
     * by one ring's worth in case a thread keeps logging. */
    for (backlog = 0; backlog < PW_TITLE_LOG_SLOTS && title_log_write_one() > 0; backlog++) {}
    atomic_store(&title_log_running, 0);
    deadline = title_log_now_ms() + PW_TITLE_LOG_SETTLE_MS;
    while (atomic_load(&title_log_inflight) && title_log_now_ms() < deadline) usleep(1000);
    while ((result = title_log_write_one()))
    {
        if (result < 0)
        {
            if (title_log_now_ms() >= deadline) break;
            usleep(1000);
        }
    }
    title_log_report_drops();
}
