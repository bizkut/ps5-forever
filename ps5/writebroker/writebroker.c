/* writebroker: performs a PS5 title's file writes from a payload process,
 * which the console's SSD write throttling does not limit (see ps5-wine
 * include/wine/ps5_writebroker.h for why and for the protocol).
 *
 * The title sends it through the ELF loader at start when no broker of its
 * protocol version (WB_VERSION) answers. It writes a fresh token to
 * WB_TOKEN_FILE, listens on 127.0.0.1:WB_PORT, and serves
 * each connection on its own thread. A copy already holding the port is
 * asked to exit and replaced. Logs go to the console log with the prefix
 * "writebroker: ". */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <sys/time.h>
#include <unistd.h>
#include <ps5/klog.h>
#include "wine/ps5_writebroker.h"
#include "../probes/psmkit-wine/writebroker-menu.h"

#ifndef WB_MENU_PREFIX_ROOT
#define WB_MENU_PREFIX_ROOT WB_USER_ROOT "prefix"
#endif
#define WB_MENU_REMOVE_DEPTH 64u

#define WB_CHUNK (1u << 20)

#ifndef WB_REPAIR_DATA_ROOT
#define WB_REPAIR_DATA_ROOT "/data"
#endif
#ifndef WB_REPAIR_MAX_ENTRIES
#define WB_REPAIR_MAX_ENTRIES 100000u
#endif
#ifndef WB_REPAIR_MAX_DEPTH
#define WB_REPAIR_MAX_DEPTH 64u
#endif
#ifndef WB_REPAIR_FSTATAT
#define WB_REPAIR_FSTATAT fstatat
#endif
#ifndef WB_REPAIR_READDIR
#define WB_REPAIR_READDIR readdir
#endif
#ifndef WB_REPAIR_CLOSEDIR
#define WB_REPAIR_CLOSEDIR closedir
#endif

static unsigned char token[WB_TOKEN_BYTES];
static unsigned long long served_writes, served_bytes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    klog_printf("writebroker: %s\n", line);
}

/* all of size bytes, or -1 with errno (EPIPE at a closed connection) */
static int transfer(int fd, void *buffer, size_t size, int sending)
{
    unsigned char *bytes = buffer;
    size_t done = 0;

    while (done < size)
    {
        ssize_t count = sending ? send(fd, bytes + done, size - done, MSG_NOSIGNAL)
                                : recv(fd, bytes + done, size - done, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EPIPE; return -1; }
        done += (size_t)count;
    }
    return 0;
}

/* receive and drop length bytes, keeping the stream in step after a refusal */
static int discard(int client, unsigned char *buffer, uint32_t length)
{
    while (length)
    {
        uint32_t chunk = length < WB_CHUNK ? length : WB_CHUNK;
        if (transfer(client, buffer, chunk, 0)) return -1;
        length -= chunk;
    }
    return 0;
}

static int menu_remove_index(const char *path)
{
    for (unsigned i = 0; wb_menu_remove_relative(i, 1); i++)
    {
        char allowed[WB_MAX_PATH + 1];
        int length = snprintf(allowed, sizeof(allowed), WB_USER_ROOT "prefix%s",
                              wb_menu_remove_relative(i, 1));
        if (length > 0 && length < (int)sizeof(allowed) && !strcmp(path, allowed)) return (int)i;
    }
    return -1;
}

static int menu_remove_path_allowed(const char *path)
{
    return menu_remove_index(path) >= 0;
}

static int menu_open_child_directory(int parent, const char *name)
{
    int child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return child < 0 ? -errno : child;
}

/* Open every component from the filesystem root, never following a link.
 * The returned descriptor anchors all later relative traversal. */
