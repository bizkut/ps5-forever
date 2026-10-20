/* Host behavior tests for Forever's popup state and bounded file operations. */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "../../title/forever-menu.h"
#include "forever-setup.h"
#include "forever-menu-settings.h"

static int download_syncs, download_renames, download_fail_sync, download_fail_rename;
static int download_test_fsync(int fd)
{
    ++download_syncs;
    if (download_fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}
static int download_test_rename(const char *from, const char *to)
{
    assert(download_syncs);
    ++download_renames;
    if (download_fail_rename) { errno = EACCES; return -1; }
    return rename(from, to);
}
#define fsync download_test_fsync
#define rename download_test_rename
#include "forever-download.h"
#undef rename
#undef fsync

static struct forever_download_info download_info;
static uint64_t download_total, download_length;

static struct
{
    const char *body;
    size_t size, offset, chunk;
    uint64_t length;
    int status, length_type, auto_redirect, request;
    int deleted, canceled, cancel_at_progress, progress_count, percent, read_error;
    struct
    {
        const char *body, *headers;
        uint64_t length;
        int status, eof_error;
    } responses[36];
    int response_count, created, range_calls, resume_logs, wait_count, cancel_backoff;
    uint64_t ranges[36], hop_lengths[36];
    int hop_logs, hops[36], hop_statuses[36], first_reads, first_results[6];
    char hosts[36][FOREVER_DOWNLOAD_HOST_BYTES], urls[36][512];
    unsigned waits[5];
    char url[512], destination[1024], part[1024];
} download_http;

int sceNetInit(void) { return 0; }
int sceNetPoolCreate(const char *name, int size, int flags)
{
    assert(name && size > 0 && !flags);
    return 1;
}
int sceNetPoolDestroy(int pool) { assert(pool == 1); return 0; }
int sceSslInit(size_t size) { assert(size > 0); return 2; }
int sceSslTerm(int ssl) { assert(ssl == 2); return 0; }
int sceHttp2Init(int pool, int ssl, size_t size, int concurrency)
{
    assert(pool == 1 && ssl == 2 && size > 0 && concurrency == 1);
    return 3;
}
int sceHttp2Term(int http) { assert(http == 3); return 0; }
int sceHttp2CreateTemplate(int http, const char *agent, int version, int flags)
{
    assert(http == 3 && agent && version == 3 && flags == 1);
    return 4;
}
int sceHttp2DeleteTemplate(int tmpl) { assert(tmpl == 4); return 0; }
int sceHttp2SetAutoRedirect(int tmpl, int enabled)
{
    assert(tmpl == 4);
    download_http.auto_redirect = enabled;
    return 0;
}
int sceHttp2SetResolveTimeOut(int tmpl, unsigned timeout)
{
    assert(tmpl == 4 && timeout == 10000000);
    return 0;
}
int sceHttp2SetConnectTimeOut(int tmpl, unsigned timeout)
{
    assert(tmpl == 4 && timeout == 10000000);
    return 0;
}
int sceHttp2SetRecvTimeOut(int tmpl, unsigned timeout)
{
    assert(tmpl == 4 && timeout == 10000000);
    return 0;
}
int sceHttp2CreateRequestWithURL(int tmpl, const char *method, const char *url, uint64_t size)
{
    assert(tmpl == 4 && !strcmp(method, "GET") && !size);
    snprintf(download_http.url, sizeof(download_http.url), "%s", url);
    assert(download_http.created < 36);
    snprintf(download_http.urls[download_http.created], sizeof(download_http.urls[0]), "%s", url);
    if (download_http.response_count)
    {
        unsigned at = (unsigned)download_http.created;
        assert(at < (unsigned)download_http.response_count);
        download_http.body = download_http.responses[at].body;
        download_http.size = strlen(download_http.body);
        download_http.offset = 0;
        download_http.status = download_http.responses[at].status;
        download_http.length = download_http.responses[at].length;
    }
    download_http.created++;
    return 5;
}
int sceHttp2DeleteRequest(int request)
{
    assert(request == 5 && download_http.request == -1);
    download_http.deleted++;
    return 0;
}
int sceHttp2AbortRequest(int request) { assert(request == 5); return 0; }
int sceHttp2AddRequestHeader(int request, const char *name, const char *value, unsigned mode)
{
    unsigned long long offset;
    assert(request == 5 && !strcmp(name, "Range") && !mode);
    assert(sscanf(value, "bytes=%llu-", &offset) == 1);
    assert(download_http.created > 1);
    download_http.ranges[download_http.created - 1] = offset;
    download_http.range_calls++;
    return 0;
}
int sceHttp2GetAllResponseHeaders(int request, char **headers, size_t *bytes)
{
    assert(request == 5 && download_http.response_count);
    const char *text = download_http.responses[download_http.created - 1].headers;
    *headers = (char *)text;
    *bytes = text ? strlen(text) + 1 : 0;
    return 0;
}
int sceHttp2SendRequest(int request, const void *body, size_t size)
{
    assert(request == 5 && !body && !size);
    return 0;
}
int sceHttp2GetStatusCode(int request, int *status)
{
    assert(request == 5);
    *status = download_http.status;
    return 0;
}
int sceHttp2GetResponseContentLength(int request, int *type, uint64_t *length)
{
    assert(request == 5);
    *type = download_http.length_type;
    *length = download_http.length;
    return 0;
}
int sceHttp2ReadData(int request, void *data, size_t size)
{
    char previous[16];
    assert(request == 5);
    assert(download_http.status == 200 || download_http.status == 206);
    /* A visible old model survives every in-flight chunk. */
    assert(forever_read_file(download_http.destination, previous, sizeof(previous)) == 3);
    assert(!strcmp(previous, "old"));
    if (download_http.read_error) return download_http.read_error;
    size_t count = download_http.size - download_http.offset;
    if (!count && download_http.response_count)
        return download_http.responses[download_http.created - 1].eof_error;
    if (count > download_http.chunk) count = download_http.chunk;
    assert(count <= size);
    memcpy(data, download_http.body + download_http.offset, count);
    download_http.offset += count;
    return (int)count;
}

static int download_canceled(void *unused)
{
    (void)unused;
    return download_http.canceled;
}
static void download_request(void *unused, int request)
{
    (void)unused;
    download_http.request = request;
}
static void download_progress(void *unused, uint64_t total, uint64_t length)
{
    (void)unused;
    assert(length && total <= length);
    download_http.percent = (int)(100 * total / length);
    download_http.progress_count++;
    if (download_http.cancel_at_progress == download_http.progress_count)
        download_http.canceled = 1;
}

static void download_resumed(void *unused, int attempt, uint64_t offset, int status)
{
    (void)unused;
    assert(attempt == download_http.resume_logs + 2);
    assert(offset == download_http.ranges[download_http.created - 1]);
    assert(status == download_http.status);
    download_http.resume_logs++;
}

static void download_hop(void *unused, int hop, int status, const char *host, uint64_t length)
{
    (void)unused;
    int at = download_http.hop_logs++;
    assert(at < 36 && !strchr(host, '?') && !strchr(host, '/') && !strchr(host, '@'));
    download_http.hops[at] = hop;
    download_http.hop_statuses[at] = status;
    download_http.hop_lengths[at] = length;
    snprintf(download_http.hosts[at], sizeof(download_http.hosts[at]), "%s", host);
}

static void download_first_read(void *unused, int result)
{
    (void)unused;
    assert(download_http.first_reads < 6);
    download_http.first_results[download_http.first_reads++] = result;
}

static void download_wait(void *unused, unsigned milliseconds)
{
    (void)unused;
    assert(download_http.wait_count < 5);
    download_http.waits[download_http.wait_count++] = milliseconds;
    if (download_http.cancel_backoff) download_http.canceled = 1;
}

static char root[256];

static void make_path(char *out, size_t size, const char *relative)
{
    assert(snprintf(out, size, "%s/%s", root, relative) > 0);
}

static void put(const char *relative, const char *text)
{
    char name[1024], *slash;
    make_path(name, sizeof(name), relative);
    slash = strrchr(name, '/');
    assert(slash);
    *slash = 0;
    assert(!forever_mkdirs(name));
    *slash = '/';
    assert(!forever_write_file(name, text, strlen(text)));
}

static int text_is(const char *relative, const char *expected)
{
    char name[1024], text[16384];
    make_path(name, sizeof(name), relative);
    return forever_read_file(name, text, sizeof(text)) >= 0 && !strcmp(text, expected);
}

static struct forever_download download_case(const char *body)
{
    memset(&download_http, 0, sizeof(download_http));
    download_syncs = download_renames = download_fail_sync = download_fail_rename = 0;
    download_http.body = body;
    download_http.size = strlen(body);
    download_http.chunk = 1;
    download_http.length = 3;
    download_http.status = 200;
    make_path(download_http.destination, sizeof(download_http.destination), "download/model");
    make_path(download_http.part, sizeof(download_http.part), "download/model.part");
    put("download/model", "old");
    return (struct forever_download){
        .url = FOREVER_VOICE_MODEL_URL, .part = download_http.part,
        .destination = download_http.destination, .size = 3,
        .sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        .canceled = download_canceled, .request = download_request,
        .progress = download_progress, .resumed = download_resumed,
        .wait = download_wait, .info = &download_info,
        .hop = download_hop, .first_read = download_first_read};
}

static int download_run(struct forever_download *download, const char **step)
{
    struct forever_https http;
    download_total = download_length = 0;
    assert(!forever_https_open(&http, step));
    assert(download_http.auto_redirect == 0);
    int result = forever_download_run(http.tmpl, download, step, &download_total, &download_length);
    forever_https_close(&http);
    assert(access(download_http.part, F_OK) && errno == ENOENT);
    return result;
}

static void test_voice_download(void)
{
    const char *step;
    struct forever_download download = download_case("abc");
    assert(FOREVER_VOICE_MODEL_BYTES == 190085487);
    assert(!strcmp(FOREVER_VOICE_MODEL_SHA256,
        "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb"));
    assert(!strcmp(FOREVER_VOICE_MODEL_URL,
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/"
        "5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small-q5_1.bin"));
    assert(!download_run(&download, &step));
    assert(!strcmp(download_http.url, FOREVER_VOICE_MODEL_URL));
    assert(download_http.deleted == 1 && download_http.percent == 100 &&
           download_http.progress_count == 4);
    assert(download_syncs == 1 && download_renames == 1 && text_is("download/model", "abc"));

    download = download_case("abc");
    download_http.length_type = 1; /* chunked response still checks the pinned size */
    download_http.length = UINT64_MAX;
    assert(!download_run(&download, &step));
    assert(text_is("download/model", "abc"));

    download = download_case("abc");
    download_http.length = 2;
    assert(download_run(&download, &step) == EINVAL);
    assert(!strcmp(step, "announced length mismatch") && text_is("download/model", "old"));

    download = download_case("ab");
    assert(download_run(&download, &step) == EINVAL);
    assert(!strcmp(step, "download length mismatch") && text_is("download/model", "old"));

    download = download_case("abcd");
    assert(download_run(&download, &step) == EFBIG);
    assert(!strcmp(step, "download exceeds expected length") && text_is("download/model", "old"));

    download = download_case("abd");
    assert(download_run(&download, &step) == EINVAL);
    assert(!strcmp(step, "voice model SHA-256 mismatch") && text_is("download/model", "old"));
    assert(!download_syncs && !download_renames);

    download = download_case("abc");
    download_http.status = 404;
    assert(download_run(&download, &step) == 404 && text_is("download/model", "old"));

    download = download_case("abc");
    download_http.read_error = -1234;
    assert(download_run(&download, &step) == -1234 && text_is("download/model", "old"));

    download = download_case("abc");
    download_http.cancel_at_progress = 2;
    assert(download_run(&download, &step) == ECANCELED && text_is("download/model", "old"));
    assert(download_http.deleted == 1 && !download_renames);

    download = download_case("abc");
    download_http.canceled = 1;
    assert(download_run(&download, &step) == ECANCELED && !download_http.deleted);

    download = download_case("abc");
    download_fail_sync = 1;
    assert(download_run(&download, &step) == EIO && !download_renames);
    assert(text_is("download/model", "old"));

    download = download_case("abc");
    download_fail_rename = 1;
    assert(download_run(&download, &step) == EACCES && text_is("download/model", "old"));

    download = download_case("abc");
    download.url = "http://huggingface.co/model";
    assert(download_run(&download, &step) == EINVAL && !download_http.deleted);

    /* The installer uses this same stream, retaining its bounded MZ check. */
    download = download_case("MZabc");
    download_http.length = 5;
    download.size = 0;
    download.cap = 256u * 1024u * 1024u;
    download.sha256 = NULL;
    download.mz = 1;
    assert(!download_run(&download, &step) && text_is("download/model", "MZabc"));
    download = download_case("abc");
    download.size = 0;
    download.cap = 3;
    download.sha256 = NULL;
    download.mz = 1;
    assert(download_run(&download, &step) == EINVAL && text_is("download/model", "old"));
}

static void test_voice_resume(void)
{
    static const char range_two[] = "HTTP/2 206\r\nContent-Range: bytes 2-2/3\r\n";
    static const char range_one[] = "content-range: bytes 1-2/3\r\n";
    struct forever_download download = download_case("ab");
    const char *step;
    download.resume = 1;
    download_http.response_count = 2;
    download_http.responses[0].body = "ab";
    download_http.responses[0].status = 200;
    download_http.responses[0].length = 3;
    download_http.responses[1].body = "c";
    download_http.responses[1].status = 206;
    download_http.responses[1].length = 1;
    download_http.responses[1].headers = range_two;
    assert(!download_run(&download, &step) && text_is("download/model", "abc"));
    assert(download_http.created == 2 && download_http.range_calls == 1);
    assert(download_http.ranges[1] == 2 && download_http.resume_logs == 1);
    assert(download_http.wait_count == 1 && download_http.waits[0] == 1000);
    assert(download_info.attempt == 2 && download_info.status == 206 && download_total == 3);

    /* A transient failure on the first read has the same bounded retry path. */
    download = download_case("");
    download.resume = 1;
    download_http.response_count = 2;
    download_http.responses[0].body = "";
    download_http.responses[0].status = 200;
    download_http.responses[0].length = 3;
    download_http.responses[0].eof_error = -1234;
    download_http.responses[1].body = "abc";
    download_http.responses[1].status = 200;
    download_http.responses[1].length = 3;
    assert(!download_run(&download, &step) && text_is("download/model", "abc"));
    assert(download_http.created == 2 && !download_http.range_calls && download_http.resume_logs == 1);
    assert(download_http.first_reads == 2 && download_http.first_results[0] == -1234 &&
           download_http.first_results[1] == 1);

    /* A read/receive error after progress uses the same append-only SHA. */
    download = download_case("a");
    download.resume = 1;
    download_http.response_count = 2;
    download_http.responses[0].body = "a";
    download_http.responses[0].status = 200;
    download_http.responses[0].length = 3;
    download_http.responses[0].eof_error = -1234;
    download_http.responses[1].body = "bc";
    download_http.responses[1].status = 206;
    download_http.responses[1].length = 2;
    download_http.responses[1].headers = range_one;
    assert(!download_run(&download, &step) && text_is("download/model", "abc"));
    assert(download_http.ranges[1] == 1);

    /* A server ignoring Range sends 200: truncate and reset SHA before retry. */
    download = download_case("ab");
    download.resume = 1;
    download_http.response_count = 2;
    download_http.responses[0].body = "ab";
    download_http.responses[0].status = 200;
    download_http.responses[0].length = 3;
    download_http.responses[1].body = "abc";
    download_http.responses[1].status = 200;
    download_http.responses[1].length = 3;
    assert(!download_run(&download, &step) && text_is("download/model", "abc"));
    assert(download_http.ranges[1] == 2 && download_info.status == 200);

    download = download_case("ab");
    download.resume = 1;
    download_http.response_count = 2;
    download_http.responses[0].body = "ab";
    download_http.responses[0].status = 200;
    download_http.responses[0].length = 3;
    download_http.responses[1].body = "c";
    download_http.responses[1].status = 206;
    download_http.responses[1].length = 1;
    download_http.responses[1].headers = range_one; /* starts at 1, requested 2 */
    assert(download_run(&download, &step) == EINVAL);
    assert(!strcmp(step, "invalid Content-Range") && text_is("download/model", "old"));
    assert(download_http.created == 2 && !download_renames);

    download = download_case("a");
    download.resume = 1;
    download_http.response_count = 6;
    for (int i = 0; i < 6; i++)
    {
        download_http.responses[i].body = i ? "" : "a";
        download_http.responses[i].status = i ? 206 : 200;
        download_http.responses[i].length = i ? 2 : 3;
        download_http.responses[i].headers = range_one;
    }
    assert(download_run(&download, &step) == EINVAL && text_is("download/model", "old"));
    assert(!strcmp(step, "download length mismatch"));
    assert(download_http.created == 6 && download_http.deleted == 6 &&
           download_http.range_calls == 5 && download_http.resume_logs == 5);
    assert(download_info.attempt == 6 && download_info.status == 206 &&
           download_total == 1 && download_length == 2);
    assert(download_http.wait_count == 5 && download_http.waits[0] == 1000 &&
           download_http.waits[1] == 2000 && download_http.waits[2] == 4000 &&
           download_http.waits[3] == 8000 && download_http.waits[4] == 8000);

    download = download_case("a");
    download.resume = 1;
    download_http.cancel_backoff = 1;
    assert(download_run(&download, &step) == ECANCELED);
    assert(!strcmp(step, "canceled") && text_is("download/model", "old"));
    assert(download_http.created == 1 && download_http.deleted == 1 &&
           download_http.wait_count == 1 && !download_http.range_calls);

    assert(!forever_download_content_range("Content-Range: bytes 2-2/*\r\n", 28, 2, 3));
    static const char *const bad_ranges[] = {
        "Content-Length: 1\r\n",
        "Content-Range: bytes 1-2/3\r\n",
        "Content-Range: bytes 2-1/3\r\n",
        "Content-Range: bytes 2-2/4\r\n",
        "Content-Range: bytes 2-2/99999999999999999999999999999999999999\r\n",
        "Content-Range: bytes 2-2/3\r\nContent-Range: bytes 2-2/3\r\n",
        "Content-Range: bytes 2-2/3junk\r\n",
    };
    for (size_t i = 0; i < sizeof(bad_ranges) / sizeof(bad_ranges[0]); i++)
        assert(forever_download_content_range(bad_ranges[i], strlen(bad_ranges[i]), 2, 3) == EINVAL);
}

static void redirect_response(int at, int status, const char *location)
{
    assert(at >= 0 && at < 36);
    download_http.responses[at].body = "redirect body must never enter the staged file";
    download_http.responses[at].status = status;
    download_http.responses[at].length = 1169;
    download_http.responses[at].headers = location;
}

static void test_download_redirects(void)
{
    const char *step;
    struct forever_download download = download_case("abc");
    download_http.response_count = 3;
    redirect_response(0, 302, "HTTP/2 302\r\nlOcAtIoN:\t https://cdn.example/model?signature=not-for-logs \r\n");
    redirect_response(1, 307, "Location: /final.bin?signature=also-not-for-logs\r\n");
    download_http.responses[2].body = "abc";
    download_http.responses[2].status = 200;
    download_http.responses[2].length = 3;
    assert(!download_run(&download, &step) && text_is("download/model", "abc"));
    assert(download_http.created == 3 && download_http.deleted == 3 && download_http.hop_logs == 3);
    assert(!strcmp(download_http.urls[0], FOREVER_VOICE_MODEL_URL));
    assert(!strcmp(download_http.urls[1], "https://cdn.example/model?signature=not-for-logs"));
    assert(!strcmp(download_http.urls[2], "https://cdn.example/final.bin?signature=also-not-for-logs"));
    assert(!strcmp(download_http.hosts[0], "huggingface.co") &&
           !strcmp(download_http.hosts[1], "cdn.example") &&
           !strcmp(download_http.hosts[2], "cdn.example"));
    assert(download_http.hops[0] == 0 && download_http.hops[2] == 2 &&
           download_http.hop_statuses[0] == 302 && download_http.hop_lengths[0] == 1169);
    assert(download_http.first_reads == 1 && download_http.first_results[0] == 1);

    /* The installer uses the same chain, including 303 as another fresh GET. */
    download = download_case("MZabc");
    download.url = "https://installer.example/Live/Setup.exe";
    download.size = 0;
    download.cap = 256u * 1024u * 1024u;
    download.sha256 = NULL;
    download.mz = 1;
    download_http.response_count = 3;
    redirect_response(0, 301, "Location: https://installer-cdn.example/Live/Setup.exe\r\n");
    redirect_response(1, 303, "Location: final.exe\r\n");
    download_http.responses[2].body = "MZabc";
    download_http.responses[2].status = 200;
    download_http.responses[2].length = 5;
    assert(!download_run(&download, &step) && text_is("download/model", "MZabc"));
    assert(!strcmp(download_http.urls[2], "https://installer-cdn.example/Live/final.exe"));
    assert(download_http.created == 3 && !download_http.range_calls);

    /* Resume re-resolves the pinned URL, then applies Range to every new hop. */
    download = download_case("ab");
    download.resume = 1;
    download_http.response_count = 4;
    redirect_response(0, 302, "Location: https://cdn.example/expired-soon?signature=old\r\n");
    download_http.responses[1].body = "ab";
    download_http.responses[1].status = 200;
    download_http.responses[1].length = 3;
    redirect_response(2, 308, "Location: https://cdn.example/refreshed?signature=new\r\n");
    download_http.responses[3].body = "c";
    download_http.responses[3].status = 206;
    download_http.responses[3].length = 1;
    download_http.responses[3].headers = "Content-Range: bytes 2-2/3\r\n";
    assert(!download_run(&download, &step) && text_is("download/model", "abc"));
    assert(!strcmp(download_http.urls[0], FOREVER_VOICE_MODEL_URL) &&
           !strcmp(download_http.urls[2], FOREVER_VOICE_MODEL_URL));
    assert(download_http.range_calls == 2 && download_http.ranges[2] == 2 &&
           download_http.ranges[3] == 2 && download_http.resume_logs == 1);
    assert(download_http.hops[2] == 0 && download_http.first_reads == 2);

    download = download_case("abc");
    download_http.response_count = 6;
    for (int i = 0; i < 5; i++) redirect_response(i, 302, "Location: /again\r\n");
    download_http.responses[5].body = "abc";
    download_http.responses[5].status = 200;
    download_http.responses[5].length = 3;
    assert(!download_run(&download, &step) && download_http.created == 6);

    download = download_case("abc");
    download_http.response_count = 6;
    for (int i = 0; i < 6; i++) redirect_response(i, 302, "Location: /again\r\n");
    assert(download_run(&download, &step) == ELOOP && !strcmp(step, "redirect hop limit"));
    assert(download_http.created == 6 && download_http.deleted == 6 &&
           !download_http.first_reads && text_is("download/model", "old"));

    static const char *const bad_locations[] = {
        "Content-Length: 1169\r\n", "Location:\t \r\n",
        "Location: http://insecure.example/model\r\n",
        "Location: https://user:secret@cdn.example/model\r\n",
        "Location: https://cdn.example/path\r\nLocation: /second\r\n",
    };
    for (size_t i = 0; i < sizeof(bad_locations) / sizeof(bad_locations[0]); i++)
    {
        download = download_case("abc");
        download_http.response_count = 1;
        redirect_response(0, 302, bad_locations[i]);
        assert(download_run(&download, &step) == EINVAL);
        assert(!strcmp(step, "invalid redirect Location") && !download_http.first_reads);
        assert(text_is("download/model", "old"));
    }

    char url[FOREVER_DOWNLOAD_URL_BYTES], host[FOREVER_DOWNLOAD_HOST_BYTES];
    size_t origin;
    static const char absolute[] = "LOCATION: https://cdn.example/model?secret=query\r\n";
    assert(!forever_download_location(absolute, sizeof(absolute) - 1,
                                     FOREVER_VOICE_MODEL_URL, url));
    assert(!strcmp(url, "https://cdn.example/model?secret=query"));
    assert(!forever_download_host(url, host, &origin) && !strcmp(host, "cdn.example"));
    static const char relative[] = "Location: model.bin\r\n";
    assert(!forever_download_location(relative, sizeof(relative) - 1,
                                     "https://cdn.example/old/path?secret=query", url));
    assert(!strcmp(url, "https://cdn.example/old/model.bin"));
    static const char network_path[] = "Location: //other.example/new\r\n";
    assert(!forever_download_location(network_path, sizeof(network_path) - 1,
                                     FOREVER_VOICE_MODEL_URL, url));
    assert(!strcmp(url, "https://other.example/new"));
    static const struct { const char *reference, *expected; } relatives[] = {
        {"../final.bin?new=1", "https://cdn.example/a/final.bin?new=1"},
        {"../../final.bin", "https://cdn.example/final.bin"},
        {"./final.bin", "https://cdn.example/a/b/final.bin"},
        {"/root/../final.bin", "https://cdn.example/final.bin"},
        {"?new=1", "https://cdn.example/a/b/old?new=1"},
        {"#new", "https://cdn.example/a/b/old?old=1#new"},
        {".", "https://cdn.example/a/b/"},
        {"..", "https://cdn.example/a/"},
    };
    for (size_t i = 0; i < sizeof(relatives) / sizeof(relatives[0]); i++)
    {
        char header[128];
        int count = snprintf(header, sizeof(header), "Location: %s\r\n", relatives[i].reference);
        assert(count > 0 && (size_t)count < sizeof(header));
        assert(!forever_download_location(header, (size_t)count,
                                         "https://cdn.example/a/b/old?old=1#old", url));
        assert(!strcmp(url, relatives[i].expected));
    }
    char overlong[FOREVER_DOWNLOAD_URL_BYTES + 32];
    memcpy(overlong, "Location: https://cdn.example/", 29);
    memset(overlong + 29, 'x', sizeof(overlong) - 30);
    overlong[sizeof(overlong) - 1] = 0;
    assert(forever_download_location(overlong, sizeof(overlong),
                                    FOREVER_VOICE_MODEL_URL, url) == EINVAL);
}

static void test_mouse_scroll_menu(void)
{
    static struct forever_menu_state menu;
    struct forever_menu_values values = {FOREVER_FG_ON, 1, 0, 0, 0};
    struct forever_menu_result result;
    char text[FOREVER_MENU_TEXT_BYTES];
    forever_menu_init(&menu);
    forever_menu_open(&menu, &values);
    forever_menu_render(&menu, text, sizeof(text));
    const char *hud = strstr(text, "Performance overlay\tOff");
    const char *mouse = strstr(text, "Invert mouse scroll\tOff");
    const char *voice = strstr(text, "Voice-to-Text\tOff");
    assert(hud && mouse && voice && hud < mouse && mouse < voice);
    assert(FOREVER_MENU_LINE_INVERT_SCROLL == FOREVER_MENU_LINE_HUD + 1);
    assert(FOREVER_MENU_LINE_VOICE == FOREVER_MENU_LINE_INVERT_SCROLL + 1);
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_INVERT_SCROLL);
    assert(result.command == FOREVER_MENU_COMMAND_SET_INVERT_SCROLL && result.enabled);
    assert(!menu.values.invert_scroll); /* commit only after a successful save */
    forever_menu_status(&menu, "Could not save the setting (error 13).");
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Invert mouse scroll\tOff") && strstr(text, "error 13"));
    forever_menu_commit(&menu, result.command, result.enabled);
    assert(menu.values.invert_scroll && !menu.status[0]);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Invert mouse scroll\tOn"));
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_SET_INVERT_SCROLL && !result.enabled);
    forever_menu_commit(&menu, result.command, result.enabled);
    assert(!menu.values.invert_scroll);
    forever_menu_move(&menu, -1);
    assert(menu.selected == FOREVER_MENU_HUD);
    forever_menu_move(&menu, 1);
    assert(menu.selected == FOREVER_MENU_INVERT_SCROLL);
    forever_menu_move(&menu, 1);
    assert(menu.selected == FOREVER_MENU_VOICE);
}

