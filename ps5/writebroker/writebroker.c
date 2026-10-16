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

#define WB_CHUNK (1u << 20)

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
    if (listen(listener, 64))
    {
        say("not started: listen errno=%d", errno);
        return 1;
    }
    if (write_token())
    {
        say("not started: cannot write %s errno=%d", WB_TOKEN_FILE, errno);
        return 1;
    }
    say("listening on 127.0.0.1:%d for %s", WB_PORT, WB_USER_ROOT);
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