static int menu_open_prefix_root(void)
{
    char component[WB_MAX_PATH + 1];
    const char *at = WB_MENU_PREFIX_ROOT;
    int current, next;

    if (!at || *at != '/') return -EINVAL;
    if ((current = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) return -errno;
    while (*at)
    {
        const char *end;
        size_t length;
        while (*at == '/') at++;
        if (!*at) break;
        end = strchr(at, '/');
        length = end ? (size_t)(end - at) : strlen(at);
        if (!length || length > WB_MAX_PATH ||
            (length == 1 && at[0] == '.') ||
            (length == 2 && at[0] == '.' && at[1] == '.'))
        {
            close(current);
            return -EINVAL;
        }
        memcpy(component, at, length);
        component[length] = 0;
        next = menu_open_child_directory(current, component);
        close(current);
        if (next < 0) return next;
        current = next;
        at += length;
    }
    return current;
}

/* Open every fixed relative ancestor, leaving the last component in leaf. */
static int menu_open_remove_parent(int root, const char *relative, char leaf[WB_MAX_PATH + 1])
{
    char path[WB_MAX_PATH + 1], *at, *slash;
    int current = root, next;
    size_t length = strlen(relative);

    if (!length || length > WB_MAX_PATH || relative[0] != '/') { close(root); return -EINVAL; }
    memcpy(path, relative + 1, length); /* includes the terminating NUL */
    at = path;
    while ((slash = strchr(at, '/')))
    {
        *slash = 0;
        if (!*at || !strcmp(at, ".") || !strcmp(at, "..")) { close(current); return -EINVAL; }
        next = menu_open_child_directory(current, at);
        close(current);
        if (next < 0) return next;
        current = next;
        at = slash + 1;
    }
    if (!*at || !strcmp(at, ".") || !strcmp(at, "..")) { close(current); return -EINVAL; }
    strcpy(leaf, at);
    return current;
}

static int menu_remove_at(int parent, const char *name, unsigned depth)
{
    struct stat before, opened;
    DIR *directory;
    struct dirent *entry;
    int child = -1, scan = -1, result = 0;

    if (depth > WB_MENU_REMOVE_DEPTH) return -ELOOP;
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(before.st_mode)) return unlinkat(parent, name, 0) ? -errno : 0;
    child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) return -errno;
    if (fstat(child, &opened))
    {
        result = -errno;
        goto done;
    }
    if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino)
    {
        result = -ESTALE;
        goto done;
    }
    if ((scan = dup(child)) < 0) { result = -errno; goto done; }
    if (!(directory = fdopendir(scan))) { result = -errno; goto done; }
    scan = -1; /* owned by directory */
    errno = 0;
    while (!result && (entry = readdir(directory)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strchr(entry->d_name, '/')) { result = -EINVAL; break; }
        result = menu_remove_at(child, entry->d_name, depth + 1);
        errno = 0;
    }
    if (!result && errno) result = -errno;
    if (closedir(directory) && !result) result = -errno;
done:
    if (scan >= 0) close(scan);
    if (child >= 0) close(child);
    if (!result && unlinkat(parent, name, AT_REMOVEDIR)) result = -errno;
    return result;
}

static int menu_remove_tree(const char *path)
{
    char leaf[WB_MAX_PATH + 1];
    int index = menu_remove_index(path), root, parent, result;
    const char *relative;

    if (index < 0) return -EACCES;
    relative = wb_menu_remove_relative((unsigned)index, 1);
    if ((root = menu_open_prefix_root()) < 0) return root == -ENOENT ? 0 : root;
    parent = menu_open_remove_parent(root, relative, leaf);
    if (parent < 0) return parent == -ENOENT ? 0 : parent;
    result = menu_remove_at(parent, leaf, 0);
    close(parent);
    return result;
}

static int serve_menu_remove(int client, const struct wb_write *request)
{
    char path[WB_MAX_PATH + 1];
    struct wb_reply reply = {.magic = WB_MAGIC};

    if (!request->path_length || request->path_length > WB_MAX_PATH ||
        request->offset || request->length || request->reserved) return -1;
    if (transfer(client, path, request->path_length, 0)) return -1;
    path[request->path_length] = 0;
    if (memchr(path, 0, request->path_length)) return -1;
    reply.result = menu_remove_path_allowed(path) ? menu_remove_tree(path) : -EACCES;
    if (reply.result) say("menu removal refused/failed %d: %s", reply.result, path);
    else say("menu removed: %s", path);
    return transfer(client, &reply, sizeof(reply), 1);
}

struct wb_permission_repair
{
    unsigned scanned;
    unsigned changed;
    unsigned skipped;
    unsigned failed;
    int result;
    dev_t device;
};

