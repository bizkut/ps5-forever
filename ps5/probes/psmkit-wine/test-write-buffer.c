#define _GNU_SOURCE 1
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wine/ps5_title_private.h"

typedef void *HANDLE;
typedef int32_t NTSTATUS;
typedef int32_t LONG;
typedef int64_t LONG64;
typedef union { struct { uint32_t LowPart; int32_t HighPart; } u; int64_t QuadPart; } LARGE_INTEGER;
typedef uint32_t ULONG;
typedef unsigned int UINT;
typedef int BOOL;
typedef uintptr_t ULONG_PTR, client_ptr_t;
typedef void (*PIO_APC_ROUTINE)(void *, void *, ULONG);
typedef void (*PNTAPCFUNC)(ULONG_PTR, ULONG_PTR, ULONG_PTR);
typedef struct { NTSTATUS Status; ULONG_PTR Information; } IO_STATUS_BLOCK;
enum server_fd_type { FD_TYPE_FILE, FD_TYPE_SERIAL, FD_TYPE_DEVICE, FD_TYPE_SOCKET };
struct io_timeouts { int unused; };
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xc0000005u)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000du)
#define TRUE 1
#define FILE_WRITE_DATA 0x0002
#define FILE_APPEND_DATA 0x0004
#define FILE_WRITE_THROUGH 0x0002
#define FILE_NO_INTERMEDIATE_BUFFERING 0x0008
#define FILE_SYNCHRONOUS_IO_ALERT 0x0010
#define FILE_SYNCHRONOUS_IO_NONALERT 0x0020
#define FILE_WRITE_TO_END_OF_FILE ((int64_t)-1)
#define TRACE(...) do {} while (0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001u)
#define STATUS_INVALID_USER_BUFFER ((NTSTATUS)0xc00000e8u)
#define STATUS_DISK_FULL ((NTSTATUS)0xc000007fu)
#define STATUS_BAD_DEVICE_TYPE ((NTSTATUS)0xc00000cau)
#define SEC_IMAGE 0x01000000u
#define FALSE 0
#define FILE_USE_FILE_POINTER_POSITION ((int64_t)-2)
#define TITLE_IO_WRITE 1
#define InterlockedExchange(ptr,val) __atomic_exchange_n((ptr),(val),__ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(ptr,val,cmp) __extension__ ({ \
    __typeof__(*(ptr)) expected_ = (cmp); \
    __atomic_compare_exchange_n((ptr), &expected_, (val), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    expected_; })
#define InterlockedIncrement(ptr) __atomic_add_fetch((ptr),1,__ATOMIC_SEQ_CST)
#define InterlockedDecrement(ptr) __atomic_sub_fetch((ptr),1,__ATOMIC_SEQ_CST)
static LONG64 title_io_coalesced;
#define InterlockedIncrement64(ptr) __atomic_add_fetch((ptr),1,__ATOMIC_SEQ_CST)

sigset_t ps5_title_block_set;
static const char *test_path;
static const char *test_drive_root = "/tmp";
static HANDLE identity_handle;
static int identity_fd = -1;
static unsigned int identity_options;
static int identity_append_only;
static unsigned int event_sets, event_resets, apc_queued, completions;
static ULONG_PTR completion_bytes, completion_context, apc_iosb, apc_context;
static NTSTATUS completion_status;
static BOOL completion_pending;
static int copy_fails, writes_fail, fail_once, zero_once, write_errno;
static int partial_once, writer_block, writer_entered, writer_done, admission_waiting_mutation;
static int close_hook_enabled, close_barrier_done, allow_close_retire, close_done;
static NTSTATUS close_status;
static unsigned int backing_calls, growing_calls;
static pthread_mutex_t broker_slot = PTHREAD_MUTEX_INITIALIZER;
static unsigned int broker_calls, pinned_calls;
static char diagnostic_line[192];
static unsigned int diagnostic_lines;
static char measurement_line[640];
static unsigned int measurement_lines;
static char drained_line[2048], offsets_line[2048];
static unsigned int drained_lines, offsets_lines;
static char extents_line[1024];
static unsigned int extents_lines;
static unsigned char flush_log[64];
static unsigned int flush_count;

static LONG64 title_io_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (LONG64)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static const char *ps5_title_drive(char drive)
{
    (void)drive;
    return test_drive_root;
}

static int server_get_unix_name(HANDLE handle, char **path)
{
    (void)handle;
    if (!test_path) return -1;
    *path = strdup(test_path);
    return *path ? 0 : -1;
}
static NTSTATUS server_get_unix_fd(HANDLE handle, unsigned int access, int *fd,
                                    int *needs_close, void *type, unsigned int *options)
{
    if (identity_append_only && access == FILE_WRITE_DATA) return STATUS_ACCESS_DENIED;
    if (handle != identity_handle || identity_fd == -1) return STATUS_BAD_DEVICE_TYPE;
    *fd = identity_fd;
    *needs_close = 0;
    if (type) *(enum server_fd_type *)type = FD_TYPE_FILE;
    if (options) *options = identity_options;
    return STATUS_SUCCESS;
}

static size_t virtual_uninterrupted_read_memory(const void *source, void *dest, size_t size)
{
    if (copy_fails) { copy_fails--; return 0; }
    memcpy(dest, source, size);
    return size;
}

static NTSTATUS errno_to_status(int error)
{
    return error == ENOSPC ? STATUS_DISK_FULL : STATUS_UNSUCCESSFUL;
}

static void sceKernelDebugOutText(int channel, const char *text)
{
    (void)channel;
    if (strstr(text, "writebuf state="))
    {
        snprintf(diagnostic_line, sizeof(diagnostic_line), "%s", text);
        diagnostic_lines++;
    }
    if (strstr(text, "writebuf diag"))
    {
        snprintf(measurement_line, sizeof(measurement_line), "%s", text);
        measurement_lines++;
    }
    if (strstr(text, "writebuf drained"))
    {
        snprintf(drained_line, sizeof(drained_line), "%s", text);
        drained_lines++;
    }
    if (strstr(text, "writebuf extents"))
    {
        snprintf(extents_line, sizeof(extents_line), "%s", text);
        extents_lines++;
    }
    if (strstr(text, "writebuf offsets"))
    {
        snprintf(offsets_line, sizeof(offsets_line), "%s", text);
        offsets_lines++;
    }
}

static void title_io_note(int kind, LONG64 bytes, LONG64 started)
{
    (void)kind;
    (void)bytes;
    (void)started;
}

static ssize_t test_pwrite(int fd, const void *bytes, size_t length, off_t offset)
{
    backing_calls++;
    {
        struct stat st;
        if (!fstat(fd, &st) && offset >= 0 &&
            (uint64_t)offset + length > (uint64_t)st.st_size)
            growing_calls++;
    }
    if (length && flush_count < sizeof(flush_log)) flush_log[flush_count++] = *(const unsigned char *)bytes;
    __atomic_store_n(&writer_entered, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&writer_block, __ATOMIC_ACQUIRE)) usleep(1000);
    if (partial_once)
    {
        size_t part = length > 2 ? 2 : length;
        partial_once = 0;
        return pwrite(fd, bytes, part, offset);
    }
    if (fail_once) { fail_once--; errno = EIO; return -1; }
    if (zero_once) { zero_once--; return 0; }
    if (writes_fail || write_errno) { errno = write_errno ? write_errno : EIO; return -1; }
    return pwrite(fd, bytes, length, offset);
}

static ssize_t title_file_pwrite_native(HANDLE handle, int fd, const void *bytes,
                                        size_t length, off_t offset, const char *path)
{
    ssize_t ret;
    int route_fd;
    (void)handle;
    pthread_mutex_lock(&broker_slot);
    broker_calls++;
    route_fd = path ? open(path, O_WRONLY) : -1;
    ret = test_pwrite(route_fd == -1 ? fd : route_fd, bytes, length, offset);
    if (route_fd != -1) close(route_fd);
    pthread_mutex_unlock(&broker_slot);
    return ret;
}

static ssize_t title_write_trace_pwrite(int fd, const void *bytes, size_t length,
                                        off_t offset, LONG64 *native_us)
{
    (void)native_us;
    pinned_calls++;
    return test_pwrite(fd, bytes, length, offset);
}
static void title_buffer_test_wait_hook(int flushing, int mutations)
{
    if (!flushing && mutations)
        __atomic_store_n(&admission_waiting_mutation, 1, __ATOMIC_RELEASE);
}
static void title_buffer_test_close_hook(HANDLE handle)
{
    (void)handle;
    if (!close_hook_enabled) return;
    __atomic_store_n(&close_barrier_done, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&allow_close_retire, __ATOMIC_ACQUIRE)) usleep(1000);
}



#include "wine-write-buffer.inc"

