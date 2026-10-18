#define _GNU_SOURCE 1
#include <assert.h>
#include <dirent.h>
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include "wine/ps5_writebroker.h"
static int client_mutex_lock(pthread_mutex_t *mutex);
#define pthread_mutex_lock(mutex) client_mutex_lock(mutex)
#include "wine/ps5_title_private.h"
#undef pthread_mutex_lock
#include "writebroker-menu.h"

#define WB_CHUNK (1u << 20)
static unsigned char token[WB_TOKEN_BYTES];
static unsigned long long served_writes, served_bytes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
sigset_t ps5_title_block_set;
static char directory[] = "/tmp/writebroker-space-XXXXXX";
static char menu_prefix_root[1024];
static char token_path[1024];
static char writes_path[1024];
static char repair_data_root[1024];
static int inject_other_device, inject_readdir_error, inject_closedir_error;
static int startup_order;
static unsigned int statfs_calls, source_broker_logs, source_fallback_logs;
static pthread_t threads[64];
static unsigned int thread_count;
static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static unsigned int waiting_requests, peak_requests, lock_attempt_mask;
static int hold_requests, track_lock_attempts;
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

    __atomic_fetch_add(&statfs_calls, 1, __ATOMIC_SEQ_CST);
    assert(!strcmp(path, WB_USER_ROOT "space"));
    if ((result = statfs(directory, &native))) return result;
    info->f_bsize = native.f_bsize;
    info->f_blocks = native.f_blocks;
    info->f_bfree = native.f_bfree;
    info->f_bavail = __atomic_load_n(&negative_available, __ATOMIC_SEQ_CST) ? -4 : (int64_t)native.f_bavail;
    return 0;
}

/* Only translate host filesystem access; path validation, framing, writes and
 * replies below are the extracted production broker implementation. */
static const char *server_path(const char *path)
{
    return !strcmp(path, WB_USER_ROOT "writes") ? writes_path : path;
}

static ssize_t server_pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
    assert(!pthread_mutex_lock(&request_lock));
    if (hold_requests)
    {
        waiting_requests++;
        if (waiting_requests > peak_requests) peak_requests = waiting_requests;
        assert(!pthread_cond_broadcast(&request_cond));
        while (hold_requests) assert(!pthread_cond_wait(&request_cond, &request_lock));
        waiting_requests--;
    }
    assert(!pthread_mutex_unlock(&request_lock));
    return pwrite(fd, buffer, length, offset);
}

static int repair_test_fstatat(int fd, const char *name, struct stat *status, int flags)
{
    int result = fstatat(fd, name, status, flags);
    if (!result && inject_other_device && !strcmp(name, "other-device"))
        status->st_dev++;
    return result;
}


static struct dirent *repair_test_readdir(DIR *directory_stream)
{
    if (inject_readdir_error)
    {
        inject_readdir_error = 0;
        errno = EIO;
        return NULL;
    }
    return readdir(directory_stream);
}

static int repair_test_closedir(DIR *directory_stream)
{
    int result = closedir(directory_stream);
    if (!result && inject_closedir_error)
    {
        inject_closedir_error = 0;
        errno = EIO;
        return -1;
    }
    return result;
}

static int fake_startup_repair(void)
{
    check(!startup_order, "broker startup repairs before token creation");
    startup_order = 1;
    return 0;
}

static int fake_startup_token(void)
{
    check(startup_order == 1, "broker token is created after startup repair");
    startup_order = 2;
    return 0;
}

#define statfs test_statfs
#define open(path, ...) open(server_path(path), __VA_ARGS__)
#define pwrite(fd, buffer, length, offset) server_pwrite(fd, buffer, length, offset)
#define WB_MENU_PREFIX_ROOT menu_prefix_root
#define WB_MENU_REMOVE_DEPTH 64u
enum { TEST_REPAIR_MAX_ENTRIES = 32, TEST_REPAIR_MAX_DEPTH = 8 };
#undef WB_REPAIR_MAX_ENTRIES
#undef WB_REPAIR_MAX_DEPTH
#define WB_REPAIR_DATA_ROOT repair_data_root
#define WB_REPAIR_MAX_ENTRIES 32u
#define WB_REPAIR_MAX_DEPTH 8u
#define WB_REPAIR_FSTATAT repair_test_fstatat
#define WB_REPAIR_READDIR repair_test_readdir
#define WB_REPAIR_CLOSEDIR repair_test_closedir
#define WB_STARTUP_REPAIR fake_startup_repair
#define WB_STARTUP_WRITE_TOKEN fake_startup_token
#include "writebroker-server.inc"
#undef WB_STARTUP_WRITE_TOKEN
#undef WB_STARTUP_REPAIR
#undef WB_REPAIR_CLOSEDIR
#undef WB_REPAIR_READDIR
#undef WB_REPAIR_FSTATAT
#undef WB_REPAIR_MAX_DEPTH
#undef WB_REPAIR_MAX_ENTRIES
#undef WB_REPAIR_DATA_ROOT
#undef WB_MENU_PREFIX_ROOT
#undef WB_MENU_REMOVE_DEPTH
#undef statfs
#undef pwrite
#undef open

