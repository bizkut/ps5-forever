#include "runtime-relaunch.h"
#include <assert.h>
#include <stdio.h>

static int loader_exists(const char *path, void *context)
{
    (void)context;
    return !strcmp(path, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB_loader.dll");
}

enum
{
    FINAL_EXE = 1u << 0,
    FINAL_LOADER = 1u << 1,
    BETA_EXE = 1u << 2,
    BETA_LOADER = 1u << 3,
    FINAL_INSTALLED = FINAL_EXE | FINAL_LOADER,
    BETA_INSTALLED = BETA_EXE | BETA_LOADER
};

static int installed_exists(const char *path, void *context)
{
    static const char *const files[] = {
        FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_/Wow.exe",
        FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_/wow_loader.dll",
        FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB.exe",
        FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB_loader.dll"
    };
    unsigned installed = *(unsigned *)context;
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (!strcmp(path, files[i])) return !!(installed & (1u << i));
    return 0;
}

static void test_direct_profiles(void)
{
    unsigned installed = BETA_INSTALLED;
    const struct pw_game_direct_profile *beta = pw_game_direct_profile(installed_exists, &installed);
    assert(beta);
    assert(!strcmp(beta->argv[0], "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_\\WowB.exe"));
    assert(!strcmp(beta->argv[1], "-launcherlogin"));
    assert(!strcmp(beta->argv[2], "-uid"));
    assert(!strcmp(beta->argv[3], "wow_classic_beta"));
    assert(!beta->argv[4]);
    assert(!strcmp(beta->windows_cwd, "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_"));
    assert(!strcmp(beta->unix_cwd, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_"));
    assert(!strcmp(beta->unix_exe, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB.exe"));
    assert(!strcmp(beta->unix_loader, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_/WowB_loader.dll"));
    /* Classic beta is the default; it wins whenever it is completely installed. */
    installed = FINAL_INSTALLED | BETA_INSTALLED;
    assert(pw_game_direct_profile(installed_exists, &installed) == beta);

    installed = FINAL_INSTALLED;
    const struct pw_game_direct_profile *final = pw_game_direct_profile(installed_exists, &installed);
    assert(final && final != beta);
    assert(!strcmp(final->argv[0], "C:\\Program Files (x86)\\World of Warcraft\\_forever_\\Wow.exe"));
    assert(!strcmp(final->argv[1], "-launcherlogin"));
    assert(!strcmp(final->argv[2], "-uid"));
    assert(!strcmp(final->argv[3], "wowf"));
    assert(!final->argv[4]);
    assert(!strcmp(final->windows_cwd, "C:\\Program Files (x86)\\World of Warcraft\\_forever_"));
    assert(!strcmp(final->unix_cwd, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_"));
    assert(!strcmp(final->unix_exe, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_/Wow.exe"));
    assert(!strcmp(final->unix_loader, FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_/wow_loader.dll"));
    installed = BETA_EXE | FINAL_INSTALLED;
    assert(pw_game_direct_profile(installed_exists, &installed) == final);
    installed = BETA_LOADER | FINAL_INSTALLED;
    assert(pw_game_direct_profile(installed_exists, &installed) == final);
    const unsigned incomplete[] = {0, FINAL_EXE, FINAL_LOADER, BETA_EXE, BETA_LOADER,
                                   FINAL_EXE | BETA_EXE, FINAL_LOADER | BETA_LOADER};
    for (unsigned i = 0; i < sizeof(incomplete) / sizeof(incomplete[0]); ++i)
    {
        installed = incomplete[i];
        assert(!pw_game_direct_profile(installed_exists, &installed));
    }
}

static void test_dynamic_channels(void)
{
    unsigned installed = FINAL_INSTALLED | BETA_INSTALLED;
    const struct
    {
        char *image;
        const char *windows_cwd;
        const char *unix_cwd;
        char *uid;
    } channels[] = {
        {"C:\\Program Files (x86)\\World of Warcraft\\_forever_\\WoW.exe",
         "C:\\Program Files (x86)\\World of Warcraft\\_forever_",
         FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_", "wowf"},
        {"C:\\Program Files (x86)\\World of Warcraft\\_forever_\\wOw.ExE",
         "C:\\Program Files (x86)\\World of Warcraft\\_forever_",
         FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_", "wowf"},
        {"C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_\\WowB.exe",
         "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_",
         FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_", "wow_classic_beta"},
        {"C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_\\WowB.EXE",
         "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_",
         FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_", "wow_classic_beta"}
    };
    for (unsigned i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i)
    {
        struct pw_game_wire wire;
        unsigned separator = 0;
        char *program[] = {channels[i].image, "-launcherlogin", "-uid", channels[i].uid, NULL};
        assert(!pw_game_wire_encode(&wire, program, channels[i].windows_cwd, channels[i].unix_cwd));
        assert(!pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                                    installed_exists, &installed, &separator));
        assert(wire.argc == 11 && separator == 6);
        assert(!strcmp(wire.argv[3], channels[i].unix_cwd));
        assert(!strcmp(wire.argv[5], channels[i].windows_cwd));
        for (unsigned j = 0; program[j]; ++j) assert(!strcmp(wire.argv[7 + j], program[j]));
        unsigned missing = 0;
        assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                                   installed_exists, &missing, NULL) == -ENOENT);
    }
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
    unsigned installed = BETA_INSTALLED;
    const struct pw_game_direct_profile *direct = pw_game_direct_profile(installed_exists, &installed);
    test_direct_profiles();
    test_dynamic_channels();
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
    puts("runtime relaunch argv: final/beta selection and wire handoff, direct-game flag, encode/decode round-trip, version/separator and 4096-byte bounds PASS");
    return 0;
}