static int ps5_direct_enabled(void) { return 1; }
static int virtual_check_buffer_for_read(const void *bytes, size_t size) { return !size || bytes != NULL; }
static client_ptr_t iosb_client_ptr(IO_STATUS_BLOCK *io) { return (client_ptr_t)io; }
static NTSTATUS server_write_file(HANDLE handle, ...) { (void)handle; abort(); }
static ssize_t title_file_pwrite(HANDLE handle, int fd, const void *bytes, size_t size, off_t offset)
{
    (void)handle;
    return test_pwrite(fd, bytes, size, offset);
}
static void set_sync_iosb(IO_STATUS_BLOCK *io, NTSTATUS status, ULONG_PTR bytes, unsigned int options)
{
    (void)options;
    io->Status = status;
    io->Information = bytes;
}
static NTSTATUS NtSetEvent(HANDLE event, void *previous)
{
    (void)event; (void)previous;
    event_sets++;
    return STATUS_SUCCESS;
}
static NTSTATUS NtResetEvent(HANDLE event, void *previous)
{
    (void)event; (void)previous;
    event_resets++;
    return STATUS_SUCCESS;
}
#define GetCurrentThread() ((HANDLE)(uintptr_t)0x44)
static NTSTATUS NtQueueApcThread(HANDLE thread, PNTAPCFUNC apc, ULONG_PTR context,
                                ULONG_PTR io, ULONG_PTR unused)
{
    (void)thread; (void)apc; (void)unused;
    apc_queued++;
    apc_context = context;
    apc_iosb = io;
    return STATUS_SUCCESS;
}
static void add_completion(HANDLE handle, ULONG_PTR context, NTSTATUS status, ULONG_PTR bytes, BOOL pending)
{
    (void)handle;
    completions++;
    completion_context = context;
    completion_status = status;
    completion_bytes = bytes;
    completion_pending = pending;
}
static void set_pending_write(HANDLE handle) { (void)handle; abort(); }

#define __PROSPERO__ 1
#include "wine-write-file.inc"
#undef __PROSPERO__

static void reset_buffer(void)
{
    struct wine_ps5_title_lock lock;

    title_buffer_lock(&lock);
    for (unsigned int i = 0; i < WINE_PS5_TITLE_BUFFER_RUNS; i++)
    {
        while (title_buffer.runs[i].flushing)
        {
            title_buffer_unlock(&lock);
            usleep(1000);
            title_buffer_lock(&lock);
        }
        if (title_buffer.runs[i].fd != -1) close(title_buffer.runs[i].fd);
        title_buffer_clear_run_locked(&title_buffer.runs[i]);
    }
    title_buffer.next_sequence = 1;
    title_buffer.excluded_count = 0;
    title_buffer.barrier_count = title_buffer.barrier_active = 0;
    memset(&title_buffer_diag, 0, sizeof(title_buffer_diag));
    title_buffer_unlock(&lock);
    title_buffer_ready = 0;
    title_buffer_shutdown = title_buffer_joined = 0;
    title_buffer_disabled = title_buffer_failure_logged = 0;
    title_buffer_enabled_logged = title_buffer_bypass_logged = title_buffer_disabled_logged = 0;
    title_buffer_mutations = 0;
    title_buffer_mutation_generation = 0;
    title_buffer_switch_mode = 1;
    copy_fails = writes_fail = fail_once = zero_once = write_errno = 0;
    partial_once = writer_block = writer_entered = writer_done = admission_waiting_mutation = 0;
    close_hook_enabled = close_barrier_done = allow_close_retire = close_done = 0;
    backing_calls = growing_calls = 0;
    unsetenv("WINEPS5_WRITE_BUFFER");
    memset(flush_log, 0, sizeof(flush_log));
    flush_count = 0;
    broker_calls = pinned_calls = 0;
    diagnostic_line[0] = 0;
    diagnostic_lines = 0;
    measurement_line[0] = 0;
    measurement_lines = 0;
    drained_line[0] = offsets_line[0] = 0;
    drained_lines = offsets_lines = 0;
    extents_line[0] = 0;
    extents_lines = 0;
    identity_handle = 0;
    identity_fd = -1;
    identity_options = 0;
    identity_append_only = 0;
    event_sets = event_resets = apc_queued = completions = 0;
    completion_bytes = completion_context = apc_iosb = apc_context = 0;
}

static int fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;
    int count = 0;
    assert(dir);
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') count++;
    closedir(dir);
    return count;
}

static int make_file(char *path)
{
    strcpy(path, "/tmp/wine-ps5-buffer-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    test_path = path;
    return fd;
}
static int make_scoped_file(char *root, char *path)
{
    char dir[256];
    strcpy(root, "/tmp/wine-ps5-scope-XXXXXX");
    assert(mkdtemp(root));
    snprintf(dir, sizeof(dir), "%s/Games", root); assert(!mkdir(dir, 0700));
    snprintf(dir, sizeof(dir), "%s/Games/World of Warcraft", root); assert(!mkdir(dir, 0700));
    snprintf(dir, sizeof(dir), "%s/Games/World of Warcraft/Data", root); assert(!mkdir(dir, 0700));
    snprintf(dir, sizeof(dir), "%s/Games/World of Warcraft/Data/data", root); assert(!mkdir(dir, 0700));
    snprintf(path, 256, "%s/Games/World of Warcraft/Data/data/data.000", root);
    test_path = path;
    return open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
}

static void remove_scoped_file(const char *root, const char *path)
{
    char dir[256];
    unlink(path);
    snprintf(dir, sizeof(dir), "%s/Games/World of Warcraft/Data/data", root); rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/Games/World of Warcraft/Data", root); rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/Games/World of Warcraft", root); rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/Games", root); rmdir(dir);
    rmdir(root);
}

static void start_run(HANDLE handle, int fd, const char *path, const char *bytes, size_t size)
{
    struct stat st;
    int handled;
    off_t pos;
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, bytes, size, 0, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == (off_t)size);
    assert(diagnostic_lines == 1 && strstr(diagnostic_line, "state=enabled reason=eligible"));
}

static void test_mapping_identity_isolation(void)
{
    char path[64], other[64], bytes[3];
    HANDLE mapped = (HANDLE)(uintptr_t)0x160, unrelated = (HANDLE)(uintptr_t)0x164;
    HANDLE alias = (HANDLE)(uintptr_t)0x168;
    char *view;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path), other_fd = make_file(other);

    reset_buffer();
    test_path = "/tmp/ProgramData/Battle.net/Agent/data/archive.bin";
    start_run(mapped, fd, path, "old", 3);
    start_run(unrelated, other_fd, other, "new", 3);
    assert(!ps5_title_prepare_buffered_section(mapped, 0));
    assert(!title_buffer_disabled);
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 1);
    view = mmap(NULL, 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(view != MAP_FAILED && !memcmp(view, "old", 3));
    memcpy(view, "map", 3);
    identity_handle = alias;
    identity_fd = fd;
    assert(!ps5_title_prepare_buffered_section(alias, 0));
    assert(title_buffer.excluded_count == 1 && !title_buffer_disabled);
    assert(!ps5_title_close_coalesced_write(mapped));
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(alias, fd, "bad", 3, 0, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled && title_buffer_test_pending_count() == 1);
    assert(!title_buffer_flush(0, 0, 0));
    assert(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!memcmp(bytes, "map", sizeof(bytes)));
    start_run(unrelated, other_fd, other, "yes", 3);
    assert(!title_buffer_disabled && title_buffer_test_pending_count() == 1);
    assert(!title_buffer_flush(0, 0, 0));
    assert(!memcmp(view, "map", 3) && !munmap(view, 3));
    close(fd); close(other_fd);
    unlink(path); unlink(other);
    puts("write-buffer production: mapping drains/excludes one identity and unrelated files keep buffering PASS");
}


static void test_apc(void *context, void *io, ULONG reserved)
{
    (void)context; (void)io; (void)reserved;
    assert(!"APC must be queued, not invoked inline");
}

