#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "protocol.h"

int pw_within(int64_t deadline)
{
    int64_t now = io_now_ms();
    return now >= 0 && now < deadline;
}

void pw_bootstrap_init(struct pw_bootstrap *bootstrap)
{
    memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->magic = PW_MAGIC;
    bootstrap->version = PW_VERSION;
    bootstrap->size = sizeof(*bootstrap);
    bootstrap->nonce = PW_NONCE;
    memcpy(bootstrap->cookie, PW_COOKIE, sizeof(bootstrap->cookie));
    memcpy(bootstrap->loader, PW_LOADER, sizeof(PW_LOADER));
    memcpy(bootstrap->prefix, PW_PREFIX, sizeof(PW_PREFIX));
    memcpy(bootstrap->server, PW_SERVER, sizeof(PW_SERVER));
    memcpy(bootstrap->program, PW_PROGRAM, sizeof(PW_PROGRAM));
}

int pw_bootstrap_valid(const struct pw_bootstrap *bootstrap)
{
    struct pw_bootstrap expected;
    pw_bootstrap_init(&expected);
    return !memcmp(bootstrap, &expected, sizeof(expected));
}

int pw_socket_valid(int fd)
{
    struct sockaddr_un local = {0}, peer = {0};
    socklen_t local_size = sizeof(local), peer_size = sizeof(peer), type_size = sizeof(int);
    int type = 0;
    return !getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) &&
           type_size == sizeof(type) && type == SOCK_STREAM &&
           !getsockname(fd, (struct sockaddr *)&local, &local_size) &&
           local_size >= offsetof(struct sockaddr_un, sun_path) && local_size <= sizeof(local) &&
           local.sun_family == AF_UNIX &&
           !getpeername(fd, (struct sockaddr *)&peer, &peer_size) &&
           peer_size >= offsetof(struct sockaddr_un, sun_path) && peer_size <= sizeof(peer) &&
           peer.sun_family == AF_UNIX;
}

/* One logical sendmsg/recvmsg, bounded by the original operation deadline.
 * Nonblocking partial stream IO is handled separately by io_transfer. */
static ssize_t descriptor_message(int fd, struct msghdr *message, int sending, int64_t overall)
{
    int64_t now = io_now_ms(), deadline = now + IO_OPERATION_MS;
    if (now < 0) return -1;
    if (deadline > overall) deadline = overall;
    for (;;)
    {
        now = io_now_ms();
        if (now < 0) return -1;
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd pollfd = {.fd = fd, .events = sending ? POLLOUT : POLLIN};
        int ready = poll(&pollfd, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (!ready) { errno = ETIMEDOUT; return -1; }
        if (pollfd.revents & POLLNVAL) { errno = EBADF; return -1; }
        ssize_t size = sending ? sendmsg(fd, message, MSG_DONTWAIT | MSG_NOSIGNAL)
                              : recvmsg(fd, message, MSG_DONTWAIT);
        if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return size;
    }
}

int pw_send_fd(int control, int descriptor, int64_t deadline)
{
    unsigned char tag = 0x57;
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct iovec data = {.iov_base = &tag, .iov_len = 1};
    struct msghdr message = {.msg_iov = &data, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    if (descriptor_message(control, &message, 1, deadline) != 1) return -1;
    if (!pw_within(deadline)) { errno = ETIMEDOUT; return -1; }
    return 0;
}

static int receive_fd(int control, int64_t deadline, int require_socket)
{
    unsigned char tag = 0;
    /* Room to reject and close multiple delivered descriptors without leaks.
     * The kernel closes descriptors discarded through MSG_CTRUNC. */
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(8 * sizeof(int))]; } ancillary = {0};
    struct iovec data = {.iov_base = &tag, .iov_len = 1};
    struct msghdr message = {.msg_iov = &data, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    ssize_t size = descriptor_message(control, &message, 0, deadline);
    if (size < 0) return -1;
    int descriptor = -1, count = 0, headers = 0;
    int valid = size == 1 && tag == 0x57 && !(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header))
    {
        headers++;
        size_t remaining = message.msg_controllen - ((unsigned char *)header - ancillary.bytes);
        if (header->cmsg_len < CMSG_LEN(0) || header->cmsg_len > remaining)
        { valid = 0; break; }
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
        { valid = 0; continue; }
        size_t bytes = header->cmsg_len - CMSG_LEN(0);
        if (bytes != sizeof(int)) valid = 0;
        for (size_t offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int))
        {
            int received;
            memcpy(&received, (unsigned char *)CMSG_DATA(header) + offset, sizeof(received));
            if (!count++) descriptor = received;
            else close(received);
        }
    }
    int timely = pw_within(deadline);
    valid = valid && headers == 1 && count == 1 && descriptor != control &&
            timely && (!require_socket || pw_socket_valid(descriptor));
    if (!valid)
    {
        if (descriptor >= 0) close(descriptor);
        errno = timely ? EPROTO : ETIMEDOUT;
        return -1;
    }
    errno = 0;
    return descriptor;
}

