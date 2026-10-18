/* Host tests for forever-setup.h against a temporary directory. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *denied_open;
static const char *watched_open;
static unsigned watched_reads, watched_opens;
static int watched_fd = -1, watched_read_started, nofollow_symlink_error;
static int managed_check_device = 1;
static dev_t simulated_device;
static const char *source_stat_root;
static unsigned source_stat_calls;
static int source_stat_fd = -1;
/* Model the PS5 foreign-owner sandbox boundary without requiring host root. */
static const char *foreign_destination;
static const char *rename_symlink_target;
static dev_t foreign_device;
static ino_t foreign_inode;
static int foreign_nlink_zero, denied_rename_error, denied_fsync;
static unsigned rename_denials, inplace_syncs;

static int setup_open(const char *name, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (denied_open && !strcmp(name, denied_open))
    {
        errno = EACCES;
        return -1;
    }
    if (nofollow_symlink_error && (flags & O_NOFOLLOW))
    {
        char link;
        if (readlink(name, &link, 1) >= 0) { errno = nofollow_symlink_error; return -1; }
    }
    int fd = open(name, flags, mode);
    if (watched_open && !strcmp(name, watched_open) && !(flags & (O_WRONLY | O_RDWR)))
    {
        ++watched_opens;
        if (fd >= 0) { watched_fd = fd; watched_read_started = 0; }
    }
    if (fd >= 0 && source_stat_root && !strncmp(name, source_stat_root, strlen(source_stat_root)))
        source_stat_fd = fd;
    return fd;
}

static inline __attribute__((unused)) int setup_lstat(const char *name, struct stat *status)
{
    (void)name;
    (void)status;
    assert(!"lstat is forbidden in the PS5 title sync path");
    errno = EPERM;
    return -1;
}

static int setup_stat(const char *name, struct stat *status)
{
    if (source_stat_root && !strncmp(name, source_stat_root, strlen(source_stat_root)))
        ++source_stat_calls;
    int result = stat(name, status);
    if (!result && foreign_destination && !strcmp(name, foreign_destination))
    {
        status->st_uid = getuid() + 1;
        if (foreign_nlink_zero) status->st_nlink = 0;
    }
    if (!result && simulated_device && watched_open && !strcmp(name, watched_open))
        status->st_dev = simulated_device;
    return result;
}

static ssize_t setup_read(int fd, void *data, size_t size)
{
    if (fd == watched_fd && !watched_read_started)
    {
        ++watched_reads;
        watched_read_started = 1;
    }
    return read(fd, data, size);
}

static int setup_close(int fd)
{
    if (fd == watched_fd) watched_fd = -1;
    if (fd == source_stat_fd) source_stat_fd = -1;
    return close(fd);
}

static int setup_fstat(int fd, struct stat *status)
{
    if (fd == source_stat_fd) ++source_stat_calls;
    int result = fstat(fd, status);
    if (!result && foreign_destination && status->st_dev == foreign_device &&
        status->st_ino == foreign_inode)
    {
        status->st_uid = getuid() + 1;
        if (foreign_nlink_zero) status->st_nlink = 0;
    }
    if (!result && simulated_device && fd == watched_fd) status->st_dev = simulated_device;
    return result;
}

static int setup_rename(const char *source, const char *destination)
{
    if (foreign_destination && denied_rename_error && !strcmp(destination, foreign_destination))
    {
        ++rename_denials;
        if (rename_symlink_target)
        {
            assert(!unlink(destination));
            assert(!symlink(rename_symlink_target, destination));
        }
        errno = denied_rename_error;
        return -1;
    }
    return rename(source, destination);
}

static int setup_fsync(int fd)
{
    struct stat status;
    if (!fstat(fd, &status) && foreign_destination && status.st_dev == foreign_device &&
        status.st_ino == foreign_inode)
    {
        ++inplace_syncs;
        if (denied_fsync) { errno = EIO; return -1; }
    }
    return fsync(fd);
}

#define open setup_open
#define lstat setup_lstat
#define stat(path, status) setup_stat(path, status)
#define read setup_read
#define close setup_close
#define fstat setup_fstat
#define rename setup_rename
#define fsync setup_fsync
#define FOREVER_MANAGED_CHECK_DEVICE managed_check_device
#include "forever-setup.h"
#undef open
#undef lstat
#undef stat
#undef read
#undef close
#undef fstat
#undef rename
#undef fsync
#undef FOREVER_MANAGED_CHECK_DEVICE

static char root[256];

static void path(char *out, const char *relative)
{
    snprintf(out, 1024, "%s/%s", root, relative);
}

static void put(const char *relative, const char *text)
{
    char p[1024];
    path(p, relative);
    char *slash = strrchr(p, '/');
    *slash = 0;
    assert(!forever_mkdirs(p));
    *slash = '/';
    assert(!forever_write_file(p, text, strlen(text)));
}

static int text_is(const char *relative, const char *text)
{
    char p[1024], buffer[4096];
    path(p, relative);
    return forever_read_file(p, buffer, sizeof(buffer)) >= 0 && !strcmp(buffer, text);
}

static void make_prefix(const char *relative)
{
    char name[512];
    static const char *const regs[] = { "system.reg", "user.reg", "userdef.reg" };
    for (int i = 0; i < 3; ++i)
    {
        snprintf(name, sizeof(name), "%s/%s", relative, regs[i]);
        put(name, "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n");
    }
    snprintf(name, sizeof(name), "%s/.update-timestamp", relative);
    put(name, "1791000000\n");
}