static void test_overlapped_write_dispatch(void)
{
    static const unsigned int barriers[] =
    {
        WINE_PS5_TITLE_BUFFER_BARRIER_READ, WINE_PS5_TITLE_BUFFER_BARRIER_READ,
        WINE_PS5_TITLE_BUFFER_BARRIER_QUERY, WINE_PS5_TITLE_BUFFER_BARRIER_QUERY,
        WINE_PS5_TITLE_BUFFER_BARRIER_CLOSE, WINE_PS5_TITLE_BUFFER_BARRIER_LOCK_FLUSH
    };
    char root[] = "/tmp/wine-ps5-overlapped-XXXXXX", path[256];
    char bytes[7168], actual[14336], *part;
    HANDLE owner = (HANDLE)(uintptr_t)0x150, reader = (HANDLE)(uintptr_t)0x154;
    HANDLE event = (HANDLE)(uintptr_t)0x158;
    void *context = (void *)(uintptr_t)0xcafe;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER offset;
    BOOL synchronous;
    struct stat st;
    int fd;
    unsigned int mode, j;

    assert(mkdtemp(root));
    test_drive_root = root;
    snprintf(path, sizeof(path), "%s/Program Files (x86)/Battle.net/.battle.net/.extract/client.dll", root);
    for (part = path + strlen(root) + 1; *part; part++)
        if (*part == '/')
        {
            *part = 0;
            assert(!mkdir(path, 0700));
            *part = '/';
        }
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    for (mode = 0; mode < 2; mode++)
        for (j = 0; j < sizeof(barriers) / sizeof(barriers[0]); j++)
        {
            reset_buffer();
            assert(!ftruncate(fd, 0) && lseek(fd, 17, SEEK_SET) == 17);
            test_path = path;
            identity_handle = owner;
            identity_fd = fd;
            identity_options = mode ? FILE_NO_INTERMEDIATE_BUFFERING : 0;
            memset(bytes, 'a', sizeof(bytes));
            offset.QuadPart = 0;
            synchronous = TRUE;
            io.Status = STATUS_UNSUCCESSFUL;
            io.Information = 0;
            assert(title_write_file(owner, event, NULL, context, &io, bytes, sizeof(bytes),
                                    &offset, NULL, &synchronous) == STATUS_PENDING);
            assert(!synchronous && io.Status == STATUS_SUCCESS && io.Information == sizeof(bytes));
            assert(event_sets == 1 && !event_resets && completions == 1 && !apc_queued);
            assert(completion_context == (ULONG_PTR)context && completion_bytes == sizeof(bytes));
            assert(completion_status == STATUS_SUCCESS && completion_pending);
            memset(bytes, 'b', sizeof(bytes));
            offset.QuadPart = sizeof(bytes);
            assert(title_write_file(owner, event, NULL, context, &io, bytes, sizeof(bytes),
                                    &offset, NULL, NULL) == STATUS_PENDING);
            assert(io.Status == STATUS_SUCCESS && io.Information == sizeof(bytes));
            assert(event_sets == 2 && completions == 2);
            assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
            assert(!fstat(fd, &st) && st.st_size == 0 && lseek(fd, 0, SEEK_CUR) == 17);
            memset(bytes, 'c', sizeof(bytes));
            if (barriers[j] == WINE_PS5_TITLE_BUFFER_BARRIER_CLOSE)
                assert(!ps5_title_close_coalesced_write(owner));
            else
            {
                identity_handle = j == 1 || j == 3 ? reader : owner;
                assert(!ps5_title_flush_coalesced_write_reason(identity_handle, barriers[j]));
            }
            assert(!title_buffer_test_pending_count() && backing_calls == 1);
            assert(!fstat(fd, &st) && st.st_size == sizeof(actual));
            assert(pread(fd, actual, sizeof(actual), 0) == sizeof(actual));
            for (unsigned int i = 0; i < sizeof(actual); i++)
                assert(actual[i] == (i < sizeof(bytes) ? 'a' : 'b'));
        }

    reset_buffer();
    assert(!ftruncate(fd, 0));
    test_path = path;
    identity_handle = owner;
    identity_fd = fd;
    identity_options = FILE_NO_INTERMEDIATE_BUFFERING;
    offset.QuadPart = 0;
    assert(title_write_file(owner, event, test_apc, context, &io, "apc", 3,
                            &offset, NULL, NULL) == STATUS_PENDING);
    assert(io.Status == STATUS_SUCCESS && io.Information == 3);
    assert(event_sets == 1 && apc_queued == 1 && !completions);
    assert(apc_context == (ULONG_PTR)context && apc_iosb == (ULONG_PTR)&io);
    assert(!ps5_title_close_coalesced_write(owner));

    for (j = 0; j < 2; j++)
    {
        reset_buffer();
        assert(!ftruncate(fd, 0));
        test_path = path;
        identity_handle = owner;
        identity_fd = fd;
        identity_options = FILE_NO_INTERMEDIATE_BUFFERING;
        offset.QuadPart = 0;
        assert(title_write_file(owner, event, NULL, context, &io, "head", 4,
                                &offset, NULL, NULL) == STATUS_PENDING);
        assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
        identity_append_only = j;
        offset.QuadPart = j ? 0 : FILE_WRITE_TO_END_OF_FILE;
        assert(title_write_file(owner, event, NULL, context, &io, "tail", 4,
                                &offset, NULL, NULL) == STATUS_PENDING);
        assert(!title_buffer_test_pending_count() && backing_calls == 2);
        assert(!fstat(fd, &st) && st.st_size == 8);
        assert(pread(fd, actual, 8, 0) == 8 && !memcmp(actual, "headtail", 8));
    }

    reset_buffer();
    assert(!ftruncate(fd, 0));
    test_path = path;
    identity_handle = owner;
    identity_fd = fd;
    offset.QuadPart = 0;
    assert(title_write_file(owner, event, NULL, context, &io, "head", 4,
                            &offset, NULL, NULL) == STATUS_PENDING);
    identity_options = FILE_NO_INTERMEDIATE_BUFFERING | FILE_WRITE_THROUGH;
    offset.QuadPart = 4;
    assert(title_write_file(owner, event, NULL, context, &io, "tail", 4,
                            &offset, NULL, NULL) == STATUS_PENDING);
    assert(!title_buffer_test_pending_count() && backing_calls == 2);
    assert(pread(fd, actual, 8, 0) == 8 && !memcmp(actual, "headtail", 8));

    reset_buffer();
    identity_handle = owner;
    identity_fd = fd;
    offset.QuadPart = FILE_USE_FILE_POINTER_POSITION;
    assert(title_write_file(owner, event, NULL, context, &io, "bad", 3,
                            &offset, NULL, NULL) == STATUS_INVALID_PARAMETER);
    assert(title_write_file(owner, event, NULL, context, &io, "bad", 3,
                            NULL, NULL, NULL) == STATUS_INVALID_PARAMETER);
    assert(!title_buffer_test_pending_count() && !backing_calls && !event_sets && event_resets == 2);
    close(fd);
    assert(!unlink(path));
    while ((part = strrchr(path, '/')) && part > path + strlen(root))
    {
        *part = 0;
        assert(!rmdir(path));
    }
    test_drive_root = "/tmp";
    assert(!rmdir(root));
    puts("write-buffer production dispatch: overlapped explicit-offset/NO_BUFFERING, IOSB/event/"
         "completion-port/APC queue, copied bytes/pointer, read/query/close/flush, "
         "append and WRITE_THROUGH boundaries PASS");
}

static void test_scoped_visibility(void)
{
    static const char *paths[] =
    {
        "Games/World of Warcraft/Data/data/data.000",
        "Games/StarCraft II/SC2Data/data/data.000",
        "Program Files (x86)/Battle.net/Battle.net.123/client.dll",
        "ProgramData/Battle.net/Agent/Agent.123/Agent.exe"
    };
    static const unsigned int reasons[] =
    {
        WINE_PS5_TITLE_BUFFER_BARRIER_READ, WINE_PS5_TITLE_BUFFER_BARRIER_QUERY,
        WINE_PS5_TITLE_BUFFER_BARRIER_READ, WINE_PS5_TITLE_BUFFER_BARRIER_QUERY,
        WINE_PS5_TITLE_BUFFER_BARRIER_CLOSE, WINE_PS5_TITLE_BUFFER_BARRIER_LOCK_FLUSH,
        WINE_PS5_TITLE_BUFFER_BARRIER_DUPLICATE
    };
    HANDLE owner = (HANDLE)(uintptr_t)0x130, reader = (HANDLE)(uintptr_t)0x134;
    char root[] = "/tmp/wine-ps5-visible-XXXXXX", path[256], bytes[7168], actual[14336];
    struct stat st;
    unsigned int i, j;

    assert(mkdtemp(root));
    test_drive_root = root;
    memset(bytes, 'b', sizeof(bytes));
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
        char *part;
        snprintf(path, sizeof(path), "%s/%s", root, paths[i]);
        for (part = path + strlen(root) + 1; *part; part++)
            if (*part == '/')
            {
                *part = 0;
                assert(!mkdir(path, 0700));
                *part = '/';
            }
        for (j = 0; j < sizeof(reasons) / sizeof(reasons[0]); j++)
        {
            char *resolved = NULL;
            uint64_t dev, ino;
            LONG64 generation;
            off_t pos;
            int handled, eligible, need_path;
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);

            assert(fd >= 0);
            reset_buffer();
            test_path = path;
            need_path = title_buffer_needs_path(owner, &generation);
            eligible = title_buffer_resolve_start(owner, fd, need_path, &resolved, &dev, &ino);
            assert(eligible);
            assert(!title_buffer_write(owner, fd, bytes, sizeof(bytes), 0, resolved,
                                       dev, ino, generation, eligible, &handled, &pos));
            free(resolved);
            assert(handled && pos == sizeof(bytes));
            assert(!title_buffer_write(owner, fd, bytes, sizeof(bytes), pos, NULL,
                                       0, 0, generation, eligible, &handled, &pos));
            assert(handled && pos == sizeof(actual));
            assert(!fstat(fd, &st) && st.st_size == 0 && backing_calls == 0);
            if (reasons[j] == WINE_PS5_TITLE_BUFFER_BARRIER_CLOSE)
                assert(!ps5_title_close_coalesced_write(owner));
            else
            {
                identity_handle = j == 2 || j == 3 ? reader : owner;
                identity_fd = fd;
                assert(!ps5_title_flush_coalesced_write_reason(identity_handle, reasons[j]));
            }
            assert(!title_buffer_test_pending_count() && backing_calls == 1);
            close(fd);
            fd = open(path, O_RDONLY);
            assert(fd >= 0);
            assert(!fstat(fd, &st) && st.st_size == sizeof(actual));
            assert(pread(fd, actual, sizeof(actual), 0) == sizeof(actual));
            assert(!memcmp(actual, bytes, sizeof(bytes)));
            assert(!memcmp(actual + sizeof(bytes), bytes, sizeof(bytes)));
            close(fd);
        }
        assert(!unlink(path));
        while ((part = strrchr(path, '/')) && part > path + strlen(root))
        {
            *part = 0;
            assert(!rmdir(path));
        }
    }
    test_drive_root = "/tmp";
    assert(!rmdir(root));
    puts("write-buffer production: WoW/SC2 CASC and Battle.net 7 KiB coalescing, same/other-handle "
         "read/query, close/reopen, flush and duplicate visibility PASS");
}