static int repair_entry_at(
    int parent, const char *name, unsigned depth, struct wb_permission_repair *repair);
static int repair_scan_directory(
    int directory_fd, unsigned depth, struct wb_permission_repair *repair);
static int write_token(void);

static int repair_open_data_root(dev_t *device)
{
    struct stat status;
    int fd = open(WB_REPAIR_DATA_ROOT,
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

    if (fd < 0) return -errno;
    if (fstat(fd, &status))
    {
        int error = -errno;
        close(fd);
        return error;
    }
    if (!S_ISDIR(status.st_mode))
    {
        close(fd);
        return -ENOTDIR;
    }
    *device = status.st_dev;
    return fd;
}


/* Resolve a fixed path relative to the already-open /data descriptor. Every
 * component is a real directory on /data's own filesystem. */
static int repair_open_relative_directory(int data_fd, const char *relative, dev_t device)
{
    char component[WB_MAX_PATH + 1];
    const char *at = relative;
    int current, next;

    if (!at || !*at || *at == '/') return -EINVAL;
    if ((current = dup(data_fd)) < 0) return -errno;
    while (*at)
    {
        const char *end;
        struct stat status;
        size_t length;

        while (*at == '/') at++;
        if (!*at) break;
        end = strchr(at, '/');
        length = end ? (size_t)(end - at) : strlen(at);
        if (!length || length > WB_MAX_PATH ||
            (length == 1 && at[0] == '.') ||
            (length == 2 && at[0] == '.' && at[1] == '.'))
        {
            close(current);
            return -EINVAL;
        }
        memcpy(component, at, length);
        component[length] = 0;
        next = openat(current, component,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
        {
            int error = -errno;
            close(current);
            return error;
        }
        close(current);
        current = next;
        if (fstat(current, &status))
        {
            int error = -errno;
            close(current);
            return error;
        }
        if (!S_ISDIR(status.st_mode) || status.st_dev != device)
        {
            close(current);
            return status.st_dev == device ? -ENOTDIR : -EXDEV;
        }
        at += length;
    }
    return current;
}

static int repair_apply_mode(
    int fd, const struct stat *status, struct wb_permission_repair *repair)
{
    mode_t wanted = S_ISDIR(status->st_mode) || (status->st_mode & 0111) ? 0777 : 0666;

    if ((status->st_mode & 07777) == wanted) return 0;
    if (!fchmod(fd, wanted))
    {
        repair->changed++;
        return 0;
    }
    repair->failed++;
    if (!repair->result) repair->result = -errno;
    return -errno;
}

static int repair_entry_at(
    int parent, const char *name, unsigned depth, struct wb_permission_repair *repair)
{
    struct stat before, opened;
    mode_t wanted;
    int fd, flags, result = 0;

    if (repair->scanned >= WB_REPAIR_MAX_ENTRIES)
    {
        repair->failed++;
        if (!repair->result) repair->result = -E2BIG;
        return -E2BIG;
    }
    repair->scanned++;
    if (depth > WB_REPAIR_MAX_DEPTH)
    {
        repair->failed++;
        if (!repair->result) repair->result = -E2BIG;
        return -E2BIG;
    }
    if (WB_REPAIR_FSTATAT(parent, name, &before, AT_SYMLINK_NOFOLLOW))
    {
        result = -errno;
        goto failed;
    }
    if (before.st_dev != repair->device)
    {
        repair->skipped++;
        return 0;
    }
    if (S_ISLNK(before.st_mode) ||
        (!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode)))
    {
        repair->skipped++;
        return 0;
    }
    if (S_ISREG(before.st_mode) && before.st_nlink > 1)
    {
        repair->skipped++;
        return 0;
    }
    wanted = S_ISDIR(before.st_mode) || (before.st_mode & 0111) ? 0777 : 0666;
    if (S_ISREG(before.st_mode) && (before.st_mode & 07777) == wanted)
        return 0;

    flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    if (S_ISDIR(before.st_mode)) flags |= O_DIRECTORY;
    if ((fd = openat(parent, name, flags)) < 0)
    {
        result = -errno;
        goto failed;
    }
    if (fstat(fd, &opened))
    {
        result = -errno;
        close(fd);
        goto failed;
    }
    if (opened.st_dev != repair->device)
    {
        repair->skipped++;
        close(fd);
        return 0;
    }
    if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        (opened.st_mode & S_IFMT) != (before.st_mode & S_IFMT))
    {
        result = -ESTALE;
        close(fd);
        goto failed;
    }
    if (S_ISREG(opened.st_mode) && opened.st_nlink > 1)
    {
        repair->skipped++;
        close(fd);
        return 0;
    }
    result = repair_apply_mode(fd, &opened, repair);
    if (S_ISDIR(opened.st_mode))
    {
        int scan_result = repair_scan_directory(fd, depth, repair);
        if (!result) result = scan_result;
    }
    close(fd);
    return result;

failed:
    repair->failed++;
    if (!repair->result) repair->result = result;
    return result;
}

