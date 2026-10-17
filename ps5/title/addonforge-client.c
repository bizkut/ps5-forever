#include "addonforge-client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef SO_NBIO
#define SO_NBIO 0x1200
#endif

static long long now_ms(void)
{
    struct timeval time;
    if (gettimeofday(&time, NULL)) return -1;
    return (long long)time.tv_sec * 1000 + time.tv_usec / 1000;
}

static int wait_fd(int fd, short events, long long deadline)
{
    for (;;)
    {
        struct pollfd wait = {.fd = fd, .events = events};
        long long now = now_ms();
        int remaining, ready;
        if (now < 0) return -EIO;
        if (now >= deadline) return -ETIMEDOUT;
        remaining = deadline - now > 0x7fffffff ? 0x7fffffff : (int)(deadline - now);
        ready = poll(&wait, 1, remaining);
        if (ready > 0)
        {
            if (wait.revents & events) return 0;
            if (wait.revents & (POLLERR | POLLHUP | POLLNVAL)) return -ECONNRESET;
        }
        else if (!ready) return -ETIMEDOUT;
        else if (errno != EINTR) return -errno;
    }
}

static int connect_loopback(unsigned short port, long long deadline)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0), result, error = 0;
    socklen_t error_size = sizeof(error);
    if (fd < 0) return -errno;
#ifdef __PROSPERO__
    {
        int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NBIO, &enabled, sizeof(enabled)))
        {
            error = errno;
            close(fd);
            return -error;
        }
    }
#else
    {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK))
        {
            error = errno;
            close(fd);
            return -error;
        }
    }
#endif
    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result && errno != EINPROGRESS)
    {
        error = errno;
        close(fd);
        return -error;
    }
    if (result && (error = -wait_fd(fd, POLLOUT, deadline)))
    {
        close(fd);
        return -error;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) || error)
    {
        if (!error) error = errno;
        close(fd);
        return -error;
    }
    return fd;
}

static int transfer(int fd, void *buffer, size_t size, int sending, long long deadline)
{
    char *bytes = buffer;
    while (size)
    {
        ssize_t count;
        int error = wait_fd(fd, sending ? POLLOUT : POLLIN, deadline);
        if (error) return error;
        count = sending ? send(fd, bytes, size, 0) : recv(fd, bytes, size, 0);
        if (count > 0)
        {
            bytes += count;
            size -= (size_t)count;
            continue;
        }
        if (!count) return sending ? -EPIPE : 0;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
    }
    return 1;
}

static const char *header_end(const char *bytes, size_t size)
{
    for (size_t i = 0; i + 4 <= size; i++)
        if (!memcmp(bytes + i, "\r\n\r\n", 4)) return bytes + i + 4;
    return NULL;
}

