/* Host tests for title-icon.h against a PE built here and a temporary
 * directory. */
#include <assert.h>
#include <stdlib.h>
#include "title-icon.h"

/* 2x2 opaque red and 4x4 opaque green RGBA PNGs */
static const uint8_t red_png[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x02,
    0x00,0x00,0x00,0x02,0x08,0x06,0x00,0x00,0x00,0x72,0xb6,0x0d,0x24,0x00,0x00,0x00,0x15,0x49,0x44,0x41,
    0x54,0x78,0x9c,0x63,0xfc,0xcf,0xc0,0xf0,0x9f,0x81,0x81,0x81,0x81,0x09,0x44,0x80,0x30,0x00,0x1f,0x17,
    0x02,0x02,0x02,0x47,0xb3,0x14,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
static const uint8_t green_png[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x04,
    0x00,0x00,0x00,0x04,0x08,0x06,0x00,0x00,0x00,0xa9,0xf1,0x9e,0x7e,0x00,0x00,0x00,0x15,0x49,0x44,0x41,
    0x54,0x78,0x9c,0x63,0x64,0x38,0xc1,0xf0,0x9f,0x01,0x09,0x30,0x21,0x73,0x88,0x13,0x00,0x00,0x74,0xa4,
    0x01,0xcf,0x43,0xf9,0x75,0xe1,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};

#define RAW 0x400u     /* the resource section's file offset */
#define RVA 0x1000u    /* and its address */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

/* A PE32+ image whose .rsrc holds resources of type `type`, one per PNG:
 * type directory -> name directory -> language directory -> data entry */
static size_t make_pe(uint8_t *pe, size_t capacity, uint32_t type, const uint8_t *const *pngs,
                      const size_t *sizes, unsigned count)
{
    uint8_t *rsrc = pe + RAW;
    size_t used, header = 0x40, optional = header + 24, section = optional + 240;

    memset(pe, 0, capacity);
    pe[0] = 'M'; pe[1] = 'Z';
    put32(pe + 0x3c, (uint32_t)header);
    memcpy(pe + header, "PE\0\0", 4);
    put16(pe + header + 4, 0x8664);
    put16(pe + header + 6, 1);              /* sections */
    put16(pe + header + 20, 240);           /* optional header size */
    put16(pe + optional, 0x20b);
    /* root: one id entry (the type) -> name directory at 0x18 */
    put16(rsrc + 14, 1);
    put32(rsrc + 16, type);
    put32(rsrc + 20, 0x80000000u | 0x18);
    put16(rsrc + 0x18 + 14, (uint16_t)count);
    used = 0x18 + 16 + 8 * count;
    for (unsigned i = 0; i < count; i++)
    {
        size_t language = used, entry = language + 24;
        put32(rsrc + 0x18 + 16 + 8 * i, i + 1);
        put32(rsrc + 0x18 + 16 + 8 * i + 4, 0x80000000u | (uint32_t)language);
        put16(rsrc + language + 14, 1);
        put32(rsrc + language + 16, 0x409);
        put32(rsrc + language + 20, (uint32_t)entry);
        used = entry + 16;
        put32(rsrc + entry, RVA + (uint32_t)used);
        put32(rsrc + entry + 4, (uint32_t)sizes[i]);
        memcpy(rsrc + used, pngs[i], sizes[i]);
        used += sizes[i];
    }
    put32(pe + optional + 112 + 2 * 8, RVA);
    put32(pe + optional + 112 + 2 * 8 + 4, (uint32_t)used);
    put32(pe + section + 8, (uint32_t)used);
    put32(pe + section + 12, RVA);
    put32(pe + section + 16, (uint32_t)used);
    put32(pe + section + 20, RAW);
    assert(RAW + used <= capacity);
    return RAW + used;
}

static char root[256];

static void path(char *out, const char *relative) { snprintf(out, 1024, "%s/%s", root, relative); }

static void put_file(const char *relative, const void *data, size_t size)
{
    char p[1024];
    path(p, relative);
    assert(!forever_write_file(p, data, size));
}

static void test_find(void)
{
    static uint8_t pe[8192];
    const uint8_t *pngs[] = {red_png, green_png};
    const size_t sizes[] = {sizeof(red_png), sizeof(green_png)};
    struct title_icon_best best;
    size_t size = make_pe(pe, sizeof(pe), TITLE_ICON_RT_ICON, pngs, sizes, 2);

    assert(title_icon_find(pe, size, &best));
    assert(best.width == 4 && best.height == 4 && best.size == sizeof(green_png));
    assert(!memcmp(best.png, green_png, sizeof(green_png))); /* the largest wins */

    /* a resource cut short is skipped, other types and bad headers give nothing */
    assert(title_icon_find(pe, size - 40, &best) && best.width == 2);
    size = make_pe(pe, sizeof(pe), 14 /* RT_GROUP_ICON */, pngs, sizes, 2);
    assert(!title_icon_find(pe, size, &best));
    size = make_pe(pe, sizeof(pe), TITLE_ICON_RT_ICON, pngs, sizes, 2);
    pe[0] = 'X';
    assert(!title_icon_find(pe, size, &best));
    pe[0] = 'M';
    put32(pe + 0x3c, 0xfffffff0u);
    assert(!title_icon_find(pe, size, &best));
    assert(!title_icon_find(pe, 16, &best));
}

static int icon_center_is(const char *icon, const uint8_t *rgb_expected)
{
    size_t png_size = 0;
    uint8_t *png = title_icon_read(icon, 4u << 20, &png_size);
    unsigned char *rgb;
    int w, h, n, ok;

    assert(png);
    assert((rgb = stbi_load_from_memory(png, (int)png_size, &w, &h, &n, 3)) && w == 512 && h == 512);
    ok = !memcmp(rgb + 3 * (256 * 512 + 256), rgb_expected, 3) &&   /* the logo, centred */
         !memcmp(rgb, title_icon_tile, 3) &&                          /* the tile's corner */
         !memcmp(rgb + 3 * (31 * 512 + 31), title_icon_tile, 3);    /* outside the logo */
    stbi_image_free(rgb);
    free(png);
    return ok;
}

static void test_update(void)
{
    static uint8_t pe[8192];
    const uint8_t *pngs[] = {red_png, green_png};
    const size_t sizes[] = {sizeof(red_png), sizeof(green_png)};
    const uint8_t green[3] = {0, 200, 0}, red[3] = {255, 0, 0};
    char program[1024], icon[1024], source[1024], missing[1024];
    size_t size = make_pe(pe, sizeof(pe), TITLE_ICON_RT_ICON, pngs, sizes, 2);

    path(program, "Battle.net Launcher.exe");
    path(icon, "icon0.png");
    path(source, ".icon-source");
    path(missing, "missing.exe");
    put_file("Battle.net Launcher.exe", pe, size);
    assert(title_icon_update(program, icon, source) == 1);
    assert(icon_center_is(icon, green));
    assert(title_icon_update(program, icon, source) == 0); /* the same program: kept */

    /* a changed program (an update) makes the icon again */
    size = make_pe(pe, sizeof(pe), TITLE_ICON_RT_ICON, pngs, sizes, 1);
    put_file("Battle.net Launcher.exe", pe, size);
    assert(title_icon_update(program, icon, source) == 1);
    assert(icon_center_is(icon, red));

    /* a lost icon file is made again even when the program is unchanged */
    assert(!unlink(icon));
    assert(title_icon_update(program, icon, source) == 1);
    assert(icon_center_is(icon, red));

    assert(title_icon_update(missing, icon, source) == -ENOENT);
    size = make_pe(pe, sizeof(pe), 14, pngs, sizes, 2);
    put_file("Battle.net Launcher.exe", pe, size);
    assert(title_icon_update(program, icon, source) == -EINVAL);
    assert(icon_center_is(icon, red)); /* a failed attempt keeps the last icon */
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/test-title-icon-%d", (int)getpid());
    assert(!forever_mkdirs(root));
    test_find();
    test_update();
    puts("title icon: largest PNG icon resource, bounds, centred logo on the tile, remade on change PASS");
    return 0;
}
