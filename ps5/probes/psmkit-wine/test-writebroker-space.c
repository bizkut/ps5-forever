#define _GNU_SOURCE 1
#include <assert.h>
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
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "wine/ps5_writebroker.h"

#define WB_CHUNK (1u << 20)
static unsigned char token[WB_TOKEN_BYTES];
static unsigned long long served_writes, served_bytes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static char directory[] = "/tmp/writebroker-space-XXXXXX";
static char token_path[1024];
static unsigned int statfs_calls, source_broker_logs, source_fallback_logs;
static pthread_t threads[64];
static unsigned int thread_count;
static int failures;
static int negative_available;

static void check(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static void say(const char *format, ...)
{
    (void)format;
}

/* Match the PS5 statfs signed available-block counter even on Linux hosts,
 * whose statfs field is unsigned. Other counters come from real statfs. */
struct test_statfs
{
    uint64_t f_bsize, f_blocks, f_bfree;
    int64_t f_bavail;
};

static int test_statfs(const char *path, struct test_statfs *info)
{
    struct statfs native;
    int result;

    statfs_calls++;
    assert(!strcmp(path, WB_USER_ROOT "space"));
    if ((result = statfs(directory, &native))) return result;
    info->f_bsize = native.f_bsize;
    info->f_blocks = native.f_blocks;
    info->f_bfree = native.f_bfree;
    info->f_bavail = __atomic_load_n(&negative_available, __ATOMIC_SEQ_CST) ? -4 : (int64_t)native.f_bavail;
    return 0;
}

#define statfs test_statfs
#include "writebroker-server.inc"
#undef statfs

static int start_server(void *(*entry)(void *), int fd)
{
    assert(thread_count < sizeof(threads) / sizeof(threads[0]));
    assert(!pthread_create(&threads[thread_count++], NULL, entry, (void *)(intptr_t)fd));
    return 0;
}

static int connection(void)
{
    int pair[2];
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_reply reply;

    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    start_server(serve, pair[1]);
    memcpy(hello.token, token, sizeof(token));
    assert(!transfer(pair[0], &hello, sizeof(hello), 1));
    assert(!transfer(pair[0], &reply, sizeof(reply), 0));
    assert(reply.magic == WB_MAGIC && !reply.result);
    return pair[0];
}

static struct wb_statfs_reply query(int fd, const char *path)
{
    struct wb_write request = {.magic = WB_STATFS_MAGIC, .path_length = strlen(path)};
    struct wb_statfs_reply reply;

    assert(!transfer(fd, &request, sizeof(request), 1));
    assert(!transfer(fd, (void *)path, request.path_length, 1));
    assert(!transfer(fd, &reply, sizeof(reply), 0));
    return reply;
}

static void protocol_tests(void)
{
    struct statfs expected;
    struct wb_statfs_reply reply;
    struct wb_write malformed[] = {
        {.magic = WB_STATFS_MAGIC},
        {.magic = WB_STATFS_MAGIC, .path_length = WB_MAX_PATH + 1},
        {.magic = WB_STATFS_MAGIC, .path_length = 1, .length = 1},
        {.magic = WB_STATFS_MAGIC, .path_length = 1, .offset = 1},
        {.magic = WB_STATFS_MAGIC, .path_length = 1, .reserved = 1},
    };
    static const char *const refused[] = {
        "/tmp/outside", WB_USER_ROOT "../outside", "/data/forever-other/space", WB_USER_ROOT,
    };
    int fd = connection();

    assert(!statfs(directory, &expected));
    reply = query(fd, WB_USER_ROOT "space");
    check(reply.magic == WB_MAGIC && !reply.result, "query reply accepted");
    check(reply.bsize == (uint64_t)expected.f_bsize && reply.blocks == expected.f_blocks,
          "real statfs block size and total");
    check(reply.bfree == expected.f_bfree && reply.bavail == expected.f_bavail,
          "real statfs free and available");
    __atomic_store_n(&negative_available, 1, __ATOMIC_SEQ_CST);
    reply = query(fd, WB_USER_ROOT "space");
    check(!reply.result && !reply.bavail, "negative statfs availability serialized as zero");
    check(reply.bfree == expected.f_bfree, "negative availability preserves actual free blocks");
    __atomic_store_n(&negative_available, 0, __ATOMIC_SEQ_CST);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    {
        unsigned int calls = statfs_calls;
        reply = query(fd, refused[i]);
        check(reply.magic == WB_MAGIC && reply.result == -EACCES, "outside root refused");
        check(statfs_calls == calls, "refused path does not reach statfs");
    }
    reply = query(fd, WB_USER_ROOT "space");
    check(!reply.result, "refused query preserves request stream");
    close(fd);
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++)
    {
        fd = connection();
        assert(!transfer(fd, &malformed[i], sizeof(malformed[i]), 1));
        check(read(fd, &reply, sizeof(reply)) == 0, "malformed query drops connection");
        close(fd);
    }
    fd = connection();
    {
        char path[] = WB_USER_ROOT "sp\0ace";
        struct wb_write request = {.magic = WB_STATFS_MAGIC, .path_length = sizeof(path) - 1};
        assert(!transfer(fd, &request, sizeof(request), 1));
        assert(!transfer(fd, path, request.path_length, 1));
        check(read(fd, &reply, sizeof(reply)) == 0, "embedded NUL query refused");
    }
    close(fd);
}

