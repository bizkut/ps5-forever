#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "protocol.h"

int sceKernelDebugOutText(int channel, const char *text)
{
    (void)channel;
    return fputs(text, stderr) < 0 ? -1 : 0;
}

static int64_t deadline(void) { return io_now_ms() + PW_OVERALL_MS; }

static void wait_child(pid_t child, int expected)
{
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

static void lifecycle(int malformed)
{
    int control[2], server[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, control));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, server));
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        close(control[0]); close(server[0]); close(server[1]);
        int result = pw_child_session(control[1], deadline());
        close(control[1]);
        _exit(result);
    }
    close(control[1]);
    struct pw_bootstrap bootstrap;
    pw_bootstrap_init(&bootstrap);
    if (malformed == 1) bootstrap.cookie[0] ^= 1;
    size_t bytes = malformed == 2 ? sizeof(bootstrap) - 1 : sizeof(bootstrap);
    assert(!io_transfer(control[0], &bootstrap, bytes, 1, deadline()));
    if (malformed)
    {
        assert(!shutdown(control[0], SHUT_WR));
        wait_child(child, 1);
    }
    else
    {
        assert(!pw_send_fd(control[0], server[1], deadline()));
        assert(!close(server[1])); server[1] = -1;
        assert(!shutdown(control[0], SHUT_WR));
        struct pw_challenge challenge = {.magic = PW_MAGIC, .version = PW_VERSION, .nonce = PW_NONCE};
        struct pw_reply reply, ack;
        assert(!io_transfer(server[0], &challenge, sizeof(challenge), 1, deadline()));
        assert(!shutdown(server[0], SHUT_WR));
        assert(!io_transfer(server[0], &reply, sizeof(reply), 0, deadline()));
        assert(!memcmp(&reply.echo, &challenge, sizeof(challenge)));
        assert(reply.pid == (uint32_t)child && reply.descriptor_only == 1);
        unsigned char extra;
        assert(io_transfer(server[0], &extra, 1, 0, deadline()) == 1 && !errno);
        assert(!io_transfer(control[0], &ack, sizeof(ack), 0, deadline()));
        assert(!memcmp(&reply, &ack, sizeof(reply)));
        assert(io_transfer(control[0], &extra, 1, 0, deadline()) == 1 && !errno);
        wait_child(child, 0);
    }
    close(control[0]); close(server[0]);
    if (server[1] >= 0) close(server[1]);
}

static int open_descriptors(void)
{
    int count = 0;
    for (int fd = 0; fd < 256; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}

static void reject_multiple_rights(void)
{
    int control[2], server[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, control));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, server));
    int before = open_descriptors();
    unsigned char tag = 0x57;
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(2 * sizeof(int))]; } ancillary = {0};
    struct iovec data = {.iov_base = &tag, .iov_len = 1};
    struct msghdr message = {.msg_iov = &data, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(2 * sizeof(int));
    memcpy(CMSG_DATA(header), server, sizeof(server));
    assert(sendmsg(control[0], &message, 0) == 1);
    assert(pw_receive_fd(control[1], deadline()) == -1 && errno == EPROTO);
    assert(open_descriptors() == before);
    for (unsigned i = 0; i < 2; i++) { close(control[i]); close(server[i]); }
}

static void reject_missing_or_wrong_rights(void)
{
    int control[2], file = open("/dev/null", O_RDONLY);
    assert(file >= 0 && !socketpair(AF_UNIX, SOCK_STREAM, 0, control));
    int before = open_descriptors();
    assert(!pw_send_fd(control[0], file, deadline()));
    assert(pw_receive_fd(control[1], deadline()) == -1 && errno == EPROTO);
    assert(open_descriptors() == before);
    unsigned char tag = 0x57;
    assert(send(control[0], &tag, 1, 0) == 1);
    assert(pw_receive_fd(control[1], deadline()) == -1 && errno == EPROTO);
    assert(pw_receive_fd(control[1], io_now_ms() + 25) == -1 && errno == ETIMEDOUT);
    close(control[0]); close(control[1]); close(file);
}