static void test_write_if_absent(void)
{
    char p[1024];
    path(p, "data/prefix/drive_c/wineps5-fg.txt");
    assert(!forever_mkdirs(root));
    put("data/prefix/drive_c/keep", "");
    assert(forever_write_if_absent(p, FOREVER_FG_DEFAULT) == 1);
    assert(text_is("data/prefix/drive_c/wineps5-fg.txt", FOREVER_FG_DEFAULT));
    put("data/prefix/drive_c/wineps5-fg.txt", "fg=off\n");
    assert(forever_write_if_absent(p, FOREVER_FG_DEFAULT) == 0);
    assert(text_is("data/prefix/drive_c/wineps5-fg.txt", "fg=off\n")); /* the user's file wins */
}

static void test_prefix_ready(void)
{
    char p[1024], stamp[1024];
    path(p, "ready");
    path(stamp, "ready/.update-timestamp");
    assert(!forever_prefix_ready(p));
    make_prefix("ready");
    assert(forever_prefix_ready(p));
    put("ready/user.reg", "not a registry\n");
    assert(!forever_prefix_ready(p));

    /* first start in an empty folder: every Wine process finds valid hives,
     * drive_c and a stale stamp, yet the prefix is not ready */
    char fresh[1024], c_drive[1024];
    path(fresh, "fresh/prefix");
    path(c_drive, "fresh/prefix/drive_c");
    assert(!forever_begin_init(fresh));
    assert(forever_is_dir(c_drive));
    assert(text_is("fresh/prefix/system.reg",
                   "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n"));
    assert(text_is("fresh/prefix/user.reg",
                   "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n"
                   "#arch=win64\n"));
    assert(text_is("fresh/prefix/.update-timestamp", "1\n"));
    assert(!forever_prefix_ready(fresh));

    /* an interrupted setup keeps its registries and reruns the full install */
    make_prefix("ready");
    put("ready/system.reg", "WINE REGISTRY Version 2\n[Software] 1\n");
    assert(!forever_begin_init(p));
    assert(text_is("ready/system.reg", "WINE REGISTRY Version 2\n[Software] 1\n"));
    assert(text_is("ready/.update-timestamp", "1\n"));
    assert(forever_exists(stamp));
    assert(!forever_prefix_ready(p));
    assert(forever_finish_init(p) == 0); /* Wine stopped before wineboot finished */
    assert(!forever_prefix_ready(p));
    put("ready/" FOREVER_WINEBOOT_DONE, "");
    assert(forever_finish_init(p) == 1);
    assert(forever_prefix_ready(p));
    assert(!forever_wineboot_done(p));
}

static void test_migration(void)
{
    char prefix[1024], home[1024], cache[1024], legacy[1024], store[1024], from[1024] = "";
    path(prefix, "forever/prefix");
    path(home, "forever/home");
    path(cache, "forever/radv-shader-cache");
    path(legacy, "homebrew/PPSA99030/wine");
    path(store, "prosperostore/previous/PPSA99030/wine");
    const char *roots[] = { legacy, store };
    char forever[1024];
    path(forever, "forever");
    assert(!forever_mkdirs(forever));

    assert(forever_migrate(prefix, home, cache, roots, 2, from, sizeof(from)) == 0); /* nothing anywhere */

    make_prefix("prosperostore/previous/PPSA99030/wine/prefix");
    put("prosperostore/previous/PPSA99030/wine/prefix/drive_c/game.txt", "48 GiB\n");
    put("prosperostore/previous/PPSA99030/wine/home/.cache/x", "h\n");
    put("prosperostore/previous/PPSA99030/radv-shader-cache/mesa_cache.db", "pipelines\n");
    assert(forever_migrate(prefix, home, cache, roots, 2, from, sizeof(from)) == 1);
    assert(!strcmp(from, store));
    assert(text_is("forever/prefix/drive_c/game.txt", "48 GiB\n"));
    assert(text_is("forever/home/.cache/x", "h\n"));
    assert(text_is("forever/radv-shader-cache/mesa_cache.db", "pipelines\n")); /* the app's cache came along */
    {
        char moved[1100];
        snprintf(moved, sizeof(moved), "%s/prefix", store);
        assert(!forever_exists(moved)); /* moved, not copied */
    }

    /* an existing prefix in /data/forever is never replaced */
    make_prefix("homebrew/PPSA99030/wine/prefix");
    assert(forever_migrate(prefix, home, cache, roots, 2, from, sizeof(from)) == 0);
    assert(text_is("forever/prefix/drive_c/game.txt", "48 GiB\n"));
}

static const char managed_v1[] =
    "lib/wine/x86_64-windows/test.dll\tdrive_c/windows/system32/test.dll\t"
    "1c34f88707b55e6104c4eb20e71ffa3d33e414b71ef689a15fad0640d0ac58cb\n";
static const char managed_v2[] =
    "lib/wine/x86_64-windows/test.dll\tdrive_c/windows/system32/test.dll\t"
    "27eb5e51506c911f6fc4bb345c0d9db6f60415fceab7c18e1e9b862637415777\n";
static char managed_app[1024], managed_list[1024], managed_prefix[1024];
static char managed_destination[1024], managed_source[1024], managed_stamp[1024];
static struct forever_managed_report managed_report;

static void write_path(const char *name, const char *text)
{
    assert(!forever_write_file(name, text, strlen(text)));
}

