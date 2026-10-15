#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../localproc/probe.h"
#include "protocol.h"

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *, const char *const [], const struct local_process_param *);
_Static_assert(sizeof(struct local_process_param) == 0x90, "launch parameter size");
_Static_assert(offsetof(struct local_process_param, socket_fd) == 4, "socket offset");

#ifndef PW_CHILD_PATH
#define PW_CHILD_PATH "/app0/psmkit-wine-descriptor-child.bin"
#endif
#ifndef PW_RUNTIME_LABEL
#define PW_RUNTIME_LABEL "BLOCKED_NATIVE_SAFE_RTLD"
#endif

int main(void)
{
    static const char path[] = PW_CHILD_PATH;
    const char *const argv[] = {path, NULL};
    int64_t start = io_now_ms(), deadline = start + PW_OVERALL_MS;
    int bootstrap[2] = {-1, -1}, server[2] = {-1, -1};
    int passed = 0, calls = 0;
    int32_t handle = -1;
    struct pw_bootstrap request;
    struct pw_reply socket_reply = {0}, control_reply = {0};
    struct pw_challenge challenge = {.magic = PW_MAGIC, .version = PW_VERSION, .nonce = PW_NONCE};
    unsigned char extra;
    io_log("psmkit-wine parent main pid=%d gate=SCM_RIGHTS_PREREQUISITE overall_ms=%d operation_ms=%d runtime=NOT_STARTED",
           (int)getpid(), PW_OVERALL_MS, IO_OPERATION_MS);
    if (start < 0 || !pw_within(deadline)) goto complete;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, bootstrap) ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, server) || !pw_within(deadline)) goto complete;
    const struct local_process_param parameter = {.size = sizeof(parameter),
        .socket_fd = bootstrap[1], .field_8 = 1, .field_c = -1, .field_10 = 1};
    calls++;
    start = io_now_ms();
    handle = sceSystemServiceAddLocalProcessForPsmKit(path, argv, &parameter);
    io_log("psmkit-wine parent launch handle=%#x errno=%d calls=%d handle_not_pid=1", (unsigned)handle, errno, calls);
    int closed = close(bootstrap[1]);
    bootstrap[1] = -1;
    if (handle < 0 || closed || io_now_ms() - start >= IO_OPERATION_MS || !pw_within(deadline)) goto complete;
    pw_bootstrap_init(&request);
    if (io_transfer(bootstrap[0], &request, sizeof(request), 1, deadline)) goto complete;
    errno = 0;
    int sent = pw_send_fd(bootstrap[0], server[1], deadline);
    io_log("psmkit-wine parent SCM_RIGHTS result=%d errno=%d bytes=1 rights=1", sent, errno);
    if (sent) goto complete;
    closed = close(server[1]);
    server[1] = -1;
    if (closed || shutdown(bootstrap[0], SHUT_WR)) goto complete;
    if (io_transfer(server[0], &challenge, sizeof(challenge), 1, deadline) ||
        shutdown(server[0], SHUT_WR)) goto complete;
    if (io_transfer(server[0], &socket_reply, sizeof(socket_reply), 0, deadline) ||
        memcmp(&socket_reply.echo, &challenge, sizeof(challenge)) ||
        socket_reply.descriptor_only != 1 || !socket_reply.pid || socket_reply.pid == (uint32_t)getpid()) goto complete;
    if (io_transfer(server[0], &extra, 1, 0, deadline) != 1 || errno) goto complete;
    if (io_transfer(bootstrap[0], &control_reply, sizeof(control_reply), 0, deadline) ||
        memcmp(&socket_reply, &control_reply, sizeof(socket_reply))) goto complete;
    if (io_transfer(bootstrap[0], &extra, 1, 0, deadline) != 1 || errno) goto complete;
    passed = calls == 1 && pw_within(deadline);
complete:
    for (unsigned i = 0; i < 2; i++)
    {
        if (bootstrap[i] >= 0 && close(bootstrap[i])) passed = 0;
        if (server[i] >= 0 && close(server[i])) passed = 0;
    }
    passed = passed && pw_within(deadline);
    io_log("psmkit-wine parent complete calls=%d result=%s child_pid=%u runtime=" PW_RUNTIME_LABEL " shared_wineserver=NOT_STARTED actual_child_exit0=EXTERNAL_GATE control_close=REQUIRED",
           calls, passed ? "SHARED_DESCRIPTOR_PASS" : "FAIL_OR_INCONCLUSIVE", socket_reply.pid);
    /* Never return to the BigApp CRT or invoke kernel exit. Runner owns close,
     * including timeout/failure; the protocol itself has already ended. */
    for (;;)
    {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
}
