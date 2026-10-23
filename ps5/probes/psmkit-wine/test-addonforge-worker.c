/* Execute the production worker/refresh path with controlled service replies. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include "wow-channels.h"
#include "../../title/addonforge-client.h"
#define PW_RUNTIME_PREFIX "/unused-worker-fixture"
struct forever_addons_request
{
    unsigned generation;
    enum forever_menu_command command;
    unsigned channel;
    enum forever_addon_tab tab;
    char key[ADDONFORGE_WAGO_KEY_BYTES];
};
struct forever_addons_progress_poll
{
    struct addonforge_client client;
    atomic_int done;
};
static struct
{
    pthread_mutex_t lock, lifecycle_lock;
    struct forever_addons_request request;
    struct forever_addon_snapshot snapshot;
    struct forever_addon_install channels[FOREVER_ADDON_CHANNELS];
    size_t channel_count;
    char response[ADDONFORGE_RESPONSE_BYTES], token[65], worker_token[65];
    unsigned generation;
    enum forever_wago_key_status wago_status;
    int busy;
} forever_addons = {
    .lock = PTHREAD_MUTEX_INITIALIZER, .lifecycle_lock = PTHREAD_MUTEX_INITIALIZER,
};
static struct forever_menu_state menu;
static struct forever_addon_snapshot published;
static unsigned publications, mutations, mutation_timeout;
static int mutation_error;
static atomic_int progress_polls;
static char progress_status[FOREVER_MENU_STATUS_BYTES];
static const char *fixture_channel;
static int forever_addons_current(unsigned generation) { return generation == forever_addons.generation; }
static int forever_addons_control(const char *command) { assert(command); return 0; }
static void io_log(const char *format, ...) { (void)format; }
static int pw_title_addon_worker_start(const char token[64])
{
    assert(token);
    return 0;
}
static int pw_title_addon_worker_stop(const char token[64])
{
    assert(token);
    return 0;
}
static void title_desktop_addons_snapshot(const struct forever_addon_snapshot *snapshot)
{
    ++publications;
    published = *snapshot;
    forever_menu_addon_snapshot(&menu, snapshot);
}
static void title_desktop_addons_progress(const char *text)
{
    assert(text);
    snprintf(progress_status, sizeof(progress_status), "%s", text);
}
static int forever_addons_command(struct addonforge_client *client, const char *command,
    const char *arguments, unsigned timeout_ms, char *plain, size_t plain_size)
{
    (void)client;
    (void)timeout_ms;
    if (!strcmp(command, "set_install_path"))
    {
        assert(strstr(arguments, fixture_channel));
        if (!strcmp(fixture_channel, "_newthing_"))
        {
            snprintf(plain, plain_size, "Add-ons aren't supported for this WoW version yet");
            return -ENOTSUP;
        }
        return 0;
    }
    const char *response = !strcmp(command, "catalog_list") ?
        "{\"ok\":true,\"result\":{\"rows\":[],\"notice\":\"No catalogue data for this WoW flavour. Use Install from GitHub in Find.\"}}" :
        !strcmp(command, "bundles") ?
        "{\"ok\":true,\"result\":{\"rows\":[],\"notice\":\"No starter packs for this WoW flavour. Use Install from GitHub in Find.\"}}" :
        "{\"ok\":true,\"result\":[]}";
    snprintf(forever_addons.response, sizeof(forever_addons.response), "%s", response);
    return 0;
}
static enum forever_wago_key_status service_wago_status;
static int fixture_set_wago_key(const struct addonforge_client *client, const char *key,
                                enum forever_wago_key_status *status)
{
    assert(client && key);
    ++mutations;
    if (mutation_error) { *status = FOREVER_WAGO_REJECTED; return mutation_error; }
    *status = service_wago_status = *key ? FOREVER_WAGO_SET : FOREVER_WAGO_NOT_SET;
    return 0;
}
static int fixture_set_wago_key_file(const struct addonforge_client *client, const char *path,
                                     enum forever_wago_key_status *status)
{
    assert(path);
    return fixture_set_wago_key(client, "file-key", status);
}
static int fixture_install_github(const struct addonforge_client *client, const char *repo,
                                   char *response, size_t size)
{
    assert(client && !strcmp(repo, "owner/repo"));
    mutation_timeout = client->timeout_ms;
    ++mutations;
    snprintf(response, size, "%s", mutation_error ?
        "{\"ok\":false,\"error\":\"Fixture mutation failed.\"}" :
        "{\"ok\":true,\"result\":{\"key\":\"github:owner/repo\"}}");
    return 0;
}
static int fixture_wago_status(const struct addonforge_client *client,
                               enum forever_wago_key_status *status)
{
    assert(client->timeout_ms == 15000);
    *status = service_wago_status;
    return 0;
}
static int fixture_http_command(const struct addonforge_client *client, const char *command,
                                const char *arguments, char *response, size_t size)
{
    assert(client && client->timeout_ms == 15000);
    assert(!strcmp(command, "operation_progress") && !strcmp(arguments, "{}"));
    atomic_fetch_add(&progress_polls, 1);
    snprintf(response, size,
             "{\"ok\":true,\"result\":{\"message\":\"Downloading add-on... 37%%\"}}");
    return 0;
}
#define addonforge_http_command fixture_http_command
#define addonforge_wago_key_status fixture_wago_status
#define addonforge_set_wago_key fixture_set_wago_key
#define addonforge_set_wago_key_file fixture_set_wago_key_file
#define addonforge_install_github fixture_install_github
#include "addonforge-worker.inc"
#undef addonforge_wago_key_status
#undef addonforge_set_wago_key
#undef addonforge_set_wago_key_file
#undef addonforge_install_github
#undef addonforge_http_command
static void run_worker(const char *channel, enum forever_menu_command command,
                       enum forever_addon_tab tab, int fail)
{
    fixture_channel = channel;
    publications = mutations = mutation_timeout = 0;
    mutation_error = fail;
    forever_addons.generation++;
    forever_addons.busy = 1;
    forever_addons.wago_status = FOREVER_WAGO_SET;
    service_wago_status = FOREVER_WAGO_SET;
    forever_addons.channel_count = 1;
    snprintf(forever_addons.channels[0].path, sizeof(forever_addons.channels[0].path),
             "C:\\Games\\World of Warcraft\\%s", channel);
    snprintf(forever_addons.channels[0].label, sizeof(forever_addons.channels[0].label), "%s", channel);
    forever_addons.request = (struct forever_addons_request){
        .generation = forever_addons.generation, .command = command, .tab = tab};
    snprintf(forever_addons.request.key, sizeof(forever_addons.request.key), "owner/repo");
    memset(&forever_addons.snapshot, 0, sizeof(forever_addons.snapshot));
    forever_menu_init(&menu);
    menu.page = FOREVER_MENU_ADDON_PAGE;
    menu.addon_tab = tab;
    menu.addon_loading = 1;
    menu.addon_pending_github = command == FOREVER_MENU_COMMAND_ADDONS_INSTALL_GITHUB;
    menu.wago_status = FOREVER_WAGO_SET;
    assert(!forever_addons_worker(NULL));
    assert(!forever_addons.busy && !menu.addon_loading && publications == 1);
}
int main(void)
{
    {
        struct forever_addons_progress_poll poll = {
            .client = {.port = 31981, .token = "fixture", .timeout_ms = 1}};
        pthread_t thread;
        atomic_store(&progress_polls, 0);
        progress_status[0] = 0;
        assert(!pthread_create(&thread, NULL, forever_addons_progress_worker, &poll));
        while (!atomic_load(&progress_polls)) usleep(1000);
        atomic_store(&poll.done, 1);
        assert(!pthread_join(thread, NULL));
        assert(!strcmp(progress_status, "Downloading add-on... 37%"));
    }
    for (size_t i = 0; i < 2; ++i)
    {
        const char *channel = i ? "_classic_era_" : "_retail_";
        run_worker(channel, FOREVER_MENU_COMMAND_ADDONS_INSTALL_GITHUB, FOREVER_ADDON_FIND, 0);
        assert(mutations == 1 && mutation_timeout == 1800000 && !published.message[0]);
        assert(!strcmp(menu.status, "Installed from GitHub."));
        run_worker(channel, FOREVER_MENU_COMMAND_WAGO_SET, FOREVER_ADDON_FIND, 0);
        assert(mutations == 1 && published.wago_saved && published.wago_status == FOREVER_WAGO_SET);
        assert(!strcmp(menu.status, "Wago key saved."));
        run_worker(channel, FOREVER_MENU_COMMAND_ADDONS_REFRESH, FOREVER_ADDON_FIND, 0);
        assert(!mutations && !published.count && !published.message[0]);
        assert(strstr(menu.status, "No catalogue data"));
        run_worker(channel, FOREVER_MENU_COMMAND_ADDONS_REFRESH, FOREVER_ADDON_STARTER_PACKS, 0);
        assert(!mutations && !published.count && !published.message[0]);
        assert(strstr(menu.status, "No starter packs"));
        run_worker(channel, FOREVER_MENU_COMMAND_ADDONS_INSTALL_GITHUB, FOREVER_ADDON_FIND, -EIO);
        assert(mutations == 1 && !strcmp(menu.status, "Fixture mutation failed."));
        assert(menu.wago_status == FOREVER_WAGO_SET);
        run_worker(channel, FOREVER_MENU_COMMAND_WAGO_SET, FOREVER_ADDON_FIND, -EACCES);
        assert(menu.wago_status == FOREVER_WAGO_REJECTED);
        run_worker(channel, FOREVER_MENU_COMMAND_WAGO_REMOVE, FOREVER_ADDON_FIND, 0);
        assert(menu.wago_status == FOREVER_WAGO_NOT_SET);
    }
    run_worker("_newthing_", FOREVER_MENU_COMMAND_ADDONS_REFRESH, FOREVER_ADDON_FIND, 0);
    assert(!mutations && menu.wago_status == FOREVER_WAGO_SET);
    assert(strstr(menu.status, "Add-ons aren't supported"));
    puts("addonforge production worker: successful mutations survive empty informational views PASS");
    return 0;
}
