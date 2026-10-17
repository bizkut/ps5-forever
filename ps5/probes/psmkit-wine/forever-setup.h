/* First-start and every-start setup of the user data in /data/forever.
 *
 * The app folder is replaced by every store update, so everything the user
 * owns (the Wine prefix with Battle.net and the games, HOME, the shader cache)
 * lives in FOREVER_ROOT. Each function takes its paths as arguments so the
 * host tests can run it against a temporary directory. Only calls the native
 * title already imports are used (open/read/write/close/stat/mkdir/rename/
 * unlink). */
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
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return -errno;
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

/* Copy source to destination through a temporary name. 0 or -errno. */
static inline int forever_copy_file(const char *source, const char *destination)
{
    static unsigned char block[1u << 16];
    char temporary[1100];
    int in, out, error = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.forever-tmp", destination) >= (int)sizeof(temporary))
        return -ENAMETOOLONG;
    if ((in = open(source, O_RDONLY)) < 0) return -errno;
    if ((out = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) { error = -errno; close(in); return error; }
    for (;;)
    {
        ssize_t count = read(in, block, sizeof(block)), done = 0;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = -errno; break; }
        if (!count) break;
        while (done < count)
        {
            ssize_t written = write(out, block + done, (size_t)(count - done));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = written < 0 ? -errno : -EIO; break; }
            done += written;
        }
        if (error) break;
    }
    close(in);
    if (close(out) && !error) error = -errno;
    if (!error && rename(temporary, destination)) error = -errno;
    if (error) unlink(temporary);
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

/* Use the same ownership signatures as setupapi's is_fake_dll(): native
 * replacements installed by the user are not Wine prefix-update targets. */
static inline int forever_is_wine_pe(const char *path)
{
    unsigned char header[96];
    size_t used = 0;
    int fd = open(path, O_RDONLY), error = 0;
    if (fd < 0) return -errno;
    while (used < sizeof(header))
    {
        ssize_t count = read(fd, header + used, sizeof(header) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = -errno; break; }
        if (!count) break;
        used += (size_t)count;
    }
    close(fd);
    if (error) return error;
    return used == sizeof(header) && header[0] == 'M' && header[1] == 'Z' &&
        (!memcmp(header + 64, "Wine builtin DLL", sizeof("Wine builtin DLL")) ||
         !memcmp(header + 64, "Wine placeholder DLL", sizeof("Wine placeholder DLL")));
}

static inline unsigned forever_managed_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    while (*name) hash = (hash ^ (unsigned char)*name++) * 16777619u;
    return hash & 4095;
}

/* Keep every shipped Wine PE and explicit native graphics override current.
 * Diff by destination/digest against the previous manifest, checking sizes
 * and missing files even when the manifest is unchanged. Publish its stamp
 * only after all required atomic file copies succeed. */
static inline int forever_sync_managed(const char *app_root, const char *list_path, const char *prefix)
{
    static char list[1u << 19], installed[1u << 19];
    static struct { const char *destination, *digest; } previous[4096];
    char stamp[1100], source[1100], destination[1100];
    ssize_t length;
    int copied = 0, error, manifest_changed;

    if ((length = forever_read_file(list_path, list, sizeof(list))) < 0) return (int)length;
    snprintf(stamp, sizeof(stamp), "%s/" FOREVER_MANAGED_STAMP, prefix);
    if (forever_read_file(stamp, installed, sizeof(installed)) < 0) installed[0] = 0;
    manifest_changed = strcmp(list, installed);
    memset(previous, 0, sizeof(previous));
    for (char *line = installed, *next; *line; line = next)
    {
        char *tab1, *tab2, *end = strchr(line, '\n');
        unsigned slot, probes = 0;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        if (!*line || *line == '#' || !(tab1 = strchr(line, '\t')) ||
            !(tab2 = strchr(tab1 + 1, '\t'))) continue;
        *tab2 = 0;
        slot = forever_managed_hash(tab1 + 1);
        while (previous[slot].destination && strcmp(previous[slot].destination, tab1 + 1))
        {
            if (++probes == 4096) return -EFBIG;
            slot = (slot + 1) & 4095;
        }
        previous[slot].destination = tab1 + 1;
        previous[slot].digest = tab2 + 1;
    }
    for (char *line = list, *next; *line; line = next)
    {
        char *tab1, *tab2, *end = strchr(line, '\n');
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        if (!*line || *line == '#') goto restore;
        if (!(tab1 = strchr(line, '\t')) || !(tab2 = strchr(tab1 + 1, '\t'))) { error = -EINVAL; goto fail; }
        *tab1 = *tab2 = 0;
        if (strstr(line, "..") || strstr(tab1 + 1, "..")) { error = -EACCES; goto fail; }
        snprintf(source, sizeof(source), "%s/%s", app_root, line);
        snprintf(destination, sizeof(destination), "%s/%s", prefix, tab1 + 1);
        {
            struct stat input, output;
            unsigned slot = forever_managed_hash(tab1 + 1), probes = 0;
            int exists;
            if (stat(source, &input)) { error = -errno; goto fail; }
            exists = !stat(destination, &output);
            if (!exists && errno != ENOENT) { error = -errno; goto fail; }
            while (previous[slot].destination && strcmp(previous[slot].destination, tab1 + 1))
            {
                if (++probes == 4096) { error = -EFBIG; goto fail; }
                slot = (slot + 1) & 4095;
            }
            if (exists && previous[slot].destination &&
                !strcmp(previous[slot].digest, tab2 + 1) && input.st_size == output.st_size)
            {
                *tab1 = *tab2 = '\t';
                goto restore;
            }
            if (exists && !strncmp(line, "lib/wine/", 9))
            {
                int builtin = forever_is_wine_pe(destination);
                if (builtin < 0) { error = builtin; goto fail; }
                if (!builtin)
                {
                    *tab1 = *tab2 = '\t';
                    goto restore;
                }
            }
        }
        {
            char *slash = strrchr(destination, '/');
            *slash = 0;
            error = forever_mkdirs(destination);
            *slash = '/';
            if (error) goto fail;
        }
        if ((error = forever_copy_file(source, destination))) goto fail;
        ++copied;
        *tab1 = *tab2 = '\t';
    restore:
        if (end) *end = '\n';
    }
    if (manifest_changed && (error = forever_write_file(stamp, list, strlen(list)))) return error;
    return copied;
fail:
    return error;
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
        if (!error) error = forever_copy_file(source, config);
        if (error) return error;
        ++installed;
    }
    return installed;
}

#endif
