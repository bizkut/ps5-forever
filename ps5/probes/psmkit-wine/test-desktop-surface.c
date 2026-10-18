/* Host contract for the real Wine surface functions and title compositor.
 * GDI bitmap creation is substituted; drawing uses the actual shared mappings. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wine/ps5_title.h"
#include "protocol.h"
#include "../../title/forever-menu.h"
#include "../../title/compositor.c"

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint32_t UINT;
typedef int32_t INT, LONG, NTSTATUS;
typedef int16_t INT16;
typedef uint16_t UINT16, WCHAR;
typedef uint64_t UINT64;
typedef uintptr_t UINT_PTR;
typedef void *HWND, *HANDLE, *HBITMAP;
typedef struct { int32_t left, top, right, bottom; } RECT;
typedef struct {
    struct { UINT biSize; INT biWidth, biHeight; UINT biPlanes, biBitCount;
             UINT biCompression, biSizeImage; } bmiHeader;
    UINT bmiColors[256];
} BITMAPINFO;
typedef struct {
    UINT Width, Height, Pitch, Format;
    void *pMemory, *hDeviceDc, *hBitmap, *hDc;
} D3DKMT_CREATEDCFROMMEMORY;
struct window_surface { HWND hwnd; RECT rect; UINT color_key; };
#define TRUE 1
#define FALSE 0
#define BI_RGB 0
#define D3DDDIFMT_A8R8G8B8 1
#define DCX_CACHE 1
#define DCX_WINDOW 2
#define FIELD_OFFSET(type, field) offsetof(type, field)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
#define TRACE(...) ((void)0)
#define IsRectEmpty(r) ((r)->left >= (r)->right || (r)->top >= (r)->bottom)
#define SetRectEmpty(r) memset(r, 0, sizeof(*(r)))
#define SetRect(r, l, t, w, h) (*(r) = (RECT){l, t, w, h})

#include "desktop-surface-types.inc"

static struct ps5_win_data {
    struct ps5_window_surface *surface;
    UINT color_key;
    BOOL published;
} owner;
static pthread_mutex_t win_data_mutex = PTHREAD_MUTEX_INITIALIZER;
static LONG surface_serial;
static BOOL ps5_title_desktop, bitmap_fallback;
static const WCHAR surface_id_prop[] = {'i', 0}, surface_rect_prop[] = {'r', 0};
static UINT64 property_id, property_rect;
static int ps5_surface_funcs, foreign_surface_funcs;
static uint32_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static unsigned damage_messages;
static struct ps5_desktop_window state;
static struct {
    int exit;
    int (*desktop_surface)(const char *, uint64_t, int);
} title_runtime;

uint32_t *display_image(void) { return image; }
void display_damage(const struct display_rect *rect) { assert(rect); }
static LONG InterlockedIncrement(LONG *value) { return ++*value; }
static HANDLE NtUserGetDCEx(HWND hwnd, HANDLE region, UINT flags) { return (HANDLE)1; }
static void NtUserReleaseDC(HWND hwnd, HANDLE dc) {}
static HANDLE NtGdiCreateCompatibleDC(HANDLE dc) { return (HANDLE)1; }
static void NtGdiDeleteObjectApp(HANDLE object) {}
static NTSTATUS NtGdiDdDDICreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc)
{
    if (bitmap_fallback) return -1;
    desc->hBitmap = desc->pMemory;
    return 0;
}
static void NtUserSetProp(HWND hwnd, const WCHAR *name, HANDLE value)
{
    if (name == surface_id_prop) property_id = (UINT_PTR)value;
    else { assert(name == surface_rect_prop); property_rect = (UINT_PTR)value; }
}
static struct window_surface *window_surface_create(size_t size, const void *funcs, HWND hwnd,
                                                     const RECT *rect, const BITMAPINFO *info, HBITMAP bitmap)
{
    struct window_surface *result = calloc(1, size);
    assert(result);
    result->hwnd = hwnd;
    result->rect = *rect;
    if (bitmap) memset(bitmap, 0xff, info->bmiHeader.biSizeImage);
    return result;
}
static struct ps5_win_data *find_win_data(HWND hwnd)
{
    return owner.surface && owner.surface->header.hwnd == hwnd ? &owner : NULL;
}
static BOOL send_window_state(struct ps5_win_data *data, BOOL zorder)
{
    compositor_window(&state);
    data->published = TRUE;
    return TRUE;
}
static BOOL ps5_send_message(const void *message, unsigned int size)
{
    const struct ps5_desktop_damage *damage = message;
    assert(size == sizeof(*damage) && damage->header.type == PS5_DESKTOP_DAMAGE);
    damage_messages++;
    compositor_damage(damage);
    compositor_compose();
    return TRUE;
}

#include "desktop-surface-callbacks.inc"
#include "desktop-surface-functions.inc"

#define WIDTH 7
#define HEIGHT 5
static int failures;
static void check(int condition, const char *name)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", name); failures++; }
}
static void render(void)
{
    /* Force an unrelated full-window redraw, not just the pending dirty area. */
    compositor_window(&state);
    compositor_compose();
}
static void check_image(const uint32_t *expected, const char *name)
{
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            if (image[(y + state.y) * DISPLAY_WIDTH + x + state.x] != (expected[y * WIDTH + x] | 0xff000000u))
            { check(0, name); return; }
}
static void paint(uint32_t *bits, uint32_t color)
{
    for (int i = 0; i < WIDTH * HEIGHT; i++) bits[i] = color + i;
}
static void expected_flush(uint32_t *expected, const uint32_t *source, RECT dirty, BOOL bottom_up)
{
    for (int y = max(dirty.top, 0); y < min(dirty.bottom, HEIGHT); y++)
        for (int x = max(dirty.left, 0); x < min(dirty.right, WIDTH); x++)
            expected[y * WIDTH + x] = source[(bottom_up ? HEIGHT - y - 1 : y) * WIDTH + x];
}
static void run_surface(BOOL title, BOOL fallback)
{
    const HWND hwnd = (HWND)(UINT_PTR)0x100;
    const RECT rect = {-3, -2, WIDTH - 3, HEIGHT - 2};
    const RECT partial = {1, 1, 5, 4}, clipped = {-2, -1, 3, 2}, empty = {-3, 0, -1, 3};
    struct ps5_window_surface *surface;
    struct ps5_foreign_surface *remote;
    uint32_t expected[WIDTH * HEIGHT], fallback_bits[WIDTH * HEIGHT];
    uint32_t expected_draw[WIDTH * HEIGHT];
    BITMAPINFO info = {.bmiHeader = {.biWidth = WIDTH, .biHeight = -HEIGHT}};
    struct stat status;
    unsigned before;
    int fd;

    ps5_title_desktop = title;
    bitmap_fallback = fallback;
    surface = (struct ps5_window_surface *)create_surface(hwnd, &rect);
    assert(surface);
    fd = shm_open(surface->shm, O_RDONLY, 0);
    assert(fd >= 0 && !fstat(fd, &status));
    check(status.st_size == 2 * WIDTH * HEIGHT * 4, "owner allocates draw and published planes");
    close(fd);
    owner = (struct ps5_win_data){.surface = surface};
    state = (struct ps5_desktop_window){
        .header = {PS5_DESKTOP_WINDOW, sizeof(state)}, .id = (UINT_PTR)hwnd,
        .x = 20, .y = 20, .width = WIDTH, .height = HEIGHT, .stride = WIDTH * 4,
        .flags = PS5_DESKTOP_VISIBLE, .alpha = 255, .color_key = ~0u,
        .visible_right = WIDTH, .visible_bottom = HEIGHT,
    };
    memcpy(state.shm, surface->shm, sizeof(state.shm));
    for (int i = 0; i < WIDTH * HEIGHT; i++) expected[i] = 0xffffffff;
    render();
    check_image(expected, "published plane starts white");

    paint(surface->bits, 0x00102030);
    render();
    check_image(expected, "unflushed owner drawing stays invisible");
    expected_flush(expected, surface->bits, partial, FALSE);
    assert(ps5_surface_flush(&surface->header, &rect, &partial, &info, surface->bits, FALSE, NULL, NULL));
    render();
    check_image(expected, "owner flush publishes exactly the dirty rectangle");
    paint(surface->bits, 0x00204060);
    render();
    check_image(expected, "drawing after the last flush stays invisible");
    expected_flush(expected, surface->bits, clipped, FALSE);
    assert(ps5_surface_flush(&surface->header, &rect, &clipped, &info, surface->bits, FALSE, NULL, NULL));
    render();
    check_image(expected, "owner dirty rows clip to surface bounds");
    before = damage_messages;
    assert(ps5_surface_flush(&surface->header, &rect, &empty, &info, surface->bits, FALSE, NULL, NULL));
    check(damage_messages == before, "empty flush sends no damage");

    remote = (struct ps5_foreign_surface *)open_foreign_surface(hwnd, property_id, property_rect);
    assert(remote);
    render();
    check_image(expected, "opening remote surface does not repaint published pixels");
    paint(remote->bits, 0x00306090);
    check(!memcmp(surface->bits, remote->bits, WIDTH * HEIGHT * 4), "owner and remote share the draw plane");
    render();
    check_image(expected, "unflushed remote drawing stays invisible");
    expected_flush(expected, remote->bits, partial, FALSE);
    assert(foreign_surface_flush(&remote->header, &rect, &partial, &info, remote->bits, FALSE, NULL, NULL));
    render();
    check_image(expected, "remote flush publishes exactly the dirty rectangle");
    paint(remote->bits, 0x004080c0);
    render();
    check_image(expected, "remote drawing after flush stays invisible");

    /* A private DIB must update the shared draw plane as well as publishing:
     * owner reads and later owner flushes must retain foreign drawing. */
    memcpy(expected_draw, surface->bits, sizeof(expected_draw));
    paint(fallback_bits, 0x0050a010);
    info.bmiHeader.biHeight = HEIGHT;
    expected_flush(expected, fallback_bits, clipped, TRUE);
    expected_flush(expected_draw, fallback_bits, clipped, TRUE);
    assert(ps5_surface_flush(&surface->header, &rect, &clipped, &info, fallback_bits, FALSE, NULL, NULL));
    check(!memcmp(surface->bits, expected_draw, sizeof(expected_draw)),
          "owner fallback updates only dirty shared draw rows");
    render();
    check_image(expected, "owner fallback copies clipped bottom-up rows to published plane");
    paint(fallback_bits, 0x0060c020);
    expected_flush(expected, fallback_bits, partial, TRUE);
    expected_flush(expected_draw, fallback_bits, partial, TRUE);
    assert(foreign_surface_flush(&remote->header, &rect, &partial, &info, fallback_bits, FALSE, NULL, NULL));
    check(!memcmp(surface->bits, expected_draw, sizeof(expected_draw)),
          "owner reads foreign private-DIB drawing from shared draw plane");
    render();
    check_image(expected, "remote fallback copies dirty rows to published plane");
    info.bmiHeader.biHeight = -HEIGHT;
    assert(ps5_surface_flush(&surface->header, &rect, &partial, &info, surface->bits, FALSE, NULL, NULL));
    render();
    check_image(expected, "owner flush preserves prior foreign private-DIB drawing");

    compositor_reset();
    foreign_surface_destroy(&remote->header);
    free(remote);
    ps5_surface_destroy(&surface->header);
    free(surface);
    owner.surface = NULL;
}