static void managed_fixture(const char *name)
{
    char relative[512], directory[1024];
    snprintf(relative, sizeof(relative), "sync/%s/app/lib/wine/x86_64-windows/test.dll", name);
    put(relative, "verified");
    snprintf(relative, sizeof(relative), "sync/%s/app/prefix-files.txt", name);
    put(relative, managed_v1);
    path(managed_list, relative);
    snprintf(relative, sizeof(relative), "sync/%s/app", name);
    path(managed_app, relative);
    snprintf(relative, sizeof(relative), "sync/%s/prefix", name);
    path(managed_prefix, relative);
    snprintf(relative, sizeof(relative), "sync/%s/prefix/drive_c/windows/system32", name);
    path(directory, relative);
    assert(!forever_mkdirs(directory));
    snprintf(relative, sizeof(relative), "sync/%s/prefix/drive_c/windows/system32/test.dll", name);
    path(managed_destination, relative);
    snprintf(relative, sizeof(relative), "sync/%s/app/lib/wine/x86_64-windows/test.dll", name);
    path(managed_source, relative);
    snprintf(relative, sizeof(relative), "sync/%s/prefix/" FOREVER_MANAGED_STAMP, name);
    path(managed_stamp, relative);
    watched_open = managed_destination;
    watched_reads = watched_opens = 0;
    watched_fd = -1;
}

static int managed_sync(void)
{
    int result = forever_sync_managed(managed_app, managed_list, managed_prefix, &managed_report);
    if (result < 0) assert(managed_report.failed_op && managed_report.failed_path[0]);
    else assert(!managed_report.failed_op && !managed_report.failed_path[0]);
    return result;
}

static void assert_managed_text(const char *text)
{
    char buffer[128];
    assert(forever_read_file(managed_destination, buffer, sizeof(buffer)) >= 0);
    assert(!strcmp(buffer, text));
}

static void test_managed_same_size_drift(void)
{
    struct stat before, after;
    struct timespec times[2];
    managed_fixture("same-size");
    assert(managed_sync() == 1);
    assert(!stat(managed_destination, &before));
    int fd = open(managed_destination, O_WRONLY);
    assert(fd >= 0 && write(fd, "tampered", 8) == 8);
    assert(!close(fd)); /* in-place, same-size drift, not just an inode replacement */
    times[0] = before.st_atim;
    times[1] = before.st_mtim;
    ++times[1].tv_sec;
    assert(!utimensat(AT_FDCWD, managed_destination, times, 0));
    assert(!stat(managed_destination, &after));
    assert(before.st_size == after.st_size && before.st_ino == after.st_ino);
    assert(managed_sync() == 1);
    assert(managed_report.checked == 1 && managed_report.hashed == 1 && managed_report.repaired == 1);
    assert(!strcmp(managed_report.paths[0], "drive_c/windows/system32/test.dll"));
    assert_managed_text("verified");
}

static void test_managed_mtime_touch(void)
{
    struct stat before, after;
    struct timespec times[2];
    managed_fixture("mtime");
    assert(managed_sync() == 1);
    assert(!stat(managed_destination, &before));
    times[0] = before.st_atim;
    times[1] = before.st_mtim;
    times[1].tv_nsec = (times[1].tv_nsec + 1) % 1000000000;
    assert(!utimensat(AT_FDCWD, managed_destination, times, 0));
    watched_reads = 0;
    assert(managed_sync() == 0);
    assert(watched_reads == 1); /* nanosecond-only metadata drift hashes, but never copies */
    assert(managed_report.checked == 1 && managed_report.hashed == 1 && !managed_report.repaired);
    assert(!stat(managed_destination, &after));
    assert(before.st_ino == after.st_ino && before.st_dev == after.st_dev);
    watched_reads = 0;
    assert(managed_sync() == 0);
    assert(watched_reads == 0); /* the refreshed record is reusable */
}

static void test_managed_unchanged_record(void)
{
    managed_fixture("unchanged");
    assert(managed_sync() == 1);
    watched_reads = watched_opens = 0;
    assert(managed_sync() == 0);
    assert(watched_reads == 0);
    assert(watched_opens == 1); /* type/identity inspection is not a content hash */
    assert(managed_report.checked == 1 && !managed_report.hashed && !managed_report.repaired);
    assert(managed_report.state_loaded == 1 && !managed_report.state_publish_attempted);
}

static void test_managed_unstable_device_cache(void)
{
    for (int check_device = 0; check_device <= 1; ++check_device)
    {
        char name[64];
        snprintf(name, sizeof(name), "unstable-device-%d", check_device);
        managed_fixture(name);
        managed_check_device = check_device;
        simulated_device = (dev_t)689700787; /* actual recorded PS5 device identity */
        assert(managed_sync() == 1);
        assert(managed_report.state_publish_attempted && !managed_report.state_publish_result);
        simulated_device = (dev_t)687931141; /* same real files through FTP's context */
        watched_reads = 0;
        assert(managed_sync() == 0);
        assert(managed_report.state_loaded == 1);
        assert(managed_report.hashed == (unsigned)check_device);
        assert(watched_reads == (unsigned)check_device);
        assert(!strcmp(managed_report.mismatch_field, "dev"));
        assert(!strcmp(managed_report.mismatch_recorded, "689700787"));
        assert(!strcmp(managed_report.mismatch_current, "687931141"));
        assert(managed_report.mismatch_ignored == !check_device);
        assert(managed_report.state_publish_attempted == check_device);

        /* Ignoring an unstable device must NEVER ignore a changed size. */
        write_path(managed_destination, "short");
        watched_reads = 0;
        assert(managed_sync() == 1);
        assert(managed_report.hashed == 1);
        assert(!strcmp(managed_report.mismatch_field, "size"));
        assert(!strcmp(managed_report.mismatch_recorded, "8"));
        assert(!strcmp(managed_report.mismatch_current, "5"));
        assert(!managed_report.mismatch_ignored);
        assert_managed_text("verified");
        simulated_device = 0;
        managed_check_device = 1;
    }
}

