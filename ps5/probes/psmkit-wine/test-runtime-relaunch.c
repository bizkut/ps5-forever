#define _GNU_SOURCE
#include "runtime-relaunch.h"
#include "forever-setup.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <signal.h>

static int installed_exists(const char *cwd, const char *requested, char *exe, char *loader, void *context)
{
    const struct { const char *cwd, *exe, *loader; } files[] = {
        {FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_forever_", "Wow.exe", "wow_loader.dll"},
        {FOREVER_DRIVE_C "/Program Files (x86)/World of Warcraft/_classic_beta_", "WowB.exe", "WowB_loader.dll"},
        {FOREVER_DRIVE_C "/Games/World of Warcraft/_retail_", "Wow.exe", "wow_loader.dll"},
        {FOREVER_PREFIX "/dosdevices/w:/Games/World of Warcraft/_ptr_", "WowT.exe", "wowt_loader.dll"}
    };
    (void)context;
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (!strcmp(cwd, files[i].cwd) && wow_ascii_equal(requested, files[i].exe))
        { strcpy(exe, files[i].exe); strcpy(loader, files[i].loader); return 0; }
    return -ENOENT;
}
static int missing_loader(const char *cwd, const char *requested, char *exe, char *loader, void *context)
{
    (void)cwd; (void)requested; (void)exe; (void)loader; (void)context; return -ENOENT;
}
static void test_recorded_launch(void)
{
    char state[] = "/tmp/forever-launch-XXXXXX", path[1100], other[1100], cleanup[1200];
    struct wow_channel channel;
    struct pw_game_wire replay;
    int recorded = -1;
    const char *windows_cwd = "C:\\Games\\World of Warcraft\\_anniversary_";
    const char *unix_cwd = FOREVER_DRIVE_C "/Games/World of Warcraft/_anniversary_";
    char *program[] = {"C:\\Games\\World of Warcraft\\_anniversary_\\WowClassic.exe",
        "-launcherlogin", "-uid", "wow_classic_anniversary", "-initialgamemode=bccfresh",
        "-launcherticket", "ticket-secret", "--Session-Token=session-secret",
        "/password:password-secret",
        "-launcheruserid", "private-user", "orphan-secret", "-launcherlogin=login-secret",
        "--api-key=key-secret", "-username", "user-secret",
        "--config=accountName=alice@example.com", "--config", "SET accountName alice@example.com",
        "--opaque=opaque-secret", "--other", "opaque-secret",
        "-uid=wow_bad=secret", "-initialgamemode=mode;secret", NULL};
    assert(mkdtemp(state));
    assert(!wow_channel_parse(program[0], &channel));
    strcpy(channel.unix_cwd, unix_cwd);
    strcpy(channel.product, "wow_anniversary");
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded));
    assert(!recorded && replay.argc == 11);
    assert(!strcmp(replay.argv[10], "wow_anniversary")); /* unchanged first-start fallback */
    assert(!pw_game_record_launch(state, program, windows_cwd, unix_cwd));
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded));
    assert(recorded && replay.argc == 12);
    assert(!strcmp(replay.argv[3], unix_cwd) && !strcmp(replay.argv[5], windows_cwd));
    assert(!strcmp(replay.argv[7], program[0]) && !strcmp(replay.argv[8], "-launcherlogin"));
    assert(!strcmp(replay.argv[10], "wow_classic_anniversary"));
    assert(!strcmp(replay.argv[11], "-initialgamemode=bccfresh"));
    assert(!pw_game_record_path(path, sizeof(path), state, channel.folder));
    char bytes[PW_GAME_ARGV_BYTES + 1];
    ssize_t size = forever_read_file(path, bytes, sizeof(bytes));
    assert(size > 0);
    assert(!memmem(bytes, (size_t)size, "secret", 6));
    assert(!memmem(bytes, (size_t)size, "private-user", 12));
    assert(!memmem(bytes, (size_t)size, "alice@", 6));
    char *switches[] = {program[0], "-uid", "wow_classic_anniversary",
                        "-console", "-initialgamemode=bccfresh", NULL};
    assert(!pw_game_record_launch(state, switches, windows_cwd, unix_cwd));
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded));
    assert(replay.argc == 12 && !strcmp(replay.argv[10], "-console") &&
           !strcmp(replay.argv[11], "-initialgamemode=bccfresh"));
    switches[3] = "-windowed";
    assert(!pw_game_record_launch(state, switches, windows_cwd, unix_cwd));
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded));
    assert(replay.argc == 12 && !strcmp(replay.argv[10], "-windowed") &&
           !strcmp(replay.argv[11], "-initialgamemode=bccfresh"));
    char *ambiguous[] = {program[0], "-uid", "wow_classic_anniversary",
                         "-opaque", "-initialgamemode=bccfresh", NULL};
    assert(pw_game_record_launch(state, ambiguous, windows_cwd, unix_cwd) == ENOTSUP);
    ambiguous[3] = "-AUTH"; ambiguous[4] = "-opaque-secret";
    assert(pw_game_record_launch(state, ambiguous, windows_cwd, unix_cwd) == ENOTSUP);
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded));
    assert(!strcmp(replay.argv[10], "-windowed") && !strcmp(replay.argv[11], "-initialgamemode=bccfresh"));
    assert(!pw_game_record_launch(state, program, windows_cwd, unix_cwd));

    struct wow_channel era;
    assert(!wow_channel_parse("C:\\Games\\World of Warcraft\\_classic_era_\\WowClassic.exe", &era));
    strcpy(era.unix_cwd, FOREVER_DRIVE_C "/Games/World of Warcraft/_classic_era_");
    strcpy(era.product, "wow_classic_era");
    assert(!pw_game_direct_launch(&replay, state, &era, &recorded));
    assert(!recorded && !strcmp(replay.argv[10], "wow_classic_era"));
    char *era_program[] = {"C:\\Games\\World of Warcraft\\_classic_era_\\WowClassic.exe",
                          "-uid", "wow_classic_era", "-future-option", "unchanged", NULL};
    assert(!pw_game_record_launch(state, era_program, era.windows_cwd, era.unix_cwd));
    assert(!pw_game_direct_launch(&replay, state, &era, &recorded));
    assert(recorded && replay.argc == 10 && !strcmp(replay.argv[9], "wow_classic_era"));
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded));
    assert(recorded && !strcmp(replay.argv[11], "-initialgamemode=bccfresh"));
    assert(!pw_game_record_launch(state, program, windows_cwd, era.unix_cwd));
    assert(pw_game_direct_launch(&replay, state, &channel, &recorded) == EINVAL);
    char *another_install[] = {"C:\\Other\\World of Warcraft\\_anniversary_\\WowClassic.exe",
                              "-uid", "wow_classic_anniversary", NULL};
    assert(!pw_game_record_launch(state, another_install,
        "C:\\Other\\World of Warcraft\\_anniversary_",
        FOREVER_DRIVE_C "/Other/World of Warcraft/_anniversary_"));
    assert(pw_game_direct_launch(&replay, state, &channel, &recorded) == EINVAL);
    char *another_executable[] = {"C:\\Games\\World of Warcraft\\_anniversary_\\WowClassicT.exe",
                                 "-uid", "wow_classic_anniversary", NULL};
    assert(!pw_game_record_launch(state, another_executable, windows_cwd, unix_cwd));
    assert(pw_game_direct_launch(&replay, state, &channel, &recorded) == EINVAL);
    char windows_trailing[WOW_PATH_BYTES], unix_trailing[WOW_PATH_BYTES];
    snprintf(windows_trailing, sizeof(windows_trailing), "%s\\", windows_cwd);
    snprintf(unix_trailing, sizeof(unix_trailing), "%s/", unix_cwd);
    assert(!pw_game_record_launch(state, program, windows_trailing, unix_trailing));
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded) && recorded);
    assert(!strcmp(replay.argv[3], unix_trailing) && !strcmp(replay.argv[5], windows_trailing));
    char *case_and_slashes[] = {"c:/Games/World of Warcraft/_anniversary_/wowclassic.EXE",
                               "-UID=wow_classic_anniversary", "-INITIALGAMEMODE", "bccfresh", NULL};
    assert(!pw_game_record_launch(state, case_and_slashes,
        "c:/Games/World of Warcraft/_anniversary_/", unix_cwd));
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded) && recorded);
    assert(!strcmp(replay.argv[10], "bccfresh"));
    assert(!pw_game_record_launch(state, program, windows_cwd, unix_cwd));
    assert(!pw_game_record_path(other, sizeof(other), state, era.folder));
    assert(!forever_write_file(other, bytes, (size_t)size)); /* foreign channel is not replayable */
    assert(pw_game_direct_launch(&replay, state, &era, &recorded) == EINVAL);
    char *bare[] = {program[0], NULL};
    assert(!pw_game_wire_encode(&replay, bare, windows_cwd, unix_cwd));
    size_t base_bytes = (size_t)(replay.argv[replay.argc - 1] - replay.bytes) +
                       strlen(replay.argv[replay.argc - 1]) + 1;
    char fields[PW_GAME_ARGC_MAX][160], *exact[PW_GAME_ARGC_MAX + 1];
    size_t remaining = PW_GAME_ARGV_BYTES - base_bytes;
    unsigned options = (unsigned)((remaining + 132) / 133);
    exact[0] = program[0];
    for (unsigned i = 0; i < options; ++i)
    {
        size_t bytes_this = remaining / (options - i);
        assert(bytes_this >= 9 && bytes_this <= 133);
        memset(fields[i], 'a', bytes_this - 1);
        memcpy(fields[i], "-uid=wow", 8); fields[i][bytes_this - 1] = 0;
        exact[i + 1] = fields[i]; remaining -= bytes_this;
    }
    exact[options + 1] = NULL;
    assert(!pw_game_record_launch(state, exact, windows_cwd, unix_cwd));
    struct stat status;
    assert(!stat(path, &status) && status.st_size == PW_GAME_ARGV_BYTES);
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded) && recorded);
    assert(!strcmp(replay.argv[7 + options], fields[options - 1]));
    /* Even a locally replaced record cannot replay credential options. */
    char *unsafe[] = {program[0], "-token=secret", NULL};
    assert(!pw_game_wire_encode(&replay, unsafe, windows_cwd, unix_cwd));
    size_t unsafe_bytes = (size_t)(replay.argv[replay.argc - 1] - replay.bytes) +
                         strlen(replay.argv[replay.argc - 1]) + 1;
    assert(!forever_write_file(path, replay.bytes, unsafe_bytes));
    assert(pw_game_direct_launch(&replay, state, &channel, &recorded) == EINVAL);
    assert(!forever_write_file(path, "game.bin\0bad-version\0", 21));
    assert(pw_game_direct_launch(&replay, state, &channel, &recorded) == EINVAL);
    assert(!unlink(path) && !mkfifo(path, 0600));
    pid_t child = fork();
    assert(child >= 0);
    if (!child)
    {
        alarm(2);
        _exit(pw_game_direct_launch(&replay, state, &channel, &recorded) == EINVAL ? 0 : 1);
    }
    int exit_status;
    assert(waitpid(child, &exit_status, 0) == child);
    assert(WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0);
    assert(!unlink(path));
    memset(bytes, 'x', sizeof(bytes));
    assert(!forever_write_file(path, bytes, sizeof(bytes)));
    assert(pw_game_direct_launch(&replay, state, &channel, &recorded) == E2BIG);
    bytes[sizeof(bytes) - 1] = 0;
    char *oversized[] = {program[0], bytes, NULL};
    assert(pw_game_record_launch(state, oversized, windows_cwd, unix_cwd) == E2BIG);
    char *too_many[PW_GAME_ARGC_MAX + 2];
    too_many[0] = program[0];
    for (unsigned i = 1; i <= PW_GAME_ARGC_MAX; ++i) too_many[i] = "-launcherlogin";
    too_many[PW_GAME_ARGC_MAX + 1] = NULL;
    assert(pw_game_record_launch(state, too_many, windows_cwd, unix_cwd) == E2BIG);
    too_many[PW_GAME_ARGC_MAX] = NULL;
    assert(!pw_game_record_launch(state, too_many, windows_cwd, unix_cwd));
    assert(!pw_game_direct_launch(&replay, state, &channel, &recorded));
    assert(recorded && replay.argc == PW_GAME_ARGC_MAX + 7);
    assert(pw_game_record_path(path, sizeof(path), state, "../bad") == EINVAL);
    assert(pw_game_record_path(path, 8, state, channel.folder) == ENAMETOOLONG);
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", state);
    assert(!system(cleanup));
}


