/* Host behavior tests for the bounded AddonForge loopback HTTP/JSON client. */
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../../title/addonforge-client.h"

struct fake_server
{
    int listener;
    unsigned short port;
};

static void send_all(int fd, const char *data, size_t size)
{
    while (size)
    {
        ssize_t count = send(fd, data, size, 0);
        assert(count > 0);
        data += count;
        size -= (size_t)count;
    }
}

static void *serve_fake(void *argument)
{
    static const char catalogue[] =
        "{\"ok\":true,\"result\":["
        "{\"id\":\"zulu\",\"key\":\"cat:zulu\",\"name\":\"Zulu\","
        "\"desc\":\"Last by name\",\"github\":\"owner/zulu\",\"installed\":true,"
        "\"downloads\":42,\"updated_at\":\"2026-10-16T00:00:00Z\"},"
        "{\"id\":\"alpha\",\"key\":\"cat:alpha\",\"name\":\"Alpha\","
        "\"desc\":\"Link only\",\"curse\":123,\"downloads\":7}]}";
    struct fake_server *server = argument;
    for (int request = 0; request < 2; request++)
    {
        char bytes[4096] = {0};
        int fd = accept(server->listener, NULL, NULL);
        assert(fd >= 0);
        ssize_t count = recv(fd, bytes, sizeof(bytes) - 1, 0);
        assert(count > 0);
        assert(strstr(bytes, "Authorization: Bearer session-secret\r\n"));
        if (!request)
        {
            static const char response[] =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: 39\r\nConnection: close\r\n\r\n"
                "{\"ok\":true,\"result\":{\"status\":\"ready\"}}";
            assert(strstr(bytes, "GET /health HTTP/1.1\r\n"));
            send_all(fd, response, sizeof(response) - 1);
        }
        else
        {
            char response[4096];
            int length;
            assert(strstr(bytes, "POST /command HTTP/1.1\r\n"));
            assert(strstr(bytes, "\"command\":\"catalog_list\""));
            assert(strstr(bytes, "\"refresh\":false"));
            length = snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                sizeof(catalogue) - 1, catalogue);
            assert(length > 0 && (size_t)length < sizeof(response));
            send_all(fd, response, (size_t)length);
        }
        close(fd);
    }
    close(server->listener);
    return NULL;
}

static void test_http_and_fake_catalogue(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct fake_server fake = {0};
    struct addonforge_client client = {.token = "session-secret", .timeout_ms = 2000};
    struct forever_addon_snapshot snapshot;
    char response[ADDONFORGE_RESPONSE_BYTES], error[FOREVER_MENU_STATUS_BYTES];
    socklen_t address_size = sizeof(address);
    pthread_t thread;

    fake.listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(fake.listener >= 0);
    assert(!bind(fake.listener, (struct sockaddr *)&address, sizeof(address)));
    assert(!listen(fake.listener, 2));
    assert(!getsockname(fake.listener, (struct sockaddr *)&address, &address_size));
    fake.port = ntohs(address.sin_port);
    client.port = fake.port;
    assert(!pthread_create(&thread, NULL, serve_fake, &fake));

    assert(!addonforge_http_health(&client));
    assert(!addonforge_http_command(&client, "catalog_list", "{\"refresh\":false}",
                                    response, sizeof(response)));
    assert(!addonforge_parse_snapshot(FOREVER_ADDON_FIND, response, &snapshot,
                                     error, sizeof(error)));
    assert(snapshot.count == 2);
    assert(!strcmp(snapshot.rows[0].name, "Alpha")); /* catalogue is sorted without network */
    assert(snapshot.rows[0].curse_only && !snapshot.rows[0].installable);
    assert(!strcmp(snapshot.rows[0].status, "Available on CurseForge only"));
    assert(!strcmp(snapshot.rows[1].name, "Zulu"));
    assert(!snapshot.rows[1].installable && snapshot.rows[1].downloads == 42);
    assert(!strcmp(snapshot.rows[1].status, "Installed"));
    assert(!strcmp(snapshot.rows[1].source, "GitHub"));
    assert(!strcmp(snapshot.rows[1].updated, "2026-10-16"));
    assert(!pthread_join(thread, NULL));
}

