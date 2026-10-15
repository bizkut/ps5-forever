/* psmkit-dmemshare parent (the title): see psmkit-dmemshare.h. klog lines
 * start with "dmemshare "; the last is "dmemshare parent complete". */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include "probe.h"
#include "psmkit-io.h"
#include "psmkit-dmemshare.h"

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *path, const char *const argv[], const struct local_process_param *param);
int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);

static long long flexible(void)
{
    size_t size = 0;
    return sceKernelAvailableFlexibleMemorySize(&size) ? -1 : (long long)size;
}

static int send_descriptor(int socket_fd, int object_fd, const struct ds_request *request)
{
    unsigned char control[CMSG_SPACE(sizeof(object_fd))] = {0};
    struct iovec iov = {.iov_base = (void *)request, .iov_len = sizeof(*request)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(object_fd));
    memcpy(CMSG_DATA(cmsg), &object_fd, sizeof(object_fd));
    return sendmsg(socket_fd, &msg, MSG_NOSIGNAL) == (ssize_t)sizeof(*request) ? 0 : -1;
}

int main(void)
{
    static const char path[] = "/app0/psmkit-dmemshare-child.bin";
    const char *const argv[] = {path, NULL};
    void *address = NULL;
    int64_t physical = -1;
    volatile unsigned char *bytes;
    int passed = 0, pair[2] = {-1, -1}, dmem_fd = -1;

    io_log("dmemshare parent pid=%d flexible=%lld", (int)getpid(), flexible());
    int32_t result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), DS_BYTES,
                                                   PS5_KERNEL_DIRECT_ALIGNMENT, 0, &physical);
    if (!result)
        result = sceKernelMapDirectMemory(&address, DS_BYTES, PROT_READ | PROT_WRITE, 0, physical,
                                          PS5_KERNEL_DIRECT_ALIGNMENT);
    io_log("dmemshare parent direct result=%#x physical=%#llx address=%p", (unsigned)result,
           (unsigned long long)physical, address);
    if (result) goto complete;
    bytes = address;
    for (size_t page = 0; page < DS_PAGES; page++)
        for (size_t i = 0; i < DS_PAGE; i += 1024) bytes[page * DS_PAGE + i] = ds_parent_byte(page);

    if ((dmem_fd = open("/dev/dmem1", O_RDWR)) < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
    {
        io_log("dmemshare parent transport open errno=%d dmem_fd=%d", errno, dmem_fd);
        goto complete;
    }
    const struct local_process_param param = {
        .size = sizeof(param), .socket_fd = pair[1], .field_8 = 1, .field_c = -1, .field_10 = 1,
    };
    int32_t handle = sceSystemServiceAddLocalProcessForPsmKit(path, argv, &param);
    int add_errno = errno;
    close(pair[1]);
    pair[1] = -1;
    io_log("dmemshare parent add result=%#x errno=%d dmem_fd=%d", (unsigned)handle, add_errno, dmem_fd);
    if (handle < 0) goto complete;

    struct ds_request request = {.magic = DS_MAGIC, .pages = DS_PAGES, .physical = physical};
    int64_t deadline = io_now_ms() + 20000;
    if (send_descriptor(pair[0], dmem_fd, &request))
    {
        io_log("dmemshare parent send descriptor errno=%d", errno);
        goto complete;
    }
    struct ds_reply reply = {0};
    if (io_transfer(pair[0], &reply, sizeof(reply), 0, deadline))
    {
        io_log("dmemshare parent receive reply errno=%d", errno);
        goto complete;
    }
    io_log("dmemshare parent reply map=%#x own_errno=%d passed_errno=%d "
           "matches=%u/%u/%u written=%u flexible=%lld/%lld/%lld",
           (unsigned)reply.map_result, reply.own_dmem_errno, reply.passed_dmem_errno,
           reply.map_matches, reply.own_dmem_matches, reply.passed_dmem_matches, reply.written,
           (long long)reply.flexible_before, (long long)reply.flexible_mapped,
           (long long)reply.flexible_after);

    size_t last = DS_PAGES - 1;
    int64_t coherence_deadline = io_now_ms() + 2000;
    while (bytes[last * DS_PAGE + DS_PAGE - 1024] != ds_child_byte(last) &&
           io_now_ms() < coherence_deadline) usleep(50000);
    uint32_t child = 0;
    for (size_t page = 0; page < DS_PAGES; page++)
    {
        size_t i;
        for (i = 0; i < DS_PAGE; i += 1024)
            if (bytes[page * DS_PAGE + i] != ds_child_byte(page)) break;
        if (i == DS_PAGE) child++;
    }
    io_log("dmemshare parent sees the child's byte in %u/%zu pages flexible=%lld", child, DS_PAGES,
           flexible());
    passed = reply.magic == DS_MAGIC && reply.passed_dmem_errno == 0 &&
             reply.passed_dmem_matches == DS_PAGES && reply.written == DS_PAGES &&
             child == DS_PAGES;
complete:
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    if (dmem_fd >= 0) close(dmem_fd);
    if (address) sceKernelMunmap(address, DS_BYTES);
    if (physical >= 0) sceKernelReleaseDirectMemory(physical, DS_BYTES);
    io_log("dmemshare parent complete result=%s", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