static void duplicate_non_socket_descriptor(void)
{
    int bridge[2] = {-1, -1}, pipefd[2], before = open_descriptors();
    unsigned char sent = 0xa5, received = 0;
    assert(!pw_duplicate_bridge_init(bridge));
    assert(!pipe(pipefd));
    int expected = 0;
    while (fcntl(expected, F_GETFD) >= 0 || errno != EBADF) expected++;
    int copy = pw_duplicate_fd(pipefd[0], bridge, deadline());
    assert(copy == expected && fcntl(copy, F_GETFD) >= 0);
    assert(!close(pipefd[0]));
    assert(write(pipefd[1], &sent, 1) == 1);
    assert(read(copy, &received, 1) == 1 && received == sent);
    assert(!close(copy) && !close(pipefd[1]));

    int stale = open("/dev/null", O_RDONLY);
    assert(stale >= 0 && !close(stale));
    errno = 0;
    assert(pw_duplicate_fd(stale, bridge, deadline()) == -1 && errno == EBADF);
    int valid = open("/dev/null", O_RDONLY);
    assert(valid >= 0);
    errno = 0;
    assert(pw_duplicate_fd(valid, bridge, io_now_ms()) == -1 && errno == ETIMEDOUT);
    errno = 0;
    assert(pw_duplicate_fd(bridge[0], bridge, deadline()) == -1 && errno == EINVAL);

    unsigned char malformed = 0x57;
    assert(send(bridge[0], &malformed, 1, 0) == 1);
    assert(pw_duplicate_fd(valid, bridge, deadline()) == -1 && errno == EPROTO);
    assert(pw_socket_valid(bridge[0]) && pw_socket_valid(bridge[1]));
    copy = pw_duplicate_fd(valid, bridge, deadline());
    assert(copy >= 0 && !close(copy));
    assert(!close(valid));
    pw_duplicate_bridge_close(bridge);
    assert(open_descriptors() == before);
}