static void test_voice_menu(void)
{
    static struct forever_menu_state menu;
    struct forever_menu_values values = {FOREVER_FG_ON, 1, 0, 0, 0};
    struct forever_menu_result result;
    char text[FOREVER_MENU_TEXT_BYTES];
    forever_menu_init(&menu);
    forever_menu_open(&menu, &values);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Voice-to-Text\tOff"));
    assert(strstr(text, "Proximity Voice Chat\tComing later"));
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_PROXIMITY_VOICE);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.selected == FOREVER_MENU_FRAME_GENERATION);
    assert(!strcmp(menu.status, "Proximity Voice Chat is coming later."));
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_VOICE);
    assert(result.command == FOREVER_MENU_COMMAND_SET_VOICE && result.enabled);
    assert(!menu.values.voice); /* no optimistic on before the verified download */
    menu.voice_downloading = 1;
    forever_menu_status(&menu, "Downloading voice model\xe2\x80\xa6 37%");
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Downloading voice model\xe2\x80\xa6 37%"));
    assert(forever_menu_select(&menu).command == FOREVER_MENU_COMMAND_NONE);
    assert(forever_menu_click_line(&menu, FOREVER_MENU_LINE_REINSTALL).command ==
           FOREVER_MENU_COMMAND_NONE && menu.page == FOREVER_MENU_MAIN);
    assert(forever_menu_back(&menu).command == FOREVER_MENU_COMMAND_CLOSE);
    menu.voice_downloading = 0;
    forever_menu_commit(&menu, FOREVER_MENU_COMMAND_SET_VOICE, 1);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Voice-to-Text\tOn"));
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_VOICE);
    assert(result.command == FOREVER_MENU_COMMAND_SET_VOICE && !result.enabled);
    forever_menu_commit(&menu, result.command, result.enabled);
    assert(!menu.values.voice);
    forever_menu_status(&menu, "Voice model failed: voice model SHA-256 mismatch (0x16). Voice-to-Text is off.");
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "SHA-256 mismatch") && strstr(text, "Voice-to-Text is off."));
    forever_menu_move(&menu, 1);
    assert(menu.selected == FOREVER_MENU_ADDONS); /* Coming later is not pad-selectable */
    forever_menu_close(&menu);
    forever_menu_open(&menu, &values);
    assert(!menu.voice_downloading && !menu.status[0]);
}

