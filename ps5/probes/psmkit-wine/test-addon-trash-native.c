#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_unlink(const char *);
static int test_rmdir(const char *);
static inline int test_fsync(int);
static int test_usleep(useconds_t);
static int test_open(const char *, int);
int forbidden_lstat(const char *, struct stat *);
#define unlink test_unlink
#define rmdir test_rmdir
#define fsync test_fsync
#define usleep test_usleep
#define open test_open
#define lstat forbidden_lstat
#include "addon-trash-native.h"
#undef unlink
#undef rmdir
#undef fsync
#undef usleep
#undef open
#undef lstat

static atomic_int *interrupt_flag;
static char watched_payload[PATH_MAX], watched_marker[PATH_MAX], watched_root[PATH_MAX];
static int interrupt_pending, fail_payload, fail_sync, fail_root;
static int payload_sync, marker_removed, interrupt_on_sync;
static int forbidden_lstat_calls, emlink_on_symlink;

static int test_open(const char *path, int flags)
{
    assert((flags & O_ACCMODE) == O_RDONLY);
    assert((flags & (O_NOFOLLOW | O_NONBLOCK)) == (O_NOFOLLOW | O_NONBLOCK));
    int fd = open(path, flags);
    if (fd < 0 && errno == ELOOP && emlink_on_symlink) errno = EMLINK;
    return fd;
}
int forbidden_lstat(const char *path, struct stat *status)
{
    (void)path;
    (void)status;
    forbidden_lstat_calls++;
    errno = EPERM;
    return -1;
}

static int test_unlink(const char *path)
{
    if (!strcmp(path, watched_payload) && fail_payload) { errno = EIO; return -1; }
    int error = unlink(path);
    if (!error && !strcmp(path, watched_payload)) interrupt_pending = 1;
    if (!error && !strcmp(path, watched_marker))
    {
        assert(payload_sync); /* commit proof cannot precede durable payload absence */
        marker_removed = 1;
    }
    return error;
}
static int test_rmdir(const char *path)
{
    if (fail_root && !strcmp(path, watched_root)) { errno = EIO; return -1; }
    return rmdir(path);
}
static inline int test_fsync(int fd)
{
    if (fail_sync) { errno = EIO; return -1; }
    int error = fsync(fd);
    if (!error && !marker_removed && watched_root[0])
    {
        struct stat root, synced;
        assert(!lstat(watched_root, &root) && !fstat(fd, &synced));
        assert(root.st_dev == synced.st_dev && root.st_ino == synced.st_ino);
        DIR *directory = opendir(watched_root);
        struct dirent *entry;
        assert(directory);
        while ((entry = readdir(directory)))
            assert(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
                   !strcmp(entry->d_name, ".complete"));
        assert(!closedir(directory));
        payload_sync = 1;
        if (interrupt_on_sync && interrupt_flag) atomic_store(interrupt_flag, 1);
    }
    return error;
}
static int test_usleep(useconds_t delay)
{
    (void)delay;
    if (interrupt_pending && interrupt_flag)
    {
        atomic_store(interrupt_flag, 1);
        interrupt_pending = 0;
    }
    return 0;
}

static void mkdirs(const char *path)
{
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *at = copy + 1; *at; at++) if (*at == '/') { *at = 0; mkdir(copy, 0755); *at = '/'; }
    mkdir(copy, 0755);
}
static void file(const char *path)
{
    char parent[PATH_MAX], *slash;
    snprintf(parent, sizeof(parent), "%s", path); slash = strrchr(parent, '/'); assert(slash); *slash = 0;
    mkdirs(parent); int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644); assert(fd >= 0); close(fd);
}
static int exists(const char *path) { struct stat st; return !lstat(path, &st); }
static void old_marker(const char *path)
{
    struct timespec times[2] = {{.tv_sec = time(NULL) - 10}, {.tv_sec = time(NULL) - 10}};
    file(path); assert(!utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW));
}
static void committed_cleanup(void)
{
    char prefix[] = "/tmp/addon-trash-commit-XXXXXX", addons[PATH_MAX], path[PATH_MAX];
    atomic_int stop = 0, pause = 0;
    struct addon_trash_result result;
    assert(mkdtemp(prefix));
    assert(!addon_trash_path(addons, sizeof(addons), prefix,
        "drive_c/Program Files (x86)/World of Warcraft/_retail_/Interface/AddOns"));
    assert(!addon_trash_path(watched_root, sizeof(watched_root), addons, ".addonforge-tmp/trash-commit"));
    assert(!addon_trash_path(watched_marker, sizeof(watched_marker), watched_root, ".complete"));
    assert(!addon_trash_path(watched_payload, sizeof(watched_payload), watched_root, "Removed/first"));

    for (int stopping = 0; stopping < 2; stopping++)
    {
        payload_sync = marker_removed = interrupt_pending = 0;
        old_marker(watched_marker);
        file(watched_payload);
        assert(!addon_trash_path(path, sizeof(path), watched_root, "Removed/nested/.complete"));
        file(path); /* only the trash root's .complete is commit proof */
        interrupt_flag = stopping ? &stop : &pause;
        assert(!addon_trash_scan(prefix, &stop, &pause, &result));
        assert(atomic_load(interrupt_flag));
        assert(!exists(watched_payload) && exists(watched_marker));
        assert(!addon_trash_path(path, sizeof(path), watched_root, "Removed"));
        assert(exists(path)); /* interruption leaves a genuinely partial payload tree */
        assert(!addon_trash_path(path, sizeof(path), addons, "Removed"));
        assert(!exists(path)); /* core recovery skips this still-marked committed trash */
        atomic_store(interrupt_flag, 0);
        interrupt_flag = NULL;
        assert(!addon_trash_scan(prefix, &stop, &pause, &result));
        assert(result.completed == 1 && marker_removed && !exists(watched_root));
    }

    /* An actual no-follow failure must leave the root proof, whatever readdir order. */
    payload_sync = marker_removed = interrupt_pending = 0;
    old_marker(watched_marker);
    assert(!addon_trash_path(path, sizeof(path), watched_root, "link"));
    assert(!symlink("/does-not-exist", path));
    assert(addon_trash_scan(prefix, &stop, &pause, &result) < 0);
    assert(exists(watched_marker) && !marker_removed && exists(path));
    assert(!unlink(path));

    file(watched_payload);
    fail_payload = 1;
    assert(addon_trash_scan(prefix, &stop, &pause, &result) == -EIO);
    assert(exists(watched_marker) && exists(watched_payload) && !marker_removed);
    fail_payload = 0;
    fail_sync = 1;
    assert(addon_trash_scan(prefix, &stop, &pause, &result) == -EIO);
    assert(exists(watched_marker) && !exists(watched_payload) && !marker_removed);
    fail_sync = 0;

    for (int stopping = 0; stopping < 2; stopping++)
    {
        interrupt_flag = stopping ? &stop : &pause;
        interrupt_on_sync = 1;
        assert(!addon_trash_scan(prefix, &stop, &pause, &result));
        assert(atomic_load(interrupt_flag) && exists(watched_marker) && !marker_removed);
        atomic_store(interrupt_flag, 0);
        interrupt_flag = NULL;
        interrupt_on_sync = 0;
    }

    /* Crash/failure after the last marker unlink leaves only a terminal empty root. */
    fail_root = 1;
    assert(addon_trash_scan(prefix, &stop, &pause, &result) == -EIO);
    assert(marker_removed && !exists(watched_marker) && exists(watched_root));
    fail_root = 0;
    assert(!addon_trash_scan(prefix, &stop, &pause, &result));
    assert(result.completed == 1 && !exists(watched_root));

    /* No commit proof grants no authority over a nonempty interrupted removal. */
    file(watched_payload);
    assert(!addon_trash_scan(prefix, &stop, &pause, &result));
    assert(!result.entries && exists(watched_payload));
    watched_payload[0] = watched_marker[0] = watched_root[0] = 0;
    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "rm -rf '%s'", prefix);
    assert(!system(command));
}