static void p1c_validation(void)
{
    char *argv[] = {"wine", "C:\\psmkit-wine-child.exe", "two words", NULL};
    char *decoded[PW_ARGC + 1];
    char *native_argv[] = {"psmkit-wine-runtime-child.bin", "7b", "1c8", NULL};
    char *extra_argv[] = {"psmkit-wine-runtime-child.bin", "psmkit-wine-runtime-child.bin",
                          "7b", "1c8", NULL};
    uint64_t generation = 0, nonce = 0;
    assert(!pw_p1c_argv_identity(3, native_argv, &generation, &nonce));
    assert(generation == 123 && nonce == 456);
    assert(!pw_p1c_argv_identity(4, extra_argv, &generation, &nonce));
    assert(generation == 123 && nonce == 456);
    assert(pw_p1c_argv_identity(2, native_argv, &generation, &nonce) == EINVAL);
    extra_argv[3] = "not-an-identity";
    assert(pw_p1c_argv_identity(4, extra_argv, &generation, &nonce) == EINVAL);
    assert(generation == 123 && nonce == 456);
    struct pw_p1c_bootstrap good, bad;
    assert(!pw_p1c_bootstrap_init(&good, 123, 456, 42, argv, "/tmp"));
    assert(!pw_p1c_bootstrap_validate(&good, 123, 456));
    pw_p1c_argv(&good, decoded);
    assert(!strcmp(decoded[0], "wine") && !strcmp(decoded[2], "two words") && !decoded[3]);
    bad = good; bad.magic ^= 1; assert(pw_p1c_bootstrap_validate(&bad, 123, 456) == EPROTO);
    bad = good; bad.version = 2; assert(pw_p1c_bootstrap_validate(&bad, 123, 456) == EPROTO);
    bad = good; bad.size--; assert(pw_p1c_bootstrap_validate(&bad, 123, 456) == EPROTO);
    assert(pw_p1c_bootstrap_validate(&good, 124, 456) == ESTALE);
    assert(pw_p1c_bootstrap_validate(&good, 123, 457) == ESTALE);
    bad = good; bad.argc = PW_ARGC + 1; assert(pw_p1c_bootstrap_validate(&bad, 123, 456) == E2BIG);
    bad = good; bad.argv_bytes = PW_ARG_BYTES + 1; assert(pw_p1c_bootstrap_validate(&bad, 123, 456) == E2BIG);
    bad = good; memset(bad.argv, 'x', sizeof(bad.argv));
    assert(pw_p1c_bootstrap_validate(&bad, 123, 456) == EPROTO);
    bad = good; memset(bad.cwd, 'x', sizeof(bad.cwd));
    assert(pw_p1c_bootstrap_validate(&bad, 123, 456) == EPROTO);
    char oversized[PW_ARG_BYTES + 1];
    memset(oversized, 'x', sizeof(oversized)); oversized[PW_ARG_BYTES] = 0;
    char *large[] = {"wine", oversized, NULL};
    assert(pw_p1c_bootstrap_init(&bad, 123, 456, 42, large, NULL) == E2BIG);
    char *many[PW_ARGC + 2];
    many[0] = "wine";
    for (unsigned int i = 1; i <= PW_ARGC; ++i) many[i] = "x";
    many[PW_ARGC + 1] = NULL;
    assert(pw_p1c_bootstrap_init(&bad, 123, 456, 42, many, NULL) == E2BIG);
}
static void p1c_fault_bootstrap(void)
{
    char *argv[] = {"wine", "C:\\psmkit-wine-child.exe",
                    PW_STALE_BOOTSTRAP_TOKEN, "stale", "2", NULL};
    char *native_argv[] = {"psmkit-wine-runtime-child.bin", "7b", "1c8", NULL};
    struct pw_p1c_bootstrap good, fault, unchanged;
    uint64_t generation = 0, nonce = 0;
    assert(!pw_p1c_argv_identity(3, native_argv, &generation, &nonce));
    assert(!pw_p1c_bootstrap_init(&good, generation, nonce, 42, argv, "/tmp"));
    fault = good;
    assert(pw_p1c_bootstrap_fault(&fault, argv) == 1);
    assert(fault.generation == (generation ^ UINT64_C(1)));
    assert(pw_p1c_bootstrap_validate(&fault, generation, nonce) == ESTALE);
    /* Comparing the complete wire record proves only generation changed. */
    unchanged = fault;
    unchanged.generation = generation;
    assert(!memcmp(&good, &unchanged, sizeof(good)));
    uint64_t after_generation = 0, after_nonce = 0;
    assert(!pw_p1c_argv_identity(3, native_argv, &after_generation, &after_nonce));
    assert(after_generation == generation && after_nonce == nonce);
    struct pw_control reply;
    pw_control_init(&reply, PW_BOOTSTRAP_REPLY, after_generation, after_nonce);
    reply.error = pw_p1c_bootstrap_validate(&fault, after_generation, after_nonce);
    struct pw_slot slot = {.generation = generation, .nonce = nonce, .occupied = 1};
    assert(pw_slot_bootstrap_reply(&slot, &reply) == ESTALE);
    argv[2] = "stale"; argv[3] = PW_STALE_BOOTSTRAP_TOKEN;
    fault = good;
    assert(!pw_p1c_bootstrap_fault(&fault, argv));
    assert(!memcmp(&good, &fault, sizeof(good)));
}