static int repair_scan_directory(
    int directory_fd, unsigned depth, struct wb_permission_repair *repair)
{
    DIR *directory;
    struct dirent *entry;
    int scan, result = 0;

    if ((scan = dup(directory_fd)) < 0)
    {
        result = -errno;
        goto failed;
    }
    if (!(directory = fdopendir(scan)))
    {
        result = -errno;
        close(scan);
        goto failed;
    }
    for (;;)
    {
        int child, read_error;
        errno = 0;
        entry = WB_REPAIR_READDIR(directory);
        read_error = errno;
        if (!entry)
        {
            if (read_error) result = -read_error;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strchr(entry->d_name, '/'))
        {
            result = -EINVAL;
            break;
        }
        child = repair_entry_at(directory_fd, entry->d_name, depth + 1, repair);
        if (child == -E2BIG)
        {
            result = child;
            break;
        }
    }
    if (WB_REPAIR_CLOSEDIR(directory) && !result) result = -errno;
    if (result && result != -E2BIG)
    {
        repair->failed++;
        if (!repair->result) repair->result = result;
    }
    return result;

failed:
    repair->failed++;
    if (!repair->result) repair->result = result;
    return result;
}

static int repair_root(
    int data_fd, const char *relative, int recursive,
    struct wb_permission_repair *repair)
{
    struct stat status;
    int fd, result = 0;

    if (repair->scanned >= WB_REPAIR_MAX_ENTRIES)
    {
        repair->failed++;
        if (!repair->result) repair->result = -E2BIG;
        return -E2BIG;
    }
    repair->scanned++;
    fd = repair_open_relative_directory(data_fd, relative, repair->device);
    if (fd == -ENOENT)
    {
        repair->skipped++;
        return 0;
    }
    if (fd == -EXDEV)
    {
        repair->skipped++;
        return 0;
    }
    if (fd < 0)
    {
        result = fd;
        goto failed;
    }
    if (fstat(fd, &status))
    {
        result = -errno;
        close(fd);
        goto failed;
    }
    if (status.st_dev != repair->device)
    {
        repair->skipped++;
        close(fd);
        return 0;
    }
    result = repair_apply_mode(fd, &status, repair);
    if (recursive)
    {
        int scan_result = repair_scan_directory(fd, 0, repair);
        if (!result) result = scan_result;
    }
    close(fd);
    return result;

failed:
    repair->failed++;
    if (!repair->result) repair->result = result;
    return result;
}

static int repair_all(struct wb_permission_repair *repair)
{
    static const struct
    {
        const char *relative;
        int recursive;
    } roots[] = {
        {"forever", 0},
        {"forever/prefix", 1},
        {"forever/home", 1},
        {"forever/radv-shader-cache", 1},
        {"homebrew/PPSA99030", 0},
        {"homebrew/PPSA99030/wine", 1},
        {"prosperostore/previous/PPSA99030", 0},
        {"prosperostore/previous/PPSA99030/wine", 1},
    };
    int data_fd = repair_open_data_root(&repair->device);

    if (data_fd < 0)
    {
        repair->failed++;
        repair->result = data_fd;
        return data_fd;
    }
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
    {
        int result = repair_root(
            data_fd, roots[i].relative, roots[i].recursive, repair);
        if (result == -E2BIG) break;
    }
    close(data_fd);
    return repair->result;
}