static void test_key_rewrite(void)
{
    char path[1024], fg[1024], hdr[1024], hud[1024], mouse[1024], voice[1024];
    int enabled = -1, hdr_enabled = -1, hud_enabled = -1, invert_scroll = -1, voice_enabled = -1;
    enum forever_frame_generation fg_mode = FOREVER_FG_OFF;

    make_path(path, sizeof(path), "switches/drive_c/wineps5-fg.txt");
    assert(!forever_mkdirs(root));
    assert(!forever_menu_rewrite_switch(path, "fg", 0));
    assert(text_is("switches/drive_c/wineps5-fg.txt", "fg=off\n"));
    assert(!forever_menu_read_switch(path, "fg", 1, &enabled) && !enabled);

    put("switches/drive_c/wineps5-fg.txt", "method=auto  fg=off\nmult=3 flow=0.75\n");
    assert(!forever_menu_rewrite_switch(path, "fg", 1));
    assert(text_is("switches/drive_c/wineps5-fg.txt",
                   "method=auto  fg=on\nmult=3 flow=0.75\n"));
    assert(!forever_menu_read_switch(path, "fg", 0, &enabled) && enabled);

    put("switches/drive_c/wineps5-fg.txt", "fg=auto method=auto mult=2\n");
    assert(!forever_menu_read_frame_generation(path, FOREVER_FG_ON, &fg_mode));
    assert(fg_mode == FOREVER_FG_AUTO);
    make_path(fg, sizeof(fg), "switches/drive_c/independent-fg.txt");
    make_path(hdr, sizeof(hdr), "switches/drive_c/independent-hdr.txt");
    make_path(hud, sizeof(hud), "switches/drive_c/independent-hud.txt");
    make_path(voice, sizeof(voice), "switches/drive_c/independent-voice.txt");
    make_path(mouse, sizeof(mouse), "switches/drive_c/wineps5-mouse.txt");
    assert(!forever_menu_read_switch(mouse, "invert_scroll", 0, &invert_scroll) && !invert_scroll);
    assert(!forever_menu_rewrite_switch(mouse, "invert_scroll", 1));
    assert(text_is("switches/drive_c/wineps5-mouse.txt", "invert_scroll=on\n"));
    put("switches/drive_c/wineps5-mouse.txt", "speed=2\tinvert_scroll=off\nother=keep\n");
    assert(!forever_menu_rewrite_switch(mouse, "invert_scroll", 1));
    assert(text_is("switches/drive_c/wineps5-mouse.txt",
                   "speed=2\tinvert_scroll=on\nother=keep\n"));
    assert(!forever_menu_read_switch(mouse, "invert_scroll", 0, &invert_scroll) && invert_scroll);
    assert(!forever_menu_rewrite_switch(mouse, "invert_scroll", 0));
    assert(text_is("switches/drive_c/wineps5-mouse.txt",
                   "speed=2\tinvert_scroll=off\nother=keep\n"));
    put("switches/drive_c/wineps5-mouse.txt", "invert_scroll=invalid other=keep\n");
    assert(forever_menu_read_switch(mouse, "invert_scroll", 0, &invert_scroll) == -EINVAL);
    assert(!invert_scroll);
    assert(!forever_menu_rewrite_switch(mouse, "invert_scroll", 1));
    assert(text_is("switches/drive_c/wineps5-mouse.txt", "invert_scroll=on other=keep\n"));
    put("switches/drive_c/independent-fg.txt", "fg=invalid\n");
    put("switches/drive_c/independent-hdr.txt", "hdr=off\n");
    put("switches/drive_c/independent-hud.txt", "hud=on\n");
    assert(!forever_menu_read_switch(voice, "voice", 0, &voice_enabled) && !voice_enabled);
    put("switches/drive_c/independent-voice.txt", "voice=on device=default\n");
    assert(forever_menu_read_settings(fg, hdr, hud, mouse, voice,
        &fg_mode, &hdr_enabled, &hud_enabled, &invert_scroll, &voice_enabled) == -EINVAL);
    assert(fg_mode == FOREVER_FG_ON);
    assert(!hdr_enabled && hud_enabled); /* one bad setting cannot hide the other two */
    assert(voice_enabled); /* bad FG must not hide the independent voice setting */
    assert(invert_scroll); /* bad FG must not hide the independent mouse setting */
    put("switches/drive_c/independent-fg.txt", "fg=on\n");
    put("switches/drive_c/wineps5-mouse.txt", "invert_scroll=on invert_scroll=off\n");
    assert(forever_menu_read_settings(fg, hdr, hud, mouse, voice,
        &fg_mode, &hdr_enabled, &hud_enabled, &invert_scroll, &voice_enabled) == -EINVAL);
    assert(invert_scroll && voice_enabled && hud_enabled && !hdr_enabled); /* invalid: default On */
    assert(forever_menu_rewrite_switch(mouse, "invert_scroll", 1) == -EINVAL);
    assert(text_is("switches/drive_c/wineps5-mouse.txt", "invert_scroll=on invert_scroll=off\n"));
    assert(!unlink(mouse));
    assert(!forever_menu_read_settings(fg, hdr, hud, mouse, voice,
        &fg_mode, &hdr_enabled, &hud_enabled, &invert_scroll, &voice_enabled));
    assert(invert_scroll); /* no mouse file: On by default */
    assert(!forever_menu_rewrite_switch(voice, "voice", 0));
    assert(text_is("switches/drive_c/independent-voice.txt", "voice=off device=default\n"));
    assert(!forever_menu_rewrite_switch(voice, "voice", 1));
    assert(text_is("switches/drive_c/independent-voice.txt", "voice=on device=default\n"));
    make_path(voice, sizeof(voice), "switches/new-voice.txt");
    assert(!forever_menu_rewrite_switch(voice, "voice", 1));
    assert(text_is("switches/new-voice.txt", "voice=on\n"));

    make_path(path, sizeof(path), "switches/drive_c/wineps5-hud.txt");
    put("switches/drive_c/wineps5-hud.txt", "hud=off pos=br scale=3");
    assert(!forever_menu_rewrite_switch(path, "hud", 1));
    assert(text_is("switches/drive_c/wineps5-hud.txt", "hud=on pos=br scale=3"));

    put("switches/drive_c/duplicate.txt", "fg=on fg=off\n");
    make_path(path, sizeof(path), "switches/drive_c/duplicate.txt");
    assert(forever_menu_rewrite_switch(path, "fg", 1) == -EINVAL);
    assert(text_is("switches/drive_c/duplicate.txt", "fg=on fg=off\n"));
}

