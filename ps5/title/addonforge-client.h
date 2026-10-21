#ifndef PS5_TITLE_ADDONFORGE_CLIENT_H
#define PS5_TITLE_ADDONFORGE_CLIENT_H

#include <stddef.h>
#include "forever-menu.h"

#define ADDONFORGE_RESPONSE_BYTES (2u * 1024u * 1024u)
#define ADDONFORGE_CORE_PORT 31981u
#define ADDONFORGE_CONTROL_PORT 31980u

#define ADDONFORGE_WAGO_KEY_BYTES 257
#define ADDONFORGE_WAGO_KEY_FILE "/data/forever/prefix/drive_c/Forever/Addons/wago-key.txt"
struct addonforge_client
{
    unsigned short port;
    const char *token;
    unsigned timeout_ms;
};

int addonforge_http_health(const struct addonforge_client *client);
int addonforge_http_command(const struct addonforge_client *client, const char *command,
                            const char *arguments, char *response, size_t response_size);
int addonforge_install_github(const struct addonforge_client *client, const char *repo,
                              char *response, size_t response_size);
int addonforge_parse_snapshot(enum forever_addon_tab tab, const char *response,
                              struct forever_addon_snapshot *snapshot,
                              char *error, size_t error_size);
int addonforge_response_error(const char *response, char *error, size_t error_size);
int addonforge_parse_progress(const char *response, char *message, size_t size);
int addonforge_install_arguments(const char *path, char *arguments, size_t size);
void addonforge_plain_error(const char *technical, char *plain, size_t size);
int addonforge_parse_wago_status(const char *response, enum forever_wago_key_status *status);
int addonforge_wago_key_status(const struct addonforge_client *client,
                               enum forever_wago_key_status *status);
/* An empty key clears it. Responses and errors never include the key. */
int addonforge_set_wago_key(const struct addonforge_client *client, const char *key,
                            enum forever_wago_key_status *status);
int addonforge_read_wago_key_file(const char *path, char *key, size_t size);
int addonforge_set_wago_key_file(const struct addonforge_client *client, const char *path,
                                 enum forever_wago_key_status *status);
void addonforge_snapshot_wago_status(struct forever_addon_snapshot *snapshot,
                                      enum forever_wago_key_status status);

#endif
