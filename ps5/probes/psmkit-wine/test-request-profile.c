#define __WINESRC__ 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "ntuser.h"
/* Some generated wire headers repeat this user-only flag with another literal.
 * The request-layout test does not use it; avoid an incidental redefinition. */
#undef SEND_HWMSG_INJECTED
#include "wine/server_protocol.h"
#include "wine/ps5_request_profile.h"

int main(void)
{
    static const int supported[] = {REQ_select, REQ_event_op, REQ_remove_completion,
                                    REQ_add_completion, REQ_set_queue_mask};
    union generic_request req, before;
    unsigned int i;
    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
    {
        memset(&req, 0xa5, sizeof(req));
        req.request_header.req = supported[i];
        before = req;
        assert(!wine_ps5_request_profile_started(&req));
        wine_ps5_request_profile_stamp(&req, UINT64_C(1234567890123));
        assert(!memcmp(&req, &before, 48));
        assert(wine_ps5_request_profile_started(&req) == UINT64_C(1234567890123));
        ((unsigned char *)&req)[63] ^= 1;
        assert(!wine_ps5_request_profile_started(&req));
    }
    memset(&req, 0, sizeof(req));
    req.request_header.req = REQ_create_file;
    before = req;
    wine_ps5_request_profile_stamp(&req, 123);
    assert(!memcmp(&req, &before, sizeof(req)));
    assert(!wine_ps5_request_profile_started(&req));
    req.request_header.req = -1;
    assert(!wine_ps5_request_profile_supported(req.request_header.req));
    req.request_header.req = REQ_NB_REQUESTS;
    assert(!wine_ps5_request_profile_supported(req.request_header.req));
    puts("REQUEST_PROFILE_HOST_PASS exact_wire_layout=1 payload_unchanged=1 unsupported_unchanged=1");
    return 0;
}
