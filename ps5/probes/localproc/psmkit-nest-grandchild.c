#include <errno.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "psmkit-nest.h"
extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const libc_requirement = &Need_sceLibcInternal;
int main(void)
{
    int64_t deadline = io_now_ms() + NEST_MS;
    struct cap_frame request = {0}, reply = {0}, stop = {0};
    int protocol = 0;
    struct nest_export descriptor = {0};
    uint8_t wire[NEST_FRAME_BYTES];
    if (io_transfer(NEST_SOCKET, wire, sizeof(wire), 0, deadline) ||
        !nest_frame_decode(&request, &descriptor, wire)) {
        io_log("psmkit-nest grandchild frame rejected=1 errno=%d", errno);
        goto complete;
    }
    reply = request; reply.kind = CAP_READY; reply.pid = (uint32_t)getpid();
    if (io_transfer(NEST_SOCKET, &reply, sizeof(reply), 1, deadline)) goto complete;
    io_log("psmkit-nest grandchild READY index=%u EXEC_pid=%u parent_pid=%u nonce=%#llx main_threads=1 probe_allocations=0", request.index, reply.pid, request.parent_pid, (unsigned long long)request.nonce);
    for (;;) {
        int64_t now = io_now_ms();
        if (now < 0 || now >= deadline) goto complete;
        struct pollfd fd = {.fd = NEST_SOCKET, .events = POLLIN};
        int remaining = (int)(deadline - now);
        int status = poll(&fd, 1, remaining < IO_OPERATION_MS ? remaining : IO_OPERATION_MS);
        if (status < 0 && errno == EINTR) continue;
        if (status < 0) goto complete;
        if (status) break;
    }
    if (io_transfer(NEST_SOCKET, &stop, sizeof(stop), 0, deadline)) goto complete;
    struct cap_frame expected = request;
    expected.kind = CAP_STOP;
    if (memcmp(&expected, &stop, sizeof(stop))) goto complete;
    unsigned char extra;
    if (io_transfer(NEST_SOCKET, &extra, 1, 0, deadline) != 1 || errno) goto complete;
    reply.kind = CAP_DONE;
    if (io_transfer(NEST_SOCKET, &reply, sizeof(reply), 1, deadline)) goto complete;
    protocol = 1;
complete:
    shutdown(NEST_SOCKET, SHUT_WR); close(NEST_SOCKET);
    io_log("psmkit-nest grandchild complete index=%u EXEC_pid=%u protocol=%d exit=0 actual_exit=EXTERNAL_GATE", request.index, (unsigned)getpid(), protocol);
    return 0;
}
