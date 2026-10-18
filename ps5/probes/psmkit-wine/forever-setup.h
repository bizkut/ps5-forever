/* First-start and every-start setup of the user data in /data/forever.
 *
 * The app folder is replaced by every store update, so everything the user
 * owns (the Wine prefix with Battle.net and the games, HOME, the shader cache)
 * lives in FOREVER_ROOT. Each function takes its paths as arguments so the
 * host tests can run it against a temporary directory. Managed inspection uses
 * stat plus no-follow open/fstat: the PS5 title sandbox rejects lstat. Empty
 * directory replacement and the rare in-place fallback have separate syscall
 * qualification requirements documented in release/BUILDING.md. */
#ifndef FOREVER_SETUP_H
#define FOREVER_SETUP_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sha256.h"

#define FOREVER_TUNE_DEFAULT "unwind_cache=on qpc=tsc log_async=on\n"
#define FOREVER_FG_DEFAULT "fg=on mult=2 flow=1.0 method=auto\n"
#define FOREVER_HUD_DEFAULT "hud=off pos=tl scale=2\n"
#define FOREVER_MANAGED_STAMP ".forever-managed"
#define FOREVER_INIT_PENDING ".forever-init-pending"
/* written by the title's wineboot -i as its last step (WINEPS5_PREFIX_INIT=1) */
#define FOREVER_WINEBOOT_DONE ".wineboot-init-done"

static inline int forever_permission_error(int result)
{
    return result == -EACCES || result == -EPERM;
}

static inline int forever_permission_message(
    char *message, size_t size, const char *path, const char *repair_root)
{
    int length = snprintf(
        message, size,
        "Forever Launcher can't write to %s (permission denied).\n"
        "On a PC, use FTP to set %s and everything in it to permission 777,\n"
        "then start Forever Launcher again.",
        path, repair_root);
    if (length < 0) return -EIO;
    return (size_t)length < size ? length : -ENAMETOOLONG;
}

static inline const char *forever_setting_permission_message(int result)
{
    return forever_permission_error(result) ?
        "Forever Launcher can't save this setting (permission denied).\n"
        "Close and start Forever Launcher again." : NULL;
}

static inline int forever_exists(const char *path)
{
    struct stat st;
    return !stat(path, &st);
}

static inline int forever_is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* mkdir -p; 0 or -errno */
static inline int forever_mkdirs(const char *path)
{
    char buffer[1024];
    size_t length = strlen(path);
    if (!length || length >= sizeof(buffer)) return -ENAMETOOLONG;
    memcpy(buffer, path, length + 1);
    for (char *p = buffer + 1; ; ++p)
    {
        if (*p != '/' && *p) continue;
        char saved = *p;
        *p = 0;
        if (mkdir(buffer, 0777) && errno != EEXIST) return -errno;
        if (!saved) break;
        *p = saved;
    }
    {
        struct stat status;
        if (stat(path, &status)) return -errno;
        return S_ISDIR(status.st_mode) ? 0 : -ENOTDIR;
    }
}

/* The Program Files folders wineboot makes in a new prefix. A prefix whose
 * first wineboot did not finish lacks them, and installers then reject their
 * default location. Only missing folders are made; an existing file of the
 * same name is never replaced. Returns how many were made, or -errno. */
static inline int forever_standard_folders(const char *prefix)
{
    static const char *const names[] = { "Program Files", "Program Files (x86)" };
    char path[1024];
    int made = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    {
        struct stat status;
        if (snprintf(path, sizeof(path), "%s/drive_c/%s", prefix, names[i]) >= (int)sizeof(path))
            return -ENAMETOOLONG;
        if (!stat(path, &status))
        {
            if (S_ISDIR(status.st_mode)) continue;
            return -ENOTDIR;
        }
        if (errno != ENOENT) return -errno;
        if (mkdir(path, 0777) && errno != EEXIST) return -errno;
        if (stat(path, &status)) return -errno;
        if (!S_ISDIR(status.st_mode)) return -ENOTDIR;
        ++made;
    }
    return made;
}

/* Write all of data to path through a temporary name, so a reader never sees
 * a partial file. 0 or -errno. */