static void test_managed_no_record_hashes_all(void)
{
    char second[1024], buffer[4096];
    managed_fixture("no-record");
    write_path(managed_destination, "verified");
    snprintf(second, sizeof(second), "%s/second.dll", managed_prefix);
    write_path(second, "verified");
    snprintf(buffer, sizeof(buffer), "%s"
             "lib/wine/x86_64-windows/test.dll\tsecond.dll\t"
             "1c34f88707b55e6104c4eb20e71ffa3d33e414b71ef689a15fad0640d0ac58cb\n",
             managed_v1);
    write_path(managed_list, buffer);
    write_path(managed_stamp, buffer); /* the old manifest-only stamp has no stat records */
    assert(managed_sync() == 0);
    assert(watched_reads == 1);
    assert(managed_report.checked == 2 && managed_report.hashed == 2 && !managed_report.repaired);
    watched_open = second;
    assert(!unlink(managed_stamp));
    watched_reads = 0;
    assert(managed_sync() == 0);
    assert(watched_reads == 1); /* both existing rows are hashed, neither is recopied */
    assert(managed_report.checked == 2 && managed_report.hashed == 2 && !managed_report.repaired);
}

static void test_managed_missing_destination(void)
{
    managed_fixture("missing");
    assert(managed_sync() == 1);
    assert(!unlink(managed_destination));
    assert(managed_sync() == 1);
    assert_managed_text("verified");
    assert(managed_report.checked == 1 && managed_report.hashed == 1 && managed_report.repaired == 1);
}

static void test_managed_changed_digest(void)
{
    managed_fixture("changed");
    assert(managed_sync() == 1);
    write_path(managed_source, "updated");
    write_path(managed_list, managed_v2);
    assert(managed_sync() == 1);
    assert_managed_text("updated");
}

static void test_managed_store_update_replaces_source_tree(void)
{
    static const char second_row[] =
        "lib/wine/x86_64-windows/second.dll\tsecond.dll\t"
        "1c34f88707b55e6104c4eb20e71ffa3d33e414b71ef689a15fad0640d0ac58cb\n";
    char previous_app[1100], source_directory[1100], second_source[1100];
    char second_destination[1100], manifest[4096], text[64];
    struct stat old_main, old_second, old_destination, current;
    struct timespec times[2];
    managed_fixture("store-update");
    snprintf(source_directory, sizeof(source_directory), "%s/lib/wine/x86_64-windows", managed_app);
    snprintf(second_source, sizeof(second_source), "%s/lib/wine/x86_64-windows/second.dll", managed_app);
    snprintf(second_destination, sizeof(second_destination), "%s/second.dll", managed_prefix);
    write_path(second_source, "verified");
    snprintf(manifest, sizeof(manifest), "%s%s", managed_v1, second_row);
    write_path(managed_list, manifest);
    assert(managed_sync() == 2);
    assert(!stat(managed_source, &old_main) && !stat(second_source, &old_second));
    assert(!stat(second_destination, &old_destination));

    /* A store install replaces the entire app folder, not the user prefix. */
    snprintf(previous_app, sizeof(previous_app), "%s.previous", managed_app);
    assert(!rename(managed_app, previous_app));
    assert(!forever_mkdirs(source_directory));
    write_path(managed_source, "updated");
    write_path(second_source, "verified"); /* identical bytes, new inode and mtime */
    times[0] = old_main.st_atim;
    times[1] = old_main.st_mtim;
    times[1].tv_sec += 60;
    assert(!utimensat(AT_FDCWD, managed_source, times, 0));
    times[0] = old_second.st_atim;
    times[1] = old_second.st_mtim;
    times[1].tv_sec += 60;
    assert(!utimensat(AT_FDCWD, second_source, times, 0));
    assert(!stat(managed_source, &current) && current.st_ino != old_main.st_ino &&
           current.st_mtime != old_main.st_mtime);
    assert(!stat(second_source, &current) && current.st_ino != old_second.st_ino &&
           current.st_mtime != old_second.st_mtime);
    snprintf(manifest, sizeof(manifest), "%s%s", managed_v2, second_row);
    write_path(managed_list, manifest);
    source_stat_root = source_directory;
    source_stat_calls = 0;
    assert(managed_sync() == 1);
    assert(managed_report.checked == 2 && managed_report.hashed == 1 && managed_report.repaired == 1);
    assert(source_stat_calls == 0); /* neither stat nor fstat is used on Wine sources */
    source_stat_root = NULL;
    assert_managed_text("updated");
    assert(!stat(second_destination, &current) && current.st_ino == old_destination.st_ino &&
           current.st_mtim.tv_sec == old_destination.st_mtim.tv_sec &&
           current.st_mtim.tv_nsec == old_destination.st_mtim.tv_nsec);
    assert(forever_read_file(second_destination, text, sizeof(text)) > 0 && !strcmp(text, "verified"));
    assert(!managed_sync() && !managed_report.hashed);
}

static void test_managed_bad_source_is_not_cached(void)
{
    char saved[4096], after[4096];
    managed_fixture("bad-source");
    assert(managed_sync() == 1);
    assert(forever_read_file(managed_stamp, saved, sizeof(saved)) > 0);
    write_path(managed_source, "corrupt"); /* same size as the expected new bytes */
    write_path(managed_list, managed_v2);
    assert(managed_sync() == -EIO);
    assert(!strcmp(managed_report.failed_op, "verify-copy"));
    assert(!managed_report.state_publish_attempted);
    assert(forever_read_file(managed_stamp, after, sizeof(after)) > 0 && !strcmp(saved, after));
    write_path(managed_source, "updated");
    assert(managed_sync() == 1);
    assert_managed_text("updated");
    assert(!managed_sync() && !managed_report.hashed);
}

