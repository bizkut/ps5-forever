/* Host tests for forever-setup.h against a temporary directory. */
#include <assert.h>
#include <stdlib.h>
#include "forever-setup.h"

static char root[256];

static void path(char *out, const char *relative)
{
    snprintf(out, 1024, "%s/%s", root, relative);
}

static void put(const char *relative, const char *text)
{
    char p[1024];
    path(p, relative);
    char *slash = strrchr(p, '/');
    *slash = 0;
    assert(!forever_mkdirs(p));
    *slash = '/';
    assert(!forever_write_file(p, text, strlen(text)));
}

static int text_is(const char *relative, const char *text)
{
    char p[1024], buffer[4096];
    path(p, relative);
    return forever_read_file(p, buffer, sizeof(buffer)) >= 0 && !strcmp(buffer, text);
}

static void make_prefix(const char *relative)
{
    char name[512];
    static const char *const regs[] = { "system.reg", "user.reg", "userdef.reg" };
    for (int i = 0; i < 3; ++i)
    {
        snprintf(name, sizeof(name), "%s/%s", relative, regs[i]);
        put(name, "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n");
    }
    snprintf(name, sizeof(name), "%s/.update-timestamp", relative);
    put(name, "1791000000\n");
}

static void test_write_if_absent(void)
{
    char p[1024];
    path(p, "data/prefix/drive_c/wineps5-fg.txt");
    assert(!forever_mkdirs(root));
    put("data/prefix/drive_c/keep", "");
    assert(forever_write_if_absent(p, FOREVER_FG_DEFAULT) == 1);
    assert(text_is("data/prefix/drive_c/wineps5-fg.txt", FOREVER_FG_DEFAULT));
    put("data/prefix/drive_c/wineps5-fg.txt", "fg=off\n");
    assert(forever_write_if_absent(p, FOREVER_FG_DEFAULT) == 0);
    assert(text_is("data/prefix/drive_c/wineps5-fg.txt", "fg=off\n")); /* the user's file wins */
}

static void test_prefix_ready(void)
{
    char p[1024], stamp[1024];
    path(p, "ready");
    path(stamp, "ready/.update-timestamp");
    assert(!forever_prefix_ready(p));
    make_prefix("ready");
    assert(forever_prefix_ready(p));
    put("ready/user.reg", "not a registry\n");
    assert(!forever_prefix_ready(p));

    /* first start in an empty folder: every Wine process finds valid hives,
     * drive_c and a stale stamp, yet the prefix is not ready */
    char fresh[1024], c_drive[1024];
    path(fresh, "fresh/prefix");
    path(c_drive, "fresh/prefix/drive_c");
    assert(!forever_begin_init(fresh));
    assert(forever_is_dir(c_drive));
    assert(text_is("fresh/prefix/system.reg",
                   "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n"));
    assert(text_is("fresh/prefix/user.reg",
                   "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n"
                   "#arch=win64\n"));
    assert(text_is("fresh/prefix/.update-timestamp", "1\n"));
    assert(!forever_prefix_ready(fresh));

    /* an interrupted setup keeps its registries and reruns the full install */
    make_prefix("ready");
    put("ready/system.reg", "WINE REGISTRY Version 2\n[Software] 1\n");
    assert(!forever_begin_init(p));
    assert(text_is("ready/system.reg", "WINE REGISTRY Version 2\n[Software] 1\n"));
    assert(text_is("ready/.update-timestamp", "1\n"));
    assert(forever_exists(stamp));
    assert(!forever_prefix_ready(p));
    assert(forever_finish_init(p) == 0); /* Wine stopped before wineboot finished */
    assert(!forever_prefix_ready(p));
    put("ready/" FOREVER_WINEBOOT_DONE, "");
    assert(forever_finish_init(p) == 1);
    assert(forever_prefix_ready(p));
    assert(!forever_wineboot_done(p));
}