static void test_sequential_and_barrier(void)
{
    char path[64], bytes[6];
    HANDLE handle = (HANDLE)(uintptr_t)0x100;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path);

    reset_buffer();
    test_path = path;
    start_run(handle, fd, path, "abc", 3);
    assert(!fstat(fd, &st) && st.st_size == 0);
    assert(!title_buffer_write(handle, fd, "def", 3, 3, NULL, 0, 0,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 6);
    assert(!title_buffer_flush(0, 0, 0));
    assert(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!memcmp(bytes, "abcdef", sizeof(bytes)) && backing_calls == 1);
    assert(title_buffer_diag.flushed_runs[WINE_PS5_TITLE_BUFFER_BARRIER_OTHER] == 1);
    assert(title_buffer_diag.flushed_bytes[WINE_PS5_TITLE_BUFFER_BARRIER_OTHER] == 6);
    close(fd);
    unlink(path);
    puts("write-buffer production: sequential explicit/file-pointer model and global barrier PASS");
}

static void test_measurement_window(void)
{
    char path[64];
    HANDLE handle = (HANDLE)(uintptr_t)0x102;
    struct wine_ps5_title_lock lock;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path);

    reset_buffer();
    start_run(handle, fd, path, "diag", 4);
    title_buffer_diag.last_us = title_io_now_us() - 10000001;
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, "x", 1, 4, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 5);
    assert(measurement_lines == 1);
    assert(strstr(measurement_line, "handles=1 identities=1 writes=1"));
    assert(strstr(measurement_line, "sizes=1/0/0/0/0"));
    assert(strstr(measurement_line, "mix=other_handle:0"));
    assert(title_buffer_diag.sample_handle_count == 1);
    assert(title_buffer_diag.samples[0].count == 1);
    assert(title_buffer_diag.samples[0].offsets[0] == 4);
    assert(title_buffer_diag.samples[0].lengths[0] == 1);
    title_buffer_lock(&lock);
    title_buffer_diag.last_us = title_io_now_us() - 10000001;
    title_buffer_diag_report_locked(title_io_now_us());
    title_buffer_unlock(&lock);
    assert(offsets_lines == 2);
    assert(strstr(offsets_line, "count=1 pairs=4+1"));
    assert(!title_buffer_flush(0, 0, 0));
    close(fd); unlink(path);
    puts("write-buffer production: 10-second measurement fields PASS");
}

static void test_extent_diagnostics(void)
{
    HANDLE handle = (HANDLE)(uintptr_t)0x122;
    struct wine_ps5_title_lock lock;

    reset_buffer();
    title_buffer_lock(&lock);
    title_buffer_diag_write_locked(handle, 1, 1, 30, 100, 0);
    title_buffer_diag_write_locked(handle, 1, 1, 30, 130, 1000);
    title_buffer_diag_write_locked(handle, 1, 1, 30, 200, 0);
    title_buffer_diag_write_locked(handle, 1, 1, 30, 50, 1000);
    assert(title_buffer_diag.extends_eof == 2);
    assert(title_buffer_diag.beyond_physical_eof == 2);
    assert(title_buffer_diag.gap30[0] == 1);
    assert(title_buffer_diag.gap30[1] == 1);
    assert(title_buffer_diag.gap30[7] == 1);
    title_buffer_diag.last_us = title_io_now_us() - 10000001;
    title_buffer_diag_report_locked(title_io_now_us());
    title_buffer_unlock(&lock);
    assert(extents_lines == 1);
    assert(strstr(extents_line, "opened=0 handle_evict=0 global_evict=0 extends_eof=2"));
    assert(strstr(extents_line, "beyond_physical=2"));
    assert(strstr(extents_line, "gap30=1/1/0/0/0/0/0/1"));
    puts("write-buffer production: extent churn, gap and EOF diagnostics PASS");
}