static void test_managed_corrupt_state(void)
{
    char saved[4096];
    managed_fixture("corrupt");
    assert(managed_sync() == 1);
    ssize_t length = forever_read_file(managed_stamp, saved, sizeof(saved));
    assert(length > 0);
    assert(!forever_write_file(managed_stamp, saved, (size_t)length / 2));
    watched_reads = 0;
    assert(managed_sync() == 0);
    assert(watched_reads == 1);
    /* A syntactically valid row with a damaged seal also invalidates the cache. */
    assert(forever_read_file(managed_stamp, saved, sizeof(saved)) > 0);
    char *seal = strchr(saved, '\t') + 1;
    *seal = *seal == '0' ? '1' : '0';
    write_path(managed_stamp, saved);
    watched_reads = 0;
    assert(managed_sync() == 0);
    assert(watched_reads == 1);
    write_path(managed_stamp, "corrupt state\n");
    write_path(managed_destination, "tampered");
    assert(managed_sync() == 1);
    assert_managed_text("verified");
}

static void test_managed_symlink_destination(void)
{
    char target[1024];
    struct stat status;
    managed_fixture("symlink");
    assert(managed_sync() == 1);
    path(target, "symlink-target");
    write_path(target, "verified");
    assert(!unlink(managed_destination));
    assert(!symlink(target, managed_destination));
    watched_reads = 0;
    assert(managed_sync() == 1);
    assert(watched_reads == 1); /* only the verified replacement was read */
    assert(!stat(managed_destination, &status) && S_ISREG(status.st_mode));
    assert(readlink(managed_destination, target, sizeof(target)) < 0 && errno == EINVAL);
    assert_managed_text("verified");
    assert(text_is("symlink-target", "verified")); /* target was neither hashed nor overwritten */
}

static void test_managed_cached_symlink_is_replaced(void)
{
    char target[1024], link[1024], text[64];
    managed_fixture("cached-symlink");
    assert(managed_sync() == 1);
    path(target, "recorded-inode");
    assert(!rename(managed_destination, target));
    assert(!symlink(target, managed_destination));
    /* stat follows to exactly the cached inode/mtime/size, but it is NOT clean. */
    assert(managed_sync() == 1);
    assert(readlink(managed_destination, link, sizeof(link)) < 0 && errno == EINVAL);
    assert(forever_read_file(target, text, sizeof(text)) > 0 && !strcmp(text, "verified"));
    assert_managed_text("verified");
}

static void test_managed_no_follow_symlink_errors(void)
{
    const int errors[] = {ELOOP, EMLINK};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
    {
        char name[64], target[1024], link[1024];
        snprintf(name, sizeof(name), "dangling-symlink-%u", i);
        managed_fixture(name);
        path(target, "absent-symlink-target");
        assert(!symlink(target, managed_destination));
        nofollow_symlink_error = errors[i];
        assert(managed_sync() == 1);
        nofollow_symlink_error = 0;
        assert(readlink(managed_destination, link, sizeof(link)) < 0 && errno == EINVAL);
        assert_managed_text("verified");
    }
}

static void test_managed_nonregular_destination(void)
{
    managed_fixture("nonregular");
    assert(!mkfifo(managed_destination, 0600));
    assert(managed_sync() == 1); /* never opens the FIFO */
    assert_managed_text("verified");
    assert(!unlink(managed_destination));
    assert(!mkdir(managed_destination, 0700));
    assert(managed_sync() == 1); /* replace an empty directory, never traverse it */
    assert_managed_text("verified");
    char keep[1100], stamp[4096], after[4096];
    assert(forever_read_file(managed_stamp, stamp, sizeof(stamp)) > 0);
    assert(!unlink(managed_destination));
    assert(!mkdir(managed_destination, 0700));
    snprintf(keep, sizeof(keep), "%s/keep", managed_destination);
    write_path(keep, "user data");
    assert(managed_sync() == -ENOTEMPTY);
    assert(!strcmp(managed_report.failed_op, "rmdir"));
    assert(!strcmp(managed_report.failed_path, "drive_c/windows/system32/test.dll"));
    assert(forever_read_file(managed_stamp, after, sizeof(after)) > 0 && !strcmp(stamp, after));
    assert(forever_read_file(keep, after, sizeof(after)) > 0 && !strcmp(after, "user data"));
}

static void test_managed_permission_error(void)
{
    char temporary[1100], saved[4096], after[4096];
    managed_fixture("permission");
    assert(managed_sync() == 1);
    assert(forever_read_file(managed_stamp, saved, sizeof(saved)) > 0);
    write_path(managed_destination, "tampered");
    snprintf(temporary, sizeof(temporary), "%s.forever-tmp", managed_destination);
    denied_open = temporary;
    assert(managed_sync() == -EACCES);
    assert(!strcmp(managed_report.failed_op, "open-temporary"));
    assert(!strcmp(managed_report.failed_path, "drive_c/windows/system32/test.dll"));
    assert(forever_permission_error(-EACCES));
    assert(forever_read_file(managed_stamp, after, sizeof(after)) > 0);
    assert(!strcmp(saved, after));
    denied_open = NULL;
    assert(managed_sync() == 1); /* the existing caller's broker restart can retry */
}

static void foreign_rename_fixture(int error, int zero_links)
{
    struct stat status;
    static unsigned serial;
    char name[64];
    snprintf(name, sizeof(name), "foreign-%u-%d-%d", serial++, error, zero_links);
    managed_fixture(name);
    assert(managed_sync() == 1);
    write_path(managed_destination, "tampered");
    assert(!chmod(managed_destination, 0777));
    assert(!stat(managed_destination, &status));
    foreign_device = status.st_dev;
    foreign_inode = status.st_ino;
    foreign_destination = managed_destination;
    foreign_nlink_zero = zero_links;
    denied_rename_error = error;
    rename_denials = inplace_syncs = 0;
    assert(!setup_stat(managed_destination, &status) && status.st_uid != getuid());
    assert(status.st_nlink == (zero_links ? 0 : 1));
}