static void test_cursor_shape(void)
{
    union
    {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message;
    struct ps5_desktop_cursor_shape *shape = &message.shape;
    compositor_init(10, 20);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 10] == 0xff000000, "default cursor arrow preserved");
    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape) + 4 * 4},
        .width = 2, .height = 2, .hotspot_x = 1, .hotspot_y = 1, .visible = 1};
    shape->pixels[0] = 0x80ff0000;
    shape->pixels[1] = 0xff00ff00;
    shape->pixels[2] = 0xff0000ff;
    shape->pixels[3] = 0;
    compositor_cursor_shape(shape);
    compositor_compose();
    check(image[19 * DISPLAY_WIDTH + 9] == 0xff89050f, "cursor alpha blended");
    check(image[19 * DISPLAY_WIDTH + 10] == 0xff00ff00, "cursor hotspot offsets image");
    check(image[20 * DISPLAY_WIDTH + 9] == 0xff0000ff, "cursor second row");
    check(image[20 * DISPLAY_WIDTH + 10] == BACKGROUND, "transparent cursor pixel");
    compositor_cursor(11, 20);
    compositor_compose();
    check(image[19 * DISPLAY_WIDTH + 9] == BACKGROUND, "old cursor bounds repainted");
    check(image[19 * DISPLAY_WIDTH + 11] == 0xff00ff00, "new cursor bounds repainted");
    *shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)}};
    compositor_cursor_shape(shape);
    compositor_compose();
    check(image[19 * DISPLAY_WIDTH + 11] == BACKGROUND, "app hide removes cursor");
    compositor_cursor(12, 20);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == BACKGROUND, "motion does not unhide app cursor");
    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape) + 4},
        .width = 1, .height = 1, .visible = 1};
    shape->pixels[0] = 0xffffffff;
    compositor_cursor_shape(shape);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == 0xffffffff, "visible shape unhides cursor");
    compositor_reset();
}