static int repair_permissions(void)
{
    struct wb_permission_repair repair = {0};
    struct timeval before, after;
    long long elapsed_ms;
    int result;

    gettimeofday(&before, NULL);
    result = repair_all(&repair);
    gettimeofday(&after, NULL);
    elapsed_ms = (after.tv_sec - before.tv_sec) * 1000LL +
                 (after.tv_usec - before.tv_usec) / 1000;
    say("permission repair scanned=%u changed=%u skipped=%u failed=%u ms=%lld "
        "uid=%u:%u effective=%u:%u result=%d",
        repair.scanned, repair.changed, repair.skipped, repair.failed, elapsed_ms,
        (unsigned)getuid(), (unsigned)getgid(),
        (unsigned)geteuid(), (unsigned)getegid(), result);
    return result;
}

#ifndef WB_STARTUP_REPAIR
#define WB_STARTUP_REPAIR repair_permissions
#endif
#ifndef WB_STARTUP_WRITE_TOKEN
#define WB_STARTUP_WRITE_TOKEN write_token
#endif

static int broker_prepare_startup(void)
{
    WB_STARTUP_REPAIR();
    return WB_STARTUP_WRITE_TOKEN();
}

static int serve_write(int client, const struct wb_write *request, unsigned char *buffer)
{
    char path[WB_MAX_PATH + 1];
    struct wb_reply reply = {.magic = WB_MAGIC};
    uint32_t remaining = request->length;
    uint64_t offset = request->offset;
    struct stat st;
    int fd = -1, result;

    if (request->path_length == 0 || request->path_length > WB_MAX_PATH || request->length > WB_MAX_WRITE)
        return -1;  /* malformed: drop the connection */
    if (transfer(client, path, request->path_length, 0)) return -1;
    path[request->path_length] = 0;
    if (memchr(path, 0, request->path_length)) return -1;

    /* the user data root is the same path for the title and this payload */
    result = wb_path_root(path) ? 0 : -EACCES;
    if (!result && (fd = open(path, O_WRONLY)) < 0) result = -errno;
    if (!result && (fstat(fd, &st) || !S_ISREG(st.st_mode))) result = -EINVAL;

    while (remaining)
    {
        uint32_t chunk = remaining < WB_CHUNK ? remaining : WB_CHUNK, put = 0;

        if (transfer(client, buffer, chunk, 0)) { if (fd >= 0) close(fd); return -1; }
        while (!result && put < chunk)
        {
            ssize_t count = pwrite(fd, buffer + put, chunk - put, (off_t)offset + put);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { result = count ? -errno : -EIO; break; }
            put += (uint32_t)count;
        }
        offset += chunk;
        remaining -= chunk;
    }
    if (fd >= 0) close(fd);
    if (result && remaining) discard(client, buffer, remaining);
    reply.result = result ? result : (int32_t)request->length;
    if (result) say("write refused %d: %s", result, path);
    else
    {
        unsigned long long writes, bytes;
        pthread_mutex_lock(&stats_lock);
        writes = ++served_writes;
        bytes = served_bytes += request->length;
        pthread_mutex_unlock(&stats_lock);
        if (!(writes % 4096)) say("served %llu writes, %llu MiB", writes, bytes >> 20);
    }
    return transfer(client, &reply, sizeof(reply), 1);
}

static int serve_statfs(int client, const struct wb_write *request)
{
    char path[WB_MAX_PATH + 1];
    struct wb_statfs_reply reply = {.magic = WB_MAGIC};
    struct statfs info;

    if (!request->path_length || request->path_length > WB_MAX_PATH ||
        request->offset || request->length || request->reserved)
        return -1;
    if (transfer(client, path, request->path_length, 0)) return -1;
    path[request->path_length] = 0;
    if (memchr(path, 0, request->path_length)) return -1;
    if (!wb_path_root(path)) reply.result = -EACCES;
    else if (statfs(path, &info)) reply.result = -errno;
    else
    {
        reply.bsize = info.f_bsize;
        reply.blocks = info.f_blocks;
        reply.bfree = info.f_bfree;
        reply.bavail = info.f_bavail > 0 ? (uint64_t)info.f_bavail : 0;
    }
    return transfer(client, &reply, sizeof(reply), 1);
}