static void test_installed_statuses_and_plain_errors(void)
{
    static const char installed[] =
        "{\"ok\":true,\"result\":["
        "{\"key\":\"cat:update\",\"name\":\"Update Me\",\"status\":\"update\","
        "\"source\":{\"kind\":\"github\",\"id\":\"owner/update\"},"
        "\"source_label\":\"GitHub · owner/update\",\"notes\":\"Details\","
        "\"updated_at\":\"2026-09-29T12:00:00Z\",\"update_available\":true},"
        "{\"key\":\"cat:pinned\",\"name\":\"Pinned\",\"status\":\"ok\",\"pinned\":true},"
        "{\"key\":\"folder:missing\",\"name\":\"Needs Library\",\"status\":\"ok\","
        "\"missing_deps\":[{\"folder\":\"Library\"}]}]}";
    struct forever_addon_snapshot snapshot;
    char error[FOREVER_MENU_STATUS_BYTES];

    assert(!addonforge_parse_snapshot(FOREVER_ADDON_INSTALLED, installed, &snapshot,
                                     error, sizeof(error)));
    assert(snapshot.count == 3);
    assert(!strcmp(snapshot.rows[0].status, "update available"));
    assert(!strcmp(snapshot.rows[0].source, "GitHub: owner/update"));
    assert(!strcmp(snapshot.rows[0].updated, "2026-09-29"));
    assert(snapshot.rows[0].update_available);
    assert(!strcmp(snapshot.rows[1].status, "pinned"));
    assert(!strcmp(snapshot.rows[2].status, "missing dependency"));
    static const char update_find[] =
        "{\"ok\":true,\"result\":[{\"id\":\"update\",\"key\":\"cat:update\","
        "\"name\":\"Update Me\",\"github\":\"owner/update\",\"installed\":true,"
        "\"update_available\":true}]}";
    assert(!addonforge_parse_snapshot(FOREVER_ADDON_FIND, update_find, &snapshot,
                                     error, sizeof(error)));
    assert(!strcmp(snapshot.rows[0].status, "Update available"));
    static const char untrusted[] =
        "{\"ok\":true,\"result\":[{\"key\":\"folder:emoji\",\"name\":\"Emoji 😀 漢字\","
        "\"status\":\"ok\",\"notes\":\"Résumé 🚀\"}]}";
    assert(!addonforge_parse_snapshot(FOREVER_ADDON_INSTALLED, untrusted, &snapshot,
                                     error, sizeof(error)));
    assert(!strcmp(snapshot.rows[0].name, "Emoji ? ??"));
    assert(!strcmp(snapshot.rows[0].description, "R?sum? ?"));
    static const char nested_package[] =
        "{\"ok\":true,\"result\":[{\"missing_deps\":[{\"catalog_id\":null,"
        "\"folder\":\"Library\",\"name\":null}],\"key\":\"folder:parent\","
        "\"name\":\"Parent Addon\",\"notes\":null,\"source\":null,"
        "\"status\":\"unknown\",\"note\":\"network unavailable\"}]}";
    assert(!addonforge_parse_snapshot(FOREVER_ADDON_INSTALLED, nested_package, &snapshot,
                                     error, sizeof(error)));
    assert(snapshot.count == 1);
    assert(!strcmp(snapshot.rows[0].name, "Parent Addon"));
    assert(snapshot.rows[0].check_failed);
    assert(!strcmp(snapshot.rows[0].status, "update unknown"));
    assert(!strcmp(snapshot.rows[0].description, "network unavailable"));
    assert(!addonforge_parse_snapshot(FOREVER_ADDON_UPDATES, nested_package, &snapshot,
                                     error, sizeof(error)));
    assert(snapshot.count == 1);

    static const char failed_update[] =
        "{\"ok\":true,\"result\":[{\"key\":\"cat:limited\",\"name\":\"Limited\","
        "\"status\":\"error\",\"note\":\"GitHub API returned 429 rate limit\","
        "\"missing_deps\":[]}]}";
    assert(!addonforge_parse_snapshot(FOREVER_ADDON_UPDATES, failed_update, &snapshot,
                                     error, sizeof(error)));
    assert(snapshot.count == 1 && snapshot.rows[0].check_failed);
    assert(!strcmp(snapshot.rows[0].status, "check failed"));
    assert(!strcmp(snapshot.rows[0].description,
                   "GitHub is limiting requests, try again in a few minutes."));
    static const char real_bundle[] =
        "{\"ok\":true,\"result\":[{\"addons\":[{\"desc\":\"Bags\","
        "\"forever\":true,\"id\":\"betterbags\",\"installable\":true,"
        "\"installed\":false,\"name\":\"BetterBags\",\"needs_key\":false}],"
        "\"desc\":\"A balanced starter selection\",\"id\":\"good-start\","
        "\"name\":\"A good start\",\"note\":\"Pick one boss mod\"}]}";
    assert(!addonforge_parse_snapshot(FOREVER_ADDON_STARTER_PACKS, real_bundle, &snapshot,
                                     error, sizeof(error)));
    assert(snapshot.count == 1);
    assert(!strcmp(snapshot.rows[0].key, "starter:good-start"));
    assert(!strcmp(snapshot.rows[0].name, "A good start"));
    assert(snapshot.rows[0].installable);

    addonforge_plain_error("GitHub API returned 429 rate limit", error, sizeof(error));
    assert(!strcmp(error, "GitHub is limiting requests, try again in a few minutes."));
    addonforge_plain_error("connect timed out", error, sizeof(error));
    assert(!strcmp(error, "The add-on service did not answer in time."));
}

int main(void)
{
    test_http_and_fake_catalogue();
    test_installed_statuses_and_plain_errors();
    puts("addonforge client: PASS (fake loopback server, JSON state, offline catalogue)");
    return 0;
}