typedef uintptr_t HANDLE;
typedef int32_t LONG;
typedef int64_t LONG64;
typedef uint64_t ULONGLONG;
typedef unsigned int NTSTATUS;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct {
    LARGE_INTEGER TotalAllocationUnits, CallerAvailableAllocationUnits, ActualAvailableAllocationUnits;
    uint32_t SectorsPerAllocationUnit, BytesPerSector;
} FILE_FS_FULL_SIZE_INFORMATION;
typedef struct {
    uint64_t ActualTotalAllocationUnits, ActualAvailableAllocationUnits, ActualPoolUnavailableAllocationUnits;
    uint64_t CallerAvailableAllocationUnits, CallerPoolUnavailableAllocationUnits, UsedAllocationUnits;
    uint64_t CallerTotalAllocationUnits, TotalReservedAllocationUnits, VolumeStorageReserveAllocationUnits;
    uint64_t AvailableCommittedAllocationUnits, PoolAvailableAllocationUnits;
    uint32_t SectorsPerAllocationUnit, BytesPerSector;
} FILE_FS_FULL_SIZE_INFORMATION_EX;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_REQUEST 1
#define TRUE 1
#define FALSE 0
#define InterlockedExchange(pointer, value) __atomic_exchange_n(pointer, value, __ATOMIC_SEQ_CST)

static LONG64 clock_us = 1000000;
static int broker_mode, title_enabled = 1, connected, old_queries;
static const char *handle_path = WB_USER_ROOT "space";
static unsigned int errno_to_status(int error) { return (unsigned int)error; }
static LONG64 title_io_now_us(void) { return clock_us; }
static int ps5_direct_enabled(void) { return title_enabled; }
static unsigned int GetCurrentThreadId(void) { return 0; }
static unsigned int server_get_unix_name(HANDLE handle, char **path)
{
    (void)handle;
    *path = strdup(handle_path);
    assert(*path);
    return 0;
}
static void sceKernelDebugOutText(int channel, const char *line)
{
    (void)channel;
    if (strstr(line, "disk space source=broker")) source_broker_logs++;
    if (strstr(line, "disk space source=synthetic")) source_fallback_logs++;
}
static int client_open(const char *path, int flags)
{
    return open(!strcmp(path, WB_TOKEN_FILE) ? token_path : path, flags);
}
static void *old_server(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct wb_hello hello;
    struct wb_reply reply = {.magic = WB_MAGIC, .result = -EPROTONOSUPPORT};
    struct wb_write request;

    assert(!transfer(fd, &hello, sizeof(hello), 0));
    assert(hello.magic == WB_MAGIC && hello.version == WB_VERSION);
    assert(!transfer(fd, &reply, sizeof(reply), 1));
    if (read(fd, &request, sizeof(request)) > 0) old_queries++;
    close(fd);
    return NULL;
}
static int client_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    int pair[2];
    (void)address;
    (void)length;
    connected++;
    if (!broker_mode) { errno = ECONNREFUSED; return -1; }
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(dup2(pair[0], fd) == fd);
    close(pair[0]);
    return start_server(broker_mode == 1 ? serve : old_server, pair[1]);
}
static int synthetic_fstatvfs(int fd, struct statvfs *info)
{
    if (!title_enabled) return fstatvfs(fd, info);
    memset(info, 0, sizeof(*info));
    info->f_bsize = info->f_frsize = 32768;
    info->f_blocks = (UINT64_C(64) << 30) / 32768;
    info->f_bfree = info->f_bavail = (UINT64_C(16) << 30) / 32768;
    return 0;
}

/* Exercise the title's statvfs branch, including the existing shim fallback. */
#undef linux
#define __PROSPERO__ 1
#define open(path, flags) client_open(path, flags)
#define connect(fd, address, length) client_connect(fd, address, length)
#define fstatvfs(fd, info) synthetic_fstatvfs(fd, info)
#include "writebroker-client.inc"
#undef fstatvfs
#undef connect
#undef open

