#ifndef PW_RUNTIME_RELAUNCH_H
#define PW_RUNTIME_RELAUNCH_H
#include "protocol.h"
#include "forever-paths.h"
#include "wow-channels.h"
#include "forever-setup.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#define PW_GAME_ARGV_BYTES 4096
#define PW_GAME_ARGC_MAX 64
struct pw_game_wire { char bytes[PW_GAME_ARGV_BYTES]; const char *argv[PW_GAME_ARGC_MAX + 8]; unsigned argc; };
typedef int (*pw_game_resolve_fn)(const char *, const char *, char *, char *, void *);
/* The file is one-shot even when its selector is unavailable or malformed. */
static inline int pw_game_direct_selector(char *text)
{
    size_t n = strlen(text), start = 0;
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) --n;
    while (start < n && (text[start] == ' ' || text[start] == '\t')) ++start;
    memmove(text, text + start, n - start); text[n - start] = 0;
    return *text && !wow_folder_name(text) ? -EINVAL : 0;
}
static inline int pw_game_direct_flag_consumed(int present, int unlink_result)
{
    return present && !unlink_result;
}
static inline int pw_game_wire_encode(struct pw_game_wire *wire, char *const *program,
                                      const char *windows_cwd, const char *unix_cwd)
{
    static const char *const prefix[] = {"game.bin", "--ps5-game-v1", "--unix-cwd", NULL,
                                         "--windows-cwd", NULL, "--"};
    size_t used = 0; unsigned count = 0, programs = 0;
    if (!wire || !program || !program[0] || !windows_cwd || !unix_cwd) return EINVAL;
    memset(wire, 0, sizeof(*wire));
#define PW_WIRE_COPY(value) do { const char *v_ = (value); size_t n_ = strlen(v_) + 1; \
    if (count >= PW_GAME_ARGC_MAX + 7 || n_ > sizeof(wire->bytes) - used) return E2BIG; \
    memcpy(wire->bytes + used, v_, n_); wire->argv[count++] = wire->bytes + used; used += n_; } while (0)
    for (unsigned i = 0; i < 7; ++i) PW_WIRE_COPY(i == 3 ? unix_cwd : i == 5 ? windows_cwd : prefix[i]);
    for (; program[programs]; ++programs)
    {
        if (programs == PW_GAME_ARGC_MAX) return E2BIG;
        PW_WIRE_COPY(program[programs]);
    }
