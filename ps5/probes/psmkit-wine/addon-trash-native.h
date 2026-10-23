#ifndef FOREVER_ADDON_TRASH_NATIVE_H
#define FOREVER_ADDON_TRASH_NATIVE_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct addon_trash_result
{
    uint64_t entries;
    uint64_t completed;
    int result;
};

static inline int addon_trash_path(char *out, size_t size, const char *left, const char *right)
{
    int length = snprintf(out, size, "%s/%s", left, right);
    return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

/* Title-safe metadata inspection: do not use sandbox-restricted lstat or follow
 * authored links. NONBLOCK also keeps special entries from blocking inspection. */
static inline int addon_trash_status(const char *path, struct stat *status)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK), error = 0;
    if (fd < 0) return -errno;
    if (fstat(fd, status)) error = -errno;
    if (close(fd) && !error) error = -errno;
    return error;
}

static inline int addon_trash_remove_tree(const char *path, const atomic_int *stop,
                                          const atomic_int *pause,
                                          struct addon_trash_result *result,
                                          const char *complete)
{
    struct stat st;
    DIR *directory;
    struct dirent *entry;
    char child[2048];
    int error = 0;
    if (atomic_load(stop)) return -ECANCELED;
    if (atomic_load(pause)) return -EAGAIN;
    error = addon_trash_status(path, &st);
    if (error == -ELOOP || error == -EMLINK) return -ENOTEMPTY; /* retain authored links and proof */
    if (error) return error == -ENOENT ? 0 : error;
    if (!S_ISDIR(st.st_mode))
    {
        if (unlink(path)) return -errno;
        result->entries++;
        usleep(5000);
        return 0;
    }
    if (!(directory = opendir(path))) return -errno;
    for (;;)
    {
        errno = 0;
        entry = readdir(directory);
        if (!entry) { error = errno ? -errno : 0; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        /* Nested .complete files belong to the add-on, not the trash transaction. */
        if (complete && !strcmp(entry->d_name, ".complete")) continue;
        if ((error = addon_trash_path(child, sizeof(child), path, entry->d_name)) ||
            (error = addon_trash_remove_tree(child, stop, pause, result, NULL))) break;
    }
    closedir(directory);
    if (error) return error;
    if (atomic_load(stop)) return -ECANCELED;
    if (atomic_load(pause)) return -EAGAIN;
    if (complete)
    {
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) return -errno;
        /* Persist removal of every payload child before discarding commit proof.
         * Core recover_tmp skips marked trash and safely removes an empty unmarked
         * terminal root; it must never see unmarked partially deleted payload. */
        if (fsync(fd)) error = -errno;
        if (!error && atomic_load(stop)) error = -ECANCELED;
        if (!error && atomic_load(pause)) error = -EAGAIN;
        if (!error && unlink(complete)) error = -errno;
        if (!error)
        {
            result->entries++;
            usleep(5000);
            if (fsync(fd)) error = -errno;
        }
        if (close(fd) && !error) error = -errno;
        if (error) return error;
        if (atomic_load(stop)) return -ECANCELED;
        if (atomic_load(pause)) return -EAGAIN;
    }
    if (rmdir(path)) return -errno;
    result->entries++;
    usleep(5000);
    return 0;
}

static inline int addon_trash_scan(const char *prefix, const atomic_int *stop,
                                   const atomic_int *pause,
                                   struct addon_trash_result *result)
{
    char world[2048], addons[2048], temp[2048], trash[2048], complete[2048];
    DIR *channels, *items;
    struct dirent *channel, *item;
    struct stat st;
    time_t now = time(NULL);
    memset(result, 0, sizeof(*result));
    if (snprintf(world, sizeof(world), "%s/drive_c/Program Files (x86)/World of Warcraft", prefix) >= (int)sizeof(world))
        return result->result = -ENAMETOOLONG;
    if (!(channels = opendir(world))) return errno == ENOENT ? 0 : (result->result = -errno);
    if (atomic_load(pause)) { closedir(channels); return 0; }
    while (!atomic_load(stop) && (channel = readdir(channels)))
    {
        if (channel->d_name[0] != '_' || strchr(channel->d_name, '/')) continue;
        if (snprintf(addons, sizeof(addons), "%s/%s/Interface/AddOns", world, channel->d_name) >= (int)sizeof(addons) ||
            addon_trash_path(temp, sizeof(temp), addons, ".addonforge-tmp")) continue;
        if (!(items = opendir(temp))) continue;
        while (!atomic_load(stop) && (item = readdir(items)))
        {
            int error;
            if (strncmp(item->d_name, "trash-", 6) || strchr(item->d_name, '/')) continue;
            if (addon_trash_path(trash, sizeof(trash), temp, item->d_name) ||
                addon_trash_path(complete, sizeof(complete), trash, ".complete") ||
                addon_trash_status(trash, &st) || !S_ISDIR(st.st_mode)) continue;
            error = addon_trash_status(complete, &st);
            if (error)
            {
                if (error != -ENOENT) continue;
                if (atomic_load(stop)) break;
                if (atomic_load(pause)) { closedir(items); closedir(channels); return 0; }
                /* Only an empty terminal root may be removed without commit proof.
                 * A nonempty interrupted rename remains exclusively core recovery's. */
                if (rmdir(trash)) continue;
                result->entries++;
                result->completed++;
                usleep(5000);
                continue;
            }
            if (!S_ISREG(st.st_mode) || now - st.st_mtime < 5) continue;
            error = addon_trash_remove_tree(trash, stop, pause, result, complete);
            if (error == -EAGAIN) { closedir(items); closedir(channels); return 0; }
            if (!error) result->completed++;
            if (error && error != -ECANCELED && !result->result) result->result = error;
        }
        closedir(items);
    }
    closedir(channels);
    return result->result;
}

#endif