int main(void)
{
    committed_cleanup();
    char root[] = "/tmp/addon-trash-native-XXXXXX", path[PATH_MAX], outside[PATH_MAX];
    atomic_int stop = 0, pause = 1; struct addon_trash_result result;
    assert(mkdtemp(root));
    char addons[512];
    assert(!addon_trash_path(addons, sizeof(addons), root,
        "drive_c/Program Files (x86)/World of Warcraft/_retail_/Interface/AddOns"));
    mkdirs(addons);
    snprintf(path, sizeof(path), "%s/Keep/Keep.toc", addons); file(path);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/not-trash/file", addons); file(path);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-incomplete/file", addons); file(path);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-fresh/.complete", addons); file(path);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-old/.complete", addons); old_marker(path);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-old/nested/a/b/file", addons); file(path);
    snprintf(outside, sizeof(outside), "%s/outside", root); file(outside);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-link/.complete", addons); old_marker(path);
    char linkpath[PATH_MAX]; snprintf(linkpath, sizeof(linkpath), "%s/.addonforge-tmp/trash-link/link", addons);
    assert(!symlink(outside, linkpath));
    char rootlink[PATH_MAX], markerlink[PATH_MAX], marker_target[PATH_MAX];
    snprintf(rootlink, sizeof(rootlink), "%s/.addonforge-tmp/trash-root-link", addons);
    assert(!symlink(root, rootlink));
    snprintf(marker_target, sizeof(marker_target), "%s/outside-marker", root);
    old_marker(marker_target);
    snprintf(markerlink, sizeof(markerlink), "%s/.addonforge-tmp/trash-marker-link/.complete", addons);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-marker-link/keep", addons); file(path);
    assert(!symlink(marker_target, markerlink));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-marker-fifo/keep", addons); file(path);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-marker-fifo/.complete", addons);
    assert(!mkfifo(path, 0644)); /* no-follow NONBLOCK inspection must not hang or accept this marker */

    assert(addon_trash_scan(root, &stop, &pause, &result) == 0 && !result.entries);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-old", addons); assert(exists(path));
    atomic_store(&pause, 0);
    emlink_on_symlink = 1; /* exercise the title's alternate no-follow link error too */
    assert(addon_trash_scan(root, &stop, &pause, &result) < 0); /* skipped link keeps its directory non-empty */
    assert(result.entries == 6 && result.completed == 1);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-old", addons); assert(!exists(path));
    snprintf(path, sizeof(path), "%s/Keep/Keep.toc", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/not-trash/file", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-incomplete/file", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-fresh/.complete", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-link/.complete", addons); assert(exists(path));
    assert(exists(outside));
    assert(exists(rootlink) && exists(markerlink) && exists(marker_target));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-marker-link/keep", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-marker-fifo/keep", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-marker-fifo/.complete", addons); assert(exists(path));

    atomic_store(&stop, 1);
    assert(addon_trash_scan(root, &stop, &pause, &result) == 0 && result.entries == 0);
    assert(!forbidden_lstat_calls);
    char command[PATH_MAX + 16]; snprintf(command, sizeof(command), "rm -rf '%s'", root); assert(!system(command));
    puts("native addon trash: no lstat, no-follow ELOOP/EMLINK, commit-last stop/pause/failure, durable payload removal, empty-root recovery, roots/age/type gates PASS");
    return 0;
}
