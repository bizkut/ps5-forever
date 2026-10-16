#ifndef PW_RUNTIME_RELAUNCH_H
#define PW_RUNTIME_RELAUNCH_H
#include "protocol.h"
#include "forever-paths.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#define PW_GAME_ARGV_BYTES 4096
#define PW_GAME_ARGC_MAX 64
struct pw_game_wire { char bytes[PW_GAME_ARGV_BYTES]; const char *argv[PW_GAME_ARGC_MAX + 8]; unsigned argc; };
typedef int (*pw_game_exists_fn)(const char *, void *);
struct pw_game_direct_profile
{
    char *argv[5];
    const char *windows_cwd;
    const char *unix_cwd;
    const char *unix_exe;
    const char *unix_loader;
};
static inline const struct pw_game_direct_profile *pw_game_direct_profile(pw_game_exists_fn exists,
                                                                         void *context)
{
    /* Keep aligned with b1-game-wow-forever.json and b1-game-wow-classic-beta.json.
     * Maintenance builds do not contain the GPU profile's generated argv.
     * Prefer final, fall back to beta, and never stage an incomplete install. */
    static const struct pw_game_direct_profile profiles[] = {
        {
            {"C:\\Program Files (x86)\\World of Warcraft\\_forever_\\WoW.exe",
             "-launcherlogin", "-uid", "wowf", NULL},
            "C:\\Program Files (x86)\\World of Warcraft\\_forever_",
            FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_",
            FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_/WoW.exe",
            FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_/wow_loader.dll"
        },
        {
            {"C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_\\WowB.exe",
             "-launcherlogin", "-uid", "wow_classic_beta", NULL},
            "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_",
            FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_",
            FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB.exe",
            FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB_loader.dll"
        }
    };
    if (!exists) return NULL;
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (exists(profiles[i].unix_exe, context) && exists(profiles[i].unix_loader, context))
            return &profiles[i];
    return NULL;
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
                                      pw_game_exists_fn exists, void *context,
                                      unsigned *separator_out)
{
    unsigned separator; size_t bytes = 0, cwd_len, base_len; const char *image, *base;
    char root[4096], loader[4096];
    if (!argv || !prefix || !exists || argc < 8 || strcmp(argv[1], "--ps5-game-v1") ||
        strcmp(argv[2], "--unix-cwd") || strcmp(argv[4], "--windows-cwd")) return -EINVAL;
    for (separator = 5; separator < (unsigned)argc && strcmp(argv[separator], "--"); ++separator);
    if (separator != 6 || separator + 1 >= (unsigned)argc ||
        (unsigned)argc - separator - 1 > PW_GAME_ARGC_MAX) return -EINVAL;
    if (snprintf(root, sizeof(root), "%s/drive_c/", prefix) < 0 ||
        strncmp(argv[3], root, strlen(root)) || strstr(argv[3], "/../")) return -EACCES;
    for (int i = 0; i < argc; ++i) if ((bytes += strlen(argv[i]) + 1) > PW_GAME_ARGV_BYTES) return -E2BIG;
    image = argv[separator + 1];
    if (!((image[0] == 'C' || image[0] == 'c' || image[0] == 'W' || image[0] == 'w') &&
          image[1] == ':' && (image[2] == '\\' || image[2] == '/'))) return -EINVAL;
    base = strrchr(image, '\\'); if (!base) base = strrchr(image, '/'); base = base ? base + 1 : image;
    base_len = strlen(base); cwd_len = strlen(argv[3]);
    if (base_len < 7 || !((base[0] | 32) == 'w' && (base[1] | 32) == 'o' && (base[2] | 32) == 'w') ||
        strcasecmp(base + base_len - 4, ".exe") ||
        cwd_len + 1 + base_len - 4 + sizeof("_loader.dll") > sizeof(loader)) return -EINVAL;
    memcpy(loader, argv[3], cwd_len); loader[cwd_len] = '/';
    memcpy(loader + cwd_len + 1, base, base_len - 4);
    memcpy(loader + cwd_len + 1 + base_len - 4, "_loader.dll", sizeof("_loader.dll"));
    if (!exists(loader, context))
    {
        /* Some channels ship a lowercase loader beside a mixed-case executable. */
        for (size_t i = cwd_len + 1; i < cwd_len + 1 + base_len - 4; ++i)
            if (loader[i] >= 'A' && loader[i] <= 'Z') loader[i] += 'a' - 'A';
        if (!exists(loader, context)) return -ENOENT;
    }
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
#endif