static inline int forever_write_file(const char *path, const void *data, size_t size)
{
    char temporary[1100];
    const unsigned char *bytes = data;
    int fd, error = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.forever-tmp", path) >= (int)sizeof(temporary))
        return -ENAMETOOLONG;
    if (unlink(temporary) && errno != ENOENT) return -errno;
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0666)) < 0) return -errno;
    while (size && !error)
    {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) error = written < 0 ? -errno : -EIO;
        else { bytes += written; size -= (size_t)written; }
    }
    if (close(fd) && !error) error = -errno;
    if (!error && rename(temporary, path)) error = -errno;
    if (error) unlink(temporary);
    return error;
}

/* 1 written, 0 already there, -errno failure: the user's own file always wins. */
static inline int forever_write_if_absent(const char *path, const char *text)
{
    struct stat status;
    if (!stat(path, &status)) return 0;
    if (errno != ENOENT) return -errno;
    int error = forever_write_file(path, text, strlen(text));
    return error ? error : 1;
}

/* Read a whole small file into buffer (NUL-terminated). Length or -errno. */
static inline ssize_t forever_read_file(const char *path, char *buffer, size_t size)
{
    size_t used = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    while (used + 1 < size)
    {
        ssize_t count = read(fd, buffer + used, size - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { int error = -errno; close(fd); return error; }
        if (!count) break;
        used += (size_t)count;
    }
    if (used + 1 >= size)
    {
        char extra;
        if (read(fd, &extra, 1) > 0) { close(fd); return -EFBIG; }
    }
    close(fd);
    buffer[used] = 0;
    return (ssize_t)used;
}

/* Copy between already-open descriptors. Record the failed I/O operation. */
static inline int forever_copy_data(int in, int out, const char **operation)
{
    static unsigned char block[1u << 16];
    for (;;)
    {
        ssize_t count = read(in, block, sizeof(block)), done = 0;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { *operation = "read-source"; return -errno; }
        if (!count) return 0;
        while (done < count)
        {
            ssize_t written = write(out, block + done, (size_t)(count - done));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0)
            {
                *operation = "write-destination";
                return written < 0 ? -errno : -EIO;
            }
            done += written;
        }
    }
}

/* Copy source to destination through a temporary name. 0 or -errno.
 * Only managed sync may decide whether a rejected rename has a safe fallback. */
static inline int forever_copy_file(const char *source, const char *destination,
                                    const char **operation)
{
    char temporary[1100];
    const char *step = "copy-path";
    int in = -1, out = -1, error = 0, created = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.forever-tmp", destination) >= (int)sizeof(temporary))
    {
        if (operation) *operation = step;
        return -ENAMETOOLONG;
    }
    step = "open-source";
    if ((in = open(source, O_RDONLY)) < 0) { error = -errno; goto done; }
    step = "unlink-temporary";
    if (unlink(temporary) && errno != ENOENT) { error = -errno; goto done; }
    step = "open-temporary";
    if ((out = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0666)) < 0) { error = -errno; goto done; }
    created = 1;
    error = forever_copy_data(in, out, &step);
    close(in);
    in = -1;
    if (close(out) && !error) { step = "close-temporary"; error = -errno; }
    out = -1;
    if (!error && rename(temporary, destination)) { step = "rename"; error = -errno; }
done:
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    if (error)
    {
        if (operation) *operation = step;
        if (created) unlink(temporary);
    }
    return error;
}

/* An initialized prefix has its three registries and the update stamp, and
 * no first-start setup left unfinished. */
static inline int forever_prefix_ready(const char *prefix)
{
    static const char *const names[] = { "system.reg", "user.reg", "userdef.reg", ".update-timestamp" };
    char path[1024], head[32];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    {
        if (snprintf(path, sizeof(path), "%s/%s", prefix, names[i]) >= (int)sizeof(path)) return 0;
        if (i < 3)
        {
            int fd = open(path, O_RDONLY);
            ssize_t got = fd < 0 ? -1 : read(fd, head, 23);
            if (fd >= 0) close(fd);
            if (got != 23 || strncmp(head, "WINE REGISTRY Version 2", 23)) return 0;
        }
        else if (!forever_exists(path)) return 0;
    }
    snprintf(path, sizeof(path), "%s/" FOREVER_INIT_PENDING, prefix);
    return !forever_exists(path);
}