static void clear_foreign_fixture(void)
{
    foreign_destination = NULL;
    rename_symlink_target = NULL;
    foreign_nlink_zero = denied_rename_error = denied_fsync = 0;
}

static void test_managed_foreign_rename_fallback(void)
{
    const int errors[] = {EACCES, EPERM};
    for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]); ++e)
        for (int zero_links = 0; zero_links <= 1; ++zero_links)
        {
            struct stat after;
            foreign_rename_fixture(errors[e], zero_links);
            assert(managed_sync() == 1);
            assert(rename_denials == 1 && inplace_syncs == 1);
            assert(!stat(managed_destination, &after) && after.st_ino == foreign_inode &&
                   after.st_dev == foreign_device); /* fallback keeps the foreign-owned inode */
            assert_managed_text("verified");
            assert(!managed_sync() && !managed_report.hashed);
            clear_foreign_fixture();
        }
}

static void test_managed_foreign_fsync_failure(void)
{
    char saved[4096], after[4096];
    foreign_rename_fixture(EACCES, 1);
    assert(forever_read_file(managed_stamp, saved, sizeof(saved)) > 0);
    denied_fsync = 1;
    assert(managed_sync() == -EIO);
    assert(!strcmp(managed_report.failed_op, "fsync-inplace"));
    assert(!strcmp(managed_report.failed_path, "drive_c/windows/system32/test.dll"));
    assert(rename_denials == 1 && inplace_syncs == 1);
    assert(forever_read_file(managed_stamp, after, sizeof(after)) > 0 && !strcmp(saved, after));
    denied_fsync = 0;
    assert(!managed_sync()); /* completed data is hashed before publishing a new record */
    assert_managed_text("verified");
    clear_foreign_fixture();
}

static void test_managed_foreign_hardlink_refused(void)
{
    char alias[1100], text[64], saved[4096], after[4096];
    foreign_rename_fixture(EPERM, 0);
    snprintf(alias, sizeof(alias), "%s/foreign-alias", managed_app);
    assert(!link(managed_destination, alias));
    assert(forever_read_file(managed_stamp, saved, sizeof(saved)) > 0);
    assert(managed_sync() == -EMLINK);
    assert(!strcmp(managed_report.failed_op, "inplace-links"));
    assert(rename_denials == 1 && !inplace_syncs);
    assert_managed_text("tampered");
    assert(forever_read_file(alias, text, sizeof(text)) > 0 && !strcmp(text, "tampered"));
    assert(forever_read_file(managed_stamp, after, sizeof(after)) > 0 && !strcmp(saved, after));
    clear_foreign_fixture();
}

static void test_managed_foreign_symlink_race(void)
{
    char target[1100], text[64], saved[4096], after[4096];
    foreign_rename_fixture(EACCES, 1);
    snprintf(target, sizeof(target), "%s/outside-target", managed_app);
    write_path(target, "untouched");
    assert(forever_read_file(managed_stamp, saved, sizeof(saved)) > 0);
    rename_symlink_target = target;
    assert(managed_sync() == -ELOOP);
    assert(!strcmp(managed_report.failed_op, "open-inplace"));
    assert(readlink(managed_destination, text, sizeof(text)) > 0);
    assert(forever_read_file(target, text, sizeof(text)) > 0 && !strcmp(text, "untouched"));
    assert(forever_read_file(managed_stamp, after, sizeof(after)) > 0 && !strcmp(saved, after));
    clear_foreign_fixture();
}

static void test_managed_failure_diagnostics(void)
{
    managed_fixture("diagnostics");
    assert(managed_sync() == 1);
    write_path(managed_destination, "tampered");
    denied_open = managed_destination;
    assert(managed_sync() == -EACCES);
    assert(!strcmp(managed_report.failed_op, "open-destination"));
    assert(!strcmp(managed_report.failed_path, "drive_c/windows/system32/test.dll"));
    denied_open = managed_source;
    assert(managed_sync() == -EACCES);
    assert(!strcmp(managed_report.failed_op, "open-source"));
    assert(!strcmp(managed_report.failed_path, "drive_c/windows/system32/test.dll"));
    denied_open = NULL;
    assert(managed_sync() == 1);
}

static void test_managed_failed_publication(void)
{
    char temporary[1100], saved[4096], after[4096];
    managed_fixture("publication");
    assert(managed_sync() == 1);
    assert(!managed_report.state_loaded && managed_report.state_publish_attempted);
    assert(managed_report.state_publish_result == 0 && managed_report.state_publish_bytes > 0);
    assert(forever_read_file(managed_stamp, saved, sizeof(saved)) > 0);
    write_path(managed_source, "updated");
    write_path(managed_list, managed_v2);
    snprintf(temporary, sizeof(temporary), "%s.forever-tmp", managed_stamp);
    assert(!mkdir(temporary, 0700));
    assert(managed_sync() < 0);
    assert(managed_report.state_loaded == 1 && managed_report.state_publish_attempted);
    assert(managed_report.state_publish_result < 0);
    assert(forever_read_file(managed_stamp, after, sizeof(after)) > 0);
    assert(!strcmp(saved, after)); /* no partly published metadata */
    assert_managed_text("updated");
    assert(!rmdir(temporary));
    assert(managed_sync() == 0); /* verify the already-copied content, then publish */
    assert(managed_report.state_publish_attempted && !managed_report.state_publish_result);
}