static void test_cursor_idle(void)
{
    union
    {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message;
    struct ps5_desktop_cursor_shape *shape = &message.shape;

    compositor_reset();
    compositor_init(10, 20);
    compositor_cursor_tick(0);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 10] == 0xff000000, "cursor initially drawn before idle");
    compositor_cursor(10, 20);
    compositor_cursor_tick(2999999);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 10] == 0xff000000, "cursor visible until three seconds");
    compositor_cursor(10, 20);
    compositor_cursor_tick(3000000);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 10] == BACKGROUND,
          "three seconds without position change repaints idle cursor");
    compositor_hint_start(3000000);
    compositor_menu("Forever menu\n> Frame generation");
    compositor_cursor_tick(4000000);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 10] == BACKGROUND,
          "menu and start-up hint do not resurrect idle cursor");
    compositor_menu("");
    compositor_hint_stop();
    compositor_cursor(12, 20);
    compositor_cursor_tick(4000001);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 10] == BACKGROUND &&
          image[20 * DISPLAY_WIDTH + 12] == 0xff000000,
          "motion redraws cursor at new position without ghost arrow");
    compositor_cursor_tick(7000001);
    compositor_compose();
    compositor_cursor_wake();
    compositor_cursor_tick(7000002);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == 0xff000000,
          "stationary click activity redraws idle cursor");

    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape) + 4},
        .width = 1, .height = 1, .visible = 1};
    shape->pixels[0] = 0xffffffff;
    compositor_cursor_shape(shape);
    compositor_cursor_tick(8000000);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == 0xffffffff, "new visible shape is drawn");
    compositor_cursor_shape(shape);
    compositor_cursor_tick(10000000);
    compositor_compose();
    compositor_cursor_tick(10999999);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == 0xffffffff,
          "new visible shape restarts full idle interval");
    compositor_cursor_tick(11000000);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == BACKGROUND,
          "repeated unchanged shape does not postpone idle hiding");
    shape->pixels[0] = 0xff00ffff;
    compositor_cursor_shape(shape);
    compositor_cursor_tick(11000001);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == 0xff00ffff,
          "changed visible shape wakes idle cursor without motion");
    compositor_cursor_tick(14000000);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == 0xff00ffff,
          "changed visible shape starts a fresh three seconds");
    compositor_cursor_tick(14000001);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 12] == BACKGROUND, "changed visible shape also idles");
    *shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)}};
    compositor_cursor_shape(shape);
    compositor_cursor(14, 20);
    compositor_cursor_tick(14000002);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 14] == BACKGROUND,
          "motion never overrides app-hidden cursor after idle");
    compositor_cursor_wake();
    compositor_cursor_tick(14000003);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 14] == BACKGROUND,
          "click never overrides app-hidden cursor");
    compositor_menu("Forever menu\n> Frame generation");
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 14] == BACKGROUND,
          "menu never overrides app-hidden cursor");
    compositor_menu("");
    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape) + 4},
        .width = 1, .height = 1, .visible = 1};
    shape->pixels[0] = 0xffffffff;
    compositor_cursor_shape(shape);
    compositor_cursor_tick(14000004);
    compositor_compose();
    check(image[20 * DISPLAY_WIDTH + 14] == 0xffffffff,
          "visible shape restores app-hidden cursor with fresh activity");
    compositor_reset();
}

