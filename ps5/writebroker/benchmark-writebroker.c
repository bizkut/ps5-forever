/* Console-only probe, built by benchmark-writebroker.py. It never starts or
 * stops a title. Each file is exclusive to the runner's fresh scratch root.
 * Native authid ABBA changes only this probe process and restores it on exit.
 * The broker probe uses the real loopback protocol, not a host TCP proxy. */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include "wine/ps5_writebroker.h"

#ifndef WB_BENCH_ROOT
#error WB_BENCH_ROOT must name the runner's fresh scratch directory
#endif
#ifndef WB_BENCH_BROKER
#define WB_BENCH_BROKER 0
#endif
#define BENCH_BYTES (128u << 20)
#define FTP_AUTHID UINT64_C(0x4801000000000013)

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static unsigned int ready, go;
static uint64_t started;
static unsigned int serial;

struct worker
{
    unsigned int id, size, threads, stream, extent, window, errors, calls, ops;
    uint64_t bytes, sum_us, finished_us, prepare_us, flush_us, close_us, verify_us;
    uint64_t *samples;
    const char *stage;
};

static uint64_t now_us(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000 + value.tv_nsec / 1000;
}

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    klog_printf("writebroker-bench: %s\n", line);
}

#if WB_BENCH_BROKER
static int transfer(int fd, void *buffer, size_t size, int sending)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t count = sending ? send(fd, (char *)buffer + done, size - done, MSG_NOSIGNAL)
                                : read(fd, (char *)buffer + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EPIPE; return -1; }
        done += (size_t)count;
    }
    return 0;
}

static int connect_broker(unsigned int window, unsigned int id)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(WB_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_reply reply;
    struct timeval timeout = {.tv_sec = 60};
    int fd, one = 1, actual_send = 0, actual_receive = 0;
    socklen_t length;
    fd = open(WB_TOKEN_FILE, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t count = read(fd, hello.token, sizeof(hello.token));
    close(fd);
    if (count != (ssize_t)sizeof(hello.token)) { errno = EIO; return -1; }
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    if (window)
    {
        int requested = (int)window;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested)))
            say("tuning_refused id=%u option=send errno=%d", id, errno);
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested)))
            say("tuning_refused id=%u option=receive errno=%d", id, errno);
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        transfer(fd, &hello, sizeof(hello), 1) || transfer(fd, &reply, sizeof(reply), 0) ||
        reply.magic != WB_MAGIC || reply.result)
    {
        say("connect_failed id=%u errno=%d", id, errno);
        close(fd);
        return -1;
    }
    length = sizeof(actual_send);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual_send, &length);
    length = sizeof(actual_receive);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_receive, &length);
    say("socket id=%u requested=%u send=%d receive=%d", id, window, actual_send, actual_receive);
    return fd;
}

static int broker_write(int fd, const char *path, void *buffer, unsigned int size, uint64_t offset)
{
    struct wb_write request = {.magic = WB_MAGIC, .path_length = strlen(path),
                               .offset = offset, .length = size};
    struct wb_reply reply;
    if (transfer(fd, &request, sizeof(request), 1) ||
        transfer(fd, (void *)path, request.path_length, 1) || transfer(fd, buffer, size, 1) ||
        transfer(fd, &reply, sizeof(reply), 0)) return -1;
    if (reply.magic != WB_MAGIC || reply.result != (int32_t)size)
    {
        say("reply_failed result=%d size=%u", reply.result, size);
        errno = EIO;
        return -1;
    }
    return 0;
}
#endif