#undef PW_WIRE_COPY
    wire->argv[count] = NULL; wire->argc = count; return 0;
}
static inline int pw_game_wire_decode(int argc, char **argv, const char *prefix,
                                      pw_game_resolve_fn resolve, void *context,
                                      unsigned *separator_out)
{
    unsigned separator; size_t bytes = 0, base_len; char *image, *base, *slash;
    char root[4096], executable[WOW_FOLDER_BYTES], loader[WOW_FOLDER_BYTES];
    int error;
    struct wow_channel channel;
    if (!argv || !prefix || !resolve || argc < 8 || strcmp(argv[1], "--ps5-game-v1") ||
        strcmp(argv[2], "--unix-cwd") || strcmp(argv[4], "--windows-cwd")) return -EINVAL;
    for (separator = 5; separator < (unsigned)argc && strcmp(argv[separator], "--"); ++separator);
    if (separator != 6 || separator + 1 >= (unsigned)argc ||
        (unsigned)argc - separator - 1 > PW_GAME_ARGC_MAX) return -EINVAL;
    if (snprintf(root, sizeof(root), "%s/drive_c/", prefix) < 0) return -EACCES;
    if (strncmp(argv[3], root, strlen(root)))
    {
        if (snprintf(root, sizeof(root), "%s/dosdevices/%c:/", prefix,
                     (argv[7][0] | 32)) < 0 || strncmp(argv[3], root, strlen(root))) return -EACCES;
    }
    if (strstr(argv[3], "/../") || strstr(argv[3], "/./")) return -EACCES;
    for (int i = 0; i < argc; ++i) if ((bytes += strlen(argv[i]) + 1) > PW_GAME_ARGV_BYTES) return -E2BIG;
    image = argv[separator + 1];
    if (wow_channel_parse(image, &channel)) return -EINVAL;
    base = strrchr(image, '\\'); slash = strrchr(image, '/');
    if (!base || (slash && slash > base)) base = slash;
    base = base ? base + 1 : image;
    if ((error = resolve(argv[3], base, executable, loader, context))) return error;
    if (!wow_ascii_equal(base, executable)) return -EINVAL;
    /* A case-insensitive leaf match has identical byte length. The wire and
     * the title's argv are writable; normalize before any downstream copy. */
    base_len = strlen(base);
    memcpy(base, executable, base_len);
    if (separator_out) *separator_out = separator;
    return 0;
}
static inline int pw_game_request_pack(struct pw_game_request *request,
                                       const struct pw_game_wire *wire)
{
    size_t bytes;
    if (!request || !wire || wire->argc < 8 || wire->argc > PW_GAME_WIRE_ARGV) return EINVAL;
    bytes = (wire->argv[wire->argc - 1] - wire->bytes) + strlen(wire->argv[wire->argc - 1]) + 1;
    if (bytes > sizeof(request->data)) return E2BIG;
    memset(request, 0, sizeof(*request));
    request->argc = wire->argc;
    request->bytes = bytes;
    memcpy(request->data, wire->bytes, bytes);
    for (unsigned i = 0; i < wire->argc; ++i)
        request->offsets[i] = (uint32_t)(wire->argv[i] - wire->bytes);
    return 0;
}
static inline int pw_game_request_unpack(const struct pw_game_request *request,
                                         char *argv[PW_GAME_WIRE_ARGV + 1])
{
    if (!request || !argv || request->argc < 8 || request->argc > PW_GAME_WIRE_ARGV ||
        !request->bytes || request->bytes > sizeof(request->data) ||
        request->data[request->bytes - 1]) return EPROTO;
    for (unsigned i = 0; i < request->argc; ++i)
    {
        uint32_t start = request->offsets[i];
        uint32_t end = i + 1 < request->argc ? request->offsets[i + 1] : request->bytes;
        if (start >= end || end > request->bytes || (i && start <= request->offsets[i - 1]) ||
            request->data[end - 1]) return EPROTO;
        argv[i] = (char *)request->data + start;
    }
    argv[request->argc] = NULL;
    return 0;
}
/* Only typed, credential-free launch options may be persisted. A deny-list
 * cannot distinguish an opaque session value from game configuration.
 * This option policy is independent of products, channels and exe names. */
