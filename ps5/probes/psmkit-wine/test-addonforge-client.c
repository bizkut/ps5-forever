/* Host behavior tests for the bounded AddonForge loopback HTTP/JSON client. */
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
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
        "{\"ok\":true,\"result\":{\"notice\":\"\",\"rows\":["
        "{\"id\":\"zulu\",\"key\":\"cat:zulu\",\"name\":\"Zulu\","
        "\"desc\":\"Last by name\",\"github\":\"owner/zulu\",\"installed\":true,"
        "\"downloads\":42,\"updated_at\":\"2026-10-16T00:00:00Z\"},"
        "{\"id\":\"alpha\",\"key\":\"cat:alpha\",\"name\":\"Alpha\","
        "\"desc\":\"Link only\",\"curse\":123,\"downloads\":7}]}}";
    struct fake_server *server = argument;
    for (int request = 0; request < 3; request++)
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
        else if (request == 1)
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
        else
        {
            static const char body[] =
                "{\"ok\":true,\"result\":{\"key\":\"github:owner/repo\",\"name\":\"Repo\"}}";
            char response[512];
            int length;
            assert(strstr(bytes, "POST /command HTTP/1.1\r\n"));
            assert(strstr(bytes,
                "{\"command\":\"install_github\",\"args\":"
                "{\"repo\":\"owner/repo\",\"allow_non_forever\":false}}"));
            length = snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                sizeof(body) - 1, body);
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
    assert(!listen(fake.listener, 3));
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
    assert(!addonforge_install_github(&client, "owner/repo", response, sizeof(response)));
    assert(!addonforge_response_error(response, error, sizeof(error)));
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
        "{\"ok\":true,\"result\":{\"notice\":\"\",\"rows\":[{\"id\":\"update\",\"key\":\"cat:update\","
        "\"name\":\"Update Me\",\"github\":\"owner/update\",\"installed\":true,"
        "\"update_available\":true}]}}";
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
    static const char cafe_find[] =
        "{\"ok\":true,\"result\":{\"notice\":\"\",\"rows\":[{\"id\":\"cafe\",\"key\":\"cat:cafe\","
        "\"name\":\"Caf\xc3\xa9\",\"desc\":\"Recipes and bags\",\"github\":\"owner/cafe\"}]}}";
    {
        struct forever_menu_state menu;
        assert(!addonforge_parse_snapshot(FOREVER_ADDON_FIND, cafe_find, &snapshot,
                                         error, sizeof(error)));
        assert(!strcmp(snapshot.rows[0].name, "Caf?"));
        forever_menu_init(&menu);
        menu.page = FOREVER_MENU_ADDON_PAGE;
        menu.addon_tab = FOREVER_ADDON_FIND;
        forever_menu_addon_snapshot(&menu, &snapshot);
        assert(forever_menu_find_set_query(&menu, "\xc3\xa9"));
        assert(menu.addon_match_count == 1);
        assert(!strcmp(menu.addon_rows[1].key, "cat:cafe"));
    }
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
        "{\"ok\":true,\"result\":{\"notice\":\"\",\"rows\":[{\"addons\":[{\"desc\":\"Bags\","
        "\"forever\":true,\"id\":\"betterbags\",\"installable\":true,"
        "\"installed\":false,\"name\":\"BetterBags\",\"needs_key\":false}],"
        "\"desc\":\"A balanced starter selection\",\"id\":\"good-start\","
        "\"name\":\"A good start\",\"note\":\"Pick one boss mod\"}]}}";
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

static void test_wago_status_and_file(void)
{
    enum forever_wago_key_status status;
    char path[128], key[257];
    assert(!addonforge_parse_wago_status(
        "{\"ok\":true,\"result\":{\"set\":false,\"valid\":null}}", &status));
    assert(status == FOREVER_WAGO_NOT_SET);
    assert(!addonforge_parse_wago_status(
        "{\"ok\":true,\"result\":{\"set\":true,\"valid\":true}}", &status));
    assert(status == FOREVER_WAGO_SET);
    assert(!addonforge_parse_wago_status(
        "{\"ok\":true,\"result\":{\"set\":true,\"valid\":null}}", &status));
    assert(status == FOREVER_WAGO_NOT_CHECKED);
    assert(!addonforge_parse_wago_status(
        "{\"ok\":false,\"error\":\"Wago rejected this key.\"}", &status));
    assert(status == FOREVER_WAGO_REJECTED);
    assert(addonforge_parse_wago_status("{\"ok\":true,\"result\":{}}", &status) < 0);
    {
        struct forever_addon_snapshot snapshot;
        char error[128];
        assert(!addonforge_parse_snapshot(FOREVER_ADDON_FIND,
            "{\"ok\":true,\"result\":{\"notice\":\"\",\"rows\":[{\"key\":\"cat:wago\",\"name\":\"Wago Addon\","
            "\"wago\":\"fixture\"}]}}", &snapshot, error, sizeof(error)));
        assert(snapshot.count == 1 && !snapshot.rows[0].installable);
        addonforge_snapshot_wago_status(&snapshot, FOREVER_WAGO_NOT_CHECKED);
        assert(snapshot.rows[0].installable);
        assert(!strcmp(snapshot.rows[0].status, "available"));
    }
    snprintf(path, sizeof(path), "/tmp/forever-wago-file-%ld", (long)getpid());
    assert(addonforge_read_wago_key_file(path, key, sizeof(key)) == -ENOENT);
    FILE *file = fopen(path, "wb");
    assert(file);
    fputs("  fake-wago-secret \r\nignored second line", file);
    assert(!fclose(file));
    assert(!addonforge_read_wago_key_file(path, key, sizeof(key)));
    assert(!strcmp(key, "fake-wago-secret"));
    file = fopen(path, "wb");
    assert(file); fputs(" \t\r\n", file); assert(!fclose(file));
    assert(addonforge_read_wago_key_file(path, key, sizeof(key)) == -EINVAL);
    assert(!key[0]);
    file = fopen(path, "wb");
    assert(file);
    for (int i = 0; i < 257; i++) fputc('x', file);
    assert(!fclose(file));
    assert(addonforge_read_wago_key_file(path, key, sizeof(key)) == -EFBIG);
    assert(!key[0]);
    file = fopen(path, "wb");
    assert(file);
    for (int i = 0; i < 256; i++) fputc('x', file);
    assert(!fclose(file));
    assert(!addonforge_read_wago_key_file(path, key, sizeof(key)) && strlen(key) == 256);
    assert(!unlink(path));
}

