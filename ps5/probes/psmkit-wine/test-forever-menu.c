/* Host behavior tests for Forever's popup state and bounded file operations. */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "../../title/forever-menu.h"
#include "forever-setup.h"
#include "forever-menu-settings.h"

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

static void test_key_rewrite(void)
{
    char path[1024], fg[1024], hdr[1024], hud[1024];
    int enabled = -1, hdr_enabled = -1, hud_enabled = -1;
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
    put("switches/drive_c/independent-fg.txt", "fg=invalid\n");
    put("switches/drive_c/independent-hdr.txt", "hdr=off\n");
    put("switches/drive_c/independent-hud.txt", "hud=on\n");
    assert(forever_menu_read_settings(fg, hdr, hud, &fg_mode, &hdr_enabled, &hud_enabled) == -EINVAL);
    assert(fg_mode == FOREVER_FG_ON);
    assert(!hdr_enabled && hud_enabled); /* one bad setting cannot hide the other two */

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
    struct forever_menu_values values = {FOREVER_FG_AUTO, 1, 0};
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

    assert(forever_menu_find_set_query(&menu, "old"));
    forever_menu_find_ime(&menu, "replacement", 0);
    assert(!strcmp(menu.addon_query, "old")); /* IME cancel keeps the old query. */
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
    struct forever_menu_values values = {FOREVER_FG_AUTO, 1, 0};
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
    assert(strstr(text, "Get a free key at addons.wago.io/account/apikeys"));
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