static void p1c_slots(void)
{
    struct pw_slot slots[PW_CHILDREN] = {0};
    int pairs[PW_CHILDREN][2], before = open_descriptors();
    for (unsigned int i = 0; i < PW_CHILDREN; ++i)
    {
        assert(pw_slot_reserve(slots, 123, i + 1, i + 42) == (int)i);
        assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]));
        slots[i].control = pairs[i][0];
        struct pw_control hello;
        pw_control_init(&hello, PW_HELLO, 123, i + 1);
        hello.pid = i + 100;
        assert(!pw_slot_hello(&slots[i], &hello));
        assert(slots[i].unix_pid == (int)i + 100);
        hello.generation++;
        assert(pw_slot_hello(&slots[i], &hello) == ESTALE);
        hello.generation--;
        hello.nonce++;
        assert(pw_slot_hello(&slots[i], &hello) == ESTALE);
        hello.nonce--;
        hello.version--;
        assert(pw_slot_hello(&slots[i], &hello) == EPROTO);
        hello.version++;
        hello.size--;
        assert(pw_slot_hello(&slots[i], &hello) == EPROTO);
        hello.size++;
        hello.pid = 0;
        assert(pw_slot_hello(&slots[i], &hello) == EPROTO);
    }
    assert(pw_slot_reserve(slots, 123, 100, 99) == -1 && errno == EAGAIN);
    for (unsigned int i = 0; i < PW_CHILDREN; ++i)
    {
        close(pairs[i][1]);
        unsigned char byte;
        assert(recv(slots[i].control, &byte, 1, MSG_DONTWAIT) == 0);
        assert(pw_slot_release(&slots[i], 124, i + 1) == ESTALE);
        assert(slots[i].occupied);
        assert(!pw_slot_release(&slots[i], 123, i + 1));
    }
    assert(open_descriptors() == before);
    assert(pw_slot_reserve(slots, 123, 100, 99) == 0);
    assert(pw_slot_release(&slots[0], 123, 1) == ESTALE);
    assert(!pw_slot_release(&slots[0], 123, 100));
}

static void p1c_transport(void)
{
    int control[2], server[2], before = open_descriptors();
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, control));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, server));
    char *argv[] = {"wine", "C:\\psmkit-wine-child.exe", NULL};
    struct pw_p1c_bootstrap bootstrap;
    assert(!pw_p1c_bootstrap_init(&bootstrap, 123, 456, 42, argv, NULL));
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        close(control[0]); close(server[0]); close(server[1]);
        struct pw_p1c_bootstrap received;
        assert(!io_transfer(control[1], &received, sizeof(received), 0, deadline()));
        assert(!pw_p1c_bootstrap_validate(&received, 123, 456));
        struct pw_control validation;
        pw_control_init(&validation, PW_BOOTSTRAP_REPLY, 123, 456);
        assert(!io_transfer(control[1], &validation, sizeof(validation), 1, deadline()));
        int fd = pw_receive_fd(control[1], deadline());
        assert(fd >= 0);
        struct pw_control message;
        pw_control_init(&message, PW_HELLO, 123, 456);
        message.pid = (unsigned int)getpid();
        assert(!io_transfer(control[1], &message, sizeof(message), 1, deadline()));
        assert(!io_transfer(control[1], &message, sizeof(message), 0, deadline()));
        assert(!pw_control_validate(&message, PW_SIGNAL, 123, 456));
        assert(message.pid == (unsigned int)getpid() && message.thread_id == 77 &&
               message.teb == UINT64_C(0x123456789abcdef0) && message.sig == 10 && !message.error);
        message.type = PW_SIGNAL_REPLY; message.error = ESRCH;
        assert(!io_transfer(control[1], &message, sizeof(message), 1, deadline()));
        close(fd); close(control[1]); _exit(0);
    }
    /* Keep server[1] open, as the wineserver keeps its end: a receiver
     * rejects an endpoint whose peer has already gone (getpeername). */
    close(control[1]);
    assert(!io_transfer(control[0], &bootstrap, sizeof(bootstrap), 1, deadline()));
    struct pw_slot initial = {.occupied = 1, .generation = 123, .nonce = 456, .control = control[0]};
    struct pw_control validation;
    assert(!io_transfer(control[0], &validation, sizeof(validation), 0, deadline()));
    assert(!pw_slot_bootstrap_reply(&initial, &validation));
    assert(!pw_send_fd(control[0], server[0], deadline()));
    struct pw_control hello, signal, reply;
    assert(!io_transfer(control[0], &hello, sizeof(hello), 0, deadline()));
    struct pw_slot slot = {.occupied = 1, .generation = 123, .nonce = 456, .control = control[0]};
    assert(!pw_slot_hello(&slot, &hello) && slot.unix_pid == child);
    pw_control_init(&signal, PW_SIGNAL, 123, 456);
    signal.pid = (unsigned int)child; signal.thread_id = 77;
    signal.teb = UINT64_C(0x123456789abcdef0); signal.sig = 10;
    assert(!io_transfer(control[0], &signal, sizeof(signal), 1, deadline()));
    assert(!io_transfer(control[0], &reply, sizeof(reply), 0, deadline()));
    assert(!pw_control_validate(&reply, PW_SIGNAL_REPLY, 123, 456) && reply.error == ESRCH);
    assert(reply.thread_id == signal.thread_id && reply.teb == signal.teb);
    wait_child(child, 0);
    unsigned char byte;
    assert(recv(control[0], &byte, 1, MSG_DONTWAIT) == 0);
    assert(!pw_slot_release(&slot, 123, 456));
    close(server[0]); close(server[1]);
    assert(open_descriptors() == before);
}