static void test_managed_identity_drift(void)
{
    struct stat before, replaced;
    struct timespec times[2];
    managed_fixture("identity");
    assert(managed_sync() == 1);
    assert(!stat(managed_destination, &before));
    write_path(managed_destination, "tampered");
    times[0] = before.st_atim;
    times[1] = before.st_mtim;
    assert(!utimensat(AT_FDCWD, managed_destination, times, 0));
    assert(!stat(managed_destination, &replaced));
    assert(before.st_ino != replaced.st_ino); /* write_path uses an atomic replacement */
    assert(before.st_size == replaced.st_size);
    assert(before.st_mtim.tv_sec == replaced.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == replaced.st_mtim.tv_nsec);
    assert(managed_sync() == 1);
    assert_managed_text("verified");
}

static void test_managed_wrong_size(void)
{
    managed_fixture("wrong-size");
    assert(managed_sync() == 1);
    write_path(managed_destination, "short");
    assert(managed_sync() == 1);
    assert_managed_text("verified");
}

static void test_managed_manifest_guards(void)
{
    managed_fixture("guards");
    /* The full two-architecture inventory exceeds the old 64-KiB bound. */
    char *large = malloc(80002);
    assert(large);
    memset(large, '#', 80000);
    large[80000] = '\n';
    large[80001] = 0;
    write_path(managed_list, large);
    assert(managed_sync() == 0);
    assert(managed_sync() == 0);
    free(large);
    write_path(managed_list, "../escape\tdrive_c/x\tcccc\n");
    assert(managed_sync() == -EACCES);
    write_path(managed_list, "missing-tabs\n");
    assert(managed_sync() == -EINVAL);
}

static void test_wow_config(void)
{
    static const char *const channels[] = {"_forever_", "_classic_beta_"};
    static const char *const executables[] = {"Wow.exe", "WowB.exe"};
    static const char preset[] = "SET GamePadEnable \"1\"\n";
    static const char mine[] = "SET mine \"1\"\n";
    char share[1024], prefix[1024], relative[512], config[1024];
    path(share, "app/share");
    path(prefix, "wow/prefix");
    put("app/share/Config.wtf", preset);
    assert(!forever_mkdirs(prefix));
    assert(forever_install_wow_config(share, prefix) == 0); /* neither installed */
    for (unsigned i = 0; i < 2; ++i)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/keep",
                 channels[i]);
        put(relative, "");
    }
    assert(forever_install_wow_config(share, prefix) == 0); /* directories alone are not an installation */
    for (unsigned i = 0; i < 2; ++i)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/%s",
                 channels[i], executables[i]);
        put(relative, "MZ");
    }
    assert(forever_install_wow_config(share, prefix) == 2);
    for (unsigned i = 0; i < 2; ++i)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[i]);
        assert(text_is(relative, preset));
    }
    assert(forever_install_wow_config(share, prefix) == 0);

    /* Either channel's existing settings win independently of the other. */
    for (unsigned existing = 0; existing < 2; ++existing)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[existing]);
        put(relative, mine);
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[1 - existing]);
        path(config, relative);
        assert(!unlink(config));
        assert(forever_install_wow_config(share, prefix) == 1);
        assert(text_is(relative, preset));
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[existing]);
        assert(text_is(relative, mine));
        assert(forever_install_wow_config(share, prefix) == 0);
        assert(text_is(relative, mine));
    }
}

/* A prefix whose first wineboot never finished lacks the Program Files
 * folders; the Battle.net installer rejects its default location then. */
static void test_standard_folders(void)
{
    char prefix[1024], dir[1024];
    path(prefix, "folders/prefix");
    put("folders/prefix/drive_c/users/keep", "");
    assert(forever_standard_folders(prefix) == 2);
    path(dir, "folders/prefix/drive_c/Program Files");
    assert(forever_is_dir(dir));
    path(dir, "folders/prefix/drive_c/Program Files (x86)");
    assert(forever_is_dir(dir));
    assert(forever_standard_folders(prefix) == 0); /* nothing left to create */

    path(prefix, "partial/prefix");
    put("partial/prefix/drive_c/Program Files (x86)/Battle.net/Battle.net.exe", "MZ");
    assert(forever_standard_folders(prefix) == 1);
    assert(text_is("partial/prefix/drive_c/Program Files (x86)/Battle.net/Battle.net.exe", "MZ"));

    path(prefix, "blocked/prefix");
    put("blocked/prefix/drive_c/Program Files", "not a folder");
    assert(forever_standard_folders(prefix) == -ENOTDIR); /* a user's file is never replaced */
    assert(text_is("blocked/prefix/drive_c/Program Files", "not a folder"));
}

static void test_permission_message(void)
{
    char message[512];
    assert(forever_permission_error(-EACCES));
    assert(forever_permission_error(-EPERM));
    assert(!forever_permission_error(-ENOENT));
    assert(forever_permission_message(
               message, sizeof(message),
               "/data/forever/prefix/drive_c/wineps5-fg.txt",
               "/data/forever/prefix") > 0);
    assert(!strcmp(message,
        "Forever Launcher can't write to /data/forever/prefix/drive_c/wineps5-fg.txt "
        "(permission denied).\n"
        "On a PC, use FTP to set /data/forever/prefix and everything in it to permission 777,\n"
        "then start Forever Launcher again."));
    assert(!strcmp(forever_setting_permission_message(-EACCES),
                   "Forever Launcher can't save this setting (permission denied).\n"
                   "Close and start Forever Launcher again."));
    assert(forever_setting_permission_message(-EPERM));
    assert(!forever_setting_permission_message(-EIO));
}

