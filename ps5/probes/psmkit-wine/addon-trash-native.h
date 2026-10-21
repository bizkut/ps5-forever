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
    int result;
};

static inline int addon_trash_path(char *out, size_t size, const char *left, const char *right)
{
    int length = snprintf(out, size, "%s/%s", left, right);
    return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

static inline int addon_trash_remove_tree(const char *path, const atomic_int *stop,
                                          struct addon_trash_result *result)
{
    struct stat st;
    DIR *directory;
    struct dirent *entry;
    char child[2048];
    if (atomic_load(stop)) return -ECANCELED;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -errno;
    if (S_ISLNK(st.st_mode)) return 0; /* never follow or remove authored links */
    if (!S_ISDIR(st.st_mode))
    {
        if (unlink(path)) return -errno;
        result->entries++;
        usleep(5000);
        return 0;
    }
    if (!(directory = opendir(path))) return -errno;
    while ((entry = readdir(directory)))
    {
        int error;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if ((error = addon_trash_path(child, sizeof(child), path, entry->d_name)) ||
            (error = addon_trash_remove_tree(child, stop, result)))
        { closedir(directory); return error; }
    }
    closedir(directory);
    if (atomic_load(stop)) return -ECANCELED;
    if (rmdir(path)) return -errno;
    result->entries++;
    usleep(5000);
    return 0;
}

static inline int addon_trash_scan(const char *prefix, const atomic_int *stop,
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
                lstat(trash, &st) || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
                lstat(complete, &st) || !S_ISREG(st.st_mode) || now - st.st_mtime < 5) continue;
            error = addon_trash_remove_tree(trash, stop, result);
            if (error && error != -ECANCELED && !result->result) result->result = error;
        }
        closedir(items);
    }
    closedir(channels);
    return result->result;
}

#endif
