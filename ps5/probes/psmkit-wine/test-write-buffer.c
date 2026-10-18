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
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001u)
#define STATUS_INVALID_USER_BUFFER ((NTSTATUS)0xc00000e8u)
#define STATUS_DISK_FULL ((NTSTATUS)0xc000007fu)
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
static int copy_fails, writes_fail, fail_once, zero_once, write_errno;
static int partial_once, writer_block, writer_entered, writer_done, admission_waiting_mutation;
static unsigned int backing_calls;
static pthread_mutex_t broker_slot = PTHREAD_MUTEX_INITIALIZER;
static unsigned int broker_calls, pinned_calls;
static char diagnostic_line[192];
static unsigned int diagnostic_lines;

static LONG64 title_io_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (LONG64)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static const char *ps5_title_drive(char drive)
{
    (void)drive;
    return "/tmp";
}

static int server_get_unix_name(HANDLE handle, char **path)
{
    (void)handle;
    if (!test_path) return -1;
    *path = strdup(test_path);
    return *path ? 0 : -1;
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
    if (strstr(text, "writebuf"))
    {
        snprintf(diagnostic_line, sizeof(diagnostic_line), "%s", text);
        diagnostic_lines++;
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


#include "wine-write-buffer.inc"

static void reset_buffer(void)
{
    struct wine_ps5_title_lock lock;

    title_buffer_lock(&lock);
    while (title_buffer.flushing) { title_buffer_unlock(&lock); usleep(1000); title_buffer_lock(&lock); }
    if (title_buffer.fd != -1) close(title_buffer.fd);
    title_buffer.handle = title_buffer.error_handle = 0;
    title_buffer.fd = -1;
    title_buffer.flushing = title_buffer.active = 0;
    title_buffer.start = title_buffer.end = 0;
    title_buffer.length = 0;
    title_buffer.first_us = title_buffer.dev = title_buffer.ino = 0;
    title_buffer.pending_status = STATUS_SUCCESS;
    title_buffer.path[0] = 0;
    title_buffer.flushing_thread_valid = 0;
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
    backing_calls = 0;
    unsetenv("WINEPS5_WRITE_BUFFER");
    broker_calls = pinned_calls = 0;
    diagnostic_line[0] = 0;
    diagnostic_lines = 0;
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
    close(fd);
    unlink(path);
    puts("write-buffer production: sequential explicit/file-pointer model and global barrier PASS");
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
        assert(!handled && !title_buffer.length);
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
    assert(title_buffer.pending_status == STATUS_UNSUCCESSFUL && title_buffer.length == 4);
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
    test_path = other;
    assert(!ps5_title_prepare_buffered_section(handle, 0));
    assert(!title_buffer_disabled);

    test_path = "/tmp/Games/World of Warcraft/Data/data/data.000";
    assert(!ps5_title_prepare_buffered_section(handle, 0));
    assert(title_buffer_disabled);
    assert(!fstat(fd, &st));
    title_buffer_shutdown = 1;
    assert(!title_buffer_write(handle, fd, "z", 1, 3, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled);
    close(other_fd);
    close(fd);
    unlink(other);
    unlink(path);
    puts("write-buffer production: SEC_IMAGE/out-of-scope mapping, scoped disable-before-drain and late writer PASS");
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
    assert(!args.status && !args.handled && !title_buffer.length);
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
    assert(title_buffer_disabled);
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
    assert(title_buffer.flushing); /* SIGQUIT remains deferred while the broker slot is held */
    __atomic_store_n(&writer_block, 0, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL));
    assert(!title_buffer.flushing && title_buffer.length == 3 && title_buffer_shutdown);
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

    assert(!ps5_title_note_buffered_rename(
        "/tmp/Games/World of Warcraft/Data/data/data.002", 0));
    assert(title_buffer_disabled);
    reset_buffer();
    assert(!ps5_title_note_buffered_rename("/tmp/Games/Staging", 1));
    assert(title_buffer_disabled);
    reset_buffer();
    test_path = path;
    assert(!ftruncate(fd, 0));
    start_run(handle, fd, path, "lnk", 3);
    assert(!title_buffer_disable_for_link());
    assert(title_buffer_disabled && !title_buffer.length);
    assert(diagnostic_lines == 2 && strstr(diagnostic_line, "state=disabled reason=link-create"));

    reset_buffer();
    assert(!fstat(fd, &st));
    assert(!setenv("WINEPS5_WRITE_BUFFER", "0", 1));
    assert(!title_buffer_write(handle, fd, "off", 3, 0, path, st.st_dev, st.st_ino,
                               title_buffer_mutation_generation, 1, &handled, &pos));
    assert(!handled && !title_buffer.length);
    assert(diagnostic_lines == 1 && strstr(diagnostic_line, "state=disabled reason=env-off"));
    close(fd);
    unlink(path);
    puts("write-buffer production: data-before-idx, rename/link disable and kill-switch direct path PASS");
}

int main(void)
{
    sigemptyset(&ps5_title_block_set);
    sigaddset(&ps5_title_block_set, SIGQUIT);
    test_sequential_and_barrier();
    test_hard_link_and_copy_rollback();
    test_partial_shutdown_retry();
    test_failed_close_orphan_identity();
    test_mapping_and_shutdown_admission();
    test_mapping_race_and_sigquit_cleanup();
    test_combined_admission_wait();
    test_ordering_rename_and_kill_switch();
    return 0;
}