static int start_server(void *(*entry)(void *), int fd)
{
    assert(!pthread_mutex_lock(&server_lock));
    assert(thread_count < sizeof(threads) / sizeof(threads[0]));
    assert(!pthread_create(&threads[thread_count++], NULL, entry, (void *)(intptr_t)fd));
    assert(!pthread_mutex_unlock(&server_lock));
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
    {
        struct wb_write capability = {.magic = WB_MENU_CAPS_MAGIC};
        struct wb_reply answer;
        assert(!transfer(fd, &capability, sizeof(capability), 1));
        assert(!transfer(fd, &answer, sizeof(answer), 0));
        check(answer.magic == WB_MAGIC && answer.result == WB_MENU_CAPS_REMOVE,
              "menu removal capability advertised");
    }
    {
        static const char outside[] =
            WB_USER_ROOT "prefix/drive_c/Program Files (x86)/World of Warcraft";
        struct wb_write removal = {
            .magic = WB_MENU_REMOVE_MAGIC, .path_length = sizeof(outside) - 1};
        struct wb_reply answer;
        assert(!transfer(fd, &removal, sizeof(removal), 1));
        assert(!transfer(fd, (void *)outside, removal.path_length, 1));
        assert(!transfer(fd, &answer, sizeof(answer), 0));
        check(answer.magic == WB_MAGIC && answer.result == -EACCES,
              "menu broker refuses every non-Battle.net root");
        check(menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net") &&
              menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/ProgramData/Battle.net") &&
              menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/users/steamuser/AppData/Local/Battle.net") &&
              menu_remove_path_allowed(WB_USER_ROOT "prefix/drive_c/Battle.net-Setup.exe") &&
              !menu_remove_path_allowed(
                  WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net/child"),
              "menu broker deletion roots are the exact fixed set");
    }

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

    check(reply.bfree > 0, "negative availability preserves actual free blocks");
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
static void fixture_mkdirs(const char *path)
{
    char copy[1024];
    size_t length = strlen(path);
    assert(length < sizeof(copy));
    memcpy(copy, path, length + 1);
    for (char *p = copy + 1; ; p++)
    {
        if (*p != '/' && *p) continue;
        char saved = *p;
        *p = 0;
        assert(!mkdir(copy, 0777) || errno == EEXIST);
        if (!saved) break;
        *p = saved;
    }
}

static void fixture_put(const char *path, const char *text)
{
    char parent[1024];
    assert(strlen(path) < sizeof(parent));
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    assert(slash);
    *slash = 0;
    fixture_mkdirs(parent);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(!close(fd));
}

static int menu_remove_request(const char *path)
{
    struct wb_write request = {
        .magic = WB_MENU_REMOVE_MAGIC, .path_length = (uint32_t)strlen(path)};
    struct wb_reply reply;
    int fd = connection();
    assert(!transfer(fd, &request, sizeof(request), 1));
    assert(!transfer(fd, (void *)path, request.path_length, 1));
    assert(!transfer(fd, &reply, sizeof(reply), 0));
    close(fd);
    assert(reply.magic == WB_MAGIC);
    return reply.result;
}


static mode_t fixture_mode(const char *path)
{
    struct stat status;
    assert(!lstat(path, &status));
    return status.st_mode & 07777;
}

static void permission_repair_tests(void)
{
    char forever[1024], prefix[1024], home[1024], cache[1024], locked[1024];
    char child[1024], executable[1024], private_dir[1024], made[1024];
    char token_file[1024], outside[1024], link_path[1024], unmanaged[1024];
    char hardlinked[1024], hardlink_outside[1024], other_device[1024];
    char legacy_parent[1024], legacy_root[1024], legacy_file[1024], sibling[1024];
    char store_parent[1024], store_root[1024], store_file[1024];
    struct wb_permission_repair repair = {0};
    struct stat status;
    dev_t device;
    int data_fd, result;

    snprintf(repair_data_root, sizeof(repair_data_root), "%s/repair-data", directory);
    snprintf(forever, sizeof(forever), "%s/forever", repair_data_root);
    snprintf(prefix, sizeof(prefix), "%s/prefix", forever);
    snprintf(home, sizeof(home), "%s/home", forever);
    snprintf(cache, sizeof(cache), "%s/radv-shader-cache", forever);
    snprintf(locked, sizeof(locked), "%s/locked", prefix);
    snprintf(child, sizeof(child), "%s/child.txt", locked);
    snprintf(executable, sizeof(executable), "%s/run.exe", locked);
    snprintf(private_dir, sizeof(private_dir), "%s/private", prefix);
    snprintf(token_file, sizeof(token_file), "%s/.writebroker-token", forever);
    snprintf(outside, sizeof(outside), "%s/outside.txt", repair_data_root);
    snprintf(unmanaged, sizeof(unmanaged), "%s/unmanaged.txt", forever);
    snprintf(hardlinked, sizeof(hardlinked), "%s/hardlinked", prefix);
    snprintf(hardlink_outside, sizeof(hardlink_outside), "%s/hardlink-outside", repair_data_root);
    snprintf(other_device, sizeof(other_device), "%s/other-device", prefix);
    snprintf(legacy_parent, sizeof(legacy_parent), "%s/homebrew/PPSA99030", repair_data_root);
    snprintf(legacy_root, sizeof(legacy_root), "%s/wine", legacy_parent);
    snprintf(legacy_file, sizeof(legacy_file), "%s/prefix/system.reg", legacy_root);
    snprintf(sibling, sizeof(sibling), "%s/app-file", legacy_parent);
    snprintf(store_parent, sizeof(store_parent), "%s/prosperostore/previous/PPSA99030",
             repair_data_root);
    snprintf(store_root, sizeof(store_root), "%s/wine", store_parent);
    snprintf(store_file, sizeof(store_file), "%s/home/settings", store_root);

    fixture_put(child, "child");
    fixture_put(executable, "exec");
    fixture_mkdirs(private_dir);
    fixture_mkdirs(home);
    fixture_mkdirs(cache);
    fixture_put(token_file, "private token");
    fixture_put(outside, "outside");
    fixture_put(unmanaged, "not an allowlisted subtree");
    fixture_put(hardlinked, "linked");
    assert(!link(hardlinked, hardlink_outside));
    fixture_put(other_device, "mounted");
    fixture_put(legacy_file, "registry");
    fixture_put(sibling, "immutable sibling");
    fixture_put(store_file, "settings");
    snprintf(link_path, sizeof(link_path), "%s/outside-link", locked);
    assert(!symlink(outside, link_path));

    assert(!chmod(forever, 0555));
    assert(!chmod(locked, 0555));
    assert(!chmod(private_dir, 0700));
    assert(!chmod(child, 0400));
    assert(!chmod(executable, 0500));
    assert(!chmod(token_file, 0600));
    assert(!chmod(outside, 0400));
    assert(!chmod(unmanaged, 0400));
    assert(!chmod(hardlinked, 0400));
    assert(!chmod(other_device, 0400));
    assert(!chmod(legacy_parent, 0555));
    assert(!chmod(legacy_file, 0400));
    assert(!chmod(sibling, 0400));
    assert(!chmod(store_file, 0400));
    snprintf(made, sizeof(made), "%s/new", locked);
    errno = 0;
    check(mkdir(made, 0700) && (errno == EACCES || errno == EPERM),
          "locked directory makes the title-like write fail before startup repair");

    inject_other_device = 1;
    result = repair_all(&repair);
    inject_other_device = 0;
    check(!result && !repair.failed && repair.changed >= 8 && repair.skipped >= 3,
          "production startup repair reports changed, skipped and failed counts");
    check(fixture_mode(forever) == 0777 && fixture_mode(locked) == 0777 &&
          fixture_mode(private_dir) == 0777 && fixture_mode(child) == 0666 &&
          fixture_mode(executable) == 0777,
          "startup repair makes 0555/0700 directories 0777 and preserves executable files");
    check(!mkdir(made, 0700), "startup repair permits the failed title step to retry once");
    check(!lstat(link_path, &status) && S_ISLNK(status.st_mode) &&
          fixture_mode(outside) == 0400,
          "startup repair skips an internal symlink without changing its target");
    check(fixture_mode(hardlinked) == 0400 && fixture_mode(hardlink_outside) == 0400,
          "startup repair skips multiply-linked regular files");
    check(fixture_mode(other_device) == 0400,
          "startup repair skips an entry reported on another device");
    check(fixture_mode(token_file) == 0600,
          "startup repair never widens the authentication token");
    check(fixture_mode(unmanaged) == 0400,
          "startup repair does not recurse through unknown forever-root children");
    check(fixture_mode(legacy_parent) == 0777 && fixture_mode(legacy_file) == 0666 &&
          fixture_mode(sibling) == 0400,
          "legacy repair changes only the rename parent and recursive wine tree");
    check(fixture_mode(store_parent) == 0777 && fixture_mode(store_file) == 0666,
          "store legacy root and rename parent are repaired");

    data_fd = repair_open_data_root(&device);
    assert(data_fd >= 0);
    assert(!rmdir(home));
    assert(!symlink(outside, home));
    memset(&repair, 0, sizeof(repair));
    repair.device = device;
    result = repair_root(data_fd, "forever/home", 1, &repair);
    check(result < 0 && repair.failed,
          "startup repair refuses a symlinked fixed root");
    assert(!unlink(home));
    fixture_mkdirs(home);

    {
        char budget[1024], entry[1100];
        snprintf(budget, sizeof(budget), "%s/budget", prefix);
        fixture_mkdirs(budget);
        for (unsigned i = 0; i < 40; i++)
        {
            snprintf(entry, sizeof(entry), "%s/link-%02u", budget, i);
            assert(!symlink(outside, entry));
        }
        memset(&repair, 0, sizeof(repair));
        repair.device = device;
        result = repair_root(data_fd, "forever/prefix/budget", 1, &repair);
        check(result == -E2BIG && repair.scanned == TEST_REPAIR_MAX_ENTRIES &&
              repair.skipped,
              "visited-entry budget is charged for skipped symlinks");
    }

    memset(&repair, 0, sizeof(repair));
    repair.device = device;
    inject_readdir_error = 1;
    result = repair_root(data_fd, "forever/radv-shader-cache", 1, &repair);
    check(result == -EIO && repair.failed,
          "root readdir error is reported as repair failure");
    memset(&repair, 0, sizeof(repair));
    repair.device = device;
    inject_closedir_error = 1;
    result = repair_root(data_fd, "forever/radv-shader-cache", 1, &repair);
    check(result == -EIO && repair.failed,
          "root closedir error is reported as repair failure");

    {
        char deep[1024];
        snprintf(deep, sizeof(deep), "%s/depth", home);
        fixture_mkdirs(deep);
        for (unsigned i = 0; i < TEST_REPAIR_MAX_DEPTH + 2; i++)
        {
            size_t used = strlen(deep);
            assert(used + 3 < sizeof(deep));
            memcpy(deep + used, "/d", 3);
            fixture_mkdirs(deep);
        }
        memset(&repair, 0, sizeof(repair));
        repair.device = device;
        result = repair_root(data_fd, "forever/home", 1, &repair);
        check(result == -E2BIG && repair.failed,
              "startup repair stops at its production depth bound");
    }
    close(data_fd);

    startup_order = 0;
    check(!broker_prepare_startup() && startup_order == 2,
          "production broker startup orders repair before token");
}

static void menu_removal_tests(void)
{
    char app[1024], wow[1024], outside[1024], ancestor[1024], local[1024], link[1024];
    struct stat status;

    snprintf(menu_prefix_root, sizeof(menu_prefix_root), "%s/menu-prefix", directory);
    snprintf(app, sizeof(app), "%s/drive_c/Program Files (x86)/Battle.net", menu_prefix_root);
    snprintf(wow, sizeof(wow), "%s/drive_c/Program Files (x86)/World of Warcraft/keep", menu_prefix_root);
    fixture_put(wow, "keep");
    {
        char nested[1024];
        snprintf(nested, sizeof(nested), "%s/sub/dir/file", app);
        fixture_put(nested, "remove");
    }
    check(!menu_remove_request(WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net"),
          "production broker recursively deletes an accepted Battle.net root");
    check(lstat(app, &status) && errno == ENOENT, "accepted Battle.net root is gone");
    check(!stat(wow, &status) && S_ISREG(status.st_mode), "accepted deletion keeps WoW");

    /* A file at an allowed root is unlinked without attempting directory traversal. */
    fixture_put(app, "regular root");
    check(!menu_remove_request(WB_USER_ROOT "prefix/drive_c/Program Files (x86)/Battle.net"),
          "production broker removes a non-directory allowed root");
    check(lstat(app, &status) && errno == ENOENT, "non-directory root is gone");

    snprintf(outside, sizeof(outside), "%s/outside/Battle.net/keep", directory);
    fixture_put(outside, "outside");
    snprintf(ancestor, sizeof(ancestor), "%s/drive_c/ProgramData", menu_prefix_root);
    {
        char parent[1024], target[1024];
        snprintf(parent, sizeof(parent), "%s/drive_c", menu_prefix_root);
        fixture_mkdirs(parent);
        snprintf(target, sizeof(target), "%s/outside", directory);
        assert(!symlink(target, ancestor));
    }
    check(menu_remove_request(WB_USER_ROOT "prefix/drive_c/ProgramData/Battle.net") < 0,
          "production broker refuses a symlinked ancestor");
    check(!stat(outside, &status) && S_ISREG(status.st_mode), "ancestor symlink keeps outside tree");
    assert(!unlink(ancestor));

    snprintf(local, sizeof(local), "%s/drive_c/users/steamuser/AppData/Local/Battle.net",
             menu_prefix_root);
    fixture_mkdirs(local);
    snprintf(link, sizeof(link), "%s/wow-link", local);
    {
        char target[1024];
        snprintf(target, sizeof(target), "%s/drive_c/Program Files (x86)/World of Warcraft",
                 menu_prefix_root);
        assert(!symlink(target, link));
    }
    check(!menu_remove_request(
              WB_USER_ROOT "prefix/drive_c/users/steamuser/AppData/Local/Battle.net"),
          "production broker unlinks an internal symlink without descending");
    check(!stat(wow, &status) && S_ISREG(status.st_mode), "internal symlink keeps WoW");
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
#define InterlockedCompareExchange(pointer, value, expected) \
    __sync_val_compare_and_swap(pointer, expected, value)
#define min(a, b) ((a) < (b) ? (a) : (b))

static LONG64 clock_us = 1000000;
static _Thread_local unsigned int current_thread_id;
static _Thread_local int observed_slot = -1;
static int broker_mode, title_enabled = 1, connected, old_queries;
static const char *handle_path = WB_USER_ROOT "space";
static unsigned int errno_to_status(int error) { return (unsigned int)error; }
static LONG64 title_io_now_us(void) { return clock_us; }
static int ps5_direct_enabled(void) { return title_enabled; }
static unsigned int GetCurrentThreadId(void) { return current_thread_id; }
static unsigned int server_get_unix_name(HANDLE handle, char **path)
{
    *path = strdup(handle == 2 ? WB_USER_ROOT "writes" : handle_path);
    assert(*path);
    return 0;
}
static void sceKernelDebugOutText(int channel, const char *line)
{
    (void)channel;
    if (strstr(line, "disk space source=broker"))
        __atomic_fetch_add(&source_broker_logs, 1, __ATOMIC_SEQ_CST);
    if (strstr(line, "disk space source=synthetic"))
        __atomic_fetch_add(&source_fallback_logs, 1, __ATOMIC_SEQ_CST);
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
    __atomic_fetch_add(&connected, 1, __ATOMIC_SEQ_CST);
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

/* Windows virtual-memory checks become ordinary host-memory access. The
 * actual bounce pool and complete client request/write code are extracted. */
static int virtual_check_buffer_for_read(const void *buffer, size_t length)
{
    return buffer || !length;
}

static size_t virtual_uninterrupted_read_memory(const void *source, void *destination, size_t length)
{
    memcpy(destination, source, length);
    return length;
}


/* Exercise the title's statvfs branch, including the existing shim fallback. */
#undef linux
#define __PROSPERO__ 1
#define open(path, flags) client_open(path, flags)
#define connect(fd, address, length) client_connect(fd, address, length)
#define fstatvfs(fd, info) synthetic_fstatvfs(fd, info)
#define pthread_mutex_lock(mutex) client_mutex_lock(mutex)
#include "writebroker-client.inc"
#undef pthread_mutex_lock
#undef fstatvfs
#undef connect
#undef open

static int client_mutex_lock(pthread_mutex_t *mutex)
{
    for (unsigned int i = 0; i < BROKER_CONNECTIONS; i++)
    {
        if (mutex != &broker[i].lock) continue;
        assert(!pthread_mutex_lock(&request_lock));
        if (track_lock_attempts)
        {
            lock_attempt_mask |= 1u << i;
            assert(!pthread_cond_broadcast(&request_cond));
        }
        assert(!pthread_mutex_unlock(&request_lock));
        observed_slot = i;
        break;
    }
    return pthread_mutex_lock(mutex);
}

static void close_connections(void)
{
    pthread_once(&broker_once, broker_init);
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
    check(info.ActualAvailableAllocationUnits.QuadPart > 0,
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

static void pool_index_tests(void)
{
    static const struct { unsigned int id, slot; } cases[] = {
        {60, 15}, {64, 0}, {68, 1}, {124, 15}, {128, 0}, {132, 1},
        {UINT32_MAX - 3, 15},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check(broker_windows_index(cases[i].id) == cases[i].slot,
              "aligned Wine thread ID selects expected slot at wrap boundaries");
    check(broker_windows_index(64) == broker_windows_index(128) &&
          broker_windows_index(60) == broker_windows_index(124),
          "Wine thread IDs one pool apart collide deterministically");
}

enum { POOL_WORKERS = 32, POOL_ROUNDS = 4, POOL_STRIDE = 128 };

struct pool_worker
{
    unsigned int number;
    unsigned char bytes[POOL_STRIDE];
    size_t length;
    ssize_t results[POOL_ROUNDS];
    int query_ok, slot;
};

static void *pool_worker(void *arg)
{
    struct pool_worker *worker = arg;
    struct wb_statfs_reply reply;

    current_thread_id = 4 * (16 + worker->number);
    /* Race the once initializer from early space queries and early writes. */
    worker->query_ok = 1;
    if (!(worker->number & 1))
        worker->query_ok = broker_query_fs(1, &reply) && observed_slot == (int)(worker->number % 16);
    for (unsigned int i = 0; i < POOL_ROUNDS; i++)
        worker->results[i] = broker_pwrite(2, worker->bytes, worker->length,
                                           worker->number * POOL_STRIDE, worker->number & 1, NULL);
    worker->slot = observed_slot;
    return NULL;
}

static int wait_for_pool(int contenders)
{
    struct timespec deadline;
    int result = 0;

    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 10;
    while (contenders ? lock_attempt_mask != 0xffffu : waiting_requests != 16)
    {
        result = pthread_cond_timedwait(&request_cond, &request_lock, &deadline);
        if (result == ETIMEDOUT) return 0;
        assert(!result);
    }
    return 1;
}

static void pool_tests(void)
{
    _Static_assert(BROKER_CONNECTIONS == 16, "broker pool must provide sixteen slots");
    struct pool_worker workers[POOL_WORKERS] = {0};
    pthread_t clients[POOL_WORKERS];
    unsigned char expected[POOL_WORKERS * POOL_STRIDE], actual[sizeof(expected)];
    unsigned long long expected_bytes = 0;
    struct stat info;
    int fd = open(writes_path, O_CREAT | O_EXCL | O_RDWR, 0600), all_waiting;

    assert(fd >= 0);
    memset(expected, 0xcc, sizeof(expected));
    assert(write(fd, expected, sizeof(expected)) == sizeof(expected));
    broker_mode = 1;
    hold_requests = 1;
    for (unsigned int i = 0; i < POOL_WORKERS; i++)
    {
        workers[i].number = i;
        workers[i].length = 32 + i;
        for (size_t j = 0; j < workers[i].length; j++) workers[i].bytes[j] = i * 7 + j;
        memcpy(expected + i * POOL_STRIDE, workers[i].bytes, workers[i].length);
        expected_bytes += workers[i].length * POOL_ROUNDS;
    }
    /* Consecutive aligned Wine IDs cover every slot; the second wave wraps
     * to those same slots and must wait for the locked request streams. */
    for (unsigned int i = 0; i < 16; i++)
        assert(!pthread_create(&clients[i], NULL, pool_worker, &workers[i]));
    assert(!pthread_mutex_lock(&request_lock));
    all_waiting = wait_for_pool(0);
    check(all_waiting && peak_requests == 16, "sixteen broker writes concurrently reach real pwrite");
    if (all_waiting)
    {
        for (unsigned int i = 0; i < BROKER_CONNECTIONS; i++)
        {
            int result = pthread_mutex_trylock(&broker[i].lock);
            check(result == EBUSY, "each distinct broker slot stays locked until reply");
            if (!result) assert(!pthread_mutex_unlock(&broker[i].lock));
            check(broker[i].fd >= 0, "each of sixteen slots owns a live connection");
        }
    }
    track_lock_attempts = 1;
    assert(!pthread_mutex_unlock(&request_lock));
    for (unsigned int i = 16; i < POOL_WORKERS; i++)
        assert(!pthread_create(&clients[i], NULL, pool_worker, &workers[i]));
    assert(!pthread_mutex_lock(&request_lock));
    if (all_waiting) check(wait_for_pool(1), "second wave attempts every occupied slot");
    track_lock_attempts = hold_requests = 0;
    assert(!pthread_cond_broadcast(&request_cond));
    assert(!pthread_mutex_unlock(&request_lock));
    for (unsigned int i = 0; i < POOL_WORKERS; i++)
    {
        assert(!pthread_join(clients[i], NULL));
        check(workers[i].query_ok, "concurrent space query completes on shared pool");
        check(workers[i].slot == (int)(i % 16), "Windows thread IDs select all sixteen slots");
        for (unsigned int j = 0; j < POOL_ROUNDS; j++)
            check(workers[i].results[j] == (ssize_t)workers[i].length,
                  "concurrent write receives its exact synchronous reply");
    }
    check(connected == 16, "busy client opens exactly sixteen pooled sockets");
    check(served_writes == POOL_WORKERS * POOL_ROUNDS && served_bytes == expected_bytes,
          "one broker write request per synchronous write, no coalescing");
    check(!fstat(fd, &info) && info.st_size == sizeof(expected) &&
          pread(fd, actual, sizeof(actual), 0) == sizeof(actual) &&
          !memcmp(actual, expected, sizeof(actual)),
          "all completed writes are immediately visible with exact size/bytes and untouched gaps");
    {
        unsigned int native_slot = (uintptr_t)pthread_self() % 16;
        current_thread_id = 4 * (16 + (native_slot + 1) % 16);
        check(broker_pwrite(2, workers[0].bytes, workers[0].length, 0, 1,
                            WB_USER_ROOT "writes") == (ssize_t)workers[0].length &&
              observed_slot == (int)native_slot,
              "known-path writes use native thread ID with the sixteen-slot pool");
    }
    close(fd);
    close_connections();
    /* Closing a stream does not reinitialize its mutex or leave fd zero. */
    current_thread_id = 4 * 31;
    check(broker_pwrite(2, workers[31].bytes, workers[31].length, 31 * POOL_STRIDE, 1, NULL) ==
          (ssize_t)workers[31].length && observed_slot == 15 && connected == 17,
          "last slot reconnects safely after connection cleanup");
    close_connections();
    current_thread_id = 4 * 16;
    broker_mode = connected = 0;
    unlink(writes_path);
}

int main(void)
{
    int fd;
    assert(WB_VERSION == 6);
    wine_ps5_title_critical_signals(&ps5_title_block_set);
    assert(mkdtemp(directory));
    snprintf(token_path, sizeof(token_path), "%s/token", directory);
    snprintf(writes_path, sizeof(writes_path), "%s/writes", directory);
    fd = open(token_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, token, sizeof(token)) == sizeof(token));
    close(fd);
    pool_index_tests();
    pool_tests();
    protocol_tests();
    menu_removal_tests();
    permission_repair_tests();
    close_connections();
    client_tests();
    for (unsigned int i = 0; i < BOUNCE_SLOTS; i++)
        if (bounce_buffer[i]) assert(!munmap(bounce_buffer[i], BOUNCE_SIZE));
    {
        char cleanup[1100];
        snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", directory);
        assert(!system(cleanup));
    }
    if (failures) return 1;
    puts("write broker: concurrent writes/statfs, no-follow removal, device-bounded startup permission repair PASS");
    return 0;
}