/* Before wineboot -i: give every title Wine process the prefix it requires
 * (empty registries, drive_c, an update stamp), marked unfinished. The stamp
 * is a stale value, so wineboot runs its full install, also after an
 * interrupted setup; registries already present are kept. 0 or -errno. */
static inline int forever_begin_init(const char *prefix)
{
    static const char *const hives[][2] = {
        { "system.reg", "Machine" },
        { "user.reg", "User\\\\S-1-5-21-0-0-0-1000" },
        { "userdef.reg", "User\\\\.Default" },
    };
    char path[1024], text[160];
    int error;

    snprintf(path, sizeof(path), "%s/drive_c", prefix);
    if ((error = forever_mkdirs(path))) return error;
    snprintf(path, sizeof(path), "%s/" FOREVER_INIT_PENDING, prefix);
    if ((error = forever_write_file(path, "", 0))) return error;
    snprintf(path, sizeof(path), "%s/" FOREVER_WINEBOOT_DONE, prefix);
    if (unlink(path) && errno != ENOENT) return -errno;
    for (size_t i = 0; i < sizeof(hives) / sizeof(hives[0]); ++i)
    {
        snprintf(path, sizeof(path), "%s/%s", prefix, hives[i][0]);
        snprintf(text, sizeof(text), "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\%s\n\n"
                 "#arch=win64\n", hives[i][1]);
        if ((error = forever_write_if_absent(path, text)) < 0) return error;
    }
    snprintf(path, sizeof(path), "%s/.update-timestamp", prefix);
    return forever_write_file(path, "1\n", 2);
}

/* wineboot -i has finished its last step: 1, or 0 while it is still running. */
static inline int forever_wineboot_done(const char *prefix)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/" FOREVER_WINEBOOT_DONE, prefix);
    return forever_exists(path);
}

/* After Wine has ended: a finished setup clears both markers. 1 finished,
 * 0 unfinished, -errno failure. */
static inline int forever_finish_init(const char *prefix)
{
    char path[1024];
    if (!forever_wineboot_done(prefix)) return 0;
    snprintf(path, sizeof(path), "%s/" FOREVER_INIT_PENDING, prefix);
    if (unlink(path) && errno != ENOENT) return -errno;
    snprintf(path, sizeof(path), "%s/" FOREVER_WINEBOOT_DONE, prefix);
    if (unlink(path) && errno != ENOENT) return -errno;
    return 1;
}

/* Move an older in-app prefix and HOME (or the store's kept copy of the old
 * app folder) to the user data root, with the RADV shader cache the app kept
 * beside them (<legacy root>/../radv-shader-cache). rename() keeps it
 * instant: both are on the console's /data. Returns 1 moved, 0 nothing to
 * move, -errno failure. */
static inline int forever_migrate(const char *prefix, const char *home, const char *cache,
                                  const char *const *legacy_roots, size_t legacy_count,
                                  char *from, size_t from_size)
{
    char legacy_prefix[1024], legacy_home[1024], legacy_cache[1024], probe[1100];
    struct stat status;
    if (from && from_size) from[0] = 0;
    if (snprintf(probe, sizeof(probe), "%s/system.reg", prefix) >= (int)sizeof(probe))
        return -ENAMETOOLONG;
    if (!stat(probe, &status)) return 0;
    if (errno != ENOENT) return -errno;
    for (size_t i = 0; i < legacy_count; ++i)
    {
        const char *slash = strrchr(legacy_roots[i], '/');
        snprintf(legacy_prefix, sizeof(legacy_prefix), "%s/prefix", legacy_roots[i]);
        snprintf(legacy_home, sizeof(legacy_home), "%s/home", legacy_roots[i]);
        snprintf(legacy_cache, sizeof(legacy_cache), "%.*s/radv-shader-cache",
                 slash ? (int)(slash - legacy_roots[i]) : 0, legacy_roots[i]);
        if (from) snprintf(from, from_size, "%s", legacy_roots[i]);
        snprintf(probe, sizeof(probe), "%s/system.reg", legacy_prefix);
        if (stat(probe, &status))
        {
            if (errno == ENOENT) { if (from && from_size) from[0] = 0; continue; }
            return -errno;
        }
        /* Move the prefix last: system.reg is the completion marker checked
         * above, so a permission failure moving HOME/cache remains retryable. */
        if (forever_is_dir(legacy_cache) && rename(legacy_cache, cache) &&
            errno != ENOTEMPTY && errno != EEXIST)
            return -errno;
        if (forever_is_dir(legacy_home) && rename(legacy_home, home) &&
            errno != ENOTEMPTY && errno != EEXIST)
            return -errno;
        /* rename() replaces an empty destination directory left by a failed start. */
        if (rename(legacy_prefix, prefix)) return -errno;
        return 1;
    }
    return 0;
}