static void install_channel(const char *channel, const char *executable)
{
    char relative[512];
    snprintf(relative, sizeof(relative), "hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/%s",
             channel, executable);
    put(relative, "MZ");
}

static void test_hdr_config(void)
{
    char share[1024], prefix[1024], flag[1024];
    int enabled = -1;

    make_path(share, sizeof(share), "hdr/share");
    make_path(prefix, sizeof(prefix), "hdr/prefix");
    make_path(flag, sizeof(flag), "hdr/prefix/drive_c/wineps5-hdr-output.txt");
    put("hdr/share/Config.wtf", "SET GxApi \"D3D12\"\nSET HDROutput \"1\"\nSET HDRFactor \"2\"\n");
    install_channel("_forever_", "Wow.exe");
    install_channel("_classic_beta_", "WowB.exe");
    install_channel("_retail_", "Wow.exe");
    install_channel("_classic_era_", "WowClassic.exe");
    install_channel("_anniversary_", "WowClassic.exe");
    install_channel("_future_", "wOwFuture.EXE");
    put("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/WTF/Config.wtf",
        "SET mine \"keep\"\r\nSET HDROutput \"1\"\r\nSET HDRFactor \"1.7\"\r\n");

    assert(forever_menu_set_wow_hdr(share, prefix, 0) == 6);
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/WTF/Config.wtf",
                   "SET mine \"keep\"\r\nSET HDROutput \"0\"\r\nSET HDRFactor \"1.7\"\r\n"));
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_classic_beta_/WTF/Config.wtf",
                   "SET GxApi \"D3D12\"\nSET HDROutput \"0\"\nSET HDRFactor \"2\"\n"));
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_retail_/WTF/Config.wtf",
                   "SET GxApi \"D3D12\"\nSET HDROutput \"0\"\nSET HDRFactor \"2\"\n"));
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_future_/WTF/Config.wtf",
                   "SET GxApi \"D3D12\"\nSET HDROutput \"0\"\nSET HDRFactor \"2\"\n"));
    assert(!forever_menu_rewrite_switch(flag, "hdr", 0));
    assert(!forever_menu_read_switch(flag, "hdr", 1, &enabled) && !enabled);
    assert(!forever_menu_hdr_runtime_enabled(flag));

    assert(forever_menu_set_wow_hdr(share, prefix, 1) == 6);
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/WTF/Config.wtf",
                   "SET mine \"keep\"\r\nSET HDROutput \"1\"\r\nSET HDRFactor \"1.7\"\r\n"));
    assert(!forever_menu_rewrite_switch(flag, "hdr", 1));
    assert(forever_menu_hdr_runtime_enabled(flag));

    make_path(flag, sizeof(flag), "hdr/missing-flag.txt");
    assert(forever_menu_hdr_runtime_enabled(flag)); /* existing HDR-on profile is the fallback */
}