static void test_measurement_off_on_parity(void)
{
    char path[64];
    HANDLE handle = (HANDLE)(uintptr_t)0x104;
    struct title_buffer_diag on, off;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path);

    reset_buffer();
    start_run(handle, fd, path, "aaaa", 4);
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, "bbbbbbbb", 8, 4, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled);
    on = title_buffer_diag;
    assert(!title_buffer_flush(0, 0, 0));

    reset_buffer();
    assert(!setenv("WINEPS5_WRITE_BUFFER", "0", 1));
    assert(!title_buffer_write(handle, fd, "aaaa", 4, 0, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled);
    assert(!title_buffer_write(handle, fd, "bbbbbbbb", 8, 4, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled);
    off = title_buffer_diag;
    assert(on.writes == off.writes && on.handle_count == off.handle_count &&
           on.identity_count == off.identity_count);
    assert(!memcmp(on.sizes, off.sizes, sizeof(on.sizes)));
    close(fd); unlink(path);
    puts("write-buffer production: off/on measurement field parity PASS");
}

static void test_interleaved_two_files(void)
{
    char path_a[64], path_b[64], got[6];
    HANDLE handle_a = (HANDLE)(uintptr_t)0x101;
    HANDLE handle_b = (HANDLE)(uintptr_t)0x105;
    struct stat st;
    int handled;
    off_t pos;
    int fd_a = make_file(path_a);
    int fd_b = make_file(path_b);

    reset_buffer();
    test_path = path_a;
    start_run(handle_a, fd_a, path_a, "aaa", 3);
    assert(!fstat(fd_b, &st));
    assert(!title_buffer_write(handle_b, fd_b, "bbb", 3, 0, path_b, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 3);
    assert(title_buffer_test_pending_count() == 2);
    assert(!fstat(fd_a, &st));
    assert(!title_buffer_write(handle_a, fd_a, "AAA", 3, 3, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 6);
    assert(!fstat(fd_b, &st));
    assert(!title_buffer_write(handle_b, fd_b, "BBB", 3, 3, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 6 && backing_calls == 0);
    assert(!title_buffer_write((HANDLE)(uintptr_t)0x109, fd_a, "idx", 3, 0, NULL, 0, 0,
                               title_buffer_mutation_generation, 0, &handled, &pos));
    assert(!handled && backing_calls == 2 && flush_count == 2);
    assert(flush_log[0] == 'a' && flush_log[1] == 'b');
    assert(title_buffer_test_pending_count() == 0);
    assert(pread(fd_a, got, sizeof(got), 0) == sizeof(got) && !memcmp(got, "aaaAAA", 6));
    assert(pread(fd_b, got, sizeof(got), 0) == sizeof(got) && !memcmp(got, "bbbBBB", 6));
    close(fd_a); close(fd_b); unlink(path_a); unlink(path_b);
    puts("write-buffer production: interleaved two-file runs and idx FIFO barrier PASS");
}
static void test_multiextent_and_overlap(void)
{
    char path_a[64], path_b[64];
    HANDLE handle_a = (HANDLE)(uintptr_t)0x106;
    HANDLE handle_b = (HANDLE)(uintptr_t)0x10a;
    struct stat st;
    int handled;
    off_t pos;
    int fd_a = make_file(path_a);
    int fd_b = make_file(path_b);

    reset_buffer();
    start_run(handle_a, fd_a, path_a, "aaa", 3);
    start_run(handle_b, fd_b, path_b, "bbb", 3);
    assert(!fstat(fd_a, &st));
    assert(!title_buffer_write(handle_a, fd_a, "XXX", 3, 100, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && backing_calls == 0 && title_buffer_test_pending_count() == 3);
    assert(!title_buffer_write(handle_a, fd_a, "ddd", 3, 3, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 6);
    assert(!title_buffer_write(handle_a, fd_a, "YYY", 3, 103, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 106 && backing_calls == 0);
    assert(!title_buffer_write(handle_a, fd_a, "overlap", 7, 2, NULL, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled && backing_calls == 2 && flush_log[0] == 'X' && flush_log[1] == 'a');
    assert(title_buffer_test_pending_count() == 1);
    assert(!title_buffer_flush(0, 0, 0));
    assert(backing_calls == 3 && flush_log[2] == 'b');

    reset_buffer();
    assert(!ftruncate(fd_a, 0));
    start_run(handle_a, fd_a, path_a, "0", 1);
    for (unsigned int i = 1; i <= WINE_PS5_TITLE_BUFFER_EXTENTS_PER_HANDLE; i++)
    {
        char byte = '0' + i;
        assert(!fstat(fd_a, &st));
        assert(!title_buffer_write(handle_a, fd_a, &byte, 1, i * 100, NULL, 0, 0,
                                   title_buffer_mutation_generation, 1, &handled, &pos));
        assert(handled);
    }
    assert(backing_calls == WINE_PS5_TITLE_BUFFER_EXTENTS_PER_HANDLE);
    assert(title_buffer_test_pending_count() == 1);
    assert(title_buffer_diag.extents_opened == 5);
    assert(title_buffer_diag.handle_evictions == 1);
    assert(title_buffer_diag.global_evictions == 0);
    assert(!title_buffer_flush_path_reason(
        path_a, WINE_PS5_TITLE_BUFFER_BARRIER_QUERY));
    assert(!title_buffer_test_pending_count());
    assert(backing_calls == WINE_PS5_TITLE_BUFFER_EXTENTS_PER_HANDLE + 1);

    close(fd_a);
    close(fd_b);
    unlink(path_a);
    unlink(path_b);
    puts("write-buffer production: two-stream extents, overlap flush and oldest eviction PASS");
}

static void test_descending_identity_flush(void)
{
    char path[64], low[4096], middle[30], high[30], byte;
    HANDLE handle = (HANDLE)(uintptr_t)0x123;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path);

    memset(low, 'L', sizeof(low));
    memset(middle, 'M', sizeof(middle));
    memset(high, 'H', sizeof(high));
    reset_buffer();
    start_run(handle, fd, path, low, sizeof(low));
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, middle, sizeof(middle), 1 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled);
    assert(!title_buffer_write(handle, fd, high, sizeof(high), 2 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled && title_buffer_test_pending_count() == 3);
    assert(!title_buffer_flush(0, 0, 0));
    assert(growing_calls == 1 && backing_calls == 3);
    assert(flush_log[0] == 'H' && flush_log[1] == 'M' && flush_log[2] == 'L');
    assert(!fstat(fd, &st) && st.st_size == (2 << 20) + sizeof(high));
    assert(pread(fd, &byte, 1, 0) == 1 && byte == 'L');
    assert(pread(fd, &byte, 1, 1 << 20) == 1 && byte == 'M');
    assert(pread(fd, &byte, 1, 2 << 20) == 1 && byte == 'H');

    reset_buffer();
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, low, sizeof(low));
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, middle, sizeof(middle), 1 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled);
    assert(!title_buffer_write(handle, fd, high, sizeof(high), 2 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled);
    fail_once = 1;
    assert(title_buffer_flush(0, 0, 0) == STATUS_UNSUCCESSFUL);
    assert(title_buffer_test_pending_count() == 3);
    assert(!fstat(fd, &st) && st.st_size == 0);
    assert(!title_buffer_flush(0, 0, 0));

    reset_buffer();
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, low, sizeof(low));
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, middle, sizeof(middle), 1 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled);
    assert(!title_buffer_write(handle, fd, high, sizeof(high), 2 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled);
    assert(!title_buffer_flush_directory_reason(
        "/tmp", WINE_PS5_TITLE_BUFFER_BARRIER_DIRECTORY));
    assert(growing_calls == 1 && backing_calls == 3);
    assert(flush_log[0] == 'H' && flush_log[1] == 'M' && flush_log[2] == 'L');

    reset_buffer();
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, low, sizeof(low));
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, middle, sizeof(middle), 1 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled);
    assert(!title_buffer_write(handle, fd, high, sizeof(high), 2 << 20, NULL,
                               st.st_dev, st.st_ino, title_buffer_mutation_generation,
                               1, &handled, &pos));
    assert(handled);
    fail_once = 1;
    assert(title_buffer_flush_directory_reason(
        "/tmp", WINE_PS5_TITLE_BUFFER_BARRIER_DIRECTORY) == STATUS_UNSUCCESSFUL);
    assert(title_buffer_test_pending_count() == 3);
    assert(!fstat(fd, &st) && st.st_size == 0);
    assert(!title_buffer_flush_directory_reason(
        "/tmp", WINE_PS5_TITLE_BUFFER_BARRIER_DIRECTORY));

    close(fd);
    unlink(path);
    puts("write-buffer production: descending identity flush has one growing write PASS");
}

static void test_nonfile_write_dispatch_fence(void)
{
    char path_a[64], path_b[64];
    HANDLE handle = (HANDLE)(uintptr_t)0x10b;
    int fd_a = make_file(path_a);
    int fd_b = make_file(path_b);

    reset_buffer();
    start_run((HANDLE)(uintptr_t)0x103, fd_a, path_a, "aaa", 3);
    start_run((HANDLE)(uintptr_t)0x107, fd_b, path_b, "bbb", 3);
    assert(!title_buffer_fence_nonfile(handle, 0));
    assert(!title_buffer_test_pending_count() && backing_calls == 2);
    assert(flush_log[0] == 'a' && flush_log[1] == 'b');
    assert(!title_buffer_fence_nonfile(handle, 1) && backing_calls == 2);
    assert(title_buffer_diag.barriers[WINE_PS5_TITLE_BUFFER_EVENT_NONFILE_WRITE] == 1);
    assert(title_buffer_diag.flushed_runs[WINE_PS5_TITLE_BUFFER_EVENT_NONFILE_WRITE] == 2);
    assert(!ps5_title_close_coalesced_write((HANDLE)(uintptr_t)0xdead));
    assert(title_buffer_diag.barriers[WINE_PS5_TITLE_BUFFER_EVENT_CLOSE_UNRELATED] == 1);
    assert(!ps5_title_flush_coalesced_write(0));
    assert(title_buffer_diag.barriers[WINE_PS5_TITLE_BUFFER_BARRIER_VIRTUAL] == 1);
    close(fd_a); close(fd_b); unlink(path_a); unlink(path_b);
    puts("write-buffer production: NtWriteFile nonregular dispatch fence PASS");
}

static void test_scoped_handle_barriers(void)
{
    char path_a[64], path_b[64];
    HANDLE owner = (HANDLE)(uintptr_t)0x120;
    HANDLE other = (HANDLE)(uintptr_t)0x121;
    struct stat st;
    int fd_a = make_file(path_a);
    int fd_b = make_file(path_b);

    reset_buffer();
    start_run(owner, fd_a, path_a, "aaa", 3);
    assert(!ps5_title_flush_coalesced_write_reason(
        other, WINE_PS5_TITLE_BUFFER_BARRIER_CONTROL));
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
    assert(!title_buffer_flush(0, 0, 0));

    reset_buffer();
    start_run(owner, fd_a, path_a, "bbb", 3);
    assert(!ps5_title_flush_coalesced_write_reason(
        owner, WINE_PS5_TITLE_BUFFER_BARRIER_CONTROL));
    assert(!title_buffer_test_pending_count() && backing_calls == 1);
    reset_buffer();
    start_run(owner, fd_a, path_a, "vol", 3);
    assert(!ps5_title_flush_coalesced_write_reason(
        owner, WINE_PS5_TITLE_BUFFER_BARRIER_VOLUME));
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
    assert(!ps5_title_flush_all_coalesced_writes_reason(
        WINE_PS5_TITLE_BUFFER_BARRIER_VOLUME));
    assert(!title_buffer_test_pending_count() && backing_calls == 1);


    reset_buffer();
    start_run(owner, fd_a, path_a, "ccc", 3);
    identity_handle = other;
    identity_fd = fd_b;
    assert(!ps5_title_flush_coalesced_write_reason(
        other, WINE_PS5_TITLE_BUFFER_BARRIER_LOCK_FLUSH));
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
    assert(!title_buffer_flush(0, 0, 0));
    reset_buffer();
    start_run(owner, fd_a, path_a, "disk", 4);
    identity_handle = other;
    identity_fd = fd_a;
    assert(!title_buffer_device_control_barrier(other, 1));
    assert(!title_buffer_test_pending_count() && backing_calls == 1);


    reset_buffer();
    start_run(owner, fd_a, path_a, "ddd", 3);
    assert(!title_buffer_flush_path_reason(
        path_b, WINE_PS5_TITLE_BUFFER_BARRIER_ATTRIBUTES));
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
    assert(!title_buffer_path_needs_mutation(path_b));
    assert(title_buffer_path_needs_mutation(path_a));
    assert(!title_buffer_flush_path_reason(
        path_a, WINE_PS5_TITLE_BUFFER_BARRIER_CREATE_OPEN));
    assert(!title_buffer_test_pending_count() && backing_calls == 1);

    reset_buffer();
    assert(!ftruncate(fd_a, 0));
    start_run(owner, fd_a, path_a, "eee", 3);
    assert(!fstat(fd_a, &st) && st.st_size == 0);
    assert(!title_buffer_flush_directory_reason(
        "/tmp", WINE_PS5_TITLE_BUFFER_BARRIER_DIRECTORY));
    assert(!fstat(fd_a, &st) && st.st_size == 3);

    reset_buffer();
    assert(!ftruncate(fd_a, 0));
    start_run(owner, fd_a, path_a, "fff", 3);
    assert(!ps5_title_flush_all_coalesced_writes_reason(
        WINE_PS5_TITLE_BUFFER_BARRIER_DUPLICATE));
    assert(!fstat(fd_a, &st) && st.st_size == 3);

    close(fd_a);
    close(fd_b);
    unlink(path_a);
    unlink(path_b);
    puts("write-buffer production: scoped control/lock/path/directory and duplicate fallback PASS");
}

static void test_unrelated_close_drain_attribution(void)
{
    char path[64];
    HANDLE owner = (HANDLE)(uintptr_t)0x10c;
    struct wine_ps5_title_lock lock;
    int fd = make_file(path);

    reset_buffer();
    start_run(owner, fd, path, "abc", 3);
    assert(!ps5_title_close_coalesced_write((HANDLE)(uintptr_t)0xdead));
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
    assert(title_buffer_diag.barriers[WINE_PS5_TITLE_BUFFER_EVENT_CLOSE_UNRELATED] == 1);
    assert(title_buffer_diag.flushed_runs[WINE_PS5_TITLE_BUFFER_EVENT_CLOSE_UNRELATED] == 0);
    assert(title_buffer_diag.flushed_bytes[WINE_PS5_TITLE_BUFFER_EVENT_CLOSE_UNRELATED] == 0);
    title_buffer_lock(&lock);
    title_buffer_diag.last_us = title_io_now_us() - 10000001;
    title_buffer_diag_report_locked(title_io_now_us());
    title_buffer_unlock(&lock);
    assert(drained_lines == 1);
    assert(strstr(drained_line, "nonfile:0,close_unrelated:0,other:0"));
    assert(!title_buffer_flush(0, 0, 0));
    close(fd);
    unlink(path);
    puts("write-buffer production: unrelated close does not drain PASS");
}

static void test_same_identity_two_handles(void)
{
    char path[64], got[6];
    HANDLE handle_a = (HANDLE)(uintptr_t)0x10d;
    HANDLE handle_b = (HANDLE)(uintptr_t)0x111;
    struct stat st;
    int handled;
    off_t pos;
    int fd_a = make_file(path);
    int fd_b = open(path, O_RDWR);

    assert(fd_b >= 0);
    reset_buffer();
    test_path = path;
    start_run(handle_a, fd_a, path, "aaa", 3);
    assert(!fstat(fd_b, &st));
    assert(!title_buffer_write(handle_b, fd_b, "bbb", 3, 3, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(handled && pos == 6 && backing_calls == 1);
    assert(title_buffer_test_pending_count() == 1);
    assert(!title_buffer_flush(0, 0, 0));
    assert(backing_calls == 2);
    assert(pread(fd_a, got, sizeof(got), 0) == sizeof(got) && !memcmp(got, "aaabbb", 6));
    close(fd_a); close(fd_b); unlink(path);
    puts("write-buffer production: same identity through two handles flushes before admission PASS");
}


static void test_per_run_idle(void)
{
    char path_a[64], path_b[64];
    HANDLE handle_a = (HANDLE)(uintptr_t)0x121;
    HANDLE handle_b = (HANDLE)(uintptr_t)0x125;
    struct wine_ps5_title_lock lock;
    int index_a, index_b;
    int fd_a = make_file(path_a);
    int fd_b = make_file(path_b);

    reset_buffer();
    start_run(handle_a, fd_a, path_a, "aaa", 3);
    start_run(handle_b, fd_b, path_b, "bbb", 3);
    title_buffer_lock(&lock);
    index_a = title_buffer_find_handle_locked(handle_a);
    index_b = title_buffer_find_handle_locked(handle_b);
    assert(index_a >= 0 && index_b >= 0);
    title_buffer.runs[index_a].first_us = 100;
    title_buffer.runs[index_b].first_us = 200;
    title_buffer_unlock(&lock);
    assert(!title_buffer_flush_idle_due(100 + WINE_PS5_TITLE_BUFFER_RESIDENCE_US, 0));
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 1 && flush_log[0] == 'a');
    assert(!title_buffer_flush_idle_due(200 + WINE_PS5_TITLE_BUFFER_RESIDENCE_US, 0));
    assert(!title_buffer_test_pending_count() && backing_calls == 2 && flush_log[1] == 'b');
    close(fd_a); close(fd_b); unlink(path_a); unlink(path_b);
    puts("write-buffer production: per-run idle deadlines PASS");
}

static void test_total_run_cap(void)
{
    char paths[WINE_PS5_TITLE_BUFFER_RUNS + 1][64];
    int fds[WINE_PS5_TITLE_BUFFER_RUNS + 1];
    unsigned int i;

    reset_buffer();
    for (i = 0; i < WINE_PS5_TITLE_BUFFER_RUNS + 1; i++)
    {
        fds[i] = make_file(paths[i]);
        start_run((HANDLE)(uintptr_t)(0x140 + i * 4), fds[i], paths[i],
                  (const char[]){(char)('a' + i), 0}, 1);
    }
    assert(title_buffer_test_pending_count() == WINE_PS5_TITLE_BUFFER_RUNS);
    assert(backing_calls == 1 && flush_log[0] == 'a');
    assert(title_buffer_diag.extents_opened == WINE_PS5_TITLE_BUFFER_RUNS + 1);
    assert(title_buffer_diag.handle_evictions == 0);
    assert(title_buffer_diag.global_evictions == 1);
    assert(!title_buffer_flush(0, 0, 0));
    for (i = 0; i < WINE_PS5_TITLE_BUFFER_RUNS + 1; i++)
    {
        close(fds[i]);
        unlink(paths[i]);
    }
    puts("write-buffer production: total run/memory cap evicts oldest PASS");
}

static void test_multirun_error_and_shutdown(void)
{
    char path_a[64], path_b[64], got[3];
    HANDLE handle_a = (HANDLE)(uintptr_t)0x171;
    HANDLE handle_b = (HANDLE)(uintptr_t)0x175;
    int fd_a = make_file(path_a);
    int fd_b = make_file(path_b);

    reset_buffer();
    start_run(handle_a, fd_a, path_a, "aaa", 3);
    start_run(handle_b, fd_b, path_b, "bbb", 3);
    fail_once = 1;
    assert(title_buffer_flush(0, 0, 0) == STATUS_UNSUCCESSFUL);
    assert(title_buffer_test_pending_count() == 1);
    assert(pread(fd_b, got, sizeof(got), 0) == sizeof(got) && !memcmp(got, "bbb", 3));
    assert(!title_buffer_flush(0, 0, 0));
    assert(pread(fd_a, got, sizeof(got), 0) == sizeof(got) && !memcmp(got, "aaa", 3));

    reset_buffer();
    assert(!ftruncate(fd_a, 0) && !ftruncate(fd_b, 0));
    start_run(handle_a, fd_a, path_a, "AAA", 3);
    start_run(handle_b, fd_b, path_b, "BBB", 3);
    ps5_title_flush_all_coalesced_writes();
    assert(!title_buffer_test_pending_count());
    assert(pread(fd_a, got, sizeof(got), 0) == sizeof(got) && !memcmp(got, "AAA", 3));
    assert(pread(fd_b, got, sizeof(got), 0) == sizeof(got) && !memcmp(got, "BBB", 3));
    close(fd_a); close(fd_b); unlink(path_a); unlink(path_b);
    puts("write-buffer production: one-run error isolation and multi-run shutdown drain PASS");
}

static void test_hard_link_and_copy_rollback(void)
{
    char root[64], path[256], alias[272];
    struct stat st;
    HANDLE handle = (HANDLE)(uintptr_t)0x104;
    uint64_t dev, ino;
    char *resolved = NULL;
    int before, handled, i;
    off_t pos;
    int fd = make_scoped_file(root, path);
    reset_buffer();
    test_path = path;
    assert(wine_ps5_title_buffer_single_link(0));

    assert(fd >= 0);
    assert(title_buffer_resolve_start(handle, fd, 1, &resolved, &dev, &ino) == 1);
    free(resolved);
    resolved = NULL;
    snprintf(alias, sizeof(alias), "%s-link", path);
    assert(!link(path, alias));
    assert(title_buffer_resolve_start(handle, fd, 1, &resolved, &dev, &ino) == 0);
    assert(diagnostic_lines == 1 && strstr(diagnostic_line, "state=bypass reason=hardlink"));
    free(resolved);
    unlink(alias);
    assert(!fstat(fd, &st) && wine_ps5_title_buffer_single_link(st.st_nlink));
    {
        LONG64 stale_generation = title_buffer_mutation_generation;
        title_buffer_begin_mutation();
        title_buffer_end_mutation(NULL);
        assert(!title_buffer_write(handle, fd, "stale", 5, 0, path, st.st_dev, st.st_ino,
                                   stale_generation, 1, &handled, &pos));
        assert(!handled && !title_buffer_test_pending_count());
    }

    before = fd_count();
    for (i = 0; i < 32; i++)
    {
        copy_fails = 1;
        assert(title_buffer_write(handle, fd, "x", 1, 0, path, st.st_dev, st.st_ino,
                                  title_buffer_mutation_generation, 1, &handled, &pos) ==
               STATUS_INVALID_USER_BUFFER);
        assert(handled);
    }
    assert(fd_count() == before);
    close(fd);
    remove_scoped_file(root, path);
    puts("write-buffer production: nlink=2/stale-START rejection and copy rollback PASS");
}

static void test_partial_shutdown_retry(void)
{
    char path[64], bytes[6];
    HANDLE handle = (HANDLE)(uintptr_t)0x108;
    int fd = make_file(path);

    reset_buffer();
    test_path = path;
    start_run(handle, fd, path, "abcdef", 6);
    partial_once = writes_fail = 1;
    assert(title_buffer_flush(0, 0, 1) == STATUS_UNSUCCESSFUL); /* idle-latched */
    assert(title_buffer.runs[0].pending_status == STATUS_UNSUCCESSFUL &&
           title_buffer.runs[0].length == 4);
    writes_fail = 0;
    ps5_title_flush_all_coalesced_writes();
    assert(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!memcmp(bytes, "abcdef", sizeof(bytes)));

    reset_buffer();
    test_path = path;
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, "full", 4);
    write_errno = ENOSPC;
    assert(title_buffer_flush(handle, 1, 0) == STATUS_DISK_FULL);
    write_errno = 0;
    assert(!title_buffer_flush(0, 0, 2));

    reset_buffer();
    test_path = path;
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, "zero", 4);
    zero_once = 1;
    assert(title_buffer_flush(handle, 1, 0) == STATUS_DISK_FULL);
    assert(!title_buffer_flush(0, 0, 2));
    close(fd);
    unlink(path);
    puts("write-buffer production: partial idle EIO, ENOSPC/zero progress and shutdown retry PASS");
}

static void test_failed_close_orphan_identity(void)
{
    char path[64], moved[80], replacement[] = "NEW";
    char old_bytes[6], new_bytes[3];
    HANDLE handle = (HANDLE)(uintptr_t)0x10c;
    int fd = make_file(path), replacement_fd;
    unsigned int broker_before, pinned_before;

    reset_buffer();
    test_path = path;
    start_run(handle, fd, path, "abcdef", 6);
    writes_fail = 1;
    assert(ps5_title_close_coalesced_write(handle) == STATUS_UNSUCCESSFUL);
    assert(title_buffer_flush(0, 0, 1) == STATUS_UNSUCCESSFUL);
    snprintf(moved, sizeof(moved), "%s-moved", path);
    assert(!rename(path, moved));
    replacement_fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    assert(replacement_fd >= 0 && write(replacement_fd, replacement, sizeof(replacement)) == sizeof(replacement));
    broker_before = broker_calls;
    pinned_before = pinned_calls;
    writes_fail = 0;
    assert(!title_buffer_flush(0, 0, 0));
    assert(broker_calls == broker_before && pinned_calls == pinned_before + 1);
    assert(pread(fd, old_bytes, sizeof(old_bytes), 0) == sizeof(old_bytes));
    assert(!memcmp(old_bytes, "abcdef", sizeof(old_bytes)));
    assert(pread(replacement_fd, new_bytes, sizeof(new_bytes), 0) == sizeof(new_bytes));
    assert(!memcmp(new_bytes, replacement, sizeof(new_bytes)));
    close(replacement_fd);
    close(fd);
    unlink(path);
    unlink(moved);
    puts("write-buffer production: failed-close orphan retry and reused-path identity pin PASS");
}

static void test_mapping_and_shutdown_admission(void)
{
    char path[64], other[64];
    HANDLE handle = (HANDLE)(uintptr_t)0x110;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path);

    reset_buffer();
    test_path = path;
    start_run(handle, fd, path, "abc", 3);
    assert(!ps5_title_prepare_buffered_section(handle, SEC_IMAGE));
    assert(!title_buffer_disabled);

    strcpy(other, "/tmp/wine-ps5-outside-XXXXXX");
    int other_fd = mkstemp(other);
    assert(other_fd >= 0);
    identity_handle = handle;
    identity_fd = other_fd;
    test_path = other;
    assert(!ps5_title_prepare_buffered_section(handle, 0));
    assert(!title_buffer_disabled);

    test_path = "/tmp/Games/World of Warcraft/Data/data/data.000";
    assert(!ps5_title_prepare_buffered_section(handle, 0));
    assert(!title_buffer_disabled && title_buffer.excluded_count == 1);
    assert(!ps5_title_prepare_buffered_section((HANDLE)(uintptr_t)0xdead, 0));
    assert(title_buffer_disabled && !title_buffer_test_pending_count());
    assert(!fstat(fd, &st));
    title_buffer_shutdown = 1;
    assert(!title_buffer_write(handle, fd, "z", 1, 3, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled);
    close(other_fd);
    close(fd);
    unlink(other);
    unlink(path);
    puts("write-buffer production: SEC_IMAGE, remembered data mappings, unresolved fallback and late writer PASS");
}

static void test_rename_identity_and_exclusion_capacity(void)
{
    char root[64], scoped[256], path[64], other[64], bytes[3];
    char data_dir[256], staging_dir[256], staging_path[256];
    HANDLE owner = (HANDLE)(uintptr_t)0x420, unrelated = (HANDLE)(uintptr_t)0x424;
    struct stat st, replaced;
    int handled;
    off_t pos;
    int fd = make_file(path), other_fd = make_file(other);
    int target_fd = make_scoped_file(root, scoped);

    assert(target_fd >= 0 && !fstat(target_fd, &replaced));
    assert(pwrite(fd, "old", 3, 0) == 3);
    reset_buffer();
    identity_handle = owner;
    identity_fd = fd;
    start_run(unrelated, other_fd, other, "new", 3);
    assert(!ps5_title_note_buffered_rename(owner, scoped, 0));
    assert(!title_buffer_disabled && title_buffer.excluded_count == 1);
    assert(title_buffer_test_pending_count() == 1 && backing_calls == 0);
    assert(!title_buffer_identity_excluded_locked(replaced.st_dev, replaced.st_ino));
    assert(!rename(path, scoped));
    assert(!fstat(fd, &st) && title_buffer_identity_excluded_locked(st.st_dev, st.st_ino));
    assert(!title_buffer_write(owner, fd, "bad", 3, 0, scoped, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled && title_buffer_test_pending_count() == 1);
    assert(pwrite(fd, "map", 3, 0) == 3);
    assert(!title_buffer_flush(0, 0, 0));
    assert(pread(fd, bytes, 3, 0) == 3 && !memcmp(bytes, "map", 3));
    start_run(unrelated, other_fd, other, "yes", 3);
    assert(!title_buffer_disabled && title_buffer_test_pending_count() == 1);
    assert(!title_buffer_flush(0, 0, 0));
    close(target_fd);
    close(fd);
    assert(!unlink(scoped));

    strcpy(data_dir, scoped);
    *strrchr(data_dir, '/') = 0;
    assert(!rmdir(data_dir));
    snprintf(staging_dir, sizeof(staging_dir), "%s/staging", root);
    snprintf(staging_path, sizeof(staging_path), "%s/data.000", staging_dir);
    assert(!mkdir(staging_dir, 0700));
    fd = open(staging_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && pwrite(fd, "dir", 3, 0) == 3);
    reset_buffer();
    test_path = staging_path;
    identity_handle = owner;
    identity_fd = fd;
    assert(!ps5_title_prepare_buffered_section(owner, 0));
    assert(!title_buffer_disabled && title_buffer.excluded_count == 1);
    assert(!ps5_title_note_buffered_rename(owner, data_dir, 1));
    assert(!title_buffer_disabled && !rename(staging_dir, data_dir));
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(owner, fd, "bad", 3, 0, scoped, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled);
    start_run(unrelated, other_fd, other, "yes", 3);
    assert(!title_buffer_flush(0, 0, 0));
    assert(pread(fd, bytes, 3, 0) == 3 && !memcmp(bytes, "dir", 3));

    reset_buffer();
    for (unsigned int i = 0; i < TITLE_BUFFER_EXCLUDED_IDENTITIES; i++)
    {
        title_buffer.excluded[i].dev = st.st_dev + 1;
        title_buffer.excluded[i].ino = i + 1;
    }
    title_buffer.excluded_count = TITLE_BUFFER_EXCLUDED_IDENTITIES;
    identity_handle = owner;
    identity_fd = fd;
    start_run(unrelated, other_fd, other, "cap", 3);
    assert(!ps5_title_prepare_buffered_section(owner, 0));
    assert(title_buffer_disabled && !title_buffer_test_pending_count());
    assert(strstr(diagnostic_line, "reason=exclusion-capacity"));
    close(fd);
    close(other_fd);
    unlink(other);
    remove_scoped_file(root, scoped);
    puts("write-buffer production: file/directory rename identity coherence, unrelated buffering "
         "and table-full global fallback PASS");
}


static void *mapping_thread(void *arg)
{
    HANDLE handle = arg;
    return (void *)(uintptr_t)ps5_title_prepare_buffered_section(handle, 0);
}

static void *flush_thread(void *arg)
{
    HANDLE handle = arg;
    return (void *)(uintptr_t)title_buffer_flush(handle, 1, 0);
}

static void *idle_retry_thread(void *arg)
{
    (void)arg;
    return (void *)(uintptr_t)title_buffer_flush(0, 0, 1);
}

static void *close_owner_thread(void *arg)
{
    close_status = ps5_title_close_coalesced_write(arg);
    __atomic_store_n(&close_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_close_waits_for_idle_retry(void)
{
    char path[64];
    HANDLE handle = (HANDLE)(uintptr_t)0x11d;
    pthread_t close_thread, retry_thread;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path);

    reset_buffer();
    test_path = path;
    start_run(handle, fd, path, "old", 3);
    writes_fail = 1;
    assert(title_buffer_flush(0, 0, 1) == STATUS_UNSUCCESSFUL);
    close_hook_enabled = 1;
    assert(!pthread_create(&close_thread, NULL, close_owner_thread, handle));
    while (!__atomic_load_n(&close_barrier_done, __ATOMIC_ACQUIRE)) usleep(1000);
    __atomic_store_n(&writer_entered, 0, __ATOMIC_RELEASE);
    writer_block = 1;
    assert(!pthread_create(&retry_thread, NULL, idle_retry_thread, NULL));
    while (!__atomic_load_n(&writer_entered, __ATOMIC_ACQUIRE)) usleep(1000);
    __atomic_store_n(&allow_close_retire, 1, __ATOMIC_RELEASE);
    usleep(10000);
    assert(!close_done);
    __atomic_store_n(&writer_block, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(retry_thread, NULL));
    assert(!pthread_join(close_thread, NULL));
    assert(close_status == STATUS_UNSUCCESSFUL);
    writes_fail = 0;
    assert(!title_buffer_flush(0, 0, 1));
    for (unsigned int i = 0; i < WINE_PS5_TITLE_BUFFER_RUNS; i++)
        assert(title_buffer.runs[i].error_handle != handle);
    assert(!fstat(fd, &st));
    assert(!title_buffer_write(handle, fd, "new", 3, 3, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled);
    close(fd); unlink(path);
    puts("write-buffer production: close waits for failing idle retry and retires owner PASS");
}

struct admission_args
{
    HANDLE handle;
    int fd, handled;
    const char *path;
    struct stat st;
    LONG64 generation;
    NTSTATUS status;
    off_t pos;
};

static void *admission_thread(void *arg)
{
    struct admission_args *write = arg;
    write->status = title_buffer_write(write->handle, write->fd, "new", 3, 0,
                                       write->path, write->st.st_dev, write->st.st_ino,
                                       write->generation, 1, &write->handled, &write->pos);
    __atomic_store_n(&writer_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_combined_admission_wait(void)
{
    char path[64], bytes[3];
    HANDLE first = (HANDLE)(uintptr_t)0x115;
    pthread_t flush, writer;
    struct admission_args args = { .handle = (HANDLE)(uintptr_t)0x119 };
    int fd = make_file(path);

    reset_buffer();
    test_path = path;
    start_run(first, fd, path, "old", 3);
    title_buffer_begin_mutation();
    args.fd = fd;
    args.path = path;
    args.generation = title_buffer_mutation_generation;
    assert(!fstat(fd, &args.st));
    assert(!pthread_create(&writer, NULL, admission_thread, &args));
    while (!__atomic_load_n(&admission_waiting_mutation, __ATOMIC_ACQUIRE)) usleep(1000);
    writer_block = 1;
    assert(!pthread_create(&flush, NULL, flush_thread, first));
    while (!__atomic_load_n(&writer_entered, __ATOMIC_ACQUIRE)) usleep(1000);
    assert(!writer_done);
    title_buffer_end_mutation(NULL);
    usleep(10000);
    assert(!writer_done);
    fail_once = 1;
    __atomic_store_n(&writer_block, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(flush, NULL));
    assert(!pthread_join(writer, NULL));
    assert(!args.status && !args.handled && !title_buffer_test_pending_count());
    assert(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!memcmp(bytes, "old", sizeof(bytes)));
    close(fd);
    unlink(path);
    puts("write-buffer production: combined mutation/flushing admission wait PASS");
}


static void quit_thread(int signal)
{
    (void)signal;
    ps5_title_stop_coalesced_writes();
    pthread_exit(NULL);
}

static void test_mapping_race_and_sigquit_cleanup(void)
{
    char path[64], bytes[3];
    HANDLE handle = (HANDLE)(uintptr_t)0x114;
    pthread_t thread;
    struct sigaction action = { .sa_handler = quit_thread };
    int fd = make_file(path);

    reset_buffer();
    test_path = path;
    start_run(handle, fd, path, "old", 3);
    test_path = "/tmp/Games/World of Warcraft/Data/data/data.001";
    writer_block = 1;
    assert(!pthread_create(&thread, NULL, mapping_thread, handle));
    while (!__atomic_load_n(&writer_entered, __ATOMIC_ACQUIRE)) usleep(1000);
    assert(!title_buffer_disabled && title_buffer.excluded_count == 1);
    __atomic_store_n(&writer_block, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL));
    assert(pwrite(fd, "new", 3, 0) == 3);
    assert(!title_buffer_flush(0, 0, 0));
    assert(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!memcmp(bytes, "new", sizeof(bytes)));

    reset_buffer();
    test_path = path;
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, "sig", 3);
    writes_fail = writer_block = 1;
    assert(!sigemptyset(&action.sa_mask));
    assert(!sigaction(SIGQUIT, &action, NULL));
    assert(!pthread_create(&thread, NULL, flush_thread, handle));
    while (!__atomic_load_n(&writer_entered, __ATOMIC_ACQUIRE)) usleep(1000);
    assert(!pthread_kill(thread, SIGQUIT));
    usleep(10000);
    assert(title_buffer_any_flushing_locked()); /* SIGQUIT remains deferred while broker slot held */
    __atomic_store_n(&writer_block, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL));
    assert(!title_buffer_any_flushing_locked() && title_buffer_test_pending_count() == 1 &&
           title_buffer_shutdown);
    writes_fail = 0;
    pthread_mutex_lock(&broker_slot); /* shutdown retry must not reacquire this slot */
    assert(!title_buffer_flush(0, 0, 2));
    pthread_mutex_unlock(&broker_slot);
    assert(pinned_calls);
    assert(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!memcmp(bytes, "sig", sizeof(bytes)));
    close(fd);
    unlink(path);
    puts("write-buffer production: mapping race, deferred SIGQUIT and pinned shutdown retry PASS");
}

static void test_ordering_rename_and_kill_switch(void)
{
    char path[64], bytes[3];
    HANDLE handle = (HANDLE)(uintptr_t)0x118;
    struct stat st;
    int handled;
    off_t pos;
    int fd = make_file(path);

    reset_buffer();
    test_path = path;
    start_run(handle, fd, path, "dat", 3);
    assert(!title_buffer_write((HANDLE)(uintptr_t)0x200, fd, "idx", 3, 0, NULL, 0, 0,
                               title_buffer_mutation_generation, 0, &handled, &pos));
    assert(!handled && pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(!memcmp(bytes, "dat", sizeof(bytes)));

    identity_handle = handle;
    identity_fd = fd;
    assert(!ps5_title_note_buffered_rename(
        handle, "/tmp/Games/World of Warcraft/Data/data/data.002", 0));
    assert(!title_buffer_disabled && title_buffer.excluded_count == 1);
    reset_buffer();
    assert(!ps5_title_note_buffered_rename(handle, "/tmp/Games/Staging", 1));
    assert(!title_buffer_disabled);
    reset_buffer();
    test_path = path;
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, "lnk", 3);
    assert(!title_buffer_disable_for_link());
    assert(title_buffer_disabled && !title_buffer_test_pending_count());
    assert(diagnostic_lines == 2 && strstr(diagnostic_line, "state=disabled reason=link-create"));

    reset_buffer();
    assert(!fstat(fd, &st));
    assert(!setenv("WINEPS5_WRITE_BUFFER", "0", 1));
    assert(!title_buffer_write(handle, fd, "off", 3, 0, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled && !title_buffer_test_pending_count());
    assert(diagnostic_lines == 1 && strstr(diagnostic_line, "state=disabled reason=env-off"));
    close(fd);
    unlink(path);
    puts("write-buffer production: data-before-idx, rename exclusion, link disable and kill-switch direct path PASS");
}

int main(void)
{
    sigemptyset(&ps5_title_block_set);
    sigaddset(&ps5_title_block_set, SIGQUIT);
    test_mapping_identity_isolation();
    test_overlapped_write_dispatch();
    test_sequential_and_barrier();
    test_scoped_visibility();
    test_measurement_window();
    test_extent_diagnostics();
    test_measurement_off_on_parity();
    test_hard_link_and_copy_rollback();
    test_interleaved_two_files();
    test_multiextent_and_overlap();
    test_descending_identity_flush();
    test_nonfile_write_dispatch_fence();
    test_scoped_handle_barriers();
    test_unrelated_close_drain_attribution();
    test_same_identity_two_handles();
    test_per_run_idle();
    test_total_run_cap();
    test_multirun_error_and_shutdown();
    test_partial_shutdown_retry();
    test_failed_close_orphan_identity();
    test_close_waits_for_idle_retry();
    test_mapping_and_shutdown_admission();
    test_rename_identity_and_exclusion_capacity();
    test_mapping_race_and_sigquit_cleanup();
    test_combined_admission_wait();
    test_ordering_rename_and_kill_switch();
    return 0;
}
