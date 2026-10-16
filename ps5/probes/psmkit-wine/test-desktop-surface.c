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

int main(void)
{
    title_runtime.exit = 1;
    title_runtime.desktop_surface = pw_desktop_surface;
    compositor_init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (int title = 0; title <= 1; title++)
        for (int fallback = 0; fallback <= 1; fallback++) run_surface(title, fallback);
    if (failures) { fprintf(stderr, "desktop surface: %d failures\n", failures); return 1; }
    puts("desktop surface: PASS (owner/remote flush isolation, clipping, fallback, title/payload mappings)");
    return 0;
}