int pw_receive_descriptor(int control, int64_t deadline)
{
    return receive_fd(control, deadline, 0);
}

int pw_receive_fd(int control, int64_t deadline)
{
    return receive_fd(control, deadline, 1);
}

void pw_duplicate_bridge_close(int channel[2])
{
    if (channel[0] >= 0) close(channel[0]);
    if (channel[1] >= 0) close(channel[1]);
    channel[0] = channel[1] = -1;
}

int pw_duplicate_bridge_init(int channel[2])
{
    pw_duplicate_bridge_close(channel);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel)) return -1;
    /* Best effort, as in Wine's PS5 server_pipe(): the title child never execs.
     * Bridge init returned EINVAL on 5.50 after socketpair, which Wine uses there. */
    fcntl(channel[0], F_SETFD, FD_CLOEXEC);
    fcntl(channel[1], F_SETFD, FD_CLOEXEC);
    errno = 0;
    return 0;
}

static int reset_failed_duplicate(int channel[2], int error)
{
    pw_duplicate_bridge_init(channel);
    errno = error;
    return -1;
}

int pw_duplicate_fd(int descriptor, int channel[2], int64_t deadline)
{
    int copy, error;
    struct stat status;

    if (descriptor < 0 || fstat(descriptor, &status))
    {
        errno = EBADF;
        return -1;
    }
    if (descriptor == channel[0] || descriptor == channel[1] ||
        !pw_socket_valid(channel[0]) || !pw_socket_valid(channel[1]))
    {
        errno = EINVAL;
        return -1;
    }
    if (!pw_within(deadline))
    {
        errno = ETIMEDOUT;
        return -1;
    }
    if (pw_send_fd(channel[0], descriptor, deadline))
        return reset_failed_duplicate(channel, errno);
    copy = receive_fd(channel[1], deadline, 0);
    if (copy == -1) return reset_failed_duplicate(channel, errno);
    if (!pw_within(deadline))
    {
        close(copy);
        error = ETIMEDOUT;
        return reset_failed_duplicate(channel, error);
    }
    errno = 0;
    return copy;
}

int pw_child_session(int control, int64_t deadline)
{
    struct pw_bootstrap bootstrap;
    struct pw_challenge challenge;
    struct pw_reply reply;
    int server = -1, passed = 0;
    unsigned char extra;
    if (!pw_within(deadline) || !pw_socket_valid(control)) goto complete;
    if (io_transfer(control, &bootstrap, sizeof(bootstrap), 0, deadline) ||
        !pw_bootstrap_valid(&bootstrap)) goto complete;
    errno = 0;
    io_log("psmkit-wine child bootstrap valid=1 bytes=%zu runtime=NOT_STARTED", sizeof(bootstrap));
    server = pw_receive_fd(control, deadline);
    io_log("psmkit-wine child SCM_RIGHTS fd=%d errno=%d shared_server=UNPROVEN", server, errno);
    if (server < 0) goto complete;
    if (io_transfer(control, &extra, 1, 0, deadline) != 1 || errno) goto complete;
    if (io_transfer(server, &challenge, sizeof(challenge), 0, deadline) ||
        challenge.magic != PW_MAGIC || challenge.version != PW_VERSION ||
        challenge.nonce != bootstrap.nonce) goto complete;
    if (io_transfer(server, &extra, 1, 0, deadline) != 1 || errno) goto complete;
    reply = (struct pw_reply){.echo = challenge, .pid = (uint32_t)getpid(), .descriptor_only = 1};
    if (io_transfer(server, &reply, sizeof(reply), 1, deadline) || shutdown(server, SHUT_WR)) goto complete;
    if (close(server)) { server = -1; goto complete; }
    server = -1;
    if (io_transfer(control, &reply, sizeof(reply), 1, deadline) || shutdown(control, SHUT_WR)) goto complete;
    passed = pw_within(deadline);
complete:
    if (server >= 0 && close(server)) passed = 0;
    return passed ? 0 : 1;
}

