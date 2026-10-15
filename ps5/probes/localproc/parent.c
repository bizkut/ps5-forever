#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include "probe.h"

int32_t sceSystemServiceAddLocalProcess(int32_t app_id, const char *path,
                                        const char *const argv[],
                                        const struct local_process_param *param);

_Static_assert(sizeof(struct local_process_param) == 0x90, "local process parameter size");
_Static_assert(sizeof(struct app_status) == 16, "app status size");

int main(void)
{
    static const char path[] = "/app0/localproc-child.bin";
    const char *const argv[] = {path, "identity", NULL};
    const struct local_process_param param = {
        .size = sizeof(param),
        .socket_fd = -1,
        .field_8 = 1,
        .field_c = -1,
        .field_10 = 1,
    };
    struct app_status status;

    int status_result = get_app_status(&status);
    probe_log("parent get app status result=%#x\n", (unsigned)status_result);
    print_identity("parent", &status);
    if (status_result < 0 || status.fields[0] <= 0)
    {
        probe_log("parent refusing launch without a valid app id\n");
        return 1;
    }

    errno = 0;
    int32_t result = sceSystemServiceAddLocalProcess(status.fields[0], path, argv, &param);
    probe_log("parent add local process app=%#x path=%s result=%#x errno=%d\n",
              (unsigned)status.fields[0], path, (unsigned)result, errno);
    if (result < 0) return 1;

    for (int seconds = 0; seconds < 20; seconds += 2)
    {
        probe_log("parent waiting for child seconds=%d\n", seconds);
        sleep(2);
    }
    probe_log("parent complete\n");
    return 0;
}