/* A regular file of at most limit bytes into buffer: its size, or -errno */
static ssize_t read_whole(const char *path, unsigned char *buffer, size_t limit)
{
    struct stat st;
    ssize_t done = 0, got = 0;
    int fd = open(path, O_RDONLY), error = 0;

    if (fd < 0) return -errno;
    if (fstat(fd, &st)) error = errno;
    else if (!S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > limit) error = EFBIG;
    while (!error && done < st.st_size && (got = read(fd, buffer + done, (size_t)(st.st_size - done))) > 0)
        done += got;
    if (!error && done != st.st_size) error = got < 0 ? errno : EIO;
    close(fd);
    return error ? -error : done;
}

/* Copy WB_ICON_FILE over each of the home screen's copies of the title's
 * icon that exists and differs (ShadowMountPlus made them when it
 * registered the title; the shell shows a new one after a restart).
 * buffer and other hold WB_CHUNK bytes. How many were replaced, or -errno. */
static int install_icon(unsigned char *buffer, unsigned char *other)
{
    static const char *const copies[] = {
        "/user/app/" WB_TITLE_ID "/icon0.png",
        "/user/app/" WB_TITLE_ID "/sce_sys/icon0.png",
        "/user/appmeta/" WB_TITLE_ID "/icon0.png",
    };
    static const unsigned char png[16] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    static const unsigned char size512[8] = {0, 0, 2, 0, 0, 0, 2, 0};
    char temporary[128];
    ssize_t size = read_whole(WB_ICON_FILE, buffer, WB_CHUNK), existing;
    int replaced = 0, present = 0;

    if (size < 0) return (int)size;
    if (size < 24 || memcmp(buffer, png, sizeof(png)) || memcmp(buffer + 16, size512, sizeof(size512)))
        return -EINVAL;
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]); i++)
    {
        int fd, ok;

        if ((existing = read_whole(copies[i], other, WB_CHUNK)) == -ENOENT) continue;
        ++present;
        if (existing == size && !memcmp(buffer, other, (size_t)size)) continue;
        snprintf(temporary, sizeof(temporary), "%s.new", copies[i]);
        if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) return -errno;
        ok = write(fd, buffer, (size_t)size) == size && !fchmod(fd, 0666);
        ok = !close(fd) && ok;
        if (!ok || rename(temporary, copies[i]))
        {
            int error = errno ? errno : EIO;
            unlink(temporary);
            return -error;
        }
        ++replaced;
    }
    say("home-screen icon: %d of %d copies replaced", replaced, present);
    return replaced;
}

static void *serve(void *arg)
{
    int client = (int)(intptr_t)arg;
    struct wb_hello hello;
    struct wb_reply reply = {.magic = WB_MAGIC};
    struct wb_write request;
    unsigned char *buffer = malloc(WB_CHUNK);

    if (!buffer || transfer(client, &hello, sizeof(hello), 0) || hello.magic != WB_MAGIC ||
        !hello.version || memcmp(hello.token, token, sizeof(token)))
    {
        say("connection refused (hello)");
        goto done;
    }
    /* another version may only ask this broker to exit */
    if (hello.version != WB_VERSION) reply.result = -EPROTONOSUPPORT;
    if (transfer(client, &reply, sizeof(reply), 1)) goto done;
    while (!transfer(client, &request, sizeof(request), 0))
    {
        if (request.magic == WB_QUIT_MAGIC)
        {
            /* a newer broker holding the same token takes over the port */
            say("replaced by a new broker; exiting");
            exit(0);
        }
        if (!reply.result && request.magic == WB_MENU_CAPS_MAGIC)
        {
            struct wb_reply answer = {.magic = WB_MAGIC, .result = WB_MENU_CAPS_REMOVE};
            if (request.path_length || request.offset || request.length || request.reserved) answer.result = -EINVAL;
            if (transfer(client, &answer, sizeof(answer), 1)) break;
            continue;
        }
        if (!reply.result && request.magic == WB_MENU_REMOVE_MAGIC)
        {
            if (serve_menu_remove(client, &request)) break;
            continue;
        }
        if (!reply.result && request.magic == WB_ICON_MAGIC)
        {
            struct wb_reply answer = {.magic = WB_MAGIC, .result = -ENOMEM};
            unsigned char *other = malloc(WB_CHUNK);

            if (request.path_length || request.length) answer.result = -EINVAL;
            else if (other) answer.result = install_icon(buffer, other);
            free(other);
            if (transfer(client, &answer, sizeof(answer), 1)) break;
            continue;
        }
        if (!reply.result && request.magic == WB_STATFS_MAGIC)
        {
            if (serve_statfs(client, &request)) break;
            continue;
        }
        if (reply.result || request.magic != WB_MAGIC || serve_write(client, &request, buffer)) break;
    }
done:
    free(buffer);
    close(client);
    return NULL;
}

