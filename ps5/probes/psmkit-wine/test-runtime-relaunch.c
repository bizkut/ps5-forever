#include "runtime-relaunch.h"
#include <assert.h>
#include <stdio.h>

static int loader_exists(const char *path, void *context)
{
    (void)context;
    return !strcmp(path, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB_loader.dll");
}

int main(void)
{
    struct pw_game_wire wire;
    char *program[] = {"C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_\\WowB.exe",
                       "-launcherlogin", "-uid", "wow_classic_beta", NULL};
    const char *unix_cwd = FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_";
    struct pw_game_request request;
    char *forwarded[PW_GAME_WIRE_ARGV + 1];
    unsigned separator = 0;
    const struct pw_game_direct_profile *direct = pw_game_direct_profile();
    assert(!pw_game_direct_flag_consumed(0, 0));
    assert(!pw_game_direct_flag_consumed(0, -1));
    assert(!pw_game_direct_flag_consumed(1, -1));
    assert(pw_game_direct_flag_consumed(1, 0));
    assert(!pw_game_wire_encode(&wire, direct->argv, direct->windows_cwd, direct->unix_cwd));
    assert(!pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                                loader_exists, NULL, &separator));
    assert(wire.argc == 11 && separator == 6);
    assert(!strcmp(wire.argv[3], unix_cwd));
    assert(!strcmp(wire.argv[5], "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_"));
    for (unsigned i = 0; program[i]; ++i) assert(!strcmp(wire.argv[7 + i], program[i]));
    assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, "/missing/prefix",
                               loader_exists, NULL, NULL) == -EACCES);
    assert(!pw_game_wire_encode(&wire, program,
                                "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_", unix_cwd));
    assert(wire.argc == 11 && !strcmp(wire.argv[1], "--ps5-game-v1") && !strcmp(wire.argv[6], "--"));
    assert(!pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                                loader_exists, NULL, &separator));
    assert(separator == 6 && !strcmp(wire.argv[7], program[0]) && !strcmp(wire.argv[10], program[3]));
    assert(!pw_game_request_pack(&request, &wire));
    assert(!pw_game_request_unpack(&request, forwarded));
    assert(!strcmp(forwarded[3], unix_cwd) && !strcmp(forwarded[7], program[0]));
    request.offsets[8] = request.offsets[7];
    assert(pw_game_request_unpack(&request, forwarded) == EPROTO);
    assert(!pw_game_request_pack(&request, &wire));
    request.data[request.bytes - 1] = 'x';
    assert(pw_game_request_unpack(&request, forwarded) == EPROTO);
    assert(!pw_game_request_pack(&request, &wire));
    wire.argv[1] = "--bad-version";
    assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                               loader_exists, NULL, NULL) == -EINVAL);
    wire.argv[1] = "--ps5-game-v1"; wire.argv[6] = "not-separator";
    assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                               loader_exists, NULL, NULL) == -EINVAL);
    wire.argv[6] = "--";
    char huge[PW_GAME_ARGV_BYTES]; memset(huge, 'x', sizeof(huge) - 1); huge[sizeof(huge) - 1] = 0;
    char *oversized[] = {program[0], huge, NULL};
    assert(pw_game_wire_encode(&wire, oversized, "C:\\WoW", unix_cwd) == E2BIG);
    puts("runtime relaunch argv: direct-game flag decision/profile, encode/decode round-trip, version/separator and 4096-byte bounds PASS");
    return 0;
}