static void test_migration(void)
{
    char prefix[1024], home[1024], cache[1024], legacy[1024], store[1024], from[1024] = "";
    path(prefix, "forever/prefix");
    path(home, "forever/home");
    path(cache, "forever/radv-shader-cache");
    path(legacy, "homebrew/PPSA99030/wine");
    path(store, "prosperostore/previous/PPSA99030/wine");
    const char *roots[] = { legacy, store };
    char forever[1024];
    path(forever, "forever");
    assert(!forever_mkdirs(forever));

    assert(forever_migrate(prefix, home, cache, roots, 2, from, sizeof(from)) == 0); /* nothing anywhere */

    make_prefix("prosperostore/previous/PPSA99030/wine/prefix");
    put("prosperostore/previous/PPSA99030/wine/prefix/drive_c/game.txt", "48 GiB\n");
    put("prosperostore/previous/PPSA99030/wine/home/.cache/x", "h\n");
    put("prosperostore/previous/PPSA99030/radv-shader-cache/mesa_cache.db", "pipelines\n");
    assert(forever_migrate(prefix, home, cache, roots, 2, from, sizeof(from)) == 1);
    assert(!strcmp(from, store));
    assert(text_is("forever/prefix/drive_c/game.txt", "48 GiB\n"));
    assert(text_is("forever/home/.cache/x", "h\n"));
    assert(text_is("forever/radv-shader-cache/mesa_cache.db", "pipelines\n")); /* the app's cache came along */
    {
        char moved[1100];
        snprintf(moved, sizeof(moved), "%s/prefix", store);
        assert(!forever_exists(moved)); /* moved, not copied */
    }

    /* an existing prefix in /data/forever is never replaced */
    make_prefix("homebrew/PPSA99030/wine/prefix");
    assert(forever_migrate(prefix, home, cache, roots, 2, from, sizeof(from)) == 0);
    assert(text_is("forever/prefix/drive_c/game.txt", "48 GiB\n"));
}

