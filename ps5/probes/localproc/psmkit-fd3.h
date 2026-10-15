#ifndef PS5_PSMKIT_FD3_H
#define PS5_PSMKIT_FD3_H

#include "psmkit-socket.h"

/* Export 4ZYuSI8i2aM's statically verified terminal body is mov eax,3; ret.
 * Treat fd3 as an ABI candidate, never as proof of a usable parent socket.
 * Acceptance is structural validation and the exact bidirectional cookie,
 * not a firmware-number branch. Runtime qualification is 5.50 only. */
#define FD3_PARENT_SOCKET 3

void fd3_log_firmware(const char *role);
int fd3_within_deadline(int64_t deadline);

#endif