static void *serve_wago(void *argument)
{
    struct fake_server *server = argument;
    static const char *const replies[] = {
        "{\"ok\":true,\"result\":{\"set\":true,\"valid\":null}}",
        "{\"ok\":true,\"result\":{\"set\":true,\"valid\":true}}",
        "{\"ok\":true,\"result\":{\"set\":false,\"valid\":null}}",
        "{\"ok\":false,\"error\":\"Wago rejected this key.\"}",
        "{\"ok\":true,\"result\":{\"set\":true,\"valid\":null}}"};
    for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++)
    {
        char request[4096] = {0}, response[1024];
        int fd = accept(server->listener, NULL, NULL);
        assert(fd >= 0);
        assert(recv(fd, request, sizeof(request) - 1, 0) > 0);
        if (i == 1) assert(strstr(request, "\"command\":\"wago_key_status\""));
        else
        {
            assert(strstr(request, "\"command\":\"set_wago_key\""));
            assert(strstr(request, i == 2 ? "\"key\":\"\"" :
                          "\"key\":\"fake-wago-secret-for-redaction\""));
        }
        int length = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(replies[i]), replies[i]);
        send_all(fd, response, (size_t)length);
        close(fd);
    }
    close(server->listener);
    return NULL;
}

static void test_wago_commands_and_redaction(void)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    struct fake_server server;
    struct addonforge_client client = {.token = "session-secret", .timeout_ms = 2000};
    enum forever_wago_key_status status;
    pthread_t thread;
    char path[128], captured[256] = {0};
    FILE *capture = tmpfile(), *file;
    int saved_stdout = dup(STDOUT_FILENO), saved_stderr = dup(STDERR_FILENO);
    assert(capture && saved_stdout >= 0 && saved_stderr >= 0);
    server.listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(server.listener >= 0);
    assert(!bind(server.listener, (struct sockaddr *)&address, sizeof(address)));
    assert(!listen(server.listener, 5));
    assert(!getsockname(server.listener, (struct sockaddr *)&address, &size));
    client.port = ntohs(address.sin_port);
    assert(!pthread_create(&thread, NULL, serve_wago, &server));
    fflush(NULL);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
    assert(!addonforge_set_wago_key(&client, "  fake-wago-secret-for-redaction\r\n", &status));
    assert(status == FOREVER_WAGO_NOT_CHECKED);
    assert(!addonforge_wago_key_status(&client, &status) && status == FOREVER_WAGO_SET);
    assert(!addonforge_set_wago_key(&client, "", &status) && status == FOREVER_WAGO_NOT_SET);
    snprintf(path, sizeof(path), "/tmp/forever-wago-import-%ld", (long)getpid());
    file = fopen(path, "wb");
    assert(file); fputs("fake-wago-secret-for-redaction\r\n", file); assert(!fclose(file));
    assert(addonforge_set_wago_key_file(&client, path, &status) == -EACCES);
    assert(status == FOREVER_WAGO_REJECTED && !access(path, F_OK));
    assert(!addonforge_set_wago_key_file(&client, path, &status));
    assert(status == FOREVER_WAGO_NOT_CHECKED && access(path, F_OK) && errno == ENOENT);
    assert(!pthread_join(thread, NULL));
    fflush(NULL);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdout); close(saved_stderr);
    rewind(capture);
    size_t count = fread(captured, 1, sizeof(captured) - 1, capture);
    assert(feof(capture) && count < sizeof(captured) - 1);
    fclose(capture);
    assert(!strstr(captured, "fake-wago-secret-for-redaction"));
    assert(!strstr(captured, "fake-"));
}

int main(void)
{
    char arguments[2300];
    assert(!addonforge_install_arguments("W:\\Games\\World of Warcraft\\_retail_", arguments, sizeof(arguments)));
    assert(!strcmp(arguments, "{\"path\":\"W:\\\\Games\\\\World of Warcraft\\\\_retail_\"}"));
    assert(!addonforge_install_arguments("C:\\Games\\World of Warcraft\\_classic_era_", arguments, sizeof(arguments)));
    assert(strstr(arguments, "_classic_era_"));
    assert(!addonforge_install_arguments("C:\\Games\\World of Warcraft\\_future_", arguments, sizeof(arguments)));
    assert(strstr(arguments, "_future_"));
    test_http_and_fake_catalogue();
    test_wago_status_and_file();
    test_wago_commands_and_redaction();
    test_installed_statuses_and_plain_errors();
    puts("addonforge client: PASS (fake loopback server, JSON state, offline catalogue)");
    return 0;
}
