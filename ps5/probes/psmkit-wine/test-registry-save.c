#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

typedef uint32_t NTSTATUS;
typedef unsigned int data_size_t;
typedef long long abstime_t;
typedef wchar_t WCHAR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define KEY_VOLATILE 0x0001
#define KEY_DIRTY 0x0004
#include "ntstatus.h"
#include "wine/ps5_prefix_init.h"

struct key {
    unsigned int flags;
    abstime_t timestamp_counter;
    int last_subkey;
    struct key **subkeys;
};
static struct key keys[3];
static struct { struct key *key; const char *filename; } save_branch_info[3];
static int save_branch_count, debug_level, config_dir_fd = -1, server_dir_fd = -1;
static const int inprocess_server = 1;
static const char *inprocess_config_dir;
static char prefix[256], filenames[3][512], nonexistent[512], proof_name[512];
static const char old_hive[] = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\Machine\n\n#arch=win64\n[Software] 1\n";
static const char saved_hive[] = "WINE REGISTRY Version 2\n;; serialized fixture bytes supplied by server object adapter\n\n#arch=win64\n[Software] 2\n";
static unsigned int failures, cases;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

enum fault { NONE, OPEN, FDOPEN, WRITE, FLUSH, SYNC, CLOSE, RENAME };
static enum fault fault;
static int fault_branch, active_fault, fdopen_calls, fsync_calls, registration_ready = 1;
static NTSTATUS request_error;
static struct { unsigned int hkey; } request = {42}, *req = &request;
static struct { data_size_t total; int branch_count; abstime_t timestamp_counter; } response, *reply = &response;
static abstime_t change_timestamp_counter;
static int protocol_snapshot_calls, release_calls;

static void set_error(NTSTATUS error) { request_error = error; }
static struct key *get_hkey_obj(unsigned int handle, unsigned int access)
{
    (void)access;
    if (handle == 42) return &keys[0];
    set_error(STATUS_INVALID_HANDLE);
    return NULL;
}
static void release_object(void *object) { (void)object; ++release_calls; }
static void inprocess_trace(const char *format, ...) { (void)format; errno = EDOM; }
static void dump_operation(struct key *key, void *value, const char *operation)
{ (void)key; (void)value; (void)operation; }
static void fatal_error(const char *format, ...) { (void)format; abort(); }
static void find_branches_for_key(struct key *key, int *branches, int *count)
{ (void)key; (void)branches; (void)count; ++protocol_snapshot_calls; abort(); }
static data_size_t save_registry(const struct key *key, char *buffer)
{ (void)key; (void)buffer; ++protocol_snapshot_calls; abort(); }
static size_t get_reply_max_size(void) { ++protocol_snapshot_calls; abort(); }
static char *set_reply_data_size(size_t size) { (void)size; ++protocol_snapshot_calls; abort(); }

/* All injected errors below come from real libc/kernel operations.  Closing
 * or downgrading a descriptor models descriptor/I/O failure; an invalid
 * destination component models a failed atomic rename. */
static void readonly_stream(FILE *stream)
{
    char path[64];
    int fd;
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fileno(stream));
    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    assert(dup2(fd, fileno(stream)) == fileno(stream));
    assert(!close(fd));
}
static FILE *fault_fdopen(int fd, const char *mode)
{
    active_fault = fdopen_calls++ == fault_branch;
    if (active_fault && fault == FDOPEN) assert(!close(fd));
    return fdopen(fd, mode);
}
static void save_all_subkeys(struct key *key, FILE *stream)
{
    (void)key;
    if (active_fault && fault == WRITE)
    {
        assert(!setvbuf(stream, NULL, _IONBF, 0));
        readonly_stream(stream);
    }
    fputs(saved_hive, stream);
}
static int fault_fflush(FILE *stream)
{
    if (active_fault && fault == FLUSH) readonly_stream(stream);
    return fflush(stream);
}
static int fault_fsync(int fd)
{
    ++fsync_calls;
    if (active_fault && fault == SYNC) assert(!close(fd));
    return fsync(fd);
}
static int fault_fclose(FILE *stream)
{
    if (active_fault && fault == CLOSE) assert(!close(fileno(stream)));
    return fclose(stream);
}
static int fault_rename(const char *from, const char *to)
{
    char invalid[1024];
    if (active_fault && fault == RENAME)
    {
        snprintf(invalid, sizeof(invalid), "%s/not-a-directory", to);
        return rename(from, invalid);
    }
    return rename(from, to);
}
static int fault_unlink(const char *path)
{
    int result = unlink(path);
    errno = ENOTEMPTY; /* Successful cleanup need not preserve the I/O errno. */
    return result;
}
#define fdopen fault_fdopen
#define fflush fault_fflush
#define fsync fault_fsync
#define fclose fault_fclose
#define rename fault_rename
#define unlink fault_unlink
#include "registry-save-functions.inc"
#undef fdopen
#undef fflush
#undef fsync
#undef fclose
#undef rename
#undef unlink

#ifndef O_BINARY
#define O_BINARY 0x40000000
#endif
static int proof_open_flags;
/* Win32 metadata and path adapters only. The flush callback executes the
 * real title flush request, save implementation and errno/status mapper. */