static inline int pw_game_record_arguments(char *const *program, char **saved, unsigned *argc)
{
    size_t bytes = 0;
    unsigned count = 0, used = 0;
    if (!program || !program[0]) return EINVAL;
    while (program[count])
    {
        if (count == PW_GAME_ARGC_MAX ||
            strlen(program[count]) + 1 > PW_GAME_ARGV_BYTES - bytes) return E2BIG;
        bytes += strlen(program[count++]) + 1;
    }
    saved[used++] = program[0];
    for (unsigned i = 1; i < count; ++i)
    {
        const char *start = program[i], *end, *value;
        unsigned option = i;
        size_t length;
        int uid, mode, valid;
        if (*start != '-' && *start != '/') continue;
        while (*start == '-' || *start == '/') ++start;
        end = start + strcspn(start, "=:");
        length = (size_t)(end - start);
        if (!*end && ((length == 13 && !strncasecmp(start, "launcherlogin", 13)) ||
                      (length == 7 && !strncasecmp(start, "console", 7)) ||
                      (length == 8 && !strncasecmp(start, "windowed", 8))))
        {
            saved[used++] = program[i]; /* boolean only: no stored IPC/login token */
            continue;
        }
        uid = length == 3 && !strncasecmp(start, "uid", 3);
        mode = length == 15 && !strncasecmp(start, "initialgamemode", 15);
        if (!uid && !mode)
        {
            if (!*end && i + 1 < count)
            {
                /* Unknown option arity is not knowable. A following switch
                 * may be a secret value or required game configuration:
                 * refuse publication rather than store it or drop it silently. */
                if (program[i + 1][0] == '-' || program[i + 1][0] == '/') return ENOTSUP;
                ++i;
            }
            continue;
        }
        value = *end ? end + 1 : i + 1 < count ? program[++i] : "";
        if (uid)
            valid = !strncasecmp(value, "wow", 3) && wow_product_valid(value);
        else
        {
            length = strlen(value);
            valid = length && length <= 32;
            for (size_t j = 0; valid && j < length; ++j)
                if (!((value[j] >= 'a' && value[j] <= 'z') ||
                      (value[j] >= 'A' && value[j] <= 'Z') ||
                      (j && ((value[j] >= '0' && value[j] <= '9') ||
                             value[j] == '_' || value[j] == '-')))) valid = 0;
        }
        if (!valid) continue;
        saved[used++] = program[option];
        if (i != option) saved[used++] = program[i];
    }
    saved[used] = NULL;
    if (argc) *argc = used;
    return 0;
}

static inline int pw_game_record_path(char *path, size_t size, const char *state, const char *folder)
{
    char key[WOW_FOLDER_BYTES];
    int length;
    if (!wow_folder_name(folder)) return EINVAL;
    for (size_t i = 0; ; ++i)
    {
        key[i] = folder[i] >= 'A' && folder[i] <= 'Z' ? folder[i] + ('a' - 'A') : folder[i];
        if (!key[i]) break;
    }
    length = snprintf(path, size, "%s/last-launch-%s.bin", state, key);
    return length < 0 || (size_t)length >= size ? ENAMETOOLONG : 0;
}

static inline int pw_game_same_windows_path(const char *a, const char *b)
{
    size_t left_size = strlen(a), right_size = strlen(b);
    while (left_size > 3 && (a[left_size - 1] == '/' || a[left_size - 1] == '\\')) --left_size;
    while (right_size > 3 && (b[right_size - 1] == '/' || b[right_size - 1] == '\\')) --right_size;
    if (left_size != right_size) return 0;
    for (size_t i = 0; i < left_size; ++i)
    {
        unsigned char left = a[i], right = b[i];
        if (left == '/') left = '\\';
        if (right == '/') right = '\\';
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return 0;
    }
    return 1;
}

static inline int pw_game_same_unix_cwd(const char *a, const char *b)
{
    size_t left = strlen(a), right = strlen(b);
    while (left > 1 && a[left - 1] == '/') --left;
    while (right > 1 && b[right - 1] == '/') --right;
    return left == right && !memcmp(a, b, left);
}

/* The on-disk format is the existing versioned wire's bounded NUL-separated
 * strings, never its process-local pointers or unsanitized Play arguments. */
static inline int pw_game_record_launch(const char *state, char *const *program,
                                        const char *windows_cwd, const char *unix_cwd)
{
    struct wow_channel channel;
    struct pw_game_wire wire;
    char path[WOW_PATH_BYTES], *saved[PW_GAME_ARGC_MAX + 1];
    int error;
    size_t bytes;
    if (!state || !windows_cwd || !unix_cwd) return EINVAL;
    if ((error = pw_game_record_arguments(program, saved, NULL))) return error;
    if (wow_channel_parse(program[0], &channel) ||
        !pw_game_same_windows_path(channel.windows_cwd, windows_cwd)) return EINVAL;
    if ((error = pw_game_wire_encode(&wire, saved, windows_cwd, unix_cwd)) ||
        (error = pw_game_record_path(path, sizeof(path), state, channel.folder))) return error;
    if ((error = forever_mkdirs(state))) return -error;
    bytes = (size_t)(wire.argv[wire.argc - 1] - wire.bytes) + strlen(wire.argv[wire.argc - 1]) + 1;
    return -forever_write_file(path, wire.bytes, bytes);
}