/* Known limitation retained from the original protocol: the token is 0644 in
 * the normally 0777 user-data directory. It authenticates cooperative title
 * clients but is not secret from other local identities that can traverse
 * /data/forever; changing the credential scheme is outside this repair. */
static int write_token(void)
{
    static const char name[] = WB_TOKEN_FILE, temporary[] = WB_TOKEN_FILE ".new";
    int fd = open("/dev/urandom", O_RDONLY), ok;

    ok = fd >= 0 && read(fd, token, sizeof(token)) == (ssize_t)sizeof(token);
    if (fd >= 0) close(fd);
    if (!ok) return -1;
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
    ok = write(fd, token, sizeof(token)) == (ssize_t)sizeof(token);
    fchmod(fd, 0644);
    close(fd);
    return ok && !rename(temporary, name) ? 0 : -1;
}

/* ask a running broker to exit: it trusts whoever can read its token. An
 * older one answers only its own version's hello (version 1 closes the
 * connection on any other), so step down until one answers. */
static void replace_running(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(WB_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct wb_hello hello = {.magic = WB_MAGIC};
    struct wb_write quit = {.magic = WB_QUIT_MAGIC};
    struct wb_reply reply;
    int fd = open(WB_TOKEN_FILE, O_RDONLY), ok;

    ok = fd >= 0 && read(fd, hello.token, sizeof(hello.token)) == (ssize_t)sizeof(hello.token);
    if (fd >= 0) close(fd);
    for (hello.version = WB_VERSION; ok && hello.version; hello.version--)
    {
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return;
        ok = !connect(fd, (struct sockaddr *)&address, sizeof(address));
        if (ok && !transfer(fd, &hello, sizeof(hello), 1) && !transfer(fd, &reply, sizeof(reply), 0))
        {
            transfer(fd, &quit, sizeof(quit), 1);
            say("asked the running broker to exit (version %u hello, reply %d)", hello.version, reply.result);
            close(fd);
            return;
        }
        close(fd);
    }
}

int main(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(WB_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int listener = socket(AF_INET, SOCK_STREAM, 0), one = 1, attempt;

    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)))
    {
        say("not started: socket errno=%d", errno);
        return 1;
    }
    /* an older copy holds the port: ask it to exit, then retry for 2 s */
    for (attempt = 0; bind(listener, (struct sockaddr *)&address, sizeof(address)); attempt++)
    {
        if (errno != EADDRINUSE || attempt == 10)
        {
            say("not started: port %d errno=%d", WB_PORT, errno);
            return 1;
        }
        if (!attempt) replace_running();
        usleep(200000);
    }
    if (broker_prepare_startup())
    {
        say("not started: cannot write %s errno=%d", WB_TOKEN_FILE, errno);
        return 1;
    }
    if (listen(listener, 64))
    {
        say("not started: listen errno=%d", errno);
        return 1;
    }
    say("listening on 127.0.0.1:%d for %s identity=%u:%u effective=%u:%u",
        WB_PORT, WB_USER_ROOT, (unsigned)getuid(), (unsigned)getgid(),
        (unsigned)geteuid(), (unsigned)getegid());
    for (;;)
    {
        int client = accept(listener, NULL, NULL);
        struct timeval timeout = {.tv_sec = 30};
        pthread_t thread;

        if (client < 0) { if (errno == EINTR) continue; say("accept errno=%d", errno); break; }
        /* an idle title connection may wait as long as it likes; a stuck reply may not */
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (pthread_create(&thread, NULL, serve, (void *)(intptr_t)client)) { close(client); continue; }
        pthread_detach(thread);
    }
    close(listener);
    return 1;
}