static void test_actual_case_paths(void)
{
    char state[] = "/tmp/forever-case-XXXXXX", prefix[1100], world[1100], cwd[1100];
    char path[1500], image[1100], windows[1100], expected[1100], cleanup[1200];
    const struct { const char *folder, *launched, *exe, *loader; } cases[] = {
        {"_classic_era_", "WoWClassic.exe", "WowClassic.exe", "WowClassic_loader.dll"},
        {"_retail_", "Wow.exe", "Wow.exe", "wow_loader.dll"},
        {"_classic_beta_", "WowB.exe", "WowB.exe", "WowB_loader.dll"}
    };
    assert(mkdtemp(state));
    snprintf(prefix, sizeof(prefix), "%s/prefix", state);
    snprintf(world, sizeof(world), "%s/drive_c/Games/World of Warcraft", prefix);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        struct pw_game_wire wire;
        struct pw_game_request request;
        struct wow_channel selected;
        char *forwarded[PW_GAME_WIRE_ARGV + 1];
        snprintf(cwd, sizeof(cwd), "%s/%s", world, cases[i].folder);
        assert(!forever_mkdirs(cwd));
        snprintf(path, sizeof(path), "%s/%s", cwd, cases[i].exe);
        assert(!forever_write_file(path, "MZ", 2));
        snprintf(path, sizeof(path), "%s/%s", cwd, cases[i].loader);
        assert(!forever_write_file(path, "loader", 6));
        snprintf(windows, sizeof(windows), "C:\\Games\\World of Warcraft\\%s", cases[i].folder);
        snprintf(image, sizeof(image), "%s\\%s", windows, cases[i].launched);
        snprintf(expected, sizeof(expected), "%s\\%s", windows, cases[i].exe);
        char *program[] = {image, "-uid", "wow_classic_era", NULL};
        assert(!pw_game_wire_encode(&wire, program, windows, cwd));
        assert(!pw_game_wire_decode((int)wire.argc, (char **)wire.argv, prefix,
                                    wow_channel_resolve_files, NULL, NULL));
        assert(!strcmp(wire.argv[7], expected));
        assert(!pw_game_request_pack(&request, &wire));
        assert(!pw_game_request_unpack(&request, forwarded));
        assert(!strcmp(forwarded[7], expected));
        assert(!pw_game_record_launch(state, (char **)wire.argv + 7, windows, cwd));
        assert(!wow_channel_read(world, "C:\\Games\\World of Warcraft", cases[i].folder, &selected));
        assert(!pw_game_direct_launch(&wire, state, &selected, NULL));
        assert(!strcmp(wire.argv[7], expected));
        if (!i)
        {
            snprintf(path, sizeof(path), "%s/WOWClassic.exe", cwd);
            assert(!forever_write_file(path, "MZ", 2));
            assert(!pw_game_wire_encode(&wire, program, windows, cwd));
            assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, prefix,
                                       wow_channel_resolve_files, NULL, NULL) == -EEXIST);
            assert(!unlink(path));
            snprintf(path, sizeof(path), "%s/WOWClassic_loader.dll", cwd);
            assert(!forever_write_file(path, "loader", 6));
            assert(!pw_game_wire_encode(&wire, program, windows, cwd));
            assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, prefix,
                                       wow_channel_resolve_files, NULL, NULL) == -EEXIST);
            assert(!wow_channel_read(world, "C:\\Games\\World of Warcraft", cases[i].folder, &selected));
            assert(!selected.unix_loader[0]);
            assert(wow_channel_select(prefix, cases[i].folder, &selected) == -ENOENT);
            assert(!unlink(path));
        }
    }
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", state);
    assert(!system(cleanup));
}