static inline int pw_game_read_record(const char *path, struct pw_game_wire *wire)
{
    struct stat status;
    size_t used = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK), error = 0;
    if (fd < 0) return errno;
    if (fstat(fd, &status)) error = errno;
    else if (!S_ISREG(status.st_mode) || status.st_size <= 0) error = EINVAL;
    else if ((uint64_t)status.st_size > sizeof(wire->bytes)) error = E2BIG;
    while (!error && used < (size_t)status.st_size)
    {
        ssize_t count = read(fd, wire->bytes + used, (size_t)status.st_size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EINVAL;
        else used += (size_t)count;
    }
    if (!error)
    {
        char extra;
        ssize_t count;
        do count = read(fd, &extra, 1); while (count < 0 && errno == EINTR);
        if (count) error = count < 0 ? errno : E2BIG;
    }
    close(fd);
    if (error) return error;
    if (wire->bytes[used - 1]) return EINVAL;
    wire->argc = 0;
    for (size_t at = 0; at < used; )
    {
        char *end = memchr(wire->bytes + at, 0, used - at);
        if (!end || wire->argc == PW_GAME_ARGC_MAX + 7) return EINVAL;
        wire->argv[wire->argc++] = wire->bytes + at;
        at = (size_t)(end - wire->bytes) + 1;
    }
    wire->argv[wire->argc] = NULL;
    return 0;
}

/* Missing records retain first-start behavior. A malformed or foreign record
 * is refused rather than silently dropping the recorded game configuration.
 * The caller still applies normal wire/prefix/loader admission before launch. */
static inline int pw_game_direct_launch(struct pw_game_wire *wire, const char *state,
                                        const struct wow_channel *channel, int *recorded)
{
    char path[WOW_PATH_BYTES], image[WOW_PATH_BYTES], *saved[PW_GAME_ARGC_MAX + 1];
    struct wow_channel parsed;
    unsigned count;
    int error;
    if (recorded) *recorded = 0;
    if ((error = pw_game_record_path(path, sizeof(path), state, channel->folder))) return error;
    error = pw_game_read_record(path, wire);
    if (error == ENOENT)
    {
        char *program[] = {image, "-launcherlogin", "-uid", (char *)wow_channel_launch_uid(channel), NULL};
        int length = snprintf(image, sizeof(image), "%s\\%s", channel->windows_cwd, channel->executable);
        if (length < 0 || (size_t)length >= sizeof(image)) return ENAMETOOLONG;
        if (!channel->product[0]) program[2] = NULL;
        return pw_game_wire_encode(wire, program, channel->windows_cwd, channel->unix_cwd);
    }
    if (error) return error;
    if (wire->argc < 8 || strcmp(wire->argv[0], "game.bin") ||
        strcmp(wire->argv[1], "--ps5-game-v1") || strcmp(wire->argv[2], "--unix-cwd") ||
        strcmp(wire->argv[4], "--windows-cwd") || strcmp(wire->argv[6], "--") ||
        wow_channel_parse(wire->argv[7], &parsed) || strcasecmp(parsed.folder, channel->folder) ||
        strcasecmp(parsed.executable, channel->executable) ||
        !pw_game_same_windows_path(parsed.windows_cwd, channel->windows_cwd) ||
        !pw_game_same_windows_path(parsed.windows_cwd, wire->argv[5]) ||
        !pw_game_same_unix_cwd(wire->argv[3], channel->unix_cwd)) return EINVAL;
    if ((error = pw_game_record_arguments((char **)wire->argv + 7, saved, &count))) return error;
    if (count != wire->argc - 7) return EINVAL;
    for (unsigned i = 0; i < count; ++i)
        if (saved[i] != wire->argv[7 + i]) return EINVAL;
    if (recorded) *recorded = 1;
    return 0;
}
#endif