static void close_connections(void)
{
    for (unsigned int i = 0; i < BROKER_CONNECTIONS; i++)
        if (broker[i].fd >= 0) { close(broker[i].fd); broker[i].fd = -1; }
    for (unsigned int i = 0; i < thread_count; i++) assert(!pthread_join(threads[i], NULL));
    thread_count = 0;
    broker_retry_at = 0;
}

static void client_tests(void)
{
    FILE_FS_FULL_SIZE_INFORMATION info;
    FILE_FS_FULL_SIZE_INFORMATION_EX extended;
    struct statfs expected;
    int fd = open(directory, O_RDONLY | O_DIRECTORY), attempts;

    assert(fd >= 0 && !statfs(directory, &expected));
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS, "absent broker falls back successfully");
    check(info.TotalAllocationUnits.QuadPart == (INT64_C(64) << 30) / 4096 &&
          info.CallerAvailableAllocationUnits.QuadPart == (INT64_C(16) << 30) / 4096,
          "absent broker preserves synthetic size");
    attempts = connected;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS && connected == attempts,
          "absent broker respects existing retry interval");
    clock_us += 31000000;
    broker_mode = 2;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS, "old broker falls back successfully");
    check(info.TotalAllocationUnits.QuadPart == (INT64_C(64) << 30) / 4096 &&
          info.ActualAvailableAllocationUnits.QuadPart == (INT64_C(16) << 30) / 4096,
          "old broker preserves synthetic size");
    close_connections();
    check(!old_queries, "old broker receives no statfs request after version rejection");
    clock_us += 31000000;
    broker_mode = 1;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS, "live broker size query succeeds");
    check(info.TotalAllocationUnits.QuadPart == (int64_t)((uint64_t)expected.f_bsize * expected.f_blocks / 4096),
          "Windows total uses broker result");
    check(info.CallerAvailableAllocationUnits.QuadPart == (int64_t)((uint64_t)expected.f_bsize * expected.f_bavail / 4096),
          "Windows caller availability uses broker result");
    check(info.ActualAvailableAllocationUnits.QuadPart == (int64_t)((uint64_t)expected.f_bsize * expected.f_bfree / 4096),
          "Windows actual availability uses broker result");
    check(info.BytesPerSector == 512 && info.SectorsPerAllocationUnit == 8, "allocation unit unchanged");
    check(get_full_size_info_ex(1, fd, &extended) == STATUS_SUCCESS &&
          extended.ActualTotalAllocationUnits == (uint64_t)info.TotalAllocationUnits.QuadPart &&
          extended.CallerAvailableAllocationUnits == (uint64_t)info.CallerAvailableAllocationUnits.QuadPart,
          "extended size information uses broker result");
    check(source_broker_logs == 1 && source_fallback_logs == 1, "each disk-space source logged once");
    __atomic_store_n(&negative_available, 1, __ATOMIC_SEQ_CST);
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS &&
          !info.CallerAvailableAllocationUnits.QuadPart,
          "negative statfs availability gives Windows zero caller availability");
    check(info.ActualAvailableAllocationUnits.QuadPart == (int64_t)((uint64_t)expected.f_bsize * expected.f_bfree / 4096),
          "negative availability preserves Windows actual availability");
    check(get_full_size_info_ex(1, fd, &extended) == STATUS_SUCCESS &&
          !extended.CallerAvailableAllocationUnits,
          "negative statfs availability gives extended Windows zero caller availability");
    __atomic_store_n(&negative_available, 0, __ATOMIC_SEQ_CST);
    attempts = connected;
    handle_path = "/app0/outside";
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS && connected == attempts &&
          info.TotalAllocationUnits.QuadPart == (INT64_C(64) << 30) / 4096,
          "outside title root uses existing shim without connecting");
    handle_path = WB_USER_ROOT "space";
    title_enabled = 0;
    check(get_full_size_info(1, fd, &info) == STATUS_SUCCESS && connected == attempts,
          "payload mode does not use broker");
    close_connections();
    close(fd);
}

int main(void)
{
    int fd;
    assert(WB_VERSION == 5);
    assert(mkdtemp(directory));
    snprintf(token_path, sizeof(token_path), "%s/token", directory);
    fd = open(token_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, token, sizeof(token)) == sizeof(token));
    close(fd);
    protocol_tests();
    close_connections();
    client_tests();
    unlink(token_path);
    rmdir(directory);
    if (failures) return 1;
    puts("write broker space: real statfs, root/malformed guards, absent/old fallback, Windows sizes PASS");
    return 0;
}