static void p1c_stale_rejection(void)
{
    int control[2], before = open_descriptors();
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, control));
    char *argv[] = {"wine", "C:\\psmkit-wine-child.exe", NULL};
    struct pw_p1c_bootstrap bootstrap;
    assert(!pw_p1c_bootstrap_init(&bootstrap, 124, 456, 42, argv, NULL));
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        close(control[0]);
        char *native_argv[] = {"psmkit-wine-runtime-child.bin", "extra", "7b", "1c8", NULL};
        uint64_t expected_generation = 0, expected_nonce = 0;
        assert(!pw_p1c_argv_identity(4, native_argv, &expected_generation, &expected_nonce));
        assert(!io_transfer(control[1], &bootstrap, sizeof(bootstrap), 0, deadline()));
        int error = pw_p1c_bootstrap_validate(&bootstrap, expected_generation, expected_nonce);
        assert(error == ESTALE);
        struct pw_control validation;
        /* Reply uses the independent trusted native-argv identity, not the
         * rejected bootstrap generation; supervisor can authenticate it. */
        pw_control_init(&validation, PW_BOOTSTRAP_REPLY, expected_generation, expected_nonce);
        validation.error = error;
        assert(!io_transfer(control[1], &validation, sizeof(validation), 1, deadline()));
        close(control[1]); _exit(1);
    }
    close(control[1]);
    assert(!io_transfer(control[0], &bootstrap, sizeof(bootstrap), 1, deadline()));
    struct pw_control validation;
    assert(!io_transfer(control[0], &validation, sizeof(validation), 0, deadline()));
    struct pw_slot slot = {.occupied = 1, .generation = 123, .nonce = 456, .control = control[0]};
    assert(pw_slot_bootstrap_reply(&slot, &validation) == ESTALE);
    validation.nonce++;
    assert(pw_slot_bootstrap_reply(&slot, &validation) == ESTALE);
    validation.nonce--;
    validation.error = -1;
    assert(pw_slot_bootstrap_reply(&slot, &validation) == EPROTO);
    wait_child(child, 1);
    assert(!pw_slot_release(&slot, 123, 456));
    assert(open_descriptors() == before);
}