#define FOREVER_MANAGED_LOG_LIMIT 8
#define FOREVER_MANAGED_STATE_HEADER "# forever-managed-v2\t"
/* PS5 filesystem device IDs are namespace/context-specific. Keep them for
 * descriptor race checks, but not for a cache persisted across title launches.
 * Host tests can select either policy while injecting changing device IDs. */
#ifndef FOREVER_MANAGED_CHECK_DEVICE
#ifdef __PROSPERO__
#define FOREVER_MANAGED_CHECK_DEVICE 0
#else
#define FOREVER_MANAGED_CHECK_DEVICE 1
#endif
#endif

struct forever_managed_report
{
    unsigned checked, hashed, repaired;
    char paths[FOREVER_MANAGED_LOG_LIMIT][1024];
    const char *failed_op;
    char failed_path[1024];
    int state_read_result, state_publish_attempted, state_publish_result;
    unsigned state_loaded;
    size_t state_publish_bytes;
    const char *mismatch_field;
    int mismatch_ignored;
    char mismatch_path[1024], mismatch_recorded[65], mismatch_current[65];
};

static inline int forever_managed_failure(struct forever_managed_report *report,
                                          const char *operation, const char *path, int error)
{
    report->failed_op = operation;
    snprintf(report->failed_path, sizeof(report->failed_path), "%s", path);
    return error;
}
/* Keep just the first actionable cache mismatch. If every differing field is
 * ignored by policy, retain the first such difference to explain the cache hit. */
static inline void forever_managed_mismatch(struct forever_managed_report *report,
                                           const char *path, const char *field,
                                           const char *recorded, size_t recorded_size,
                                           const char *current, size_t current_size, int ignored)
{
    if (report->mismatch_field && (!report->mismatch_ignored || ignored)) return;
    report->mismatch_field = field;
    report->mismatch_ignored = ignored;
    snprintf(report->mismatch_path, sizeof(report->mismatch_path), "%s", path);
    if (recorded_size >= sizeof(report->mismatch_recorded))
        recorded_size = sizeof(report->mismatch_recorded) - 1;
    if (current_size >= sizeof(report->mismatch_current))
        current_size = sizeof(report->mismatch_current) - 1;
    memcpy(report->mismatch_recorded, recorded, recorded_size);
    report->mismatch_recorded[recorded_size] = 0;
    memcpy(report->mismatch_current, current, current_size);
    report->mismatch_current[current_size] = 0;
}

/* Compare the canonical five-field metadata without integer-parser/runtime
 * dependencies. Size is always field zero and is never ignored. Preserve v2
 * records: dropping PS5's unstable device field must not force another audit. */
static inline int forever_managed_cache_matches(const char *recorded, const char *current,
                                                const char *path,
                                                struct forever_managed_report *report)
{
    static const char *const fields[] = {"size", "mtime_sec", "mtime_nsec", "dev", "ino"};
    const char *old = recorded, *now = current;
    for (unsigned i = 0; i < 5; ++i)
    {
        const char *old_end = i < 4 ? strchr(old, ' ') : old + strlen(old);
        const char *now_end = i < 4 ? strchr(now, ' ') : now + strlen(now);
        if (!old_end || !now_end || old_end == old || now_end == now ||
            (i == 4 && (strchr(old, ' ') || strchr(now, ' '))))
        {
            forever_managed_mismatch(report, path, "metadata", recorded, strlen(recorded),
                                     current, strlen(current), 0);
            return 0;
        }
        size_t old_size = (size_t)(old_end - old), now_size = (size_t)(now_end - now);
        if (old_size != now_size || memcmp(old, now, old_size))
        {
            int ignored = i == 3 && !FOREVER_MANAGED_CHECK_DEVICE;
            forever_managed_mismatch(report, path, fields[i], old, old_size, now, now_size, ignored);
            if (!ignored) return 0;
        }
        if (i < 4) { old = old_end + 1; now = now_end + 1; }
    }
    return 1;
}