static void test_managed_repair_log_cap(void)
{
    char manifest[4096];
    size_t used;
    managed_fixture("log-cap");
    used = (size_t)snprintf(manifest, sizeof(manifest), "%s", managed_v1);
    for (unsigned i = 0; i < FOREVER_MANAGED_LOG_LIMIT + 1; ++i)
    {
        int written = snprintf(manifest + used, sizeof(manifest) - used,
            "lib/wine/x86_64-windows/test.dll\textra-%u.dll\t"
            "1c34f88707b55e6104c4eb20e71ffa3d33e414b71ef689a15fad0640d0ac58cb\n", i);
        assert(written > 0 && (size_t)written < sizeof(manifest) - used);
        used += (size_t)written;
    }
    write_path(managed_list, manifest);
    assert(managed_sync() == FOREVER_MANAGED_LOG_LIMIT + 2);
    assert(managed_report.checked == FOREVER_MANAGED_LOG_LIMIT + 2 &&
           managed_report.hashed == managed_report.checked);
    assert(!strcmp(managed_report.paths[0], "drive_c/windows/system32/test.dll"));
    assert(!strcmp(managed_report.paths[FOREVER_MANAGED_LOG_LIMIT - 1], "extra-6.dll"));
    assert(!managed_sync() && !managed_report.hashed);
}

/* Measure the real staged inventory without touching its files or a user
 * prefix. Hard links avoid fixture-copy cost on the same host filesystem. */
static void benchmark_managed_sync(const char *app, const char *manifest)
{
    static char list[1u << 19];
    char prefix[1024], source[1100], destination[1100];
    struct forever_managed_report report;
    struct timespec start, finish;
    unsigned long long bytes = 0;
    unsigned files = 0;
    double first, unchanged;
    path(prefix, "benchmark");
    assert(!forever_mkdirs(prefix));
    assert(forever_read_file(manifest, list, sizeof(list)) >= 0);
    for (char *line = list, *next; *line; line = next)
    {
        struct stat status;
        char *end = strchr(line, '\n'), *tab1, *tab2, *slash;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        if (!*line || *line == '#') continue;
        assert((tab1 = strchr(line, '\t')) && (tab2 = strchr(tab1 + 1, '\t')));
        *tab1++ = *tab2 = 0;
        assert(snprintf(source, sizeof(source), "%s/%s", app, line) < (int)sizeof(source));
        assert(snprintf(destination, sizeof(destination), "%s/%s", prefix, tab1) < (int)sizeof(destination));
        assert(!stat(source, &status) && S_ISREG(status.st_mode));
        bytes += (unsigned long long)status.st_size;
        ++files;
        slash = strrchr(destination, '/');
        *slash = 0;
        assert(!forever_mkdirs(destination));
        *slash = '/';
        if (link(source, destination))
            assert(errno == EXDEV && !forever_copy_file(source, destination, NULL));
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    assert(!forever_sync_managed(app, manifest, prefix, &report));
    assert(!clock_gettime(CLOCK_MONOTONIC, &finish));
    first = (double)(finish.tv_sec - start.tv_sec) + (finish.tv_nsec - start.tv_nsec) / 1e9;
    assert(report.checked == files && report.hashed == files && !report.repaired);
    printf("prefix-sync benchmark files=%u bytes=%llu first_seconds=%.6f checked=%u hashed=%u repaired=%u\n",
           files, bytes, first, report.checked, report.hashed, report.repaired);
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    assert(!forever_sync_managed(app, manifest, prefix, &report));
    assert(!clock_gettime(CLOCK_MONOTONIC, &finish));
    unchanged = (double)(finish.tv_sec - start.tv_sec) + (finish.tv_nsec - start.tv_nsec) / 1e9;
    assert(report.checked == files && !report.hashed && !report.repaired);
    printf("prefix-sync benchmark unchanged_seconds=%.6f checked=%u hashed=%u repaired=%u\n",
           unchanged, report.checked, report.hashed, report.repaired);
}

int main(int argc, char **argv)
{
    snprintf(root, sizeof(root), "/tmp/forever-setup-test-%ld", (long)getpid());
    if (argc == 4 && !strcmp(argv[1], "--benchmark"))
    {
        benchmark_managed_sync(argv[2], argv[3]);
        goto cleanup;
    }
    assert(argc == 1);
    test_write_if_absent();
    test_prefix_ready();
    test_migration();
    test_managed_same_size_drift();
    test_managed_mtime_touch();
    test_managed_unchanged_record();
    test_managed_unstable_device_cache();
    test_managed_no_record_hashes_all();
    test_managed_missing_destination();
    test_managed_changed_digest();
    test_managed_store_update_replaces_source_tree();
    test_managed_bad_source_is_not_cached();
    test_managed_corrupt_state();
    test_managed_symlink_destination();
    test_managed_cached_symlink_is_replaced();
    test_managed_no_follow_symlink_errors();
    test_managed_nonregular_destination();
    test_managed_permission_error();
    test_managed_failed_publication();
    test_managed_foreign_rename_fallback();
    test_managed_foreign_fsync_failure();
    test_managed_foreign_hardlink_refused();
    test_managed_foreign_symlink_race();
    test_managed_failure_diagnostics();
    test_managed_identity_drift();
    test_managed_wrong_size();
    test_managed_manifest_guards();
    test_managed_repair_log_cap();
    test_wow_config();
    test_standard_folders();
    test_permission_message();
cleanup:;
    char command[300];
    snprintf(command, sizeof(command), "rm -rf %s", root);
    if (system(command)) return 1;
    puts("test-forever-setup: PASS");
    return 0;
}