static void test_deletion_scope(void)
{
    char prefix[512], path[1024];

    make_path(prefix, sizeof(prefix), "delete/prefix");

    snprintf(path, sizeof(path), "%s/drive_c/Program Files (x86)/Battle.net/child", prefix);
    assert(!forever_menu_delete_path_allowed(prefix, path, 0)); /* a child is never an allowed root */
    snprintf(path, sizeof(path), "%s/drive_c/Program Files (x86)/World of Warcraft", prefix);
    assert(!forever_menu_delete_path_allowed(prefix, path, 1));
    snprintf(path, sizeof(path), "%s/drive_c/Program Files (x86)/Battle.net", prefix);
    assert(forever_menu_delete_path_allowed(prefix, path, 0));
    snprintf(path, sizeof(path), "%s/drive_c/Battle.net-Setup.exe", prefix);
    assert(!forever_menu_delete_path_allowed(prefix, path, 0));
    assert(forever_menu_delete_path_allowed(prefix, path, 1));

    /* Traversal is broker-owned and is exercised through serve_menu_remove;
     * this helper only proves that no caller can widen the fixed root set. */
}

static void open_find(struct forever_menu_state *menu, struct forever_addon_snapshot *snapshot)
{
    struct forever_menu_values values = {FOREVER_FG_AUTO, 1, 0, 0, 0};
    forever_menu_init(menu);
    forever_menu_open(menu, &values);
    menu->page = FOREVER_MENU_ADDON_PAGE;
    menu->addon_tab = FOREVER_ADDON_FIND;
    forever_menu_addon_snapshot(menu, snapshot);
}