static void test_dynamic_channels(void)
{
    unsigned installed = 1;
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
        ,{"C:\\Games\\World of Warcraft\\_retail_\\Wow.exe",
          "C:\\Games\\World of Warcraft\\_retail_",
          FOREVER_DRIVE_C "/Games/World of Warcraft/_retail_", "wow"}
        ,{"W:\\Games\\World of Warcraft\\_ptr_\\WowT.exe",
          "W:\\Games\\World of Warcraft\\_ptr_",
          FOREVER_PREFIX "/dosdevices/w:/Games/World of Warcraft/_ptr_", "wowt"}
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
        assert(wow_ascii_equal(wire.argv[7], program[0])); /* actual disk spelling is canonical */
        for (unsigned j = 1; program[j]; ++j) assert(!strcmp(wire.argv[7 + j], program[j]));
        assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                                   missing_loader, NULL, NULL) == -ENOENT);
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
    char selector[] = " \t_classic_era_\r\n";
    assert(!pw_game_direct_selector(selector) && !strcmp(selector, "_classic_era_"));
    char invalid[] = "../_retail_";
    assert(pw_game_direct_selector(invalid) == -EINVAL);
    test_dynamic_channels();
    test_recorded_launch();
    test_actual_case_paths();
    assert(!pw_game_direct_flag_consumed(0, 0));
    assert(!pw_game_direct_flag_consumed(0, -1));
    assert(!pw_game_direct_flag_consumed(1, -1));
    assert(pw_game_direct_flag_consumed(1, 0));
    assert(!pw_game_wire_encode(&wire, program,
                                "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_", unix_cwd));
    assert(!pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                                installed_exists, NULL, &separator));
    assert(wire.argc == 11 && separator == 6);
    assert(!strcmp(wire.argv[3], unix_cwd));
    assert(!strcmp(wire.argv[5], "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_"));
    for (unsigned i = 0; program[i]; ++i) assert(!strcmp(wire.argv[7 + i], program[i]));
    assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, "/missing/prefix",
                               installed_exists, NULL, NULL) == -EACCES);
    assert(!pw_game_wire_encode(&wire, program,
                                "C:\\Program Files (x86)\\World of Warcraft\\_classic_beta_", unix_cwd));
    assert(wire.argc == 11 && !strcmp(wire.argv[1], "--ps5-game-v1") && !strcmp(wire.argv[6], "--"));
    assert(!pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                                installed_exists, NULL, &separator));
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
                               installed_exists, NULL, NULL) == -EINVAL);
    wire.argv[1] = "--ps5-game-v1"; wire.argv[6] = "not-separator";
    assert(pw_game_wire_decode((int)wire.argc, (char **)wire.argv, FOREVER_PREFIX,
                               installed_exists, NULL, NULL) == -EINVAL);
    wire.argv[6] = "--";
    char huge[PW_GAME_ARGV_BYTES]; memset(huge, 'x', sizeof(huge) - 1); huge[sizeof(huge) - 1] = 0;
    char *oversized[] = {program[0], huge, NULL};
    assert(pw_game_wire_encode(&wire, oversized, "C:\\WoW", unix_cwd) == E2BIG);
    puts("runtime relaunch argv: final/beta selection and wire handoff, direct-game flag, encode/decode round-trip, version/separator and 4096-byte bounds PASS");
    return 0;
}