/* PS5 may forbid replacing another uid's directory entry even when its file
 * is writable. Only after that specific rename failure, overwrite the same
 * no-follow regular inode. Never truncate until identity/type/link checks pass.
 * PS5 reports st_nlink=0 for ordinary files; both 0 and 1 are safe here. */
static inline int forever_managed_overwrite(const char *source, const char *destination,
                                            const struct stat *identity, const char **operation)
{
    struct stat opened;
    const char *step = "open-source";
    int in = -1, out = -1, error = 0;
    if ((in = open(source, O_RDONLY)) < 0) { error = -errno; goto done; }
    step = "open-inplace";
    if ((out = open(destination, O_WRONLY | O_NOFOLLOW | O_NONBLOCK)) < 0)
    { error = -errno; goto done; }
    step = "fstat-inplace";
    if (fstat(out, &opened)) { error = -errno; goto done; }
    step = "inplace-type";
    if (!S_ISREG(opened.st_mode)) { error = -EINVAL; goto done; }
    step = "inplace-identity";
    if (opened.st_dev != identity->st_dev || opened.st_ino != identity->st_ino)
    { error = -EAGAIN; goto done; }
    step = "inplace-links";
    if (identity->st_nlink > 1 || opened.st_nlink > 1) { error = -EMLINK; goto done; }
    step = "truncate-inplace";
    if (ftruncate(out, 0)) { error = -errno; goto done; }
    error = forever_copy_data(in, out, &step);
    if (!error)
    {
        step = "fsync-inplace";
        if (fsync(out)) error = -errno;
    }
done:
    if (in >= 0) close(in);
    if (out >= 0 && close(out) && !error) { step = "close-inplace"; error = -errno; }
    if (error) *operation = step;
    return error;
}

struct forever_managed_record
{
    const char *destination, *digest, *metadata;
};

/* stat is title-supported but follows symlinks. Never trust its regular-file
 * metadata until a no-follow descriptor proves the leaf's type and identity.
 * A symlink (including a dangling one) is a replace target, not an error.
 * Nonregular files are never read; only directory candidates need a no-follow
 * open to distinguish a real directory from a symlink to one. */
static inline int forever_managed_open(const char *path, struct stat *status,
                                       int *fd, int *exists, const char **operation)
{
    int opened, error;
    *fd = -1;
    memset(status, 0, sizeof(*status));
    *exists = !stat(path, status);
    if (!*exists && errno != ENOENT && errno != ELOOP && errno != EMLINK)
    { *operation = "stat"; return -errno; }
    if (*exists && !S_ISREG(status->st_mode) && !S_ISDIR(status->st_mode)) return 0;
    opened = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (opened < 0)
    {
        error = errno;
        status->st_mode = 0; /* followed metadata must not enable in-place repair */
        if (error == ENOENT) { *exists = 0; return 0; }
        if (error == ELOOP || error == EMLINK) { *exists = 1; return 0; }
        *operation = "open-destination";
        return -error;
    }
    if (fstat(opened, status))
    {
        error = -errno;
        close(opened);
        *operation = "fstat-destination";
        return error;
    }
    *exists = 1;
    if (!S_ISREG(status->st_mode))
    {
        if (close(opened)) { *operation = "close-destination"; return -errno; }
        return 0;
    }
    *fd = opened;
    return 0;
}

static inline unsigned forever_managed_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    while (*name) hash = (hash ^ (unsigned char)*name++) * 16777619u;
    return hash & 4095;
}

static inline void forever_managed_hex(const unsigned char digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i)
    {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
}

static inline int forever_managed_digest_valid(const char *digest)
{
    if (strlen(digest) != 64) return 0;
    for (unsigned i = 0; i < 64; ++i)
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f'))) return 0;
    return 1;
}

/* Canonical text avoids depending on the host/console struct stat layout.
 * Nanoseconds are available in both the PS5 FreeBSD ABI and POSIX.1-2008. */
