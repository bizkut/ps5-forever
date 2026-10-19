/* Shared native HTTPS streaming path for Battle.net and the voice model.
 * Each redirect is a fresh GET: sceHttp2 auto-redirect can lose the final body. */
#ifndef FOREVER_DOWNLOAD_H
#define FOREVER_DOWNLOAD_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sha256.h"

#include "../../title/voice-model.h"

int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2Term(int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2AbortRequest(int);
int sceHttp2AddRequestHeader(int, const char *, const char *, unsigned);
int sceHttp2GetAllResponseHeaders(int, char **, size_t *);
int sceHttp2SetAutoRedirect(int, int);
int sceHttp2SetResolveTimeOut(int, unsigned);
int sceHttp2SetConnectTimeOut(int, unsigned);
int sceHttp2SetRecvTimeOut(int, unsigned);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2GetResponseContentLength(int, int *, uint64_t *);
int sceHttp2ReadData(int, void *, size_t);

struct forever_https
{
    int pool, ssl, http, tmpl;
};

static inline void forever_https_close(struct forever_https *http)
{
    if (http->tmpl >= 0) sceHttp2DeleteTemplate(http->tmpl);
    if (http->http >= 0) sceHttp2Term(http->http);
    if (http->ssl >= 0) sceSslTerm(http->ssl);
    if (http->pool >= 0) sceNetPoolDestroy(http->pool);
    *http = (struct forever_https){-1, -1, -1, -1};
}

/* Positive Sony handles, zero on success, negative Sony errors are preserved. */
static inline int forever_https_open(struct forever_https *http, const char **step)
{
    int result;
    *http = (struct forever_https){-1, -1, -1, -1};
#define FOREVER_HTTPS_CALL(label, call) \
    do { *step = label; if ((result = (call)) < 0) return result; } while (0)
    FOREVER_HTTPS_CALL("initializing Net", sceNetInit());
    FOREVER_HTTPS_CALL("creating network pool", http->pool = sceNetPoolCreate("forever-download", 64 * 1024, 0));
    FOREVER_HTTPS_CALL("initializing SSL", http->ssl = sceSslInit(256 * 1024));
    FOREVER_HTTPS_CALL("initializing HTTP2", http->http = sceHttp2Init(http->pool, http->ssl, 256 * 1024, 1));
    FOREVER_HTTPS_CALL("creating HTTP2 template", http->tmpl = sceHttp2CreateTemplate(http->http, "Mozilla/5.0", 3, 1));
    FOREVER_HTTPS_CALL("disabling automatic HTTPS redirects", sceHttp2SetAutoRedirect(http->tmpl, 0));
    /* Microseconds. Cancellation also aborts the active request, while these
     * bound DNS/connect/read waits if an abort races request submission. */
    FOREVER_HTTPS_CALL("setting DNS timeout", sceHttp2SetResolveTimeOut(http->tmpl, 10000000));
    FOREVER_HTTPS_CALL("setting connection timeout", sceHttp2SetConnectTimeOut(http->tmpl, 10000000));
    FOREVER_HTTPS_CALL("setting receive timeout", sceHttp2SetRecvTimeOut(http->tmpl, 10000000));
#undef FOREVER_HTTPS_CALL
    return 0;
}

struct forever_download_info
{
    int status, attempt;
};

struct forever_download
{
    const char *url, *part, *destination;
    uint64_t size, cap; /* size zero accepts the server's length (installer only) */
    const char *sha256; /* 64 lowercase hex digits, or NULL for the MZ installer */
    int mz;
    int resume; /* exact-size, hashed voice model only; never the installer */
    struct forever_download_info *info;
    int (*canceled)(void *);
    void (*request)(void *, int); /* -1 unregisters before deleting the request */
    void (*progress)(void *, uint64_t, uint64_t);
    void (*resumed)(void *, int, uint64_t, int);
    void (*hop)(void *, int, int, const char *, uint64_t);
    void (*first_read)(void *, int);
    void (*wait)(void *, unsigned); /* milliseconds; NULL uses cancelable sleep */
    void *opaque;
};

static inline int forever_download_canceled(const struct forever_download *download)
{
    return download->canceled && download->canceled(download->opaque);
}

static inline int forever_download_digest(struct sha256_context *hash, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    sha256_final(hash, digest);
    if (strlen(expected) != 64) return EINVAL;
    for (unsigned i = 0; i < 32; i++)
        if (hex[digest[i] >> 4] != expected[i * 2] ||
            hex[digest[i] & 15] != expected[i * 2 + 1]) return EINVAL;
    return 0;
}

static inline int forever_download_number(const char **at, const char *end, uint64_t *number)
{
    const char *start = *at;
    *number = 0;
    while (*at < end && **at >= '0' && **at <= '9')
    {
        unsigned digit = (unsigned)(*(*at)++ - '0');
        if (*number > (UINT64_MAX - digit) / 10) return EINVAL;
        *number = *number * 10 + digit;
    }
    return *at == start ? EINVAL : 0;
}

/* Return one trimmed header value without allocating or trusting a terminator.
 * Duplicate fields are refused rather than choosing an ambiguous redirect/range. */
static inline int forever_download_header(const char *headers, size_t bytes, const char *name,
                                           const char **value, size_t *value_bytes)
{
    size_t name_bytes = strlen(name);
    int found = 0;
    if (!headers || bytes > 65536) return EINVAL;
    if (bytes && !headers[bytes - 1]) bytes--;
    for (size_t line = 0; line < bytes;)
    {
        size_t end = line, next;
        while (end < bytes && headers[end] != '\n') end++;
        next = end < bytes ? end + 1 : end;
        if (end > line && headers[end - 1] == '\r') end--;
        if (end - line > name_bytes && headers[line + name_bytes] == ':')
        {
            size_t i = 0;
            for (; i < name_bytes; i++)
            {
                char c = headers[line + i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
                if (c != name[i]) break;
            }
            if (i == name_bytes)
            {
                size_t start = line + name_bytes + 1;
                if (found++) return EINVAL;
                while (start < end && (headers[start] == ' ' || headers[start] == '\t')) start++;
                while (end > start && (headers[end - 1] == ' ' || headers[end - 1] == '\t')) end--;
                *value = headers + start;
                *value_bytes = end - start;
            }
        }
        line = next;
    }
    return found == 1 ? 0 : EINVAL;
}

#define FOREVER_DOWNLOAD_URL_BYTES 8192
#define FOREVER_DOWNLOAD_HOST_BYTES 256

/* An authority-only diagnostic never exposes a CDN's presigned path or query.
 * Userinfo and whitespace/control characters are not accepted as URL input. */
static inline int forever_download_host(const char *url, char *host, size_t *origin_bytes)
{
    size_t authority_end = 8, bytes = strlen(url);
    if (bytes >= FOREVER_DOWNLOAD_URL_BYTES || bytes <= 8 || strncmp(url, "https://", 8)) return EINVAL;
    for (size_t i = 8; i < bytes; i++)
        if ((unsigned char)url[i] <= ' ' || url[i] == '\\') return EINVAL;
    while (authority_end < bytes && url[authority_end] != '/' &&
           url[authority_end] != '?' && url[authority_end] != '#')
    {
        if (url[authority_end] == '@') return EINVAL;
        authority_end++;
    }
    if (authority_end == 8 || authority_end - 8 >= FOREVER_DOWNLOAD_HOST_BYTES) return EINVAL;
    memcpy(host, url + 8, authority_end - 8);
    host[authority_end - 8] = 0;
    *origin_bytes = authority_end;
    return 0;
}

/* RFC path dot segments are removed without touching the query/fragment. */
static inline void forever_download_normalize_path(char *url, size_t origin_bytes)
{
    char *start = url + origin_bytes, *at = start, *out = start, *end = start;
    if (*start != '/') return;
    while (*end && *end != '?' && *end != '#') end++;
    while (at < end)
    {
        if (end - at >= 3 && !memcmp(at, "/./", 3)) { at += 2; continue; }
        if (end - at == 2 && !memcmp(at, "/.", 2)) { at += 2; *out++ = '/'; continue; }
        if ((end - at >= 4 && !memcmp(at, "/../", 4)) ||
            (end - at == 3 && !memcmp(at, "/..", 3)))
        {
            int terminal = end - at == 3;
            at += 3;
            while (out > start && out[-1] != '/') out--;
            if (out > start) out--;
            if (terminal) *out++ = '/';
            continue;
        }
        *out++ = *at++;
        while (at < end && *at != '/') *out++ = *at++;
    }
    memmove(out, end, strlen(end) + 1);
}

/* Absolute HTTPS and relative references retain HTTPS. Relative file paths
 * use the current directory, not the origin root; queries keep the base path. */
static inline int forever_download_location(const char *headers, size_t bytes, const char *current,
                                             char url[FOREVER_DOWNLOAD_URL_BYTES])
{
    const char *value = NULL;
    size_t value_bytes = 0, origin_bytes = 0, prefix = 0;
    char host[FOREVER_DOWNLOAD_HOST_BYTES];
    if (forever_download_header(headers, bytes, "location", &value, &value_bytes) ||
        !value_bytes || value_bytes >= FOREVER_DOWNLOAD_URL_BYTES) return EINVAL;
    for (size_t i = 0; i < value_bytes; i++)
        if ((unsigned char)value[i] <= ' ' || value[i] == '\\') return EINVAL;
    if (value_bytes >= 8 && !memcmp(value, "https://", 8))
    {
        memcpy(url, value, value_bytes);
    }
    else if (value_bytes >= 2 && value[0] == '/' && value[1] == '/')
    {
        prefix = 6; /* retain HTTPS for a network-path reference */
        if (prefix + value_bytes >= FOREVER_DOWNLOAD_URL_BYTES) return EINVAL;
        memcpy(url, "https:", prefix);
        memcpy(url + prefix, value, value_bytes);
    }
    else
    {
        for (size_t i = 0; i < value_bytes && value[i] != '/' && value[i] != '?' && value[i] != '#'; i++)
            if (value[i] == ':') return EINVAL;
        if (forever_download_host(current, host, &origin_bytes)) return EINVAL;
        size_t base_bytes = origin_bytes, path_end = origin_bytes;
        int add_slash = 0;
        if (value[0] != '/')
        {
            while (current[path_end] && current[path_end] != '?' && current[path_end] != '#') path_end++;
            if (value[0] == '?') base_bytes = path_end;
            else if (value[0] == '#')
            {
                base_bytes = path_end;
                while (current[base_bytes] && current[base_bytes] != '#') base_bytes++;
            }
            else
                for (size_t i = origin_bytes; i < path_end; i++)
                    if (current[i] == '/') base_bytes = i + 1;
            add_slash = base_bytes == origin_bytes;
        }
        prefix = base_bytes + add_slash;
        if (prefix + value_bytes >= FOREVER_DOWNLOAD_URL_BYTES) return EINVAL;
        memmove(url, current, base_bytes);
        if (add_slash) url[base_bytes] = '/';
        memcpy(url + prefix, value, value_bytes);
    }
    url[prefix + value_bytes] = 0;
    if (forever_download_host(url, host, &origin_bytes)) return EINVAL;
    forever_download_normalize_path(url, origin_bytes);
    return 0;
}

/* The resumed response must cover exactly the missing suffix of this model. */
static inline int forever_download_content_range(const char *headers, size_t bytes,
                                                  uint64_t offset, uint64_t size)
{
    const char *at = NULL, *limit;
    size_t value_bytes = 0;
    uint64_t start, last, total;
    if (forever_download_header(headers, bytes, "content-range", &at, &value_bytes) ||
        value_bytes < 6 || memcmp(at, "bytes ", 6)) return EINVAL;
    limit = at + value_bytes;
    at += 6;
    if (forever_download_number(&at, limit, &start) || at == limit || *at++ != '-' ||
        forever_download_number(&at, limit, &last) || at == limit || *at++ != '/' ||
        start != offset || !size || last != size - 1) return EINVAL;
    if (at < limit && *at == '*') at++;
    else if (forever_download_number(&at, limit, &total) || total != size) return EINVAL;
    return at == limit ? 0 : EINVAL;
}

static inline int forever_download_backoff(const struct forever_download *download, unsigned seconds)
{
    if (forever_download_canceled(download)) return ECANCELED;
    if (download->wait) download->wait(download->opaque, seconds * 1000);
    else
        for (unsigned elapsed = 0; elapsed < seconds * 1000; elapsed += 10)
        {
            if (forever_download_canceled(download)) return ECANCELED;
            usleep(10000);
        }
    return forever_download_canceled(download) ? ECANCELED : 0;
}

/* Returns zero only after fsync + close + atomic rename. An error or cancel
 * removes .part without touching an existing published file. Memory is bounded
 * independently of model size, and a chunked response still has an exact cap. */
static inline int forever_download_run(int tmpl, const struct forever_download *download,
                                       const char **step, uint64_t *total, uint64_t *length)
{
    static const unsigned backoff[] = {1, 2, 4, 8, 8};
    unsigned char buffer[65536], first[2] = {0};
    struct sha256_context hash;
    int req = -1, fd = -1, result = 0;
    int resumable = download->resume && download->size && download->sha256;
    int attempts = resumable ? 6 : 1;
    uint64_t cap = download->size ? download->size : download->cap;
    *total = *length = 0;
    if (download->info) *download->info = (struct forever_download_info){0};
    if (download->sha256) sha256_init(&hash);
#define FOREVER_DOWNLOAD_CALL(label, call) \
    do { *step = label; if ((result = (call)) < 0) goto done; } while (0)
#define FOREVER_DOWNLOAD_CANCEL() \
    do { if (forever_download_canceled(download)) { *step = "canceled"; result = ECANCELED; goto done; } } while (0)
    *step = "invalid HTTPS URL";
    if (!download->url || strncmp(download->url, "https://", 8) || !download->url[8])
    { result = EINVAL; goto done; }
    for (int attempt = 1; attempt <= attempts; attempt++)
    {
        char url[FOREVER_DOWNLOAD_URL_BYTES], host[FOREVER_DOWNLOAD_HOST_BYTES];
        int status = 0, length_type = 0, first_read = 1;
        uint64_t offset = *total, received = 0;
        size_t origin_bytes = 0;
        *length = 0;
        if (download->info)
            *download->info = (struct forever_download_info){.attempt = attempt};
        *step = "invalid HTTPS URL";
        if (forever_download_host(download->url, host, &origin_bytes))
        { result = EINVAL; goto done; }
        memcpy(url, download->url, strlen(download->url) + 1);
        for (int hop = 0; ; hop++)
        {
            FOREVER_DOWNLOAD_CANCEL();
            FOREVER_DOWNLOAD_CALL("creating request", req = sceHttp2CreateRequestWithURL(tmpl, "GET", url, 0));
            if (download->request) download->request(download->opaque, req);
            if (offset)
            {
                char range[48];
                snprintf(range, sizeof(range), "bytes=%llu-", (unsigned long long)offset);
                FOREVER_DOWNLOAD_CALL("setting download range", sceHttp2AddRequestHeader(req, "Range", range, 0));
            }
            FOREVER_DOWNLOAD_CANCEL();
            FOREVER_DOWNLOAD_CALL("sending request", sceHttp2SendRequest(req, NULL, 0));
            FOREVER_DOWNLOAD_CANCEL();
            FOREVER_DOWNLOAD_CALL("reading HTTP status", sceHttp2GetStatusCode(req, &status));
            if (download->info) download->info->status = status;
            FOREVER_DOWNLOAD_CALL("reading content length", sceHttp2GetResponseContentLength(req, &length_type, length));
            if (length_type != 0) *length = 0;
            if (download->hop) download->hop(download->opaque, hop, status, host, *length);
            if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) break;
            *step = "redirect hop limit";
            if (hop == 5) { result = ELOOP; goto done; }
            char *headers = NULL;
            size_t header_bytes = 0;
            FOREVER_DOWNLOAD_CALL("reading redirect Location", sceHttp2GetAllResponseHeaders(req, &headers, &header_bytes));
            *step = "invalid redirect Location";
            if ((result = forever_download_location(headers, header_bytes, url, url))) goto done;
            if ((result = forever_download_host(url, host, &origin_bytes))) goto done;
            /* Do not consume a redirect body or carry auto-redirect state into
             * the final request: only a fresh GET may supply model/installer bytes. */
            if (download->request) download->request(download->opaque, -1);
            sceHttp2DeleteRequest(req);
            req = -1;
        }
        if (attempt > 1 && download->resumed)
            download->resumed(download->opaque, attempt, offset, status);
        if (offset && status == 206)
        {
            char *headers = NULL;
            size_t header_bytes = 0;
            FOREVER_DOWNLOAD_CALL("reading Content-Range", sceHttp2GetAllResponseHeaders(req, &headers, &header_bytes));
            *step = "invalid Content-Range";
            if ((result = forever_download_content_range(headers, header_bytes, offset, download->size))) goto done;
        }
        else if (status == 200)
        {
            if (offset)
            {
                /* A CDN may ignore Range. Discard both the partial bytes and
                 * their hash before accepting its complete replacement body. */
                *step = "restarting temporary download";
                result = close(fd);
                fd = -1;
                if (result) { result = errno; goto done; }
                *total = offset = 0;
                if (download->sha256) sha256_init(&hash);
            }
        }
        else
        {
            *step = "HTTP status is not 200 after redirects";
            result = status;
            goto done;
        }
        *step = "announced length mismatch";
        if ((*length && *length > cap - offset) ||
            (download->size && *length && *length != download->size - offset))
        { result = EINVAL; goto done; }
        if (fd < 0)
        {
            *step = "opening temporary download";
            fd = open(download->part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) { result = errno; goto done; }
        }
        if (download->progress) download->progress(download->opaque, *total, download->size ? download->size : *length);
        for (;;)
        {
            FOREVER_DOWNLOAD_CANCEL();
            *step = "reading download";
            result = sceHttp2ReadData(req, buffer, sizeof(buffer));
            if (first_read && download->first_read)
                download->first_read(download->opaque, result);
            first_read = 0;
            if (result < 0)
            {
                if (resumable) goto retry;
                goto done;
            }
            if (!result)
            {
                if (resumable && *total < download->size)
                {
                    *step = "download length mismatch";
                    result = EINVAL;
                    goto retry;
                }
                break;
            }
            size_t count = (size_t)result, written_bytes = 0;
            FOREVER_DOWNLOAD_CANCEL();
            *step = "download exceeds expected length";
            if (count > sizeof(buffer) || count > cap - *total || (*length && count > *length - received))
            { result = EFBIG; goto done; }
            for (size_t i = 0; *total + i < 2 && i < count; i++) first[*total + i] = buffer[i];
            *step = "writing download";
            while (written_bytes < count)
            {
                ssize_t written = write(fd, buffer + written_bytes, count - written_bytes);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { result = written < 0 ? errno : EIO; goto done; }
                written_bytes += (size_t)written;
            }
            if (download->sha256) sha256_update(&hash, buffer, count);
            *total += count;
            received += count;
            if (download->progress) download->progress(download->opaque, *total, download->size ? download->size : *length);
        }
        FOREVER_DOWNLOAD_CANCEL();
        *step = "download length mismatch";
        if ((download->size && *total != download->size) || (*length && received != *length))
        { result = EINVAL; goto done; }
        break;
retry:
        if (attempt == attempts) goto done;
        if (download->request) download->request(download->opaque, -1);
        sceHttp2DeleteRequest(req);
        req = -1;
        if ((result = forever_download_backoff(download, backoff[attempt - 1])))
        { *step = "canceled"; goto done; }
    }
    *step = "installer MZ signature mismatch";
    if (download->mz && (*total < 2 || first[0] != 'M' || first[1] != 'Z'))
    { result = EINVAL; goto done; }
    *step = "voice model SHA-256 mismatch";
    if (download->sha256 && (result = forever_download_digest(&hash, download->sha256))) goto done;
    *step = "syncing download";
    if (fsync(fd)) { result = errno; goto done; }
    *step = "closing download";
    result = close(fd);
    fd = -1;
    if (result) { result = errno; goto done; }
    FOREVER_DOWNLOAD_CANCEL();
    *step = "publishing download";
    if (rename(download->part, download->destination)) { result = errno; goto done; }
    result = 0;
done:
    if (fd >= 0) close(fd);
    if (download->request && req >= 0) download->request(download->opaque, -1);
    if (req >= 0) sceHttp2DeleteRequest(req);
    if (result) unlink(download->part);
#undef FOREVER_DOWNLOAD_CANCEL
#undef FOREVER_DOWNLOAD_CALL
    return result;
}

#endif
