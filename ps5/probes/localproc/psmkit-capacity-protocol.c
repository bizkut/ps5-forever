#include <string.h>
#include "psmkit-capacity.h"

void cap_request(struct cap_frame *frame, unsigned index, uint64_t seed,
                 uint32_t parent_pid, uint32_t app_id)
{
    *frame = (struct cap_frame){.magic = CAP_MAGIC, .version = CAP_VERSION,
        .kind = CAP_REQUEST, .index = index, .nonce = seed + index + 1,
        .parent_pid = parent_pid, .app_id = app_id};
    /* Unique binary cookies, not credentials: full run nonce plus index. */
    memcpy(frame->cookie, "CAP1", 4);
    memcpy(frame->cookie + 4, &frame->nonce, 8);
    uint32_t wire_index = index;
    memcpy(frame->cookie + 12, &wire_index, 4);
}

int cap_request_valid(const struct cap_frame *frame)
{
    struct cap_frame expected;
    if (frame->index >= CAP_LIMIT || !frame->nonce || !frame->parent_pid || !frame->app_id)
        return 0;
    cap_request(&expected, frame->index, frame->nonce - frame->index - 1,
                frame->parent_pid, frame->app_id);
    return !memcmp(frame, &expected, sizeof(expected));
}

int cap_echo(const struct cap_frame *reply, const struct cap_frame *request, unsigned kind)
{
    struct cap_frame expected = *request;
    expected.kind = kind;
    expected.pid = reply->pid;
    return reply->pid && reply->pid != request->parent_pid &&
           !memcmp(reply, &expected, sizeof(expected));
}

