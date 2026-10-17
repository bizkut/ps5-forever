#ifndef PS5_TITLE_ADDONFORGE_CLIENT_H
#define PS5_TITLE_ADDONFORGE_CLIENT_H

#include <stddef.h>
#include "forever-menu.h"

#define ADDONFORGE_RESPONSE_BYTES (512u * 1024u)
#define ADDONFORGE_CORE_PORT 31981u
#define ADDONFORGE_CONTROL_PORT 31980u

struct addonforge_client
{
    unsigned short port;
    const char *token;
    unsigned timeout_ms;
};

int addonforge_http_health(const struct addonforge_client *client);
int addonforge_http_command(const struct addonforge_client *client, const char *command,
                            const char *arguments, char *response, size_t response_size);
int addonforge_parse_snapshot(enum forever_addon_tab tab, const char *response,
                              struct forever_addon_snapshot *snapshot,
                              char *error, size_t error_size);
int addonforge_response_error(const char *response, char *error, size_t error_size);
void addonforge_plain_error(const char *technical, char *plain, size_t size);

#endif