static inline void forever_managed_metadata(const struct stat *status, char text[160])
{
#ifdef __APPLE__
    long nanoseconds = status->st_mtimespec.tv_nsec;
#else
    long nanoseconds = status->st_mtim.tv_nsec;
#endif
    snprintf(text, 160, "%lld %lld %ld %llu %llu",
             (long long)status->st_size, (long long)status->st_mtime, nanoseconds,
             (unsigned long long)status->st_dev, (unsigned long long)status->st_ino);
}

/* Hash an already inspected no-follow regular descriptor, binding the result
 * to the same size/mtime/identity after the read. The caller always closes fd.
 * 1 match, 0 drift, -errno failure. */
static inline int forever_managed_matches(int fd, const char *expected,
                                         struct stat *status, const char **operation)
{
    static unsigned char block[1u << 16];
    struct sha256_context hash;
    struct stat after;
    unsigned char digest[32];
    char hex[65], original[160], current[160];
    const char *step = "hash-type";
    int error = 0;
    if (!S_ISREG(status->st_mode)) { *operation = step; return -EINVAL; }
    forever_managed_metadata(status, original);
    sha256_init(&hash);
    step = "read-destination";
    for (;;)
    {
        ssize_t count = read(fd, block, sizeof(block));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = -errno; goto done; }
        if (!count) break;
        sha256_update(&hash, block, (size_t)count);
    }
    step = "fstat-after-hash";
    if (fstat(fd, &after)) { error = -errno; goto done; }
    forever_managed_metadata(&after, current);
    step = "hash-identity";
    if (strcmp(original, current)) { error = -EAGAIN; goto done; }
    sha256_final(&hash, digest);
    forever_managed_hex(digest, hex);
    *status = after;
    error = !strcmp(hex, expected);
done:
    if (error < 0) *operation = step;
    return error;
}

/* Any unsupported, corrupt or truncated stamp is a cache miss for ALL rows.
 * The seal covers the complete row set, not just records that happen to parse. */
static inline int forever_managed_load(char *state, struct forever_managed_record previous[4096])
{
    const size_t header_size = sizeof(FOREVER_MANAGED_STATE_HEADER) - 1;
    struct sha256_context hash;
    unsigned char digest[32];
    char hex[65], *body = strchr(state, '\n');
    int records = 0;
    if (!body || strncmp(state, FOREVER_MANAGED_STATE_HEADER, header_size) ||
        (size_t)(body - state) != header_size + 64) return 0;
    *body++ = 0;
    sha256_init(&hash);
    sha256_update(&hash, body, strlen(body));
    sha256_final(&hash, digest);
    forever_managed_hex(digest, hex);
    if (strcmp(state + header_size, hex)) return 0;
    for (char *line = body, *next; *line; line = next)
    {
        char *tab1 = strchr(line, '\t'), *tab2, *end = strchr(line, '\n');
        unsigned slot, probes = 0;
        if (!end || !tab1 || tab1 > end || !(tab2 = strchr(tab1 + 1, '\t')) ||
            tab2 > end) return 0;
        next = end + 1;
        *end = *tab1 = *tab2 = 0;
        if (!*line || !forever_managed_digest_valid(tab1 + 1) || !tab2[1] ||
            strchr(tab2 + 1, '\t')) return 0;
        slot = forever_managed_hash(line);
        while (previous[slot].destination)
        {
            if (!strcmp(previous[slot].destination, line) || ++probes == 4096) return 0;
            slot = (slot + 1) & 4095;
        }
        previous[slot].destination = line;
        previous[slot].digest = tab1 + 1;
        previous[slot].metadata = tab2 + 1;
        ++records;
    }
    return records + 1;
}

/* Every manifest row is app-owned, including formerly native replacements.
 * A matching stat record skips data I/O; any other regular destination is
 * checked against the manifest SHA-256. Only a fully successful run atomically
 * replaces the versioned, sealed stamp. Return repaired count or -errno. */
