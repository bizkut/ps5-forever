#include "writebroker-autostart.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_PROBES 64

struct fake_broker
{
    int64_t now;
    int probe_results[FAKE_PROBES];
    int probe_errors[FAKE_PROBES];
    int probe_advances[FAKE_PROBES];
    int probe_count;
    int probe_limit;
    int64_t probe_deadlines[FAKE_PROBES];
    int send_result;
    int send_error;
    int send_advance;
    size_t send_bytes;
    int send_count;
    int64_t send_deadline;
    int wait_error;
    int wait_count;
};

static int64_t fake_now(void *context)
{
    return ((struct fake_broker *)context)->now;
}

static int fake_probe(void *context, int64_t deadline, int *error)
{
    struct fake_broker *fake = context;
    int index = fake->probe_count++;
    int result = index < fake->probe_limit ? fake->probe_results[index] : 0;

    assert(index < FAKE_PROBES);
    assert(deadline >= fake->now);
    fake->probe_deadlines[index] = deadline;
    fake->now += fake->probe_advances[index];
    if (fake->now > deadline) fake->now = deadline;
    *error = fake->probe_errors[index];
    return result;
}

static int fake_send(void *context, int64_t deadline, size_t *sent, int *error)
{
    struct fake_broker *fake = context;

    assert(!fake->send_count++);
    assert(deadline >= fake->now);
    fake->send_deadline = deadline;
    fake->now += fake->send_advance;
    if (fake->now > deadline) fake->now = deadline;
    *sent = fake->send_bytes;
    *error = fake->send_error;
    return fake->send_result;
}

static int fake_wait(void *context, int64_t deadline, int *error)
{
    struct fake_broker *fake = context;

    assert(deadline >= fake->now);
    fake->wait_count++;
    fake->now = deadline;
    *error = fake->wait_error;
    return fake->wait_error ? -1 : 0;
}

static const struct pw_writebroker_ops fake_ops =
{
    fake_now, fake_probe, fake_send, fake_wait,
};

static struct fake_broker fresh_fake(void)
{
    struct fake_broker fake;

    memset(&fake, 0, sizeof(fake));
    fake.send_result = PW_WRITEBROKER_SEND_OK;
    fake.send_bytes = 111584;
    return fake;
}

int main(void)
{
    struct fake_broker fake = fresh_fake();
    struct pw_writebroker_outcome outcome;

    fake.probe_limit = 1;
    fake.probe_results[0] = 1;
    outcome = pw_writebroker_autostart(&fake_ops, &fake);
    assert(outcome.status == PW_WRITEBROKER_PRESENT && !outcome.error);
    assert(fake.probe_count == 1 && fake.probe_deadlines[0] == PW_WRITEBROKER_PROBE_MS);
    assert(!fake.send_count && !fake.wait_count);

    fake = fresh_fake();
    fake.probe_limit = 3;
    fake.probe_results[0] = 0;
    fake.probe_results[1] = 0;
    fake.probe_results[2] = 1;
    outcome = pw_writebroker_autostart(&fake_ops, &fake);
    assert(outcome.status == PW_WRITEBROKER_STARTED && !outcome.error);
    assert(outcome.sent == fake.send_bytes && fake.send_deadline == PW_WRITEBROKER_ELFLDR_MS);
    assert(fake.probe_deadlines[1] == PW_WRITEBROKER_POLL_MS);
    assert(fake.probe_deadlines[2] == 2 * PW_WRITEBROKER_POLL_MS);
    assert(fake.wait_count == 1 && fake.now == PW_WRITEBROKER_POLL_MS);

    fake = fresh_fake();
    fake.probe_results[0] = 0;
    fake.send_result = PW_WRITEBROKER_SEND_ELFLDR_UNAVAILABLE;
    fake.send_error = ECONNREFUSED;
    fake.send_bytes = 0;
    outcome = pw_writebroker_autostart(&fake_ops, &fake);
    assert(outcome.status == PW_WRITEBROKER_ELFLDR_UNAVAILABLE);
    assert(outcome.error == ECONNREFUSED && !outcome.sent && !fake.wait_count);

    fake = fresh_fake();
    fake.probe_limit = 1;
    fake.probe_results[0] = -1;
    fake.probe_errors[0] = EACCES;
    outcome = pw_writebroker_autostart(&fake_ops, &fake);
    assert(outcome.status == PW_WRITEBROKER_ERROR && outcome.error == EACCES);
    assert(!fake.send_count);

    fake = fresh_fake();
    fake.probe_results[0] = 0;
    fake.send_result = PW_WRITEBROKER_SEND_ERROR;
    fake.send_error = EPIPE;
    fake.send_bytes = 4096;
    outcome = pw_writebroker_autostart(&fake_ops, &fake);
    assert(outcome.status == PW_WRITEBROKER_ERROR && outcome.error == EPIPE);
    assert(outcome.sent == 4096);

    fake = fresh_fake();
    fake.probe_results[0] = 0;
    outcome = pw_writebroker_autostart(&fake_ops, &fake);
    assert(outcome.status == PW_WRITEBROKER_TIMEOUT && outcome.error == ETIMEDOUT);
    assert(fake.now == PW_WRITEBROKER_START_MS);
    assert(fake.probe_count == 31 && fake.wait_count == 30);

    fake = fresh_fake();
    fake.probe_results[0] = 0;
    fake.probe_advances[0] = PW_WRITEBROKER_PROBE_MS;
    fake.send_advance = PW_WRITEBROKER_ELFLDR_MS;
    outcome = pw_writebroker_autostart(&fake_ops, &fake);
    assert(outcome.status == PW_WRITEBROKER_TIMEOUT && outcome.error == ETIMEDOUT);
    assert(fake.send_deadline == PW_WRITEBROKER_PROBE_MS + PW_WRITEBROKER_ELFLDR_MS);
    assert(fake.now == PW_WRITEBROKER_TOTAL_MS);
    assert(fake.probe_count == 29 && fake.wait_count == 28);

    assert(!strcmp(pw_writebroker_status_name(PW_WRITEBROKER_STARTED), "started"));
    assert(!strcmp(pw_writebroker_status_name(PW_WRITEBROKER_ELFLDR_UNAVAILABLE),
                   "elfldr_unavailable"));
    puts("write broker autostart: present/start/error, 3-second readiness and 4-second cap PASS");
    return 0;
}