static int parse_identity(const char *text, uint64_t *value)
{
    uint64_t parsed = 0;
    unsigned int length = 0;
    if (!text || !*text) return EINVAL;
    for (; *text; ++text)
    {
        unsigned int digit;
        if (++length > 16) return EINVAL;
        if (*text >= '0' && *text <= '9') digit = *text - '0';
        else if (*text >= 'a' && *text <= 'f') digit = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F') digit = *text - 'A' + 10;
        else return EINVAL;
        parsed = (parsed << 4) | digit;
    }
    if (!parsed) return EINVAL;
    *value = parsed;
    return 0;
}

int pw_p1c_argv_identity(int argc, char *const argv[], uint64_t *generation, uint64_t *nonce)
{
    uint64_t parsed_generation, parsed_nonce;
    if (argc < 3 || !argv || !generation || !nonce ||
        parse_identity(argv[argc - 2], &parsed_generation) ||
        parse_identity(argv[argc - 1], &parsed_nonce)) return EINVAL;
    *generation = parsed_generation;
    *nonce = parsed_nonce;
    return 0;
}

int pw_p1c_bootstrap_init(struct pw_p1c_bootstrap *b, uint64_t generation,
                         uint64_t nonce, unsigned int pid, char *const argv[], const char *cwd)
{
    memset(b, 0, sizeof(*b));
    b->magic = PW_MAGIC; b->version = PW_P1C_VERSION; b->size = sizeof(*b);
    b->generation = generation; b->nonce = nonce; b->windows_pid = pid;
    if (!generation || !nonce || !pid || !argv || !argv[0] || strcmp(argv[0], "wine"))
        return EINVAL;
    for (; argv[b->argc]; ++b->argc)
    {
        if (b->argc == PW_ARGC) return E2BIG;
        size_t length = strnlen(argv[b->argc], PW_ARG_BYTES) + 1;
        if (length > PW_ARG_BYTES - b->argv_bytes) return E2BIG;
        memcpy(b->argv + b->argv_bytes, argv[b->argc], length);
        b->argv_bytes += length;
    }
    if (b->argc < 2) return EINVAL;
    if (cwd)
    {
        size_t length = strnlen(cwd, sizeof(b->cwd));
        if (length == sizeof(b->cwd)) return ENAMETOOLONG;
        memcpy(b->cwd, cwd, length + 1);
    }
    return 0;
}
#ifdef PW_P1C_FAULT_INJECTION
/* argv[0] is Wine, argv[1] the PE, argv[2] its reserved first argument.
 * Never change the independent native argv identity or any other wire byte. */
int pw_p1c_bootstrap_fault(struct pw_p1c_bootstrap *b, char *const argv[])
{
    if (!argv || !argv[0] || !argv[1] || !argv[2] ||
        strcmp(argv[2], PW_STALE_BOOTSTRAP_TOKEN)) return 0;
    b->generation ^= UINT64_C(1);
    return 1;
}
#endif


int pw_p1c_bootstrap_validate(const struct pw_p1c_bootstrap *b,
                              uint64_t generation, uint64_t nonce)
{
    if (b->magic != PW_MAGIC || b->version != PW_P1C_VERSION ||
        b->size != sizeof(*b)) return EPROTO;
    if (!generation || b->generation != generation || !nonce || b->nonce != nonce)
        return ESTALE;
    if (!b->windows_pid || b->argc < 2 || b->argc > PW_ARGC ||
        !b->argv_bytes || b->argv_bytes > PW_ARG_BYTES) return E2BIG;
    size_t offset = 0;
    for (unsigned int i = 0; i < b->argc; ++i)
    {
        if (offset >= b->argv_bytes) return EPROTO;
        const char *end = memchr(b->argv + offset, 0, b->argv_bytes - offset);
        if (!end) return EPROTO;
        offset = (size_t)(end - b->argv) + 1;
    }
    if (offset != b->argv_bytes || strcmp(b->argv, "wine") ||
        !memchr(b->cwd, 0, sizeof(b->cwd))) return EPROTO;
    return 0;
}

void pw_p1c_argv(struct pw_p1c_bootstrap *b, char *argv[PW_ARGC + 1])
{
    size_t offset = 0;
    for (unsigned int i = 0; i < b->argc; ++i)
    {
        argv[i] = b->argv + offset;
        offset += strlen(argv[i]) + 1;
    }
    argv[b->argc] = NULL;
}