static void *worker_main(void *arg)
{
    struct worker *worker = arg;
    const unsigned int quota = BENCH_BYTES / worker->threads;
    const unsigned char pattern = (unsigned char)(0x51 + worker->id);
    unsigned char *buffer = malloc(worker->size);
    char path[512];
    int fd = -1, connection = -1;
    uint64_t at = now_us();
    snprintf(path, sizeof(path), WB_BENCH_ROOT "/native/probe-%u-%u.bin", serial, worker->id);
    worker->samples = malloc((quota / worker->size) * sizeof(*worker->samples));
    if (buffer) memset(buffer, pattern, worker->size);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (!buffer || !worker->samples || fd < 0) worker->errors++;
    if (!worker->errors && worker->extent == 2)
    {
        for (unsigned int offset = 0; offset < quota; offset += worker->size)
        {
            ssize_t count;
            do { count = pwrite(fd, buffer, worker->size, offset); } while (count < 0 && errno == EINTR);
            if (count != (ssize_t)worker->size) { worker->errors++; break; }
        }
        if (fsync(fd)) worker->errors++;
    }
#if WB_BENCH_BROKER
    if (!worker->errors && (connection = connect_broker(worker->window, worker->id)) < 0)
        worker->errors++;
#endif
    worker->prepare_us = now_us() - at;
    pthread_mutex_lock(&start_lock);
    ready++;
    pthread_cond_broadcast(&start_cond);
    while (!go) pthread_cond_wait(&start_cond, &start_lock);
    pthread_mutex_unlock(&start_lock);
    while (!worker->errors && worker->bytes < quota)
    {
        uint64_t elapsed = 0;
#if WB_BENCH_BROKER
        uint64_t before = now_us();
        worker->calls++;
        if (broker_write(connection, path, buffer, worker->size, worker->bytes)) worker->errors++;
        elapsed = now_us() - before;
#else
        unsigned int done = 0;
        while (done < worker->size)
        {
            worker->calls++;
            uint64_t before = now_us();
            ssize_t count = worker->stream ? write(fd, buffer + done, worker->size - done)
                : pwrite(fd, buffer + done, worker->size - done, (off_t)worker->bytes + done);
            elapsed += now_us() - before;
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { worker->errors++; break; }
            done += (unsigned int)count;
        }
#endif
        if (worker->errors) break;
        worker->samples[worker->ops++] = elapsed;
        worker->sum_us += elapsed;
        worker->bytes += worker->size;
    }
    worker->finished_us = now_us();
    at = now_us();
    if (fd >= 0 && fsync(fd)) worker->errors++;
    worker->flush_us = now_us() - at;
    at = now_us();
    if (connection >= 0) close(connection);
    if (fd >= 0 && close(fd)) worker->errors++;
    worker->close_us = now_us() - at;
    at = now_us();
    if (!worker->errors)
    {
        struct stat info;
        fd = open(path, O_RDONLY);
        if (fd < 0 || fstat(fd, &info) || info.st_size != quota) worker->errors++;
        uint64_t verified = 0;
        while (!worker->errors && verified < quota)
        {
            ssize_t count = read(fd, buffer, worker->size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { worker->errors++; break; }
            for (ssize_t i = 0; i < count; i++)
                if (buffer[i] != pattern) { worker->errors++; break; }
            verified += (uint64_t)count;
        }
        if (fd >= 0) close(fd);
    }
    worker->verify_us = now_us() - at;
    /* A failed run retains its exclusive file for diagnosis. */
    if (!worker->errors && unlink(path)) worker->errors++;
    if (worker->errors) say("worker_failed stage=%s id=%u path=%s bytes=%llu errno=%d",
        worker->stage, worker->id, path, (unsigned long long)worker->bytes, errno);
    free(buffer);
    return NULL;
}

static int compare(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static unsigned int scenario(const char *stage, unsigned int size, unsigned int threads,
                             unsigned int stream, unsigned int extent, unsigned int window)
{
    struct worker workers[8] = {{0}};
    pthread_t ids[8];
    unsigned int created = 0, errors = 0, ops = 0, calls = 0;
    uint64_t wall = 0, bytes = 0, sum = 0, prepare = 0, flush = 0, closing = 0, verify = 0;
    uint64_t *samples = malloc((BENCH_BYTES / size) * sizeof(*samples));
    if (!samples) return 1;
    serial++;
    ready = go = 0;
    for (unsigned int i = 0; i < threads; i++)
    {
        workers[i] = (struct worker){.id = i, .size = size, .threads = threads, .stream = stream,
                                     .extent = extent, .window = window, .stage = stage};
        if (pthread_create(&ids[i], NULL, worker_main, &workers[i])) { errors++; break; }
        created++;
    }
    pthread_mutex_lock(&start_lock);
    while (ready != created) pthread_cond_wait(&start_cond, &start_lock);
    started = now_us();
    go = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_lock);
    for (unsigned int i = 0; i < created; i++)
    {
        pthread_join(ids[i], NULL);
        struct worker *worker = &workers[i];
        if (worker->ops) memcpy(samples + ops, worker->samples, worker->ops * sizeof(*samples));
        free(worker->samples);
        ops += worker->ops;
        calls += worker->calls;
        bytes += worker->bytes;
        sum += worker->sum_us;
        if (worker->finished_us - started > wall) wall = worker->finished_us - started;
        prepare += worker->prepare_us;
        flush += worker->flush_us;
        closing += worker->close_us;
        verify += worker->verify_us;
        errors += worker->errors;
    }
    qsort(samples, ops, sizeof(*samples), compare);
    say("result kind=%s stage=%s method=%s extent=%s timing=%s size=%u threads=%u window=%u bytes=%llu ops=%u calls=%u wall_us=%llu MiBs=%.3f io_us=%llu mean_us=%.3f p50_us=%llu p99_us=%llu max_us=%llu prepare_us=%llu fsync_us=%llu close_us=%llu verify_us=%llu errors=%u",
        WB_BENCH_BROKER ? "broker" : "native", stage, WB_BENCH_BROKER ? "request" : stream ? "write" : "pwrite",
        extent ? "overwrite" : "grow",
        WB_BENCH_BROKER ? "loopback_request" : "syscalls", size, threads, window,
        (unsigned long long)bytes, ops, calls, (unsigned long long)wall,
        wall ? (double)bytes * 1000000 / wall / 1048576 : 0, (unsigned long long)sum,
        ops ? (double)sum / ops : 0,
        (unsigned long long)(ops ? samples[(ops - 1) / 2] : 0),
        (unsigned long long)(ops ? samples[(ops * 99 + 99) / 100 - 1] : 0),
        (unsigned long long)(ops ? samples[ops - 1] : 0), (unsigned long long)prepare,
        (unsigned long long)flush, (unsigned long long)closing, (unsigned long long)verify, errors);
    free(samples);
    return errors;
}

int main(void)
{
    const unsigned int sizes[] = {32768, 262144, 1048576, 4194304};
    unsigned int errors = 0;
    uint64_t original = kernel_get_ucred_authid(getpid());
    if (!kernel_get_proc_ucred(getpid()))
    {
        say("done errors=1 credentials_unavailable=1");
        return 1;
    }
    if (mkdir(WB_BENCH_ROOT "/native", 0777))
    {
        say("done errors=1 scratch_refused=%d", errno);
        return 1;
    }
    say("start root=%s kind=%s original_authid=%016llx ftp_authid=%016llx bytes_per_scenario=%u",
        WB_BENCH_ROOT, WB_BENCH_BROKER ? "broker" : "native",
        (unsigned long long)original, (unsigned long long)FTP_AUTHID, BENCH_BYTES);
#if WB_BENCH_BROKER
    for (unsigned int window = 0; window < 2; window++)
        for (unsigned int threads = 1; threads <= 8; threads *= 2)
        {
            if (threads == 2) continue;
            for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
                errors += scenario("pool", sizes[i], threads, 0, 0, window ? 4u << 20 : 0);
        }
#else
    const uint64_t authids[] = {original, FTP_AUTHID, FTP_AUTHID, original};
    const char *const stages[] = {"original-1", "ftp-1", "ftp-2", "original-2"};
    for (unsigned int phase = 0; phase < 4; phase++)
    {
        if (kernel_set_ucred_authid(getpid(), authids[phase]) ||
            kernel_get_ucred_authid(getpid()) != authids[phase])
        {
            say("authid_failed stage=%s errno=%d", stages[phase], errno);
            errors++;
            break;
        }
        say("authid stage=%s value=%016llx degenerate=%u", stages[phase],
            (unsigned long long)authids[phase], original == FTP_AUTHID);
        for (unsigned int threads = 1; threads <= 4; threads *= 4)
            for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
                for (unsigned int extent = 0; extent <= 2; extent += 2)
                    for (unsigned int stream = 0; stream < 2; stream++)
                        errors += scenario(stages[phase], sizes[i], threads, stream, extent, 0);
    }
    if (kernel_set_ucred_authid(getpid(), original) || kernel_get_ucred_authid(getpid()) != original)
    {
        say("authid_restore_failed errno=%d", errno);
        errors++;
    }
#endif
    if (!errors && rmdir(WB_BENCH_ROOT "/native")) errors++;
    say("done errors=%u restored_authid=%016llx", errors,
        (unsigned long long)kernel_get_ucred_authid(getpid()));
    return errors ? 1 : 0;
}
