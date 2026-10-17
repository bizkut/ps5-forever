#ifndef PW_WRITEBROKER_AUTOSTART_H
#define PW_WRITEBROKER_AUTOSTART_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define PW_WRITEBROKER_PORT 9141
#define PW_WRITEBROKER_ELFLDR_PORT 9021
#define PW_WRITEBROKER_ELF_MAX_BYTES (1024u * 1024u)
#define PW_WRITEBROKER_PROBE_MS 200
#define PW_WRITEBROKER_ELFLDR_MS 1000
/* Startup waits for the full measured 3.1-5.7 s ELF-loader range, with
 * margin for the broker's bounded permission traversal before it publishes
 * its token and accepts clients. */
#define PW_WRITEBROKER_START_MS 10000
#define PW_WRITEBROKER_POLL_MS 100
#define PW_WRITEBROKER_TOTAL_MS 12000

enum pw_writebroker_status
{
    PW_WRITEBROKER_PRESENT,
    PW_WRITEBROKER_STARTED,
    PW_WRITEBROKER_ELFLDR_UNAVAILABLE,
    PW_WRITEBROKER_TIMEOUT,
    PW_WRITEBROKER_ERROR,
};

enum pw_writebroker_send_result
{
    PW_WRITEBROKER_SEND_OK,
    PW_WRITEBROKER_SEND_ELFLDR_UNAVAILABLE,
    PW_WRITEBROKER_SEND_ERROR = -1,
};

struct pw_writebroker_ops
{
    int64_t (*now_ms)(void *context);
    /* probe returns 1 when connected, 0 only for ECONNREFUSED, and -1 for an error. */
    int (*probe)(void *context, int64_t deadline, int *error);
    int (*send_elf)(void *context, int64_t deadline, size_t *sent, int *error);
    int (*wait_until)(void *context, int64_t deadline, int *error);
};

struct pw_writebroker_outcome
{
    enum pw_writebroker_status status;
    size_t sent;
    int error;
};

static inline int pw_writebroker_claim_repair_restart(atomic_int *claimed)
{
    int expected = 0;
    if (!claimed) return -EINVAL;
    return atomic_compare_exchange_strong(claimed, &expected, 1) ? 0 : -EALREADY;
}

static inline int64_t pw_writebroker_min_deadline(int64_t first, int64_t second)
{
    return first < second ? first : second;
}

static inline struct pw_writebroker_outcome pw_writebroker_autostart(
    const struct pw_writebroker_ops *ops, void *context)
{
    struct pw_writebroker_outcome outcome = {PW_WRITEBROKER_ERROR, 0, EINVAL};
    int64_t start, now, total_deadline, poll_deadline;
    int state, error = 0;

    if (!ops || !ops->now_ms || !ops->probe || !ops->send_elf || !ops->wait_until)
        return outcome;
    if ((start = ops->now_ms(context)) < 0)
    {
        outcome.error = EIO;
        return outcome;
    }
    total_deadline = start + PW_WRITEBROKER_TOTAL_MS;
    state = ops->probe(context, pw_writebroker_min_deadline(
                           start + PW_WRITEBROKER_PROBE_MS, total_deadline), &error);
    if (state == 1)
    {
        outcome.status = PW_WRITEBROKER_PRESENT;
        outcome.error = 0;
        return outcome;
    }
    if (state < 0)
    {
        outcome.error = error ? error : EIO;
        return outcome;
    }

    if ((now = ops->now_ms(context)) < 0)
    {
        outcome.error = EIO;
        return outcome;
    }
    if (now >= total_deadline)
    {
        outcome.status = PW_WRITEBROKER_TIMEOUT;
        outcome.error = ETIMEDOUT;
        return outcome;
    }
    error = 0;
    state = ops->send_elf(context, pw_writebroker_min_deadline(
                              now + PW_WRITEBROKER_ELFLDR_MS, total_deadline),
                          &outcome.sent, &error);
    if (state == PW_WRITEBROKER_SEND_ELFLDR_UNAVAILABLE)
    {
        outcome.status = PW_WRITEBROKER_ELFLDR_UNAVAILABLE;
        outcome.error = error ? error : ECONNREFUSED;
        return outcome;
    }
    if (state != PW_WRITEBROKER_SEND_OK)
    {
        outcome.error = error ? error : EIO;
        return outcome;
    }

    if ((now = ops->now_ms(context)) < 0)
    {
        outcome.error = EIO;
        return outcome;
    }
    if (now >= total_deadline)
    {
        outcome.status = PW_WRITEBROKER_TIMEOUT;
        outcome.error = ETIMEDOUT;
        return outcome;
    }
    poll_deadline = pw_writebroker_min_deadline(now + PW_WRITEBROKER_START_MS,
                                                total_deadline);
    for (;;)
    {
        int64_t step_deadline;

        if (now >= poll_deadline)
        {
            outcome.status = PW_WRITEBROKER_TIMEOUT;
            outcome.error = ETIMEDOUT;
            return outcome;
        }
        step_deadline = pw_writebroker_min_deadline(now + PW_WRITEBROKER_POLL_MS,
                                                    poll_deadline);
        error = 0;
        state = ops->probe(context, step_deadline, &error);
        if (state == 1)
        {
            outcome.status = PW_WRITEBROKER_STARTED;
            outcome.error = 0;
            return outcome;
        }
        if (state < 0)
        {
            outcome.error = error ? error : EIO;
            return outcome;
        }
        if ((now = ops->now_ms(context)) < 0)
        {
            outcome.error = EIO;
            return outcome;
        }
        if (now < step_deadline)
        {
            error = 0;
            if (ops->wait_until(context, step_deadline, &error))
            {
                outcome.error = error ? error : EIO;
                return outcome;
            }
            if ((now = ops->now_ms(context)) < 0)
            {
                outcome.error = EIO;
                return outcome;
            }
        }
    }
}

static inline const char *pw_writebroker_status_name(enum pw_writebroker_status status)
{
    switch (status)
    {
        case PW_WRITEBROKER_PRESENT: return "present";
        case PW_WRITEBROKER_STARTED: return "started";
        case PW_WRITEBROKER_ELFLDR_UNAVAILABLE: return "elfldr_unavailable";
        case PW_WRITEBROKER_TIMEOUT: return "timeout";
        default: return "error";
    }
}

#endif
