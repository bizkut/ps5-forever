/* Shared discovery, product parsing, path grammar and direct-start contracts. */
#include "wow-channels.h"
#include "forever-setup.h"
#include <assert.h>
#include <stdlib.h>
static char root[256];
static void put(const char *relative, const char *text)
{
    char path[2048], *slash;
    assert(snprintf(path, sizeof(path), "%s/%s", root, relative) > 0);
    slash = strrchr(path, '/'); assert(slash); *slash = 0;
    assert(!forever_mkdirs(path)); *slash = '/';
    assert(!forever_write_file(path, text, strlen(text)));
}
static int collect(const struct wow_channel *channel, void *context)
{
    unsigned *count = context;
    assert(channel->folder[0] == '_'); ++*count; return 0;
}
int main(void)
{
    char pattern[] = "/tmp/forever-channels-XXXXXX", prefix[1024], world[1100];
    struct wow_channel channel;
    unsigned count = 0;
    assert(mkdtemp(pattern)); snprintf(root, sizeof(root), "%s", pattern);
    snprintf(prefix, sizeof(prefix), "%s/prefix", root);
    snprintf(world, sizeof(world), "%s/drive_c/Program Files (x86)/World of Warcraft", prefix);
    assert(wow_executable_name("Wow.exe")); assert(wow_executable_name("wOwClassicT.EXE"));
    assert(!wow_executable_name("Wow64.exe")); assert(!wow_executable_name("Wow-64.exe"));
    assert(!wow_executable_name("BlizzardError.exe")); assert(!wow_executable_name("Wow.exe.bak"));
    assert(wow_folder_name("_future.event 2_"));
    assert(!wow_folder_name("_bad/name_"));
    assert(!wow_channel_parse("W:\\Games\\World of Warcraft\\_newthing_\\wOwFuture.EXE", &channel));
    assert(!strcmp(channel.folder, "_newthing_"));
    assert(!wow_channel_parse("c:/games/World of Warcraft/_ptr_/WowT.exe", &channel));
    assert(wow_channel_parse("D:\\World of Warcraft\\_ptr_\\WowT.exe", &channel) < 0);
    assert(wow_channel_parse("C:\\Other\\_ptr_\\WowT.exe", &channel) < 0);
    assert(wow_channel_parse("C:\\World of Warcraft\\_ptr_\\nested\\WowT.exe", &channel) < 0);
    assert(wow_channel_parse("C:\\World of Warcraft\\..\\Wow.exe", &channel) < 0);
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_retail_/Wow.exe", "MZ");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_retail_/wow_loader.dll", "loader");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_retail_/.flavor.info", "Product Flavor!STRING:0\r\nwow\r\n");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_classic_beta_/WowB.exe", "MZ");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_classic_beta_/WowB_loader.dll", "loader");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_classic_beta_/.flavor.info", "Product Flavor!STRING:0\nwow_classic_beta\n");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_future_/WowFuture.exe", "MZ");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_future_/WowFuture_loader.dll", "loader");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_future_/.flavor.info", "Product Flavor!STRING:0\nwow_unannounced\n");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_ptr_/WowT.exe", "MZ");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_pending_/.flavor.info", "Product Flavor!STRING:0\nwow_pending\n");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_empty_/BlizzardError.exe", "MZ");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/.build.info", "Branch!STRING:0|Version!STRING:0|Product!STRING:0\nptr|12.1.5.70448|wowt\n");
    assert(!wow_channel_read(world, "C:\\Program Files (x86)\\World of Warcraft", "_ptr_", &channel));
    assert(!strcmp(channel.product, "wowt"));
    assert(!strcmp(wow_channel_launch_uid(&channel), "wow_ptr"));
    assert(!wow_channels_scan(prefix, collect, &count)); assert(count == 5);
    assert(!wow_channel_select(prefix, "", &channel)); assert(!strcmp(channel.folder, "_classic_beta_"));
    assert(!wow_channel_select(prefix, "_retail_", &channel)); assert(!strcmp(channel.product, "wow"));
    assert(strstr(channel.unix_loader, "/wow_loader.dll"));
    assert(!strcmp(wow_channel_label(&channel), "WoW Retail"));
    assert(!wow_channel_select(prefix, "_future_", &channel)); assert(!strcmp(channel.product, "wow_unannounced"));
    assert(!strcmp(wow_channel_label(&channel), "_future_"));
    assert(!strcmp(wow_channel_launch_uid(&channel), "wow_unannounced"));
    assert(wow_channel_select(prefix, "_pending_", &channel) == -ENOENT);
    assert(wow_channel_select(prefix, "_missing_", &channel) == -ENOENT);
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/.build.info", "Branch!STRING:0|Version!STRING:0|Product!STRING:0\nwow|12.1.5.70448|wow\n");
    assert(!wow_channel_read(world, "C:\\Program Files (x86)\\World of Warcraft", "_ptr_", &channel));
    assert(!channel.product[0]);
    assert(!wow_channel_read(world, "C:\\Program Files (x86)\\World of Warcraft", "_classic_beta_", &channel));
    assert(!strcmp(channel.product, "wow_classic_beta"));
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/Wow.exe", "MZ");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/wow_loader.dll", "loader");
    put("prefix/drive_c/Program Files (x86)/World of Warcraft/_forever_/.flavor.info", "Product Flavor!STRING:0\nwowf\n");
    assert(!wow_channel_select(prefix, "", &channel));
    assert(!strcmp(channel.folder, "_classic_beta_"));
    char missing[1200];
    snprintf(missing, sizeof(missing), "%s/_classic_beta_/WowB_loader.dll", world);
    assert(!unlink(missing));
    assert(!wow_channel_select(prefix, "", &channel));
    assert(!strcmp(channel.folder, "_forever_"));
    assert(wow_channel_select(prefix, "_classic_beta_", &channel) == -ENOENT);
    snprintf(missing, sizeof(missing), "%s/_forever_/wow_loader.dll", world);
    assert(!unlink(missing));
    assert(!wow_channel_select(prefix, "", &channel)); /* any new complete channel */
    assert(!strcmp(channel.folder, "_future_"));
    char cleanup[320];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    assert(!system(cleanup));
    puts("WoW channel discovery/path/product/direct selection PASS");
    return 0;
}