static void p1c_poison_and_shutdown(void)
{
    int pair[2], before = open_descriptors();
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    struct pw_slot slot = {.occupied = 1, .generation = 123, .nonce = 456,
        .unix_pid = 99, .control = pair[0], .handle = 42};
    assert(pw_slot_routable(&slot, 123, 99));
    struct pw_control reply;
    pw_control_init(&reply, PW_SIGNAL_REPLY, 123, 456);
    reply.size--;
    assert(pw_control_validate(&reply, PW_SIGNAL_REPLY, 123, 456) == EPROTO);
    pw_slot_poison(&slot);
    assert(slot.occupied && slot.failed && !pw_slot_routable(&slot, 123, 99));
    unsigned char byte = 0;
    assert(recv(pair[1], &byte, 1, MSG_DONTWAIT) == 0);
    assert(io_transfer(slot.control, &byte, 1, 1, deadline()) == -1 && errno == EPIPE);
    close(pair[1]);
    assert(recv(slot.control, &byte, 1, MSG_DONTWAIT) == 0);
    assert(!pw_slot_release(&slot, 123, 456));

    /* Drive the real supervisor deadline policy with deterministic time:
     * normal EOF in grace incurs no kill; stuck/kill-failed slots abandon
     * exactly at the final deadline, never waiting indefinitely. */
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    slot = (struct pw_slot){.occupied = 1, .generation = 123, .nonce = 457,
        .unix_pid = 100, .control = pair[0], .handle = 43};
    assert(pw_slot_shutdown_action(&slot, 1000, 3000, 5000) == PW_SHUTDOWN_WAIT);
    assert(pw_slot_shutdown_action(&slot, 2999, 3000, 5000) == PW_SHUTDOWN_WAIT);
    close(pair[1]);
    assert(recv(slot.control, &byte, 1, MSG_DONTWAIT) == 0);
    assert(!pw_slot_release(&slot, 123, 457));
    assert(pw_slot_shutdown_action(&slot, 3000, 3000, 5000) == PW_SHUTDOWN_WAIT);

    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    slot = (struct pw_slot){.occupied = 1, .generation = 123, .nonce = 458,
        .unix_pid = 101, .control = pair[0], .handle = 44};
    assert(pw_slot_shutdown_action(&slot, 2999, 3000, 5000) == PW_SHUTDOWN_WAIT);
    assert(pw_slot_shutdown_action(&slot, 3000, 3000, 5000) == PW_SHUTDOWN_KILL);
    slot.kill_requested = 1; /* The production kill attempt sets this even on failure. */
    assert(pw_slot_shutdown_action(&slot, 3001, 3000, 5000) == PW_SHUTDOWN_WAIT);
    assert(pw_slot_shutdown_action(&slot, 4999, 3000, 5000) == PW_SHUTDOWN_WAIT);
    assert(pw_slot_shutdown_action(&slot, 5000, 3000, 5000) == PW_SHUTDOWN_ABANDON);
    pw_slot_poison(&slot);
    assert(!pw_slot_routable(&slot, 123, 101));
    assert(!pw_slot_release(&slot, 123, 458));
    close(pair[1]);
    assert(open_descriptors() == before);
}

static void spawn_log_redaction(void)
{
    char text[256];
    char *argv[] = {"wine", "C:\\Game\\WowB.exe", "-launcherlogin", "-uid",
                    "private-product", "--session=private-session", NULL};
    assert(!strcmp(pw_redact_spawn_argv(argv, text, sizeof(text)),
                   "C:\\Game\\WowB.exe -launcherlogin -uid ... --session=..."));
    assert(!strstr(text, "private-product") && !strstr(text, "private-session"));
    char *chromium[] = {"wine", "Battle.net.exe", "--type=renderer", "--token=private", NULL};
    assert(!strcmp(pw_redact_spawn_argv(chromium, text, sizeof(text)),
                   "Battle.net.exe --type=renderer --token=..."));
}

int main(void)
{
    alarm(10); /* Bound a regression which otherwise strands waitpid. */
    struct pw_bootstrap bootstrap;
    pw_bootstrap_init(&bootstrap);
    assert(pw_bootstrap_valid(&bootstrap));
    for (size_t i = 0; i < sizeof(bootstrap); i++)
    {
        ((unsigned char *)&bootstrap)[i] ^= 1;
        assert(!pw_bootstrap_valid(&bootstrap));
        ((unsigned char *)&bootstrap)[i] ^= 1;
    }
    lifecycle(0); lifecycle(1); lifecycle(2);
    reject_multiple_rights(); reject_missing_or_wrong_rights();
    duplicate_non_socket_descriptor();
    p1c_validation(); p1c_slots(); p1c_transport();
    p1c_stale_rejection(); p1c_fault_bootstrap(); p1c_poison_and_shutdown();
    spawn_log_redaction();
    puts("psmkit-wine host: trailing argv identity, bootstrap v3/stale errno, fault generation-only/ESTALE, HELLO, seven-slot EOF/reuse, SIGNAL/poisoning, bounded shutdown policy PASS; Wine NOT RUN");
    return 0;
}