static void test_find_search_and_github(void)
{
    static struct forever_menu_state menu;
    static struct forever_addon_snapshot snapshot = {.tab = FOREVER_ADDON_FIND, .count = 4};
    struct forever_menu_result result;
    char repo[FOREVER_ADDON_KEY_BYTES], error[FOREVER_MENU_STATUS_BYTES];
    char text[FOREVER_MENU_TEXT_BYTES];

    snprintf(snapshot.rows[0].key, sizeof(snapshot.rows[0].key), "cat:zulu");
    snprintf(snapshot.rows[0].name, sizeof(snapshot.rows[0].name), "Zulu");
    snprintf(snapshot.rows[0].description, sizeof(snapshot.rows[0].description),
             "A bags organizer.");
    snapshot.rows[0].downloads = 10;
    snapshot.rows[0].installable = 1;
    snprintf(snapshot.rows[1].key, sizeof(snapshot.rows[1].key), "cat:cafe");
    snprintf(snapshot.rows[1].name, sizeof(snapshot.rows[1].name), "Caf\xc3\xa9");
    snprintf(snapshot.rows[1].description, sizeof(snapshot.rows[1].description), "Recipes.");
    snapshot.rows[1].downloads = 5;
    snapshot.rows[1].installable = 1;
    snprintf(snapshot.rows[2].key, sizeof(snapshot.rows[2].key), "cat:alpha");
    snprintf(snapshot.rows[2].name, sizeof(snapshot.rows[2].name), "Alpha");
    snprintf(snapshot.rows[2].description, sizeof(snapshot.rows[2].description),
             "RAID tools.");
    snapshot.rows[2].downloads = 30;
    snapshot.rows[2].installable = 1;
    snprintf(snapshot.rows[3].key, sizeof(snapshot.rows[3].key), "cat:beta");
    snprintf(snapshot.rows[3].name, sizeof(snapshot.rows[3].name), "Beta");
    snprintf(snapshot.rows[3].description, sizeof(snapshot.rows[3].description), "Nothing.");
    snapshot.rows[3].downloads = 20;
    snapshot.rows[3].installable = 1;

    open_find(&menu, &snapshot);
    assert(menu.addon_count == 6);
    assert(!strcmp(menu.addon_rows[0].name, "Search\xe2\x80\xa6"));
    assert(!strcmp(menu.addon_rows[5].name, "Add from GitHub link\xe2\x80\xa6"));

    assert(forever_menu_find_append(&menu, "rA"));
    assert(forever_menu_find_append(&menu, "iD"));
    assert(!strcmp(menu.addon_query, "rAiD"));
    assert(menu.addon_count == 3 && menu.addon_selected == 1);
    assert(!strcmp(menu.addon_rows[1].name, "Alpha")); /* description match */
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Find: \"rAiD\""));
    menu.addon_loading = 1; /* Circle still clears while an install is running. */

    result = forever_menu_back(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_ADDON_PAGE && !menu.addon_query[0]);
    assert(menu.addon_count == 6); /* Circle clears before leaving Find. */
    result = forever_menu_back(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_CLOSE);

    open_find(&menu, &snapshot);
    assert(forever_menu_find_set_query(&menu, "\xc3\xa9"));
    assert(menu.addon_count == 3 && !strcmp(menu.addon_rows[1].name, "Caf\xc3\xa9"));
    assert(forever_menu_find_set_query(&menu, "absent"));
    assert(menu.addon_count == 2 && menu.addon_selected == 0);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "No add-ons match \"absent\""));

    assert(forever_menu_find_set_query(&menu, "old")); /* the live keyboard query */
    forever_menu_find_ime(&menu, "replacement", 0);
    assert(!strcmp(menu.addon_query, "old")); /* IME cancel keeps the typed query. */
    forever_menu_find_ime(&menu, "be", 1);
    assert(!strcmp(menu.addon_query, "be") && !strcmp(menu.addon_rows[1].name, "Beta"));
    assert(forever_menu_find_backspace(&menu));
    assert(!strcmp(menu.addon_query, "b"));
    {
        char long_query[96];
        memset(long_query, 'w', sizeof(long_query) - 1);
        long_query[sizeof(long_query) - 1] = 0;
        assert(forever_menu_find_set_query(&menu, long_query));
        assert(strlen(menu.addon_query) == FOREVER_ADDON_QUERY_BYTES - 1);
        assert(!forever_menu_find_append(&menu, "x"));
    }

    assert(forever_menu_find_set_query(&menu, ""));
    forever_menu_addon_button(&menu, FOREVER_ADDON_SQUARE);
    assert(menu.addon_sort_downloads);
    assert(!strcmp(menu.addon_rows[1].name, "Alpha"));
    assert(!strcmp(menu.addon_rows[2].name, "Beta"));
    assert(forever_menu_find_set_query(&menu, "a"));
    assert(!strcmp(menu.addon_rows[1].name, "Alpha"));
    assert(!strcmp(menu.addon_rows[2].name, "Beta")); /* filtering preserves sort */

    assert(!forever_menu_github_repo("owner/repo", repo, sizeof(repo), error, sizeof(error)));
    assert(!strcmp(repo, "owner/repo"));
    assert(!forever_menu_github_repo("owner/repo.git/", repo, sizeof(repo), error, sizeof(error)));
    assert(!strcmp(repo, "owner/repo"));
    assert(!forever_menu_github_repo("https://github.com/Owner/re_po/releases/latest",
                                     repo, sizeof(repo), error, sizeof(error)));
    assert(!strcmp(repo, "Owner/re_po"));
    assert(!forever_menu_github_repo("https://www.github.com/a/b.git/", repo, sizeof(repo),
                                     error, sizeof(error)));
    assert(!strcmp(repo, "a/b"));
    assert(forever_menu_github_repo("http://github.com/a/b", repo, sizeof(repo),
                                    error, sizeof(error)));
    assert(forever_menu_github_repo("https://example.com/a/b", repo, sizeof(repo),
                                    error, sizeof(error)));
    assert(forever_menu_github_repo("owner", repo, sizeof(repo), error, sizeof(error)));
    assert(forever_menu_github_repo("owner/re po", repo, sizeof(repo), error, sizeof(error)));
    assert(forever_menu_github_repo("owner/repo?x=1", repo, sizeof(repo), error, sizeof(error)));
    {
        char too_long[110] = {0};
        memset(too_long, 'a', 101);
        snprintf(too_long + 101, sizeof(too_long) - 101, "/b");
        assert(forever_menu_github_repo(too_long, repo, sizeof(repo), error, sizeof(error)));
    }

    open_find(&menu, &snapshot);
    result = forever_menu_select(&menu); /* pad CROSS on Search */
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME);
    forever_menu_find_ime(&menu, "bags", 1); /* diag `text bags` confirms the IME */
    result = forever_menu_select(&menu); /* next pad CROSS selects the first result */
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_INSTALL);
    assert(!strcmp(result.key, "cat:zulu"));

    open_find(&menu, &snapshot);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME);
    menu.addon_selected = menu.addon_count - 1;
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_GITHUB_IME);
    result = forever_menu_github_ime(&menu, "https://github.com/owner/repo.git", 0);
    assert(result.command == FOREVER_MENU_COMMAND_NONE &&
           menu.addon_tab == FOREVER_ADDON_FIND);
    result = forever_menu_github_ime(&menu, "https://example.com/owner/repo", 1);
    assert(result.command == FOREVER_MENU_COMMAND_NONE &&
           menu.addon_tab == FOREVER_ADDON_FIND);
    assert(!strcmp(menu.status,
                   "Enter owner/repo or an https://github.com/owner/repo link."));
    result = forever_menu_github_ime(&menu, "https://github.com/owner/repo.git", 1);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_INSTALL_GITHUB);
    assert(!strcmp(result.key, "owner/repo"));
    assert(result.tab == FOREVER_ADDON_INSTALLED && menu.addon_loading);
}