static void write_menu_preview(void)
{
    const char *path = getenv("FOREVER_MENU_PREVIEW_PPM");
    if (!path || !*path) return;
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (size_t i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        unsigned char rgb[] = {
            (unsigned char)(image[i] >> 16),
            (unsigned char)(image[i] >> 8),
            (unsigned char)image[i],
        };
        assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
    }
    assert(!fclose(file));
}

static void write_addon_preview(const char *name)
{
    const char *directory = getenv("FOREVER_ADDONS_PREVIEW_DIR");
    char path[1024];
    FILE *file;
    if (!directory || !*directory) return;
    assert(snprintf(path, sizeof(path), "%s/addons-%s.ppm", directory, name) > 0);
    file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (size_t i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        unsigned char rgb[] = {
            (unsigned char)(image[i] >> 16),
            (unsigned char)(image[i] >> 8),
            (unsigned char)image[i],
        };
        assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
    }
    assert(!fclose(file));
}

static void test_menu_overlay(void)
{
    struct forever_menu_state menu;
    struct forever_menu_values values = {FOREVER_FG_AUTO, 1, 0};
    struct forever_addon_snapshot snapshot = {0};
    struct display_rect panel, title, selected, hdr, fg_value, hdr_value, hud_value, addons_value;
    struct display_rect addon_row, addon_value, hint, status, query_line;
    char text[FOREVER_MENU_TEXT_BYTES];
    unsigned antialiased = 0, title_pixels = 0, hint_text_pixels = 0;
    check(compositor_menu_text_supported("\xe2\x80\x94 \xe2\x80\xa6 \xe2\x80\x99 "
                                         "\xe2\x86\x92 \xe2\x97\x8b \xe2\x9c\x95"),
          "all static non-ASCII menu characters have exact glyphs");

    forever_menu_init(&menu);
    forever_menu_open(&menu, &values);
    forever_menu_render(&menu, text, sizeof(text));
    compositor_reset();
    compositor_init(0, 0);
    check(compositor_menu_text_supported("OPTIONS  Forever menu      L2  Back"),
          "start-up hint has no fallback glyphs");
    compositor_hint_start(1000);
    compositor_compose();
    check(compositor_hint_active() && compositor_hint_bounds(&hint),
          "start-up hint is shown when started");
    check(hint.bottom < DISPLAY_HEIGHT && hint.top > DISPLAY_HEIGHT / 2,
          "start-up hint is bottom-centred");
    check(compositor_menu_line_at((hint.left + hint.right) / 2,
                                  (hint.top + hint.bottom) / 2) == -1,
          "start-up hint does not participate in click hit testing");
    for (int y = hint.top + 2; y < hint.bottom - 2; y++)
        for (int x = hint.left + 2; x < hint.right - 2; x++)
            if (image[(size_t)y * DISPLAY_WIDTH + x] != MENU_PANEL)
                hint_text_pixels++;
    check(hint_text_pixels > 100, "start-up hint draws visible Noto text inside its panel");
    write_addon_preview("startup-hint");
    compositor_message("Downloading the Battle.net installer: 12.0 of 150.0 MB");
    compositor_compose();
    check(compositor_hint_bounds(&hint) && compositor_message_bounds(&status) &&
          (hint.right <= status.left || hint.left >= status.right ||
           hint.bottom <= status.top || hint.top >= status.bottom),
          "start-up hint and installer status panel do not overlap");
    compositor_hint_tick(8001000);
    compositor_compose();
    check(!compositor_hint_active() && !compositor_hint_bounds(&hint),
          "start-up hint is removed after eight seconds");
    compositor_message("");
    compositor_menu(text);
    compositor_compose();
    panel = menu_rect;
    check(menu_rect.right - menu_rect.left == MENU_PANEL_WIDTH,
          "menu uses the large fixed-width TV panel");
    check(compositor_menu_line_bounds(0, &title) &&
          compositor_menu_line_bounds(FOREVER_MENU_LINE_FRAME_GENERATION, &selected) &&
          compositor_menu_line_bounds(FOREVER_MENU_LINE_HDR, &hdr),
          "menu publishes title and item geometry");
    check(compositor_menu_line_at((hdr.left + hdr.right) / 2, (hdr.top + hdr.bottom) / 2) ==
          FOREVER_MENU_LINE_HDR, "mouse hit testing follows the HDR row geometry");
    check(compositor_menu_value_bounds(FOREVER_MENU_LINE_FRAME_GENERATION, &fg_value) &&
          compositor_menu_value_bounds(FOREVER_MENU_LINE_HDR, &hdr_value) &&
          compositor_menu_value_bounds(FOREVER_MENU_LINE_HUD, &hud_value) &&
          compositor_menu_value_bounds(FOREVER_MENU_LINE_ADDONS, &addons_value) &&
          fg_value.right == hdr_value.right && hdr_value.right == hud_value.right &&
          hud_value.right == addons_value.right,
          "item values share one right-aligned column");
    check(compositor_menu_line_at(menu_rect.left - 1, (hdr.top + hdr.bottom) / 2) == -1,
          "mouse outside menu selects nothing");
    check(image[(size_t)(selected.top + 2) * DISPLAY_WIDTH + selected.left + 2] ==
          MENU_SELECTION, "selected item has a full-width highlight bar");
    for (int y = title.top; y < title.bottom; y++)
        for (int x = title.left; x < title.right; x++)
        {
            uint32_t pixel = image[(size_t)y * DISPLAY_WIDTH + x];
            if (pixel != MENU_PANEL && pixel != MENU_BORDER) title_pixels++;
            if (pixel != MENU_PANEL && pixel != MENU_TEXT && pixel != MENU_BORDER) antialiased++;
        }
    check(title_pixels > 100, "pre-rasterized title text is drawn inside the panel");
    check(antialiased > 20, "glyph coverage is alpha blended");
    check(image[(size_t)panel.top * DISPLAY_WIDTH + panel.left - 1] == BACKGROUND,
          "menu drawing changes nothing outside its panel");
    write_menu_preview();
    menu.selected = FOREVER_MENU_ADDONS;
    assert(forever_menu_select(&menu).command == FOREVER_MENU_COMMAND_ADDONS_OPEN);
    for (int tab = 0; tab < FOREVER_ADDON_TAB_COUNT; tab++)
    {
        static const char *const names[] = {"installed", "updates", "find", "starter-packs"};
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.tab = (enum forever_addon_tab)tab;
        snapshot.count = 1;
        snprintf(snapshot.rows[0].key, sizeof(snapshot.rows[0].key), "%s",
                 tab == FOREVER_ADDON_STARTER_PACKS ? "starter:essentials" : "cat:fixture");
        snprintf(snapshot.rows[0].name, sizeof(snapshot.rows[0].name), "%s",
                 tab == FOREVER_ADDON_STARTER_PACKS ? "Forever Essentials" : "Fixture Add-on");
        snprintf(snapshot.rows[0].status, sizeof(snapshot.rows[0].status), "%s",
                 tab == FOREVER_ADDON_UPDATES ? "update available" : "up to date");
        snprintf(snapshot.rows[0].description, sizeof(snapshot.rows[0].description),
                 "A small fixture used to verify the add-on page layout.");
        snprintf(snapshot.rows[0].source, sizeof(snapshot.rows[0].source), "GitHub");
        snprintf(snapshot.rows[0].updated, sizeof(snapshot.rows[0].updated), "2026-10-17");
        snapshot.rows[0].installable = 1;
        snapshot.rows[0].update_available = tab == FOREVER_ADDON_UPDATES;
        menu.addon_tab = snapshot.tab;
        forever_menu_addon_snapshot(&menu, &snapshot);
        forever_menu_render(&menu, text, sizeof(text));
        compositor_menu(text);
        compositor_compose();
        check(compositor_menu_text_supported(text),
              "rendered add-on page has no fallback glyphs");
        check(!compositor_menu_value_bounds(0, &addon_value),
              "add-on title has no overlapping right-side hint");
        check(compositor_menu_footer_fits(),
              "active add-on footer fits inside the padded panel");
        check(compositor_menu_line_bounds(FOREVER_MENU_LINE_ADDON_FIRST, &addon_row) &&
              compositor_menu_value_bounds(FOREVER_MENU_LINE_ADDON_FIRST, &addon_value),
              "add-on row publishes hit and right-aligned status geometry");
        check(compositor_menu_line_at((addon_row.left + addon_row.right) / 2,
                                      (addon_row.top + addon_row.bottom) / 2) ==
              FOREVER_MENU_LINE_ADDON_FIRST,
              "mouse hit testing follows the selected add-on row");
        check(addon_value.right == addons_value.right,
              "add-on statuses use the main menu's right-aligned value column");
        write_addon_preview(names[tab]);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.tab = FOREVER_ADDON_FIND;
    snapshot.count = 1;
    snprintf(snapshot.rows[0].key, sizeof(snapshot.rows[0].key), "cat:bags");
    snprintf(snapshot.rows[0].name, sizeof(snapshot.rows[0].name), "Better Bags");
    snprintf(snapshot.rows[0].description, sizeof(snapshot.rows[0].description),
             "Organizes inventory and bank bags.");
    snapshot.rows[0].installable = 1;
    snprintf(snapshot.rows[0].source, sizeof(snapshot.rows[0].source), "GitHub");
    snprintf(snapshot.rows[0].updated, sizeof(snapshot.rows[0].updated), "2026-10-18");
    menu.addon_tab = FOREVER_ADDON_FIND;
    forever_menu_addon_snapshot(&menu, &snapshot);
    assert(forever_menu_find_set_query(&menu, "bags"));
    forever_menu_render(&menu, text, sizeof(text));
    compositor_menu(text);
    compositor_compose();
    check(compositor_menu_title_fits(), "Find title fits without panel overlap");
    check(compositor_menu_line_bounds(0, &title) &&
          compositor_menu_line_bounds(1, &query_line) &&
          query_line.bottom - query_line.top < title.bottom - title.top,
          "active Find query uses a smaller line below the title");
    check(compositor_menu_text_supported(text),
          "active-query Find page has no fallback glyphs");
    write_addon_preview("find-search");

    assert(forever_menu_find_set_query(&menu, "nothing"));
    forever_menu_render(&menu, text, sizeof(text));
    compositor_menu(text);
    compositor_compose();
    check(strstr(text, "No add-ons match \"nothing\"") != NULL,
          "empty Find result has the plain no-match message");
    check(compositor_menu_title_fits(), "no-result Find title fits without panel overlap");
    check(compositor_menu_text_supported(text),
          "no-result Find page has no fallback glyphs");
    write_addon_preview("find-no-results");
    assert(forever_menu_find_set_query(
        &menu, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"));
    forever_menu_render(&menu, text, sizeof(text));
    compositor_menu(text);
    check(compositor_menu_title_fits(), "maximum-byte Find query uses a separate fitting line");
    check(strstr(text, "\n Find: \"WWWW") != NULL,
          "long Find query is separated from the title");
    forever_menu_find_set_query(&menu, "");
    menu.wago_status = FOREVER_WAGO_NOT_CHECKED;
    menu.addon_selected = menu.addon_count;
    forever_menu_render(&menu, text, sizeof(text));
    compositor_menu(text);
    compositor_compose();
    check(compositor_menu_line_bounds(9, &addon_row) &&
          compositor_menu_value_bounds(9, &addon_value),
          "Wago key row publishes hit and status geometry");
    check(strstr(text, "Wago key\tset (not checked)") != NULL,
          "Wago row shows stored but unchecked status without key text");
    write_addon_preview("wago-key-row");
    forever_menu_select(&menu);
    forever_menu_render(&menu, text, sizeof(text));
    compositor_menu(text);
    compositor_compose();
    check(compositor_menu_text_supported(text) && compositor_menu_title_fits(),
          "Wago choice has supported text and a fitting title");
    check(strstr(text, "Type key") && strstr(text, "Read from file") &&
          strstr(text, "Remove key"), "Wago choice shows all three actions");
    write_addon_preview("wago-key-choice");
    forever_menu_notice(
        &menu,
        "Could not remove all Battle.net files (error 13). Your games were not touched.");
    forever_menu_render(&menu, text, sizeof(text));
    compositor_menu(text);
    compositor_compose();
    check(compositor_menu_line_bounds(3, &hdr) &&
          hdr.bottom - hdr.top >= 2 * MENU_ROW_HEIGHT,
          "long status notices wrap into additional measured rows");
    compositor_menu(NULL);
    compositor_compose();
    check(image[(size_t)panel.top * DISPLAY_WIDTH + panel.left] == BACKGROUND,
          "closing menu repaints its old bounds");
    compositor_reset();
}

static void test_cursor_ownership(void)
{
    struct ps5_desktop_window a = {
        .header = {PS5_DESKTOP_WINDOW, sizeof(a)}, .id = 100,
        .x = 90, .y = 90, .width = 40, .height = 40, .flags = PS5_DESKTOP_VISIBLE,
        .visible_right = 40, .visible_bottom = 40};
    struct ps5_desktop_window b = a;
    union {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message;
    struct ps5_desktop_cursor_shape *shape = &message.shape;
    compositor_reset();
    compositor_init(100, 100);
    compositor_window(&a);
    b.id = 200; b.x = 120;
    compositor_window(&b);
    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape) + 4},
        .id = a.id, .width = 1, .height = 1, .visible = 1};
    shape->pixels[0] = 0xffff0000;
    compositor_cursor_shape(shape);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "pointer owner A selects its cursor");
    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)}, .id = b.id};
    compositor_cursor_shape(shape);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "stale owner B hide cannot hide A");
    compositor_cursor(130, 100);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 130] == BACKGROUND, "enter B selects its remembered hide");
    compositor_cursor(100, 100);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "return A restores cached cursor without new message");
    *shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape) + 4},
        .id = b.id, .width = 1, .height = 1, .visible = 1};
    shape->pixels[0] = 0xff00ff00;
    compositor_cursor_shape(shape);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "stale B shape cannot replace A");
    b.x = 95; b.flags |= PS5_DESKTOP_TOPMOST;
    compositor_window(&b);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xff00ff00, "topmost visible window owns stationary pointer");
    b.visible_left = b.visible_top = 20;
    compositor_window(&b);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "surface padding cannot steal pointer ownership");
    compositor_cursor(120, 115);
    compositor_compose();
    check(image[115 * DISPLAY_WIDTH + 120] == 0xff00ff00, "visible inset selects top window");
    compositor_cursor(100, 100);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "leaving visible inset restores underlying owner");
    b.flags &= ~PS5_DESKTOP_VISIBLE;
    compositor_window(&b);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "invisible window cannot own pointer");
    *shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)}};
    compositor_cursor_shape(shape);
    compositor_compose();
    check(image[100 * DISPLAY_WIDTH + 100] == 0xffff0000, "NULL hwnd desktop hide does not hide window cursor");
    compositor_cursor(0, 0);
    compositor_compose();
    check(image[0] == BACKGROUND, "desktop state selected outside windows");
    compositor_reset();
}

int main(void)
{
    title_runtime.exit = 1;
    title_runtime.desktop_surface = pw_desktop_surface;
    test_cursor_shape();
    test_cursor_idle();
    test_menu_overlay();
    test_cursor_ownership();
    compositor_init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (int title = 0; title <= 1; title++)
        for (int fallback = 0; fallback <= 1; fallback++) run_surface(title, fallback);
    if (failures) { fprintf(stderr, "desktop surface: %d failures\n", failures); return 1; }
    puts("desktop surface/menu: PASS (owner/remote isolation, overlay order/hit testing, cursors)");
    return 0;
}