static inline int forever_sync_managed(const char *app_root, const char *list_path,
                                      const char *prefix, struct forever_managed_report *report)
{
    static char list[1u << 19], installed[1u << 20], updated[1u << 20];
    static struct forever_managed_record previous[4096];
    const size_t header_size = sizeof(FOREVER_MANAGED_STATE_HEADER) - 1 + 64 + 1;
    char stamp[1100], source[1100], destination[1100], metadata[160], hex[65], header[128];
    size_t used = header_size;
    struct sha256_context hash;
    unsigned char digest[32];
    int error, changed = 0, loaded = 0;
    const char *operation;
    ssize_t length;

    memset(report, 0, sizeof(*report));
    if ((length = forever_read_file(list_path, list, sizeof(list))) < 0)
        return forever_managed_failure(report, "read-manifest", ".", (int)length);
    if (snprintf(stamp, sizeof(stamp), "%s/" FOREVER_MANAGED_STAMP, prefix) >= (int)sizeof(stamp))
        return forever_managed_failure(report, "stamp-path", ".", -ENAMETOOLONG);
    memset(previous, 0, sizeof(previous));
    length = forever_read_file(stamp, installed, sizeof(installed));
    report->state_read_result = (int)length;
    if (length < 0 || memchr(installed, 0, (size_t)length) ||
        !(loaded = forever_managed_load(installed, previous)))
    {
        memset(previous, 0, sizeof(previous));
        changed = 1;
    }
    if (loaded > 0) report->state_loaded = (unsigned)loaded - 1;
    for (char *line = list, *next; *line; line = next)
    {
        char *tab1, *tab2, *end = strchr(line, '\n');
        struct stat output;
        unsigned slot, probes = 0;
        int exists, fd, matches = 0, recorded, written;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        if (!*line || *line == '#') continue;
        if (!(tab1 = strchr(line, '\t')) || !(tab2 = strchr(tab1 + 1, '\t')))
            return forever_managed_failure(report, "parse-manifest", ".", -EINVAL);
        *tab1++ = *tab2++ = 0;
        if (!*line || !*tab1 || *line == '/' || *tab1 == '/' ||
            strstr(line, "..") || strstr(tab1, ".."))
            return forever_managed_failure(report, "validate-path", tab1, -EACCES);
        if (!forever_managed_digest_valid(tab2))
            return forever_managed_failure(report, "manifest-digest", tab1, -EINVAL);
        if (snprintf(source, sizeof(source), "%s/%s", app_root, line) >= (int)sizeof(source) ||
            snprintf(destination, sizeof(destination), "%s/%s", prefix, tab1) >= (int)sizeof(destination))
            return forever_managed_failure(report, "resolve-path", tab1, -ENAMETOOLONG);
        if (++report->checked > 4096)
            return forever_managed_failure(report, "manifest-limit", tab1, -EFBIG);
        slot = forever_managed_hash(tab1);
        while (previous[slot].destination && strcmp(previous[slot].destination, tab1))
        {
            if (++probes == 4096)
                return forever_managed_failure(report, "state-lookup", tab1, -EFBIG);
            slot = (slot + 1) & 4095;
        }
        recorded = previous[slot].destination && !strcmp(previous[slot].digest, tab2);
        if (!previous[slot].destination)
            forever_managed_mismatch(report, tab1, "record", "missing", 7, "present", 7, 0);
        else if (!recorded)
            forever_managed_mismatch(report, tab1, "digest", previous[slot].digest, 64, tab2, 64, 0);
        error = forever_managed_open(destination, &output, &fd, &exists, &operation);
        if (error) return forever_managed_failure(report, operation, tab1, error);
        if (fd >= 0)
        {
            forever_managed_metadata(&output, metadata);
            if (recorded && forever_managed_cache_matches(previous[slot].metadata, metadata, tab1, report))
                matches = 1;
            else
            {
                ++report->hashed;
                matches = forever_managed_matches(fd, tab2, &output, &operation);
                changed = 1;
            }
            if (close(fd) && matches >= 0) { operation = "close-destination"; matches = -errno; }
            if (matches < 0) return forever_managed_failure(report, operation, tab1, matches);
        }
        else if (recorded)
            forever_managed_mismatch(report, tab1, exists ? "type" : "existence",
                                     "regular", 7, exists ? "not-regular" : "missing",
                                     exists ? 11 : 7, 0);
        if (!matches)
        {
            char *slash = strrchr(destination, '/');
            *slash = 0;
            error = forever_mkdirs(destination);
            *slash = '/';
            if (error) return forever_managed_failure(report, "mkdir", tab1, error);
            if (!exists || !S_ISREG(output.st_mode)) ++report->hashed;
            /* rename replaces symlinks/FIFOs without opening them. Empty
             * directories need rmdir; never recursively delete user data. */
            if (exists && S_ISDIR(output.st_mode) && rmdir(destination))
            {
                return forever_managed_failure(report, "rmdir", tab1, -errno);
            }
            error = forever_copy_file(source, destination, &operation);
            if (forever_permission_error(error) && !strcmp(operation, "rename") &&
                exists && S_ISREG(output.st_mode))
                error = forever_managed_overwrite(source, destination, &output, &operation);
            if (error) return forever_managed_failure(report, operation, tab1, error);
            error = forever_managed_open(destination, &output, &fd, &exists, &operation);
            if (error) return forever_managed_failure(report, operation, tab1, error);
            if (fd < 0)
                return forever_managed_failure(report, "copy-type", tab1, -EINVAL);
            error = forever_managed_matches(fd, tab2, &output, &operation);
            if (close(fd) && error >= 0) { operation = "close-destination"; error = -errno; }
            if (error <= 0)
                return forever_managed_failure(report, error < 0 ? operation : "verify-copy",
                                               tab1, error < 0 ? error : -EIO);
            if (report->repaired < FOREVER_MANAGED_LOG_LIMIT)
                snprintf(report->paths[report->repaired], sizeof(report->paths[0]), "%s", tab1);
            ++report->repaired;
            changed = 1;
        }
        forever_managed_metadata(&output, metadata);
        written = snprintf(updated + used, sizeof(updated) - used, "%s\t%s\t%s\n", tab1, tab2, metadata);
        if (written < 0 || (size_t)written >= sizeof(updated) - used)
            return forever_managed_failure(report, "format-state", tab1, -EFBIG);
        used += (size_t)written;
    }
    if (changed || (unsigned)loaded != report->checked + 1)
    {
        sha256_init(&hash);
        sha256_update(&hash, updated + header_size, used - header_size);
        sha256_final(&hash, digest);
        forever_managed_hex(digest, hex);
        snprintf(header, sizeof(header), FOREVER_MANAGED_STATE_HEADER "%s\n", hex);
        memcpy(updated, header, header_size);
        report->state_publish_attempted = 1;
        report->state_publish_bytes = used;
        report->state_publish_result = forever_write_file(stamp, updated, used);
        if (report->state_publish_result)
            return forever_managed_failure(report, "publish-state", FOREVER_MANAGED_STAMP,
                                           report->state_publish_result);
    }
    return (int)report->repaired;
}