static BOOL wineboot_crypto_registration_ready(void) { return registration_ready; }
static BOOL wineboot_flush_crypto_registry(void)
{
    memset(&response, 0xcc, sizeof(response));
    request_error = STATUS_SUCCESS;
    test_flush_key();
    CHECK(reply->total == 0 && reply->branch_count == 0);
    CHECK(reply->timestamp_counter == change_timestamp_counter);
    CHECK(protocol_snapshot_calls == 0);
    return request_error == STATUS_SUCCESS;
}
static void ascii_path(const WCHAR *wide, char *path, size_t size)
{
    size_t n = wcstombs(path, wide, size);
    assert(n < size);
    for (size_t i = 0; i < n; ++i) if (path[i] == '\\') path[i] = '/';
}
static int host_wopen(const WCHAR *wide, int flags, mode_t mode)
{
    char path[1024];
    proof_open_flags = flags;
    ascii_path(wide, path, sizeof(path));
    return open(path, flags, mode);
}
static int host_wunlink(const WCHAR *wide)
{
    char path[1024];
    ascii_path(wide, path, sizeof(path));
    return unlink(path);
}
#define _wopen host_wopen
#define _wunlink host_wunlink
#define lstrcpyW wcscpy
#define lstrcatW wcscat
#include "registry-save-proof.inc"
#undef _wopen
#undef _wunlink
#undef lstrcpyW
#undef lstrcatW

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(!fclose(f));
}
static int text_is(const char *path, const char *text)
{
    char buffer[512];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(buffer, 1, sizeof(buffer), f);
    assert(!ferror(f));
    assert(!fclose(f));
    return n == strlen(text) && !memcmp(buffer, text, n);
}
static void prepare(enum fault selected, int branches, int branch)
{
    fault = selected;
    fault_branch = branch;
    active_fault = fdopen_calls = fsync_calls = protocol_snapshot_calls = release_calls = 0;
    proof_open_flags = 0;
    request_error = STATUS_SUCCESS;
    registration_ready = TRUE;
    save_branch_count = branches;
    inprocess_config_dir = fault == OPEN ? nonexistent : prefix;
    for (int i = 0; i < 3; ++i)
    {
        keys[i] = (struct key){KEY_DIRTY, 1, -1, NULL};
        save_branch_info[i].key = &keys[i];
        save_branch_info[i].filename = filenames[i];
        put(filenames[i], old_hive);
    }
    if (unlink(proof_name)) assert(errno == ENOENT);
}
static NTSTATUS expected_status(enum fault selected)
{
    if (selected == OPEN) return STATUS_NO_SUCH_FILE;
    if (selected == RENAME) return STATUS_NOT_A_DIRECTORY;
    return STATUS_INVALID_HANDLE;
}
static void check_fault(enum fault selected, int branches, int branch)
{
    WCHAR directory[256];
    prepare(selected, branches, branch);
    assert(mbstowcs(directory, prefix, sizeof(directory) / sizeof(directory[0])) <
           sizeof(directory) / sizeof(directory[0]));
    CHECK(!wineboot_finish_title_prefix(directory, TRUE));
    CHECK(request_error == expected_status(selected));
    CHECK(access(proof_name, F_OK) == -1 && errno == ENOENT);
    CHECK(text_is(filenames[branch], old_hive));
    CHECK(keys[branch].flags & KEY_DIRTY);
    for (int i = 0; i < branches; ++i)
        if (i != branch)
        {
            CHECK(text_is(filenames[i], saved_hive));
            CHECK(!(keys[i].flags & KEY_DIRTY));
        }
    CHECK(release_calls == 1);
    CHECK(protocol_snapshot_calls == 0);
    ++cases;
}
int main(void)
{
    WCHAR directory[256];
    strcpy(prefix, "/tmp/forever-registry-save-XXXXXX");
    assert(mkdtemp(prefix));
    snprintf(nonexistent, sizeof(nonexistent), "%s/absent-parent", prefix);
    snprintf(proof_name, sizeof(proof_name), "%s/.wineboot-init-done", prefix);
    for (int i = 0; i < 3; ++i)
        snprintf(filenames[i], sizeof(filenames[i]), "%s/hive-%d.reg", prefix, i);
    for (enum fault selected = OPEN; selected <= RENAME; ++selected)
        check_fault(selected, 1, 0);
    check_fault(SYNC, 3, 0); /* Later successful branches cannot hide first errno. */
    check_fault(RENAME, 3, 1);
    prepare(NONE, 3, -1);
    assert(mbstowcs(directory, prefix, sizeof(directory) / sizeof(directory[0])) <
           sizeof(directory) / sizeof(directory[0]));
    CHECK(wineboot_finish_title_prefix(directory, TRUE));
    CHECK(request_error == STATUS_SUCCESS);
    CHECK(text_is(proof_name, WINE_PS5_PREFIX_INIT_PROOF));
    CHECK(proof_open_flags & O_BINARY);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(text_is(filenames[i], saved_hive));
        CHECK(!(keys[i].flags & KEY_DIRTY));
    }
    CHECK(fsync_calls == 3);
    ++cases;
    prepare(NONE, 1, -1);
    CHECK(!wineboot_finish_title_prefix(directory, FALSE));
    CHECK(fdopen_calls == 0 && access(proof_name, F_OK) == -1);
    registration_ready = FALSE;
    CHECK(!wineboot_finish_title_prefix(directory, TRUE));
    CHECK(fdopen_calls == 0 && access(proof_name, F_OK) == -1);
    ++cases;
    /* Invalid handles must fail before any save/proof operation. */
    request.hkey = 0;
    request_error = STATUS_SUCCESS;
    test_flush_key();
    CHECK(request_error == STATUS_INVALID_HANDLE && fdopen_calls == 0);
    ++cases;
    for (int i = 0; i < 3; ++i) assert(!unlink(filenames[i]));
    if (unlink(proof_name)) assert(errno == ENOENT);
    assert(!rmdir(prefix));
    printf("REGISTRY_SAVE_FAULTS cases=%u failures=%u old_hive_retained=checked proof_withheld=checked status=production\n", cases, failures);
    return failures ? 1 : 0;
}
