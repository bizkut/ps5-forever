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
    put("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/WTF/Config.wtf",
        "SET mine \"keep\"\r\nSET HDROutput \"1\"\r\nSET HDRFactor \"1.7\"\r\n");

    assert(forever_menu_set_wow_hdr(share, prefix, 0) == 2);
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/WTF/Config.wtf",
                   "SET mine \"keep\"\r\nSET HDROutput \"0\"\r\nSET HDRFactor \"1.7\"\r\n"));
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_classic_beta_/WTF/Config.wtf",
                   "SET GxApi \"D3D12\"\nSET HDROutput \"0\"\nSET HDRFactor \"2\"\n"));
    assert(!forever_menu_rewrite_switch(flag, "hdr", 0));
    assert(!forever_menu_read_switch(flag, "hdr", 1, &enabled) && !enabled);
    assert(!forever_menu_hdr_runtime_enabled(flag));

    assert(forever_menu_set_wow_hdr(share, prefix, 1) == 2);
    assert(text_is("hdr/prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/WTF/Config.wtf",
                   "SET mine \"keep\"\r\nSET HDROutput \"1\"\r\nSET HDRFactor \"1.7\"\r\n"));
    assert(!forever_menu_rewrite_switch(flag, "hdr", 1));
    assert(forever_menu_hdr_runtime_enabled(flag));

    make_path(flag, sizeof(flag), "hdr/missing-flag.txt");
    assert(forever_menu_hdr_runtime_enabled(flag)); /* existing HDR-on profile is the fallback */
}

static void test_deletion_scope(void)
{
    char prefix[1024], path[1024];

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

static void test_state_machine(void)
{
    struct forever_menu_state menu;
    struct forever_menu_values values = {FOREVER_FG_AUTO, 1, 0};
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
    assert(menu.selected == FOREVER_MENU_REINSTALL); /* disabled Add-ons is skipped */
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_CONFIRM_REINSTALL && menu.confirmation_selected == 0);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE && menu.page == FOREVER_MENU_MAIN);
    assert(menu.selected == FOREVER_MENU_REINSTALL);
    forever_menu_select(&menu);
    forever_menu_move(&menu, 1);
    result = forever_menu_select(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_REINSTALL);

    forever_menu_open(&menu, &values);
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_UNINSTALL);
    assert(result.command == FOREVER_MENU_COMMAND_NONE);
    assert(menu.page == FOREVER_MENU_CONFIRM_UNINSTALL && menu.confirmation_selected == 0);
    result = forever_menu_back(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_NONE && menu.page == FOREVER_MENU_MAIN);
    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_ADDONS);
    assert(result.command == FOREVER_MENU_COMMAND_NONE && menu.page == FOREVER_MENU_MAIN);
    forever_menu_render(&menu, text, sizeof(text));
    assert(strstr(text, "Add-ons…\tComing soon"));
    assert(strstr(text, "Frame generation\tAuto"));

    result = forever_menu_click_line(&menu, FOREVER_MENU_LINE_FRAME_GENERATION);
    assert(result.command == FOREVER_MENU_COMMAND_SET_FRAME_GENERATION && !result.enabled);
    forever_menu_commit(&menu, result.command, result.enabled);
    assert(menu.values.frame_generation == FOREVER_FG_OFF);
    result = forever_menu_back(&menu);
    assert(result.command == FOREVER_MENU_COMMAND_CLOSE);
}

int main(void)
{
    char command[320];
    snprintf(root, sizeof(root), "/tmp/forever-menu-test-%ld", (long)getpid());
    test_key_rewrite();
    test_hdr_config();
    test_deletion_scope();
    test_state_machine();
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    assert(!system(command));
    puts("forever menu helpers/state: PASS");
    return 0;
}