/* Each supported WoW channel gets the preset only when its executable exists
 * and its own Config.wtf is absent. Returns the number installed (0..2), or
 * -errno on failure; earlier installations may already have succeeded. */
static inline int forever_install_wow_config(const char *share, const char *prefix)
{
    static const struct
    {
        const char *directory;
        const char *executable;
    } channels[] = {
        {"_forever_", "Wow.exe"},
        {"_classic_beta_", "WowB.exe"}
    };
    char game[1100], executable[1100], wtf[1100], config[1100], source[1100];
    int length, error, installed = 0;
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i)
    {
        if ((length = snprintf(game, sizeof(game), "%s/drive_c/Program Files (x86)/World of Warcraft/%s",
                               prefix, channels[i].directory)) < 0 ||
            length >= (int)sizeof(game)) return -ENAMETOOLONG;
        if ((length = snprintf(executable, sizeof(executable), "%s/%s", game, channels[i].executable)) < 0 ||
            length >= (int)sizeof(executable)) return -ENAMETOOLONG;
        if (!forever_exists(executable)) continue;
        if ((length = snprintf(wtf, sizeof(wtf), "%s/WTF", game)) < 0 ||
            length >= (int)sizeof(wtf)) return -ENAMETOOLONG;
        if ((length = snprintf(config, sizeof(config), "%s/Config.wtf", wtf)) < 0 ||
            length >= (int)sizeof(config)) return -ENAMETOOLONG;
        {
            struct stat status;
            if (!stat(config, &status)) continue;
            if (errno != ENOENT) return -errno;
        }
        if ((length = snprintf(source, sizeof(source), "%s/Config.wtf", share)) < 0 ||
            length >= (int)sizeof(source)) return -ENAMETOOLONG;
        error = forever_mkdirs(wtf);
        if (!error) error = forever_copy_file(source, config, NULL);
        if (error) return error;
        ++installed;
    }
    return installed;
}

#endif
