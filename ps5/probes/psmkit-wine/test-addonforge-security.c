#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../../addonforge/addonforge-control.h"
#include "../../title/addonforge-security.h"

static int fill_ok(uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) bytes[i] = (uint8_t)i;
    return 0;
}

static int fill_fail(uint8_t *bytes, size_t size)
{
    (void)bytes; (void)size;
    return -1;
}

int main(void)
{
    static const char secret[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char token[] = "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd";
    struct addonforge_control_request request;
    char line[256], output[65];

    snprintf(line, sizeof(line), "AUTH %s START %s", secret, token);
    assert(!addonforge_control_parse(line, secret, &request));
    assert(request.action == ADDONFORGE_CONTROL_START && !strcmp(request.core_token, token));
    snprintf(line, sizeof(line), "AUTH %s STOP", secret);
    assert(!addonforge_control_parse(line, secret, &request));
    assert(request.action == ADDONFORGE_CONTROL_STOP);
    snprintf(line, sizeof(line), "AUTH %064d STOP", 0);
    assert(addonforge_control_parse(line, secret, &request) == -EACCES);
    assert(addonforge_control_parse("STOP", secret, &request) == -EINVAL);
    snprintf(line, sizeof(line), "AUTH %s START short", secret);
    assert(addonforge_control_parse(line, secret, &request) == -EINVAL);
    assert(addonforge_control_secret_available(secret, strlen(secret)));
    assert(!addonforge_control_secret_available(NULL, 0));
    assert(!addonforge_control_secret_available(secret, strlen(secret) - 1));

    assert(!forever_entropy_hex(output, sizeof(output), 32, fill_ok));
    assert(!strcmp(output,
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
    memset(output, 'x', sizeof(output));
    assert(addonforge_deadline_remaining(1000, 3000) == 2000);
    assert(addonforge_deadline_remaining(3000, 3000) == 0);
    assert(addonforge_deadline_remaining(4000, 3000) == 0);
    assert(!addonforge_shutdown_should_wait(ADDONFORGE_SHUTDOWN_UNDELIVERED));
    assert(addonforge_shutdown_should_wait(ADDONFORGE_SHUTDOWN_DELIVERED));
    assert(addonforge_shutdown_should_wait(ADDONFORGE_SHUTDOWN_ACKNOWLEDGED));
    assert(forever_entropy_hex(output, sizeof(output), 32, NULL) == -EINVAL);
    assert(forever_entropy_hex(output, sizeof(output), 32, fill_fail) == -EIO);
    assert(forever_entropy_hex(output, sizeof(output) - 1, 32, fill_ok) == -EINVAL);
    puts("addonforge control authentication and fail-closed entropy: PASS");
    return 0;
}
