/* Exercise extracted production request/probe/transfer with real socket I/O.
 * Only connection setup and controlled socket failure/delay scheduling differ. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "sha256.h"
#include "../../addon-worker/protocol.h"

static int64_t io_now_ms(void)
{
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

#define pw_writebroker_transfer actual_socket_transfer
#include "addon-worker-transport.inc"
#undef pw_writebroker_transfer

static int client_fd = -1, send_failure_stage, transfer_stage;
static int connect_error;

static int pw_writebroker_connect_port(unsigned port, int64_t deadline)
{
    int fd = client_fd;
    assert(port == AW_PORT && deadline > 0);
    if (connect_error) { errno = connect_error; return -1; }
    assert(fd >= 0);
    client_fd = -1;
    return fd;
}

static int pw_writebroker_transfer(int fd, void *data, size_t size,
                                   int sending, int64_t deadline)
{
    ++transfer_stage;
    if (transfer_stage == send_failure_stage)
    {
        assert(sending);
        /* Force a real EPIPE from send(), rather than mocking its return. */
        assert(!shutdown(fd, SHUT_WR));
    }
    return actual_socket_transfer(fd, data, size, sending, deadline);
}

#include "addon-worker-request.inc"

enum server_mode
{
    SERVER_VALID, SERVER_HOLD, SERVER_CLOSE, SERVER_BAD_CHALLENGE,
    SERVER_BAD_AUTH_HEADER, SERVER_BAD_PROOF, SERVER_DENIED, SERVER_BAD_REPLY,
    SERVER_DELAY_AUTH,
};

struct server_fixture
{
    int fd, stage;
    enum server_mode mode;
    char token[AW_TOKEN_BYTES];
};

