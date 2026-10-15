#ifndef PS5_LOCALPROC_PROBE_H
#define PS5_LOCALPROC_PROBE_H

#include <stdint.h>

/* As firmware 5.50's NKWeb and GLSlim launchers fill it (0x90 bytes). */
struct local_process_param
{
    uint32_t size;
    int32_t socket_fd;
    int32_t field_8;
    int32_t field_c;
    uint32_t field_10;
    uint32_t reserved_14;
    uint64_t field_18;
    uint32_t field_20;
    uint8_t reserved_24[0x90 - 0x24];
};

struct app_status
{
    int32_t fields[4];
};

void probe_log(const char *format, ...);
int get_app_status(struct app_status *status);
void print_identity(const char *role, const struct app_status *status);

#endif
