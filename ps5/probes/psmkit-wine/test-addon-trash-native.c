#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "addon-trash-native.h"

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
int main(void)
{
    char root[] = "/tmp/addon-trash-native-XXXXXX", path[PATH_MAX], outside[PATH_MAX];
    atomic_int stop = 0, pause = 1; struct addon_trash_result result;
    assert(mkdtemp(root));
    snprintf(path, sizeof(path), "%s/drive_c/Program Files (x86)/World of Warcraft/_retail_/Interface/AddOns", root);
    mkdirs(path);
    char addons[PATH_MAX]; snprintf(addons, sizeof(addons), "%s", path);
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

    assert(addon_trash_scan(root, &stop, &pause, &result) == 0 && !result.entries);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-old", addons); assert(exists(path));
    atomic_store(&pause, 0);
    assert(addon_trash_scan(root, &stop, &pause, &result) < 0); /* skipped link keeps its directory non-empty */
    assert(result.entries == 7 && result.completed == 1);
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-old", addons); assert(!exists(path));
    snprintf(path, sizeof(path), "%s/Keep/Keep.toc", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/not-trash/file", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-incomplete/file", addons); assert(exists(path));
    snprintf(path, sizeof(path), "%s/.addonforge-tmp/trash-fresh/.complete", addons); assert(exists(path));
    assert(exists(outside));

    atomic_store(&stop, 1);
    assert(addon_trash_scan(root, &stop, &pause, &result) == 0 && result.entries == 0);
    char command[PATH_MAX + 16]; snprintf(command, sizeof(command), "rm -rf '%s'", root); system(command);
    puts("native addon trash: exact roots, age/complete gates, nested delete, symlink skip PASS");
    return 0;
}
