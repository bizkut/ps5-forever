#include <stdint.h>
#include <unistd.h>
#include "probe.h"
extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const local_process_libc_requirement =
    &Need_sceLibcInternal;


int main(void)
{
    struct app_status status;

    int result = get_app_status(&status);
    probe_log("child get app status result=%#x\n", (unsigned)result);
    print_identity("child", &status);
    probe_log("child identity complete; no explicit memory allocation\n");
    sleep(15);
    return result < 0 ? 1 : 0;
}