static int http_request(const struct addonforge_client *client, const char *method,
                        const char *path, const char *body, char *response, size_t response_size)
{
    char request[4096], incoming[4096];
    const char *token = client && client->token ? client->token : "";
    size_t body_size = body ? strlen(body) : 0, used = 0, content_length = 0;
    long long deadline;
    int fd, length, result = 0, have_length = 0;
    if (!client || !client->port || !client->timeout_ms || !method || !path ||
        !response || response_size < 2 || strlen(token) > 128 || body_size > 2048)
        return -EINVAL;
    length = snprintf(request, sizeof(request),
                      "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
                      "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                      method, path, client->port, token, body_size, body ? body : "");
    if (length < 0 || (size_t)length >= sizeof(request)) return -EFBIG;
    deadline = now_ms();
    if (deadline < 0) return -EIO;
    deadline += client->timeout_ms;
    fd = connect_loopback(client->port, deadline);
    if (fd < 0) return fd;
    result = transfer(fd, request, (size_t)length, 1, deadline);
    if (result < 0) goto done;
    for (;;)
    {
        ssize_t count;
        result = wait_fd(fd, POLLIN, deadline);
        if (result == -ECONNRESET) break;
        if (result) goto done;
        count = recv(fd, incoming, sizeof(incoming), 0);
        if (!count) break;
        if (count < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            result = -errno;
            goto done;
        }
        if (used + (size_t)count >= response_size)
        {
            result = -EFBIG;
            goto done;
        }
        memcpy(response + used, incoming, (size_t)count);
        used += (size_t)count;
    }
    response[used] = 0;
    {
        const char *body_at = header_end(response, used);
        const char *line;
        size_t header_size;
        if (!body_at || strncmp(response, "HTTP/1.1 200 ", 13))
        {
            result = -EPROTO;
            goto done;
        }
        header_size = (size_t)(body_at - response);
        line = response;
        while (line < body_at)
        {
            const char *end = strstr(line, "\r\n");
            if (!end || end > body_at) break;
            if ((size_t)(end - line) > 15 && !strncasecmp(line, "Content-Length:", 15))
            {
                char *number_end;
                unsigned long parsed = strtoul(line + 15, &number_end, 10);
                while (number_end < end && isspace((unsigned char)*number_end)) number_end++;
                if (number_end != end || parsed >= response_size)
                {
                    result = -EPROTO;
                    goto done;
                }
                content_length = parsed;
                have_length = 1;
            }
            line = end + 2;
        }
        if (!have_length || used - header_size != content_length)
        {
            result = -EPROTO;
            goto done;
        }
        memmove(response, body_at, content_length);
        response[content_length] = 0;
    }
    result = 0;
done:
    close(fd);
    return result;
}

int addonforge_http_health(const struct addonforge_client *client)
{
    char response[256];
    int result = http_request(client, "GET", "/health", "", response, sizeof(response));
    if (result) return result;
    return strstr(response, "\"ok\":true") && strstr(response, "\"status\":\"ready\"") ?
        0 : -EPROTO;
}

int addonforge_http_command(const struct addonforge_client *client, const char *command,
                            const char *arguments, char *response, size_t response_size)
{
    char body[3072];
    int length;
    if (!command || !*command || strlen(command) > 64 ||
        strspn(command, "abcdefghijklmnopqrstuvwxyz_") != strlen(command)) return -EINVAL;
    if (!arguments) arguments = "{}";
    length = snprintf(body, sizeof(body), "{\"command\":\"%s\",\"args\":%s}",
                      command, arguments);
    if (length < 0 || (size_t)length >= sizeof(body)) return -EFBIG;
    return http_request(client, "POST", "/command", body, response, response_size);
}


static void json_space(const char **cursor, const char *end)
{
    while (*cursor < end && isspace((unsigned char)**cursor)) (*cursor)++;
}

static int json_skip_string(const char **cursor, const char *end,
                            const char **content, size_t *content_length)
{
    const char *at = *cursor, *start;
    int escaped = 0;
    if (at >= end || *at++ != '"') return 0;
    start = at;
    while (at < end)
    {
        if (escaped) escaped = 0;
        else if (*at == '\\') escaped = 1;
        else if (*at == '"')
        {
            if (content) *content = start;
            if (content_length) *content_length = (size_t)(at - start);
            *cursor = at + 1;
            return 1;
        }
        at++;
    }
    return 0;
}

static int json_skip_value(const char **cursor, const char *end, unsigned depth)
{
    const char *at = *cursor;
    if (depth > 32) return 0;
    json_space(&at, end);
    if (at >= end) return 0;
    if (*at == '"')
    {
        if (!json_skip_string(&at, end, NULL, NULL)) return 0;
    }
    else if (*at == '{')
    {
        at++;
        json_space(&at, end);
        if (at < end && *at == '}') at++;
        else for (;;)
        {
            if (!json_skip_string(&at, end, NULL, NULL)) return 0;
            json_space(&at, end);
            if (at >= end || *at++ != ':') return 0;
            if (!json_skip_value(&at, end, depth + 1)) return 0;
            json_space(&at, end);
            if (at < end && *at == '}') { at++; break; }
            if (at >= end || *at++ != ',') return 0;
            json_space(&at, end);
        }
    }
    else if (*at == '[')
    {
        at++;
        json_space(&at, end);
        if (at < end && *at == ']') at++;
        else for (;;)
        {
            if (!json_skip_value(&at, end, depth + 1)) return 0;
            json_space(&at, end);
            if (at < end && *at == ']') { at++; break; }
            if (at >= end || *at++ != ',') return 0;
            json_space(&at, end);
        }
    }
    else
    {
        const char *start = at;
        while (at < end && !strchr(",]} \t\r\n", *at)) at++;
        if (at == start) return 0;
    }
    *cursor = at;
    return 1;
}

static const char *json_value(const char *begin, const char *end, const char *key)
{
    const char *at = begin;
    size_t wanted = strlen(key);
    json_space(&at, end);
    if (at >= end || *at++ != '{') return NULL;
    json_space(&at, end);
    if (at < end && *at == '}') return NULL;
    for (;;)
    {
        const char *member, *value, *next;
        size_t member_length;
        if (!json_skip_string(&at, end, &member, &member_length)) return NULL;
        json_space(&at, end);
        if (at >= end || *at++ != ':') return NULL;
        json_space(&at, end);
        value = at;
        next = at;
        if (!json_skip_value(&next, end, 1)) return NULL;
        if (member_length == wanted && !memchr(member, '\\', member_length) &&
            !memcmp(member, key, wanted)) return value;
        at = next;
        json_space(&at, end);
        if (at < end && *at == '}') return NULL;
        if (at >= end || *at++ != ',') return NULL;
        json_space(&at, end);
    }
}

static int menu_codepoint_supported(unsigned codepoint)
{
    return (codepoint >= 32 && codepoint < 127) ||
           codepoint == 0x2014 || codepoint == 0x2019 ||
           codepoint == 0x2022 || codepoint == 0x2026 ||
           codepoint == 0x2192 || codepoint == 0x25a1 ||
           codepoint == 0x25b3 || codepoint == 0x25cb ||
           codepoint == 0x2715;
}

static void sanitize_menu_text(char *text)
{
    const unsigned char *source = (const unsigned char *)text;
    unsigned char *target = (unsigned char *)text;
    while (*source)
    {
        const unsigned char *start = source;
        unsigned codepoint;
        size_t bytes;
        if (*source < 0x80) { codepoint = *source++; bytes = 1; }
        else if ((*source & 0xe0) == 0xc0 && source[1] &&
                 (source[1] & 0xc0) == 0x80)
        {
            codepoint = ((unsigned)(source[0] & 0x1f) << 6) | (source[1] & 0x3f);
            source += 2; bytes = 2;
        }
        else if ((*source & 0xf0) == 0xe0 && source[1] && source[2] &&
                 (source[1] & 0xc0) == 0x80 && (source[2] & 0xc0) == 0x80)
        {
            codepoint = ((unsigned)(source[0] & 0x0f) << 12) |
                        ((unsigned)(source[1] & 0x3f) << 6) | (source[2] & 0x3f);
            source += 3; bytes = 3;
        }
        else if ((*source & 0xf8) == 0xf0 && source[1] && source[2] && source[3] &&
                 (source[1] & 0xc0) == 0x80 && (source[2] & 0xc0) == 0x80 &&
                 (source[3] & 0xc0) == 0x80)
        {
            codepoint = ((unsigned)(source[0] & 7) << 18) |
                        ((unsigned)(source[1] & 0x3f) << 12) |
                        ((unsigned)(source[2] & 0x3f) << 6) | (source[3] & 0x3f);
            source += 4; bytes = 4;
        }
        else { codepoint = 0; source++; bytes = 1; }
        if (!menu_codepoint_supported(codepoint))
            *target++ = '?';
        else
        {
            memmove(target, start, bytes);
            target += bytes;
        }
    }
    *target = 0;
}

static int json_string(const char *begin, const char *end, const char *key,
                       char *output, size_t size)
{
    const char *at = json_value(begin, end, key);
    size_t used = 0;
    if (!size) return 0;
    output[0] = 0;
    if (!at || at >= end || *at != '"') return 0;
    for (at++; at < end && *at != '"'; at++)
    {
        unsigned char value = (unsigned char)*at;
        if (value == '\\')
        {
            if (++at >= end) return 0;
            switch (*at)
            {
            case '"': value = '"'; break;
            case '\\': value = '\\'; break;
            case '/': value = '/'; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case 'u':
                if (end - at < 5) return 0;
                at += 4;
                value = '?';
                break;
            default: return 0;
            }
        }
        if (used + 1 < size) output[used++] = (char)value;
    }
    if (at >= end || *at != '"') return 0;
    output[used] = 0;
    sanitize_menu_text(output);
    return 1;
}

static int json_bool(const char *begin, const char *end, const char *key)
{
    const char *at = json_value(begin, end, key);
    return at && end - at >= 4 && !memcmp(at, "true", 4);
}

static unsigned long long json_u64(const char *begin, const char *end, const char *key)
{
    const char *at = json_value(begin, end, key);
    unsigned long long value = 0;
    if (!at) return 0;
    while (at < end && *at >= '0' && *at <= '9')
    {
        unsigned digit = (unsigned)(*at++ - '0');
        if (value > (~0ull - digit) / 10) return ~0ull;
        value = value * 10 + digit;
    }
    return value;
}

static int object_end(const char *begin, const char *end, const char **after);

static int json_array_any_bool(const char *begin, const char *end,
                               const char *array_name, const char *member_name)
{
    const char *at = json_value(begin, end, array_name);
    if (!at || at >= end || *at++ != '[') return 0;
    for (;;)
    {
        const char *after;
        json_space(&at, end);
        if (at >= end || *at == ']') return 0;
        if (!object_end(at, end, &after)) return 0;
        if (json_bool(at, after, member_name)) return 1;
        at = after;
        json_space(&at, end);
        if (at < end && *at == ']') return 0;
        if (at >= end || *at++ != ',') return 0;
    }
}

static int json_present(const char *begin, const char *end, const char *key)
{
    const char *at = json_value(begin, end, key);
    return at && !(end - at >= 4 && !memcmp(at, "null", 4));
}

static int json_array_nonempty(const char *begin, const char *end, const char *key)
{
    const char *at = json_value(begin, end, key);
    if (!at || at >= end || *at != '[') return 0;
    for (at++; at < end && isspace((unsigned char)*at); at++);
    return at < end && *at != ']';
}

static int object_end(const char *begin, const char *end, const char **after)
{
    const char *at = begin;
    if (!json_skip_value(&at, end, 0) || begin >= end || *begin != '{') return 0;
    *after = at;
    return 1;
}
static void copy_date(char *output, size_t size, const char *input)
{
    size_t length = strlen(input);
    if (length > 10) length = 10;
    if (length >= size) length = size - 1;
    memcpy(output, input, length);
    output[length] = 0;
}

static int row_name_compare(const void *left, const void *right)
{
    const struct forever_addon_row *a = left, *b = right;
    return strcasecmp(a->name, b->name);
}

static int parse_row(enum forever_addon_tab tab, const char *begin, const char *end,
                     struct forever_addon_row *row)
{
    char id[FOREVER_ADDON_KEY_BYTES] = {0}, updated[64] = {0}, raw_status[48] = {0};
    char source_kind[24] = {0}, source_id[FOREVER_ADDON_SOURCE_BYTES] = {0};
    char update_note[FOREVER_ADDON_DETAIL_BYTES] = {0};
    const char *source_object, *source_end;
    int github, wago, wowi, tukui, curse;
    memset(row, 0, sizeof(*row));
    if (!json_string(begin, end, "name", row->name, sizeof(row->name))) return 0;
    json_string(begin, end, "key", row->key, sizeof(row->key));
    json_string(begin, end, "desc", row->description, sizeof(row->description));
    if (!row->description[0]) json_string(begin, end, "notes", row->description, sizeof(row->description));
    json_string(begin, end, "source_label", row->source, sizeof(row->source));
    json_string(begin, end, "updated_at", updated, sizeof(updated));
    json_string(begin, end, "note", update_note, sizeof(update_note));
    source_object = json_value(begin, end, "source");
    if (source_object && object_end(source_object, end, &source_end))
    {
        json_string(source_object, source_end, "kind", source_kind, sizeof(source_kind));
        json_string(source_object, source_end, "id", source_id, sizeof(source_id));
    }
    if (source_kind[0] && source_id[0])
        snprintf(row->source, sizeof(row->source), "%s: %s",
                 !strcmp(source_kind, "github") ? "GitHub" :
                 !strcmp(source_kind, "wago") ? "Wago" :
                 !strcmp(source_kind, "wowi") ? "WoWInterface" :
                 !strcmp(source_kind, "tukui") ? "TukUI" : source_kind,
                 source_id);
    copy_date(row->updated, sizeof(row->updated), updated);
    row->downloads = json_u64(begin, end, "downloads");
    row->update_available = json_bool(begin, end, "update_available");
    github = json_present(begin, end, "github");
    wago = json_present(begin, end, "wago");
    wowi = json_present(begin, end, "wowi");
    tukui = json_present(begin, end, "tukui");
    curse = json_present(begin, end, "curse");
    if (!row->source[0])
        snprintf(row->source, sizeof(row->source), "%s",
                 github ? "GitHub" : wowi ? "WoWInterface" : tukui ? "TukUI" :
                 wago ? "Wago" : curse ? "CurseForge" : "Catalogue");
    if (tab == FOREVER_ADDON_FIND)
    {
        int installed = json_bool(begin, end, "installed");
        int wago_only = wago && !github && !wowi && !tukui;
        row->curse_only = curse && !github && !wago && !wowi && !tukui;
        row->installable = !installed && !row->curse_only && !wago_only &&
            (github || wowi || tukui);
        snprintf(row->status, sizeof(row->status), "%s",
                 row->update_available ? "Update available" :
                 installed ? "Installed" :
                 row->curse_only ? "Available on CurseForge only" :
                 wago_only ? "Wago key required" :
                 row->downloads ? "available" : "catalogue");
    }
    else if (tab == FOREVER_ADDON_STARTER_PACKS)
    {
        if (!json_string(begin, end, "id", id, sizeof(id))) return 0;
        snprintf(row->key, sizeof(row->key), "starter:%s", id);
        row->installable = json_array_any_bool(begin, end, "addons", "installable");
        snprintf(row->source, sizeof(row->source), "Starter pack");
        snprintf(row->status, sizeof(row->status), "%s", row->installable ? "ready" : "unavailable");
    }
    else
    {
        json_string(begin, end, "status", raw_status, sizeof(raw_status));
        row->update_available = row->update_available || !strcmp(raw_status, "update");
        if (row->update_available) snprintf(row->status, sizeof(row->status), "update available");
        else if (!strcmp(raw_status, "error") || !strcmp(raw_status, "unknown"))
        {
            row->check_failed = 1;
            snprintf(row->status, sizeof(row->status), "%s",
                     !strcmp(raw_status, "error") ? "check failed" : "update unknown");
            addonforge_plain_error(update_note[0] ? update_note :
                                   "Could not determine update status.",
                                   row->description, sizeof(row->description));
        }
        else if (!strcmp(raw_status, "checking"))
            snprintf(row->status, sizeof(row->status), "checking");
        else if (!strcmp(raw_status, "no-source"))
        {
            row->check_failed = 1;
            snprintf(row->status, sizeof(row->status), "no update source");
        }
        else if (!strcmp(raw_status, "no-key"))
        {
            row->check_failed = 1;
            snprintf(row->status, sizeof(row->status), "Wago key required");
        }
        else if (json_bool(begin, end, "pinned")) snprintf(row->status, sizeof(row->status), "pinned");
        else if (json_array_nonempty(begin, end, "missing_deps"))
            snprintf(row->status, sizeof(row->status), "missing dependency");
        else if (!strcmp(raw_status, "curse-only"))
        {
            snprintf(row->status, sizeof(row->status), "CurseForge only");
            row->curse_only = 1;
        }
        else snprintf(row->status, sizeof(row->status), "up to date");
        row->installable = 1;
    }
    return row->key[0] != 0;
}

void addonforge_plain_error(const char *technical, char *plain, size_t size)
{
    if (!size) return;
    if (!technical) technical = "";
    if (strstr(technical, "429") || strstr(technical, "rate limit") ||
        strstr(technical, "Rate limit") || strstr(technical, "403"))
        snprintf(plain, size, "GitHub is limiting requests, try again in a few minutes.");
    else if (strstr(technical, "timed out") || strstr(technical, "timeout") ||
             strstr(technical, "ETIMEDOUT"))
        snprintf(plain, size, "The add-on service did not answer in time.");
    else if (strstr(technical, "No WoW install") || strstr(technical, "install selected"))
        snprintf(plain, size, "This WoW channel is not installed yet.");
    else if (strstr(technical, "connection") || strstr(technical, "connect"))
        snprintf(plain, size, "Could not reach the add-on service.");
    else if (*technical)
        snprintf(plain, size, "%s", technical);
    else
        snprintf(plain, size, "The add-on operation failed.");
}

int addonforge_response_error(const char *response, char *error, size_t error_size)
{
    const char *end, *ok;
    char technical[FOREVER_MENU_STATUS_BYTES] = {0};
    if (!response || !error || !error_size) return -EINVAL;
    error[0] = 0;
    end = response + strlen(response);
    ok = json_value(response, end, "ok");
    if (ok && end - ok >= 4 && !memcmp(ok, "true", 4)) return 0;
    if (ok && end - ok >= 5 && !memcmp(ok, "false", 5))
    {
        if (!json_string(response, end, "error", technical, sizeof(technical)))
            snprintf(technical, sizeof(technical), "The add-on operation failed.");
        addonforge_plain_error(technical, error, error_size);
        return -EIO;
    }
    return -EPROTO;
}

int addonforge_parse_snapshot(enum forever_addon_tab tab, const char *response,
                              struct forever_addon_snapshot *snapshot,
                              char *error, size_t error_size)
{
    const char *end, *result, *at, *ok;
    char technical[FOREVER_MENU_STATUS_BYTES] = {0};
    if (!response || !snapshot || !error || !error_size) return -EINVAL;
    error[0] = 0;
    end = response + strlen(response);
    ok = json_value(response, end, "ok");
    if (ok && end - ok >= 5 && !memcmp(ok, "false", 5))
    {
        if (!json_string(response, end, "error", technical, sizeof(technical)))
            snprintf(technical, sizeof(technical), "The add-on operation failed.");
        addonforge_plain_error(technical, error, error_size);
        return -EIO;
    }
    if (!ok || end - ok < 4 || memcmp(ok, "true", 4) ||
        !(result = json_value(response, end, "result")) || result >= end || *result != '[')
        return -EPROTO;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->tab = tab;
    at = result + 1;
    while (at < end)
    {
        const char *after;
        while (at < end && (isspace((unsigned char)*at) || *at == ',')) at++;
        if (at >= end || *at == ']') break;
        if (!object_end(at, end, &after)) return -EPROTO;
        if (snapshot->count < FOREVER_ADDON_ROWS)
        {
            struct forever_addon_row row;
            if (parse_row(tab, at, after, &row) &&
                (tab != FOREVER_ADDON_UPDATES || row.update_available || row.check_failed))
                snapshot->rows[snapshot->count++] = row;
        }
        at = after;
    }
    if (tab == FOREVER_ADDON_FIND)
        qsort(snapshot->rows, snapshot->count, sizeof(snapshot->rows[0]), row_name_compare);
    return 0;
}