static int server_receive(int fd, void *data, size_t size)
{
    size_t used = 0;
    while (used < size)
    {
        ssize_t count = recv(fd, (unsigned char *)data + used, size - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        used += (size_t)count;
    }
    return 1;
}

static int server_send(int fd, const void *data, size_t size)
{
    size_t used = 0;
    while (used < size)
    {
        ssize_t count = send(fd, (const unsigned char *)data + used, size - used, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        used += (size_t)count;
    }
    return 1;
}

static int server_stop_at(struct server_fixture *fixture, int stage)
{
    if (fixture->stage != stage) return 0;
    if (fixture->mode == SERVER_CLOSE) return 1;
    if (fixture->mode == SERVER_HOLD)
    {
        unsigned char byte;
        /* Remain silent until the production deadline closes the client. */
        while (server_receive(fixture->fd, &byte, 1)) {}
        return 1;
    }
    return 0;
}

static void *serve(void *opaque)
{
    struct server_fixture *fixture = opaque;
    unsigned char hello[AW_HELLO_BYTES], challenge[AW_CHALLENGE_REPLY_BYTES] = {0};
    unsigned char proof[AW_AUTH_BYTES], expected[AW_AUTH_BYTES];
    unsigned char auth[AW_AUTH_REPLY_BYTES] = {0};
    unsigned char request[AW_REQUEST_BYTES], reply[AW_REPLY_BYTES] = {0};
    int fd = fixture->fd;
    if (!server_receive(fd, hello, sizeof(hello))) goto done;
    assert(pw_addon_worker_get_u32(hello) == AW_MAGIC);
    assert(pw_addon_worker_get_u32(hello + 4) == AW_VERSION);
    if (server_stop_at(fixture, 2)) goto done;
    pw_addon_worker_put_u32(challenge, AW_MAGIC);
    pw_addon_worker_put_u32(challenge + 4, AW_VERSION);
    memset(challenge + 8, 0x5a, AW_CHALLENGE_BYTES);
    if (fixture->mode == SERVER_BAD_CHALLENGE) challenge[0] ^= 1;
    if (!server_send(fd, challenge, sizeof(challenge))) goto done;
    if (!server_receive(fd, proof, sizeof(proof))) goto done;
    pw_addon_worker_auth_tag(fixture->token, challenge + 8, 'C', expected);
    assert(!memcmp(proof, expected, sizeof(proof)));
    if (server_stop_at(fixture, 4)) goto done;
    if (fixture->mode == SERVER_DELAY_AUTH) usleep(150000);
    pw_addon_worker_put_u32(auth, AW_MAGIC);
    pw_addon_worker_put_u32(auth + 4, AW_VERSION);
    pw_addon_worker_auth_tag(fixture->token, challenge + 8, 'S', auth + 12);
    if (fixture->mode == SERVER_BAD_AUTH_HEADER) auth[0] ^= 1;
    if (fixture->mode == SERVER_BAD_PROOF) auth[12] ^= 1;
    if (fixture->mode == SERVER_DENIED) pw_addon_worker_put_u32(auth + 8, (uint32_t)-EACCES);
    if (!server_send(fd, auth, sizeof(auth))) goto done;
    if (!server_receive(fd, request, sizeof(request))) goto done;
    assert(pw_addon_worker_get_u32(request) == AW_MAGIC);
    assert(pw_addon_worker_get_u32(request + 4) == AW_COMMAND_PROBE);
    if (server_stop_at(fixture, 6)) goto done;
    pw_addon_worker_put_u32(reply, AW_MAGIC);
    pw_addon_worker_put_u32(reply + 4, AW_VERSION);
    pw_addon_worker_put_u32(reply + 8, AW_PHASE_COMPLETE);
    if (fixture->mode == SERVER_BAD_REPLY) reply[0] ^= 1;
    server_send(fd, reply, sizeof(reply));
 done:
    assert(!close(fd));
    return NULL;
}

static unsigned cases, failures;

static void run_case(const char *name, enum server_mode mode, int stage,
                     int fail_send, int probe, int timeout_ms,
                     int expected_result, int expected_error)
{
    int sockets[2], result, error = -1;
    pthread_t thread;
    struct server_fixture fixture = {.mode = mode, .stage = stage};
    struct pw_addon_worker_start_context context = {.token = fixture.token};
    memset(fixture.token, 'k', sizeof(fixture.token));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    assert(!fcntl(sockets[0], F_SETFL, O_NONBLOCK));
    fixture.fd = sockets[1];
    client_fd = sockets[0];
    send_failure_stage = fail_send;
    transfer_stage = 0;
    assert(!pthread_create(&thread, NULL, serve, &fixture));
    int64_t deadline = io_now_ms() + timeout_ms;
    if (probe) result = pw_addon_worker_probe(&context, deadline, &error);
    else result = pw_addon_worker_request_until(fixture.token, AW_COMMAND_PROBE, deadline);
    assert(!pthread_join(thread, NULL));
    ++cases;
    if (result != expected_result || (probe && error != expected_error))
    {
        ++failures;
        printf("FAIL %s result=%d expected=%d error=%d expected_error=%d\n",
               name, result, expected_result, error, expected_error);
    }
}

int main(void)
{
    run_case("authenticated probe", SERVER_VALID, 0, 0, 1, 1000, 1, 0);
    for (int stage = 1; stage <= 5; stage += 2)
    {
        char name[80];
        snprintf(name, sizeof(name), "send stage %d preserves EPIPE", stage);
        run_case(name, SERVER_VALID, 0, stage, 0, 1000, -EPIPE, 0);
    }
    for (int stage = 2; stage <= 6; stage += 2)
    {
        char name[80];
        snprintf(name, sizeof(name), "receive stage %d preserves EPIPE", stage);
        run_case(name, SERVER_CLOSE, stage, 0, 0, 1000, -EPIPE, 0);
        snprintf(name, sizeof(name), "receive stage %d preserves timeout", stage);
        run_case(name, SERVER_HOLD, stage, 0, 0, 100, -ETIMEDOUT, 0);
        snprintf(name, sizeof(name), "probe receive stage %d timeout stays pending", stage);
        run_case(name, SERVER_HOLD, stage, 0, 1, 100, 0, 0);
    }
    run_case("delayed valid proof stays pending at 100ms", SERVER_DELAY_AUTH, 0, 0, 1, 100, 0, 0);
    run_case("delayed valid proof succeeds within budget", SERVER_DELAY_AUTH, 0, 0, 1, 1000, 1, 0);
    run_case("invalid challenge header", SERVER_BAD_CHALLENGE, 0, 0, 0, 1000, -EPROTONOSUPPORT, 0);
    run_case("invalid auth header", SERVER_BAD_AUTH_HEADER, 0, 0, 0, 1000, -EACCES, 0);
    run_case("invalid server proof is fatal", SERVER_BAD_PROOF, 0, 0, 1, 1000, -1, EACCES);
    run_case("explicit auth rejection is fatal", SERVER_DENIED, 0, 0, 1, 1000, -1, EACCES);
    run_case("invalid complete reply", SERVER_BAD_REPLY, 0, 0, 0, 1000, -EIO, 0);
    connect_error = ETIMEDOUT;
    struct pw_addon_worker_start_context context = {.token = "unused"};
    int error = -1;
    int result = pw_addon_worker_probe(&context, io_now_ms() + 100, &error);
    ++cases;
    if (result || error)
    {
        ++failures;
        printf("FAIL connect timeout stays pending result=%d error=%d\n", result, error);
    }
    printf("ADDON_WORKER_REQUEST production=request,probe,transfer sockets=REAL cases=%u failures=%u %s\n",
           cases, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