static void test_state_machine(void)
{
    static struct forever_menu_state menu;
    struct forever_menu_values values = {FOREVER_FG_AUTO, 1, 0, 0, 0};
    static struct forever_addon_snapshot snapshot = {.tab = FOREVER_ADDON_FIND};
    struct forever_menu_result result;
    char text[FOREVER_MENU_TEXT_BYTES];

    forever_menu_init(&menu);
    assert(!forever_menu_is_open(&menu));
    forever_menu_open(&menu, &values);
    assert(menu.page == FOREVER_MENU_MAIN && menu.selected == FOREVER_MENU_FRAME_GENERATION);
    forever_menu_move(&menu, -1);
    assert(menu.selected == FOREVER_MENU_CLOSE);
    forever_menu_move(&menu, 1);
    assert(menu.selected == FOREVER_MENU_FRAME_GENERATION);
    forever_menu_move(&menu, 1);
    forever_menu_move(&menu, 1);
    forever_menu_move(&menu, 1);
    assert(menu.selected == FOREVER_MENU_INVERT_SCROLL);
    forever_menu_move(&menu, 1);
    assert(menu.selected == FOREVER_MENU_VOICE);
    forever_menu_move(&menu, 1);
    assert(menu.selected == FOREVER_MENU_ADDONS);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_OPEN);
    assert(menu.page == FOREVER_MENU_ADDON_PAGE && menu.addon_loading);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Detecting installed WoW channels"));
    assert(strstr(text, "[Installed]"));
    snapshot.tab = FOREVER_ADDON_INSTALLED;
    snapshot.count = 0;
    snapshot.channel_count = 3;
    snprintf(snapshot.channels[0].label, sizeof(snapshot.channels[0].label), "WoW Retail");
    snprintf(snapshot.channels[1].label, sizeof(snapshot.channels[1].label), "WoW Classic Era");
    snprintf(snapshot.channels[2].label, sizeof(snapshot.channels[2].label), "_future_");
    forever_menu_addon_snapshot(&menu, &snapshot);

    result = forever_menu_addon_button(&menu, FOREVER_ADDON_RIGHT);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_REFRESH);
    assert(result.tab == FOREVER_ADDON_UPDATES);
    snapshot.tab = FOREVER_ADDON_UPDATES;
    forever_menu_addon_snapshot(&menu, &snapshot);
    result = forever_menu_addon_button(&menu, FOREVER_ADDON_R1);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_REFRESH);
    assert(result.channel == 1);
    snapshot.tab = FOREVER_ADDON_UPDATES;
    snapshot.count = 0;
    forever_menu_addon_snapshot(&menu, &snapshot);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "WoW Classic Era"));
    result = forever_menu_addon_button(&menu, FOREVER_ADDON_R1);
    assert(result.channel == 2);
    forever_menu_addon_snapshot(&menu, &snapshot);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "_future_"));
    snprintf(snapshot.message, sizeof(snapshot.message), "Add-ons aren't supported for this WoW version yet");
    forever_menu_addon_snapshot(&menu, &snapshot);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Add-ons aren't supported for this WoW version yet"));
    snapshot.message[0] = 0;
    result = forever_menu_addon_button(&menu, FOREVER_ADDON_R1);
    assert(result.channel == 0);
    forever_menu_addon_snapshot(&menu, &snapshot);
    result = forever_menu_addon_button(&menu, FOREVER_ADDON_RIGHT);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_REFRESH);
    assert(result.tab == FOREVER_ADDON_FIND);
    snapshot.tab = FOREVER_ADDON_FIND;
    snapshot.count = 0;
    forever_menu_addon_snapshot(&menu, &snapshot);
    forever_menu_render(&menu, text, sizeof(text));
    assert(!strstr(text, " Source\t") && !strstr(text, " Last update\t"));

    snapshot.tab = FOREVER_ADDON_FIND;
    snapshot.count = 2;
    snprintf(snapshot.rows[0].key, sizeof(snapshot.rows[0].key), "cat:alpha");
    snprintf(snapshot.rows[0].name, sizeof(snapshot.rows[0].name), "Alpha");
    snprintf(snapshot.rows[0].description, sizeof(snapshot.rows[0].description), "A useful addon.");
    snprintf(snapshot.rows[0].source, sizeof(snapshot.rows[0].source), "GitHub");
    snapshot.rows[0].installable = 1;
    snapshot.rows[0].downloads = 7;
    snprintf(snapshot.rows[1].key, sizeof(snapshot.rows[1].key), "cat:curse");
    snprintf(snapshot.rows[1].name, sizeof(snapshot.rows[1].name), "Curse Example");
    snprintf(snapshot.rows[1].description, sizeof(snapshot.rows[1].description), "Link only.");
    snprintf(snapshot.rows[1].source, sizeof(snapshot.rows[1].source), "CurseForge");
    snapshot.rows[1].curse_only = 1;
    snapshot.rows[1].downloads = 42;
    forever_menu_addon_snapshot(&menu, &snapshot);
    menu.addon_selected = 1;
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, " Last update\tUnknown"));
    assert(menu.addon_tab == FOREVER_ADDON_FIND && menu.addon_count == 4);
    result = forever_menu_addon_button(&menu, FOREVER_ADDON_SQUARE);
    assert(result.command == FOREVER_MENU_COMMAND_NONE && menu.addon_sort_downloads);
    assert(!strcmp(menu.addon_rows[1].name, "Curse Example"));
    forever_menu_addon_button(&menu, FOREVER_ADDON_SQUARE);
    assert(!menu.addon_sort_downloads && !strcmp(menu.addon_rows[1].name, "Alpha"));
    menu.addon_selected = 1;
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_INSTALL);
    assert(!strcmp(result.key, "cat:alpha"));
    forever_menu_addon_snapshot(&menu, &snapshot);
    menu.addon_selected = 2;
    forever_menu_addon_snapshot(&menu, &snapshot);
    assert(menu.addon_selected == 2);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(strstr(menu.status, "Available on CurseForge only"));
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Curse Example"));
    assert(strstr(text, "Available on CurseForge only"));

    result = forever_menu_addon_button(&menu, FOREVER_ADDON_RIGHT);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_REFRESH);
    assert(result.tab == FOREVER_ADDON_STARTER_PACKS);
    snapshot.tab = FOREVER_ADDON_STARTER_PACKS;
    snapshot.count = 1;
    memset(&snapshot.rows[0], 0, sizeof(snapshot.rows[0]));
    snprintf(snapshot.rows[0].key, sizeof(snapshot.rows[0].key), "starter:essentials");
    snprintf(snapshot.rows[0].name, sizeof(snapshot.rows[0].name), "Forever Essentials");
    snapshot.rows[0].installable = 1;
    forever_menu_addon_snapshot(&menu, &snapshot);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_ADDON_CONFIRM && !menu.confirmation_selected);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE && menu.page == FOREVER_MENU_ADDON_PAGE);
    forever_menu_select(&menu);
    forever_menu_move(&menu, 1);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_INSTALL_PACK);
    assert(!strcmp(result.key, "starter:essentials"));

    menu.page = FOREVER_MENU_ADDON_PAGE;
    menu.addon_tab = FOREVER_ADDON_INSTALLED;
    menu.addon_loading = 0;
    snapshot.tab = FOREVER_ADDON_INSTALLED;
    snapshot.count = 1;
    memset(&snapshot.rows[0], 0, sizeof(snapshot.rows[0]));
    snprintf(snapshot.rows[0].key, sizeof(snapshot.rows[0].key), "cat:alpha");
    snprintf(snapshot.rows[0].name, sizeof(snapshot.rows[0].name), "Alpha");
    forever_menu_addon_snapshot(&menu, &snapshot);
    result = forever_menu_addon_button(&menu, FOREVER_ADDON_R3);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_ADDON_CONFIRM && !menu.confirmation_selected);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    forever_menu_addon_button(&menu, FOREVER_ADDON_R3);
    forever_menu_move(&menu, 1);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_REMOVE);

    menu.page = FOREVER_MENU_ADDON_PAGE;
    result = forever_menu_back(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_CLOSE);
    assert(menu.page == FOREVER_MENU_MAIN && menu.selected == FOREVER_MENU_ADDONS);
    assert(strstr(menu.status, "Finishing add-on changes"));
    result = forever_menu_back(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_CLOSE);

    forever_menu_open(&menu, &values);
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_UNINSTALL);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_CONFIRM_UNINSTALL && menu.confirmation_selected == 0);
    result = forever_menu_back(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE && menu.page == FOREVER_MENU_MAIN);
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_ADDONS);
    assert(result.command == FOREVER_MENU_COMMAND_ADDONS_OPEN);

    forever_menu_open(&menu, &values);
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_FRAME_GENERATION);
    assert(result.command == FOREVER_MENU_COMMAND_SET_FRAME_GENERATION && !result.enabled);
    forever_menu_commit(&menu, result.command, result.enabled);
    assert(menu.values.frame_generation == FOREVER_FG_OFF);
}

