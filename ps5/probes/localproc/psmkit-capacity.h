#ifndef PS5_PSMKIT_CAPACITY_H
#define PS5_PSMKIT_CAPACITY_H

#include "psmkit-io.h"

#define CAP_MAGIC UINT32_C(0x50434150)
#define CAP_VERSION 1
#define CAP_LIMIT 64
#define CAP_LAUNCH_MS 80000
#define CAP_PROTOCOL_MS 105000
#define CAP_HOLD_MS 15000
#define CAP_CHILD_MS 120000
#define CAP_SOCKET 3

enum cap_kind { CAP_REQUEST = 1, CAP_READY, CAP_STOP, CAP_DONE };
enum cap_category { CAP_NONE, CAP_LAUNCH, CAP_SOCKETPAIR, CAP_READY_ERROR, CAP_TIME, CAP_RESOURCE };
struct cap_frame {
    uint32_t magic, version, kind, index;
    uint64_t nonce;
    unsigned char cookie[16];
    uint32_t parent_pid, app_id, pid;
    int32_t status;
};
_Static_assert(sizeof(struct cap_frame) == 56, "capacity wire size");
struct cap_slot {
    int fd, child_fd, started, ready, stop, done, eof;
    int32_t handle;
    uint32_t pid;
    struct cap_frame request;
};
struct cap_result {
    struct cap_slot slots[CAP_LIMIT];
    unsigned calls, started, highest_ready;
    int failed_index, status, error, cleanup_ok, ceiling;
    enum cap_category category;
};
/* Native adapters and host simulations exercise the same bounded controller. */
struct cap_ops {
    int64_t (*now)(void);
    int (*pair)(int pair[2]);
    int32_t (*launch)(int child_fd);
    int (*transfer)(int fd, void *frame, size_t size, int writing, int64_t deadline);
    int (*shutdown_write)(int fd);
    int (*close_fd)(int fd);
    int (*stats)(const char *phase, unsigned ready, int64_t deadline);
    int (*live)(int fd);
    int (*held)(const struct cap_result *result, int64_t deadline);
};
void cap_request(struct cap_frame *frame, unsigned index, uint64_t seed,
                 uint32_t parent_pid, uint32_t app_id);
int cap_request_valid(const struct cap_frame *frame);
int cap_echo(const struct cap_frame *reply, const struct cap_frame *request, unsigned kind);
const char *cap_category_name(enum cap_category category);
void cap_run(struct cap_result *result, const struct cap_ops *ops, uint64_t seed,
             uint32_t parent_pid, uint32_t app_id, int64_t start);
#endif