static void test_managed_sync(void)
{
    char app[1024], list[1024], prefix[1024];
    path(app, "app");
    path(list, "app/share/forever/prefix-files.txt");
    path(prefix, "managed/prefix");
    assert(!forever_mkdirs(prefix));
    put("app/lib/wine/x86_64-windows/ntdll.dll", "ntdll-v1");
    put("app/share/forever/system32/d3d12core.dll", "core-v1");
    put("app/share/forever/prefix-files.txt",
        "# forever managed prefix files\n"
        "lib/wine/x86_64-windows/ntdll.dll\tdrive_c/windows/system32/ntdll.dll\t1111\n"
        "share/forever/system32/d3d12core.dll\tdrive_c/windows/system32/d3d12core.dll\taaaa\n");
    assert(forever_sync_managed(app, list, prefix) == 2);
    assert(text_is("managed/prefix/drive_c/windows/system32/ntdll.dll", "ntdll-v1"));
    assert(text_is("managed/prefix/drive_c/windows/system32/d3d12core.dll", "core-v1"));
    assert(forever_sync_managed(app, list, prefix) == 0); /* unchanged list: nothing copied */

    /* a new app version: the list changes, every listed file follows the app */
    put("app/share/forever/system32/d3d12core.dll", "core-v2");
    put("app/share/forever/prefix-files.txt",
        "share/forever/system32/d3d12core.dll\tdrive_c/windows/system32/d3d12core.dll\tbbbb\n");
    assert(forever_sync_managed(app, list, prefix) == 1);
    assert(text_is("managed/prefix/drive_c/windows/system32/d3d12core.dll", "core-v2"));

    /* Per-entry digests: changing one DLL must not rewrite another unchanged
     * entry; missing/wrong-size destinations are repaired even with a stamp. */
    put("app/share/forever/system32/unchanged.dll", "same");
    put("app/share/forever/system32/changed.dll", "new");
    put("managed/prefix/drive_c/windows/system32/unchanged.dll", "mine");
    put("managed/prefix/drive_c/windows/system32/changed.dll", "old");
    put("managed/prefix/" FOREVER_MANAGED_STAMP,
        "share/forever/system32/unchanged.dll\tdrive_c/windows/system32/unchanged.dll\taaaa\n"
        "share/forever/system32/changed.dll\tdrive_c/windows/system32/changed.dll\tbbbb\n");
    put("app/share/forever/prefix-files.txt",
        "share/forever/system32/unchanged.dll\tdrive_c/windows/system32/unchanged.dll\taaaa\n"
        "share/forever/system32/changed.dll\tdrive_c/windows/system32/changed.dll\tcccc\n");
    assert(forever_sync_managed(app, list, prefix) == 1);
    assert(text_is("managed/prefix/drive_c/windows/system32/unchanged.dll", "mine"));
    assert(text_is("managed/prefix/drive_c/windows/system32/changed.dll", "new"));
    char missing[1024];
    path(missing, "managed/prefix/drive_c/windows/system32/changed.dll");
    assert(!unlink(missing));
    assert(forever_sync_managed(app, list, prefix) == 1);
    put("managed/prefix/drive_c/windows/system32/changed.dll", "wrong-size");
    assert(forever_sync_managed(app, list, prefix) == 1);
    assert(forever_sync_managed(app, list, prefix) == 0);

    /* Failed publication must leave the previous manifest stamp intact. */
    put("app/share/forever/prefix-files.txt",
        "share/forever/system32/missing.dll\tdrive_c/windows/system32/missing.dll\tdddd\n");
    assert(forever_sync_managed(app, list, prefix) == -ENOENT);
    assert(text_is("managed/prefix/" FOREVER_MANAGED_STAMP,
        "share/forever/system32/unchanged.dll\tdrive_c/windows/system32/unchanged.dll\taaaa\n"
        "share/forever/system32/changed.dll\tdrive_c/windows/system32/changed.dll\tcccc\n"));

    /* Old Wine-owned copies are replaced; user native replacements survive.
     * Explicit app-owned native graphics rows are tested above. */
    unsigned char old_pe[96] = {'M', 'Z'}, new_pe[96];
    memcpy(old_pe + 64, "Wine builtin DLL", sizeof("Wine builtin DLL"));
    memcpy(new_pe, old_pe, sizeof(new_pe));
    new_pe[95] = 1;
    char binary[1024];
    path(binary, "managed/prefix/drive_c/windows/system32/advapi32.dll");
    assert(!forever_write_file(binary, old_pe, sizeof(old_pe)));
    path(binary, "app/lib/wine/x86_64-windows/advapi32.dll");
    assert(!forever_write_file(binary, new_pe, sizeof(new_pe)));
    put("managed/prefix/drive_c/windows/system32/native.dll", "native replacement");
    put("app/lib/wine/x86_64-windows/native.dll", "new Wine copy");
    put("app/share/forever/prefix-files.txt",
        "lib/wine/x86_64-windows/advapi32.dll\tdrive_c/windows/system32/advapi32.dll\teeee\n"
        "lib/wine/x86_64-windows/native.dll\tdrive_c/windows/system32/native.dll\tffff\n");
    assert(forever_sync_managed(app, list, prefix) == 1);
    assert(text_is("managed/prefix/drive_c/windows/system32/native.dll", "native replacement"));
    path(binary, "managed/prefix/drive_c/windows/system32/advapi32.dll");
    unsigned char copied[96];
    int fd = open(binary, O_RDONLY);
    assert(fd >= 0 && read(fd, copied, sizeof(copied)) == sizeof(copied));
    close(fd);
    assert(!memcmp(copied, new_pe, sizeof(copied)));

    /* The full two-architecture inventory exceeds the old 64-KiB bound. */
    char *large = malloc(80002);
    assert(large);
    memset(large, '#', 80000);
    large[80000] = '\n';
    large[80001] = 0;
    put("app/share/forever/prefix-files.txt", large);
    assert(forever_sync_managed(app, list, prefix) == 0);
    assert(forever_sync_managed(app, list, prefix) == 0);
    free(large);

    put("app/share/forever/prefix-files.txt", "../escape\tdrive_c/x\tcccc\n");
    assert(forever_sync_managed(app, list, prefix) == -EACCES);
    put("app/share/forever/prefix-files.txt", "missing-tabs\n");
    assert(forever_sync_managed(app, list, prefix) == -EINVAL);
}

