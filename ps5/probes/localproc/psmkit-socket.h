#ifndef PS5_PSMKIT_SOCKET_H
#define PS5_PSMKIT_SOCKET_H

/* Reuse the IO gate's exact versioned request, cookie and bounded transfers.
 * This target never calls its direct-memory/checksum test. */
#include "psmkit-io.h"

struct socket_reply
{
    struct io_request echo;
    int32_t socket_status;
};

_Static_assert(sizeof(struct socket_reply) == 28, "socket reply wire size");

#endif
