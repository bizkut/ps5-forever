#include <time.h>
#include <unistd.h>

#include "probe.h"
#include "psmkit-io.h"
#include "../processbroker/client-core.h"

static void title_log(void *context, const char *line)
{
    (void)context;
    io_log("process-broker-title %s", line);
}

int main(void)
{
    struct app_status identity = {0};
    int identity_status = get_app_status(&identity);
    print_identity("process-broker-title", &identity);
    const struct pb_client_config config = {
        .port = PB_PORT,
        .timeout_ms = 15000,
        .log = title_log,
    };
    struct pb_client_result result = {0};
    int status = identity_status < 0 ? -1 : pb_client_run(&config, &result);
    int passed = !status && result.passed && result.workers_ready == PB_WORKERS &&
                 result.workers_stopped == PB_WORKERS && result.workers_reaped == PB_WORKERS;
    io_log("process-broker-title complete result=%s pool_epoch=%#llx title_epoch=%#llx broker_pid=%u workers_ready=%u workers_stopped=%u workers_reaped=%u listener_closed=%u control_close=REQUIRED",
           passed ? "BROKER_LIFECYCLE_PASS" : "FAIL_OR_INCONCLUSIVE",
           (unsigned long long)result.pool_epoch, (unsigned long long)result.title_epoch,
           result.broker_pid, result.workers_ready, result.workers_stopped,
           result.workers_reaped, result.listener_closed);
    int64_t hold_end = pb_now_ms() + 15000;
    for (;;) {
        int64_t now = pb_now_ms();
        if (now < 0 || now >= hold_end) break;
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    _exit(passed ? 0 : 1);
}