static void test_wow_config(void)
{
    static const char *const channels[] = {"_forever_", "_classic_beta_"};
    static const char *const executables[] = {"Wow.exe", "WowB.exe"};
    static const char preset[] = "SET GamePadEnable \"1\"\n";
    static const char mine[] = "SET mine \"1\"\n";
    char share[1024], prefix[1024], relative[512], config[1024];
    path(share, "app/share");
    path(prefix, "wow/prefix");
    put("app/share/Config.wtf", preset);
    assert(!forever_mkdirs(prefix));
    assert(forever_install_wow_config(share, prefix) == 0); /* neither installed */
    for (unsigned i = 0; i < 2; ++i)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/keep",
                 channels[i]);
        put(relative, "");
    }
    assert(forever_install_wow_config(share, prefix) == 0); /* directories alone are not an installation */
    for (unsigned i = 0; i < 2; ++i)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/%s",
                 channels[i], executables[i]);
        put(relative, "MZ");
    }
    assert(forever_install_wow_config(share, prefix) == 2);
    for (unsigned i = 0; i < 2; ++i)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[i]);
        assert(text_is(relative, preset));
    }
    assert(forever_install_wow_config(share, prefix) == 0);

    /* Either channel's existing settings win independently of the other. */
    for (unsigned existing = 0; existing < 2; ++existing)
    {
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[existing]);
        put(relative, mine);
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[1 - existing]);
        path(config, relative);
        assert(!unlink(config));
        assert(forever_install_wow_config(share, prefix) == 1);
        assert(text_is(relative, preset));
        snprintf(relative, sizeof(relative), "wow/prefix/drive_c/Program Files (x86)/World of Warcraft/%s/WTF/Config.wtf",
                 channels[existing]);
        assert(text_is(relative, mine));
        assert(forever_install_wow_config(share, prefix) == 0);
        assert(text_is(relative, mine));
    }
}

/* A prefix whose first wineboot never finished lacks the Program Files
 * folders; the Battle.net installer rejects its default location then. */
static void test_standard_folders(void)
{
    char prefix[1024], dir[1024];
    path(prefix, "folders/prefix");
    put("folders/prefix/drive_c/users/keep", "");
    assert(forever_standard_folders(prefix) == 2);
    path(dir, "folders/prefix/drive_c/Program Files");
    assert(forever_is_dir(dir));
    path(dir, "folders/prefix/drive_c/Program Files (x86)");
    assert(forever_is_dir(dir));
    assert(forever_standard_folders(prefix) == 0); /* nothing left to create */

    path(prefix, "partial/prefix");
    put("partial/prefix/drive_c/Program Files (x86)/Battle.net/Battle.net.exe", "MZ");
    assert(forever_standard_folders(prefix) == 1);
    assert(text_is("partial/prefix/drive_c/Program Files (x86)/Battle.net/Battle.net.exe", "MZ"));

    path(prefix, "blocked/prefix");
    put("blocked/prefix/drive_c/Program Files", "not a folder");
    assert(forever_standard_folders(prefix) == -ENOTDIR); /* a user's file is never replaced */
    assert(text_is("blocked/prefix/drive_c/Program Files", "not a folder"));
}

static void test_permission_message(void)
{
    char message[512];
    assert(forever_permission_error(-EACCES));
    assert(forever_permission_error(-EPERM));
    assert(!forever_permission_error(-ENOENT));
    assert(forever_permission_message(
               message, sizeof(message),
               "/data/forever/prefix/drive_c/wineps5-fg.txt",
               "/data/forever/prefix") > 0);
    assert(!strcmp(message,
        "Forever Launcher can't write to /data/forever/prefix/drive_c/wineps5-fg.txt "
        "(permission denied).\n"
        "On a PC, use FTP to set /data/forever/prefix and everything in it to permission 777,\n"
        "then start Forever Launcher again."));
    assert(!strcmp(forever_setting_permission_message(-EACCES),
                   "Forever Launcher can't save this setting (permission denied).\n"
                   "Close and start Forever Launcher again."));
    assert(forever_setting_permission_message(-EPERM));
    assert(!forever_setting_permission_message(-EIO));
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/forever-setup-test-%ld", (long)getpid());
    test_write_if_absent();
    test_prefix_ready();
    test_migration();
    test_managed_sync();
    test_wow_config();
    test_standard_folders();
    test_permission_message();
    char command[300];
    snprintf(command, sizeof(command), "rm -rf %s", root);
    if (system(command)) return 1;
    puts("test-forever-setup: PASS");
    return 0;
}
