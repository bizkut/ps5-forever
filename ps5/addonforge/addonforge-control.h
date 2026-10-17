#ifndef FOREVER_ADDONFORGE_CONTROL_H
#define FOREVER_ADDONFORGE_CONTROL_H

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define ADDONFORGE_CONTROL_SECRET_HEX 64
#define ADDONFORGE_CORE_TOKEN_MAX 128

enum addonforge_control_action
{
    ADDONFORGE_CONTROL_START,
    ADDONFORGE_CONTROL_STOP,
};

struct addonforge_control_request
{
    enum addonforge_control_action action;
    char core_token[ADDONFORGE_CORE_TOKEN_MAX + 1];
};

enum addonforge_shutdown_delivery
{
    ADDONFORGE_SHUTDOWN_UNDELIVERED,
    ADDONFORGE_SHUTDOWN_DELIVERED,
    ADDONFORGE_SHUTDOWN_ACKNOWLEDGED,
};

static inline int addonforge_shutdown_should_wait(enum addonforge_shutdown_delivery delivery)
{
    return delivery != ADDONFORGE_SHUTDOWN_UNDELIVERED;
}

static inline unsigned addonforge_deadline_remaining(uint64_t now, uint64_t deadline)
{
    uint64_t remaining;
    if (now >= deadline) return 0;
    remaining = deadline - now;
    return remaining > 0xffffffffu ? 0xffffffffu : (unsigned)remaining;
}

static inline int addonforge_hex_token(const char *text, size_t minimum, size_t maximum)
{
    size_t length = text ? strlen(text) : 0;
    if (length < minimum || length > maximum) return 0;
    for (size_t i = 0; i < length; i++)
        if (!isxdigit((unsigned char)text[i])) return 0;
    return 1;
}

static inline int addonforge_control_secret_available(const char *secret, size_t length)
{
    return length == ADDONFORGE_CONTROL_SECRET_HEX &&
           addonforge_hex_token(secret, ADDONFORGE_CONTROL_SECRET_HEX,
                                ADDONFORGE_CONTROL_SECRET_HEX);
}

static inline int addonforge_secret_equal(const char *left, const char *right)
{
    unsigned difference = 0;
    if (!left || !right || strlen(left) != ADDONFORGE_CONTROL_SECRET_HEX ||
        strlen(right) != ADDONFORGE_CONTROL_SECRET_HEX) return 0;
    for (size_t i = 0; i < ADDONFORGE_CONTROL_SECRET_HEX; i++)
        difference |= (unsigned char)left[i] ^ (unsigned char)right[i];
    return difference == 0;
}

static inline int addonforge_control_parse(
    char *line, const char *expected_secret, struct addonforge_control_request *request)
{
    char *secret, *action, *argument = NULL, *space;
    if (!line || !request || strncmp(line, "AUTH ", 5)) return -EINVAL;
    secret = line + 5;
    if (!(space = strchr(secret, ' '))) return -EINVAL;
    *space = 0;
    action = space + 1;
    if ((space = strchr(action, ' '))) { *space = 0; argument = space + 1; }
    if (!addonforge_secret_equal(secret, expected_secret)) return -EACCES;
    if (!strcmp(action, "STOP") && !argument)
    {
        memset(request, 0, sizeof(*request));
        request->action = ADDONFORGE_CONTROL_STOP;
        return 0;
    }
    if (!strcmp(action, "START") && argument &&
        addonforge_hex_token(argument, 32, ADDONFORGE_CORE_TOKEN_MAX))
    {
        memset(request, 0, sizeof(*request));
        request->action = ADDONFORGE_CONTROL_START;
        memcpy(request->core_token, argument, strlen(argument) + 1);
        return 0;
    }
    return -EINVAL;
}

#endif