void pw_control_init(struct pw_control *c, unsigned int type,
                     uint64_t generation, uint64_t nonce)
{
    memset(c, 0, sizeof(*c));
    c->magic = PW_MAGIC; c->version = PW_P1C_VERSION; c->size = sizeof(*c);
    c->type = type; c->generation = generation; c->nonce = nonce;
}

int pw_control_validate(const struct pw_control *c, unsigned int type,
                        uint64_t generation, uint64_t nonce)
{
    if (c->magic != PW_MAGIC || c->version != PW_P1C_VERSION ||
        c->size != sizeof(*c) || c->type != type) return EPROTO;
    if (c->generation != generation || c->nonce != nonce) return ESTALE;
    return 0;
}

int pw_slot_reserve(struct pw_slot slots[PW_CHILDREN], uint64_t generation,
                    uint64_t nonce, unsigned int pid)
{
    if (!generation || !nonce || !pid) { errno = EINVAL; return -1; }
    for (unsigned int i = 0; i < PW_CHILDREN; ++i)
        if (!slots[i].occupied)
        {
            slots[i] = (struct pw_slot){.generation = generation, .nonce = nonce,
                .windows_pid = pid, .control = -1, .handle = -1, .occupied = 1};
            return (int)i;
        }
    errno = EAGAIN;
    return -1;
}

int pw_slot_hello(struct pw_slot *slot, const struct pw_control *hello)
{
    if (!slot->occupied) return ESRCH;
    int error = pw_control_validate(hello, PW_HELLO, slot->generation, slot->nonce);
    if (error) return error;
    if (!hello->pid || hello->pid > INT32_MAX || hello->thread_id ||
        hello->teb || hello->sig || hello->error) return EPROTO;
    slot->unix_pid = (int)hello->pid;
    return 0;
}

int pw_slot_release(struct pw_slot *slot, uint64_t generation, uint64_t nonce)
{
    if (!slot->occupied || slot->generation != generation || slot->nonce != nonce)
        return ESTALE;
    if (slot->control >= 0) close(slot->control);
    *slot = (struct pw_slot){.control = -1, .handle = -1};
    return 0;
}

int pw_slot_bootstrap_reply(const struct pw_slot *slot, const struct pw_control *reply)
{
    if (!slot->occupied) return ESRCH;
    int error = pw_control_validate(reply, PW_BOOTSTRAP_REPLY, slot->generation, slot->nonce);
    if (error) return error;
    if (reply->pid || reply->thread_id || reply->teb || reply->sig ||
        reply->error < 0 || reply->error > 4095) return EPROTO;
    return reply->error;
}

int pw_slot_routable(const struct pw_slot *slot, uint64_t generation, int pid)
{
    return slot->occupied && !slot->failed && slot->control >= 0 &&
           slot->generation == generation && slot->unix_pid == pid;
}

void pw_slot_poison(struct pw_slot *slot)
{
    slot->failed = 1;
    /* Preserve the read side for EOF/reaping, but no subsequent control write
     * is possible even if a caller accidentally retains this descriptor. */
    if (slot->control >= 0) shutdown(slot->control, SHUT_WR);
}

enum pw_shutdown_action pw_slot_shutdown_action(const struct pw_slot *slot,
                                                 int64_t now, int64_t grace, int64_t deadline)
{
    if (!slot->occupied) return PW_SHUTDOWN_WAIT;
    if (now < 0 || now >= deadline) return PW_SHUTDOWN_ABANDON;
    if (now >= grace && !slot->kill_requested) return PW_SHUTDOWN_KILL;
    return PW_SHUTDOWN_WAIT;
}

const char *pw_redact_spawn_argv(char *const argv[], char *text, size_t size)
{
    size_t used = 0;
    text[0] = 0;
    for (char *const *arg = argv ? argv + 1 : NULL; arg && *arg && used + 1 < size; arg++)
    {
        const char *value = strchr(*arg, '=');
        int count;
        if (arg == argv + 1 || !strncmp(*arg, "--type=", 7) || (**arg == '-' && !value))
            count = snprintf(text + used, size - used, "%s%s", used ? " " : "", *arg);
        else if (**arg == '-')
            count = snprintf(text + used, size - used, "%s%.*s=...", used ? " " : "",
                             (int)(value - *arg), *arg);
        else
            count = snprintf(text + used, size - used, "%s...", used ? " " : "");
        if (count < 0) break;
        used += (size_t)count;
    }
    return text;
}
