/* psmkit-dmemshare child (CPU-helper profile, no SystemService or GPU
 * import): see psmkit-dmemshare.h. fd 3 is the parent transport. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "psmkit-io.h"
#include "psmkit-dmemshare.h"

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;
int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);

static long long flexible(void)
{
    size_t size = 0;
    return sceKernelAvailableFlexibleMemorySize(&size) ? -1 : (long long)size;
}

static uint32_t matches(volatile unsigned char *bytes)
{
    uint32_t count = 0;
    for (size_t page = 0; page < DS_PAGES; page++)
    {
        size_t i;
        for (i = 0; i < DS_PAGE; i += 1024)
            if (bytes[page * DS_PAGE + i] != ds_parent_byte(page)) break;
        if (i == DS_PAGE) count++;
    }
    return count;
}

static int receive_descriptor(struct ds_request *request, int *object_fd)
{
    unsigned char control[CMSG_SPACE(4 * sizeof(int))] = {0};
    struct iovec iov = {.iov_base = request, .iov_len = sizeof(*request)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control)};
    ssize_t bytes = recvmsg(3, &msg, 0);
    int count = 0;

    *object_fd = -1;
    if (bytes <= 0 || bytes > (ssize_t)sizeof(*request) ||
        (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) return -1;
    if (bytes < (ssize_t)sizeof(*request) &&
        io_transfer(3, (unsigned char *)request + bytes, sizeof(*request) - (size_t)bytes,
                    0, io_now_ms() + 2000)) return -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
        memcpy(object_fd, CMSG_DATA(cmsg), sizeof(*object_fd));
        count++;
    }
    return count == 1 && request->magic == DS_MAGIC && request->pages == DS_PAGES ? 0 : -1;
}

int main(void)
{
    void *first = NULL, *own = MAP_FAILED, *passed = MAP_FAILED;
    struct ds_request request = {0};
    struct ds_reply reply = {.magic = DS_MAGIC};
    int passed_fd = -1, own_fd = -1;

    if (receive_descriptor(&request, &passed_fd))
    {
        io_log("dmemshare child receive failed errno=%d fd=%d", errno, passed_fd);
        return 1;
    }
    reply.flexible_before = flexible();
    io_log("dmemshare child pid=%d physical=%#llx passed_fd=%d flexible=%lld", (int)getpid(),
           (unsigned long long)request.physical, passed_fd, (long long)reply.flexible_before);

    reply.map_result = sceKernelMapDirectMemory(&first, DS_BYTES, PROT_READ | PROT_WRITE, 0,
                                                request.physical, PS5_KERNEL_DIRECT_ALIGNMENT);
    if (!reply.map_result) reply.map_matches = matches(first);

    errno = 0;
    if ((own_fd = open("/dev/dmem1", O_RDWR)) >= 0)
    {
        own = mmap(NULL, DS_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, own_fd, request.physical);
        if (own == MAP_FAILED) reply.own_dmem_errno = errno;
        else reply.own_dmem_matches = matches(own);
    }
    else reply.own_dmem_errno = errno;

    errno = 0;
    passed = mmap(NULL, DS_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, passed_fd, request.physical);
    if (passed == MAP_FAILED) reply.passed_dmem_errno = errno;
    else reply.passed_dmem_matches = matches(passed);
    reply.flexible_mapped = flexible();

    volatile unsigned char *target = passed != MAP_FAILED ? passed :
                                     !reply.map_result ? first : own != MAP_FAILED ? own : NULL;
    if (target)
        for (size_t page = 0; page < DS_PAGES; page++)
        {
            for (size_t i = 0; i < DS_PAGE; i += 1024)
                target[page * DS_PAGE + i] = ds_child_byte(page);
            reply.written++;
        }

    if (passed != MAP_FAILED) munmap(passed, DS_BYTES);
    if (own != MAP_FAILED) munmap(own, DS_BYTES);
    if (!reply.map_result) sceKernelMunmap(first, DS_BYTES);
    if (passed_fd >= 0) close(passed_fd);
    if (own_fd >= 0) close(own_fd);
    reply.flexible_after = flexible();
    if (io_transfer(3, &reply, sizeof(reply), 1, io_now_ms() + 2000))
        io_log("dmemshare child reply failed errno=%d", errno);
    io_log("dmemshare child result map=%#x own_errno=%d passed_errno=%d matches=%u/%u/%u "
           "written=%u flexible=%lld/%lld/%lld",
           (unsigned)reply.map_result, reply.own_dmem_errno, reply.passed_dmem_errno,
           reply.map_matches, reply.own_dmem_matches, reply.passed_dmem_matches, reply.written,
           (long long)reply.flexible_before, (long long)reply.flexible_mapped,
           (long long)reply.flexible_after);
    return reply.passed_dmem_errno || reply.passed_dmem_matches != DS_PAGES ||
           reply.written != DS_PAGES;
}