static void test_wago_menu(void)
{
    static struct forever_menu_state menu;
    static struct forever_addon_snapshot snapshot = {.tab = FOREVER_ADDON_INSTALLED};
    char text[FOREVER_MENU_TEXT_BYTES];
    static const char *const labels[] = {"not set", "set", "set (not checked)", "rejected"};
    forever_menu_init(&menu);
    menu.page = FOREVER_MENU_ADDON_PAGE;
    for (int status = FOREVER_WAGO_NOT_SET; status <= FOREVER_WAGO_REJECTED; status++)
    {
        snapshot.wago_status = status;
        forever_menu_addon_snapshot(&menu, &snapshot);
        forever_menu_render(&menu, text, sizeof(text));
        assert(strstr(text, "Wago key"));
        assert(strstr(text, labels[status]));
    }
    forever_menu_move(&menu, -1);
    assert(menu.addon_selected == menu.addon_count);
    assert(forever_menu_select(&menu).command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_WAGO_KEY);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Type key") && strstr(text, "Read from file") && strstr(text, "Remove key"));
    assert(strstr(text, "Wago Patreon supporters get a key: addons.wago.io/patreon"));
    assert(forever_menu_select(&menu).command == FOREVER_MENU_COMMAND_WAGO_IME);
    menu.page = FOREVER_MENU_WAGO_KEY;
    forever_menu_move(&menu, 1);
    assert(forever_menu_select(&menu).command == FOREVER_MENU_COMMAND_WAGO_FILE);
    menu.page = FOREVER_MENU_WAGO_KEY;
    forever_menu_move(&menu, 1);
    assert(forever_menu_select(&menu).command == FOREVER_MENU_COMMAND_WAGO_REMOVE);
    menu.page = FOREVER_MENU_WAGO_KEY;
    assert(forever_menu_back(&menu).command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_ADDON_PAGE);
    snapshot.wago_status = FOREVER_WAGO_NOT_CHECKED;
    snapshot.wago_saved = 1;
    forever_menu_addon_snapshot(&menu, &snapshot);
    assert(!strcmp(menu.status, "Wago key saved."));
    forever_menu_addon_error(&menu, "Wago rejected this key.");
    assert(menu.wago_status == FOREVER_WAGO_REJECTED);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Wago key\trejected"));
    snapshot.wago_status = FOREVER_WAGO_NOT_SET;
    snapshot.wago_saved = 2;
    forever_menu_addon_snapshot(&menu, &snapshot);
    assert(!strcmp(menu.status, "Wago key removed."));
    assert(forever_menu_click_line(&menu, 9).command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_WAGO_KEY);
    assert(forever_menu_click_line(&menu, 5).command == FOREVER_MENU_COMMAND_WAGO_FILE);
}

int main(void)
{
    char command[320];
    snprintf(root, sizeof(root), "/tmp/forever-menu-test-%ld", (long)getpid());
    test_key_rewrite();
    test_voice_download();
    test_voice_resume();
    test_download_redirects();
    test_mouse_scroll_menu();
    test_voice_menu();
    test_hdr_config();
    test_deletion_scope();
    test_state_machine();
    test_find_search_and_github();
    test_wago_menu();
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    assert(!system(command));
    puts("forever menu helpers/state: PASS");
    return 0;
}
