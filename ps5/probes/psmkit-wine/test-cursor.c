/* Host contract for the real driver callback, GDI reads and embedded receiver. */
#define __WINE_PS5DRV_H
#define __PROSPERO__
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wine/ps5_desktop.h>
#include <wine/ps5_title.h>

typedef int BOOL;
typedef unsigned int UINT, DWORD;
typedef uintptr_t UINT_PTR;
typedef void *HWND, *HCURSOR, *HBITMAP, *HDC;
typedef struct { int bmWidth, bmHeight; } BITMAP;
typedef struct { BOOL fIcon; DWORD xHotspot, yHotspot; HBITMAP hbmMask, hbmColor; } ICONINFO;
typedef struct {
    uint32_t biSize; int biWidth, biHeight; uint16_t biPlanes, biBitCount;
    uint32_t biCompression, biSizeImage; int biXPelsPerMeter, biYPelsPerMeter;
    uint32_t biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
#define FALSE 0
#define TRUE 1
typedef struct { uint8_t rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[2]; } BITMAPINFO;
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define WARN(...) ((void)0)
#define TRACE(...) ((void)0)
#define GA_ROOT 2
static HWND NtUserGetAncestor(HWND hwnd, unsigned kind)
{ assert(kind == GA_ROOT); return hwnd == (void *)101 ? (void *)100 : hwnd; }
static HWND NtUserGetDesktopWindow(void) { return (void *)0xde5; }
_Alignas(8) static uint32_t captured[5000], color[4096];
static unsigned char mask[1024];
static unsigned sent, native_sent, icon_reads, deleted;
static int width = 4, height = 1, monochrome, available = 1, fail_send, native_accept = 1;
static int display_available = 1;
static DWORD hotspot_x = 1, hotspot_y;
static BOOL NtUserGetIconInfo(HCURSOR cursor, ICONINFO *info, void *a, void *b, void *c, int frame)
{
    (void)a; (void)b; (void)c;
    assert(cursor && !frame);
    icon_reads++;
    *info = (ICONINFO){0, hotspot_x, hotspot_y, (void *)1, monochrome ? NULL : (void *)2};
    return TRUE;
}
static int NtGdiExtGetObjectW(HBITMAP bitmap, unsigned size, BITMAP *out)
{
    assert(size == sizeof(*out));
    *out = (BITMAP){width, height * (bitmap == (void *)1 && monochrome ? 2 : 1)};
    return size;
}
static HDC NtGdiCreateCompatibleDC(HDC dc) { assert(!dc); return (void *)3; }
static int NtGdiGetDIBitsInternal(HDC dc, HBITMAP bitmap, unsigned start, unsigned rows,
                                 void *bits, BITMAPINFO *info, unsigned usage, unsigned max_bits, unsigned max_info)
{
    assert(dc == (void *)3 && !start && !usage && !max_bits && !max_info);
    assert(info->bmiHeader.biWidth == width && info->bmiHeader.biHeight == -(int)rows);
    if (bitmap == (void *)2)
    {
        assert(info->bmiHeader.biBitCount == 32 && rows == (unsigned)height);
        memcpy(bits, color, width * height * 4);
    }
    else
    {
        assert(bitmap == (void *)1 && info->bmiHeader.biBitCount == 1);
        assert(rows == (unsigned)(height * (monochrome ? 2 : 1)));
        assert(info->bmiColors[1].rgbRed == 255 && info->bmiColors[1].rgbGreen == 255 &&
               info->bmiColors[1].rgbBlue == 255);
        memcpy(bits, mask, ((width + 31) / 32 * 4) * rows);
    }
    return rows;
}
static BOOL NtGdiDeleteObjectApp(void *object) { assert(object); deleted++; return TRUE; }
static BOOL ps5_send_cursor_message(const void *message, unsigned size)
{
    if (fail_send) return FALSE;
    memcpy(captured, message, size);
    sent++;
    return TRUE;
}
static int native_cursor(const void *message, unsigned size)
{
    if (!native_accept) return 0;
    memcpy(captured, message, size);
    native_sent++;
    return 1;
}
void *__wine_ps5_get_vulkan_proc(const char *name)
{
    if (!strcmp(name, "wineSetCursor")) return available ? native_cursor : NULL;
    assert(!strcmp(name, "wineGetDisplayModes"));
    return display_available ? native_cursor : NULL;
}
#define WINE_DEFAULT_DEBUG_CHANNEL(name)
#include "cursor.c"

/* The installed table comes from the real win32u headless-title loader. */
struct user_driver_funcs {
    void (*pSetCursor)(HWND, HCURSOR);
    void (*pDestroyCursorIcon)(HCURSOR);
    int (*pProcessEvents)(unsigned);
    int (*pCreateWindow)(HWND);
};
static void nulldrv_SetCursor(HWND, HCURSOR);
static void nulldrv_DestroyCursorIcon(HCURSOR);
static int null_events(unsigned mask) { (void)mask; return 0; }
static int null_window(HWND hwnd) { (void)hwnd; return 1; }
static struct user_driver_funcs null_user_driver = {
    .pSetCursor = nulldrv_SetCursor, .pDestroyCursorIcon = nulldrv_DestroyCursorIcon,
    .pProcessEvents = null_events, .pCreateWindow = null_window};
static struct user_driver_funcs ps5_title_gpu_driver, installed_driver;
static struct wine_ps5_cursor_cache ps5_title_cursor_cache = WINE_PS5_CURSOR_CACHE_INIT;
static wine_ps5_cursor_send ps5_title_cursor_send;
static pthread_once_t ps5_title_gpu_once = PTHREAD_ONCE_INIT;
static unsigned display_cache_calls;
static char driver_log[192];
#define WINE_GDI_DRIVER_VERSION 1
#define STATUS_UNSUCCESSFUL 1
#define ERR(...) ((void)0)
static void __wine_set_user_driver(const struct user_driver_funcs *driver, unsigned version)
{ assert(version == WINE_GDI_DRIVER_VERSION); installed_driver = *driver; }
static BOOL update_display_cache(BOOL force) { assert(force); display_cache_calls++; return TRUE; }
static void *GetCurrentProcess(void) { return (void *)-1; }
static void NtTerminateProcess(void *process, unsigned status) { (void)process; (void)status; abort(); }
static void sceKernelDebugOutText(int level, const char *text)
{ assert(!level && strlen(text) < sizeof(driver_log)); strcpy(driver_log, text); }
#include "cursor-title-driver.inc"

#define PS5_TITLE_EMBEDDED
#include "../../title/hub.c"
static int delivered, disconnected, incoming_size;
static unsigned char incoming[20000];
int pw_desktop_title_connect(void) { return 1; }
int pw_desktop_title_send(const void *message, size_t size) { (void)message; (void)size; return 1; }
int pw_desktop_title_receive(void *message, size_t capacity)
{
    int size = incoming_size;
    assert(size <= (int)capacity);
    memcpy(message, incoming, size);
    incoming_size = 0;
    return size;
}
static void receive_message(const void *message, void *context)
{ (void)message; (void)context; delivered++; }
static void disconnect_message(void *context) { (void)context; disconnected++; }
static void deliver(const void *message, unsigned size)
{
    memcpy(incoming, message, size);
    incoming_size = size;
    hub_poll(0);
}
int main(int argc, char **argv)
{
    struct ps5_desktop_cursor_shape *shape = (void *)captured;
    unsigned before;
    if (argc == 2 && !strcmp(argv[1], "--cursor-no-modes"))
    {
        display_available = 0; color[0] = 0x80402010;
        assert(ps5_load_headless_driver());
        installed_driver.pSetCursor((void *)100, (void *)1);
        assert(native_sent == 1 && icon_reads == 1 && shape->pixels[0] == 0x80402010);
        assert(!display_cache_calls && strstr(driver_log, "driver=title-gpu"));
        installed_driver.pSetCursor((void *)100, NULL);
        assert(native_sent == 2 && !shape->visible);
        installed_driver.pDestroyCursorIcon((void *)1);
        puts("win32u cursor proc without display modes: PASS (native cursor driver selected)");
        return 0;
    }
    if (argc > 1)
    {
        assert(argc == 2 && (!strcmp(argv[1], "--no-proc") || !strcmp(argv[1], "--no-cursor-proc")));
        available = 0;
        display_available = strcmp(argv[1], "--no-proc") != 0;
        assert(ps5_load_headless_driver());
        assert(installed_driver.pSetCursor == null_user_driver.pSetCursor);
        assert(installed_driver.pDestroyCursorIcon == null_user_driver.pDestroyCursorIcon);
        installed_driver.pSetCursor((void *)100, (void *)1);
        installed_driver.pSetCursor((void *)100, NULL);
        installed_driver.pDestroyCursorIcon((void *)1);
        assert(!icon_reads && !native_sent && display_cache_calls == (unsigned)display_available);
        assert(installed_driver.pProcessEvents == null_user_driver.pProcessEvents);
        puts("win32u no cursor proc: PASS (plain null cursor callbacks, no GDI conversion)");
        return 0;
    }
    color[0] = 0x80402010; color[1] = 0xff123456; color[2] = 0x00334455; color[3] = 0xffabcdef;
    PS5_SetCursor((void *)101, (void *)1);
    assert(sent == 1 && !native_sent && icon_reads == 1 && deleted == 3);
    assert(shape->header.type == PS5_DESKTOP_CURSOR_SHAPE && shape->visible);
    assert(shape->width == 4 && shape->height == 1 && shape->hotspot_x == 1);
    assert(shape->id == 100); /* child HWND is normalized to its top-level ancestor */
    assert(shape->pixels[0] == color[0] && shape->pixels[1] == color[1]);
    assert(!shape->pixels[2] && shape->pixels[3] == color[3]);
    PS5_SetCursor((void *)200, (void *)1);
    assert(sent == 2 && icon_reads == 1); /* always republish; cache conversion only */
    assert(shape->id == 200); /* same pixels, different process/window ownership */
    PS5_SetCursor(NULL, NULL);
    assert(sent == 3 && !shape->visible && shape->header.size == sizeof(*shape) && !shape->id);
    PS5_SetCursor(NULL, NULL);
    assert(sent == 4);
    /* Legacy colour bitmap: AND mask supplies alpha only when all alpha is zero. */
    color[0] = 0x00112233; color[1] = 0x00445566; color[2] = 0; color[3] = 0xffffff;
    mask[0] = 0x40;
    PS5_SetCursor(NULL, (void *)2);
    assert(shape->pixels[0] == 0xff112233 && !shape->pixels[1]);
    assert(shape->pixels[2] == 0xff000000 && shape->pixels[3] == 0xffffffff);
    before = icon_reads;
    unsigned published = sent;
    PS5_SetCursor((void *)100, (void *)1);
    assert(sent == published + 1 && icon_reads == before && shape->pixels[0] == 0x80402010);
    /* AND/XOR: black, white, transparent, invert->black. */
    monochrome = 1; mask[0] = 0x30; mask[4] = 0x50;
    assert(ps5_load_headless_driver());
    assert(installed_driver.pSetCursor != null_user_driver.pSetCursor);
    assert(installed_driver.pDestroyCursorIcon != null_user_driver.pDestroyCursorIcon);
    assert(installed_driver.pProcessEvents == null_user_driver.pProcessEvents);
    assert(installed_driver.pCreateWindow == null_user_driver.pCreateWindow);
    assert(display_cache_calls == 1 && strstr(driver_log, "driver=title-gpu"));
    before = sent;
    installed_driver.pSetCursor((void *)101, (void *)3);
    assert(native_sent == 1 && sent == before);
    assert(!shape->id && shape->hotspot_x == 1 && !shape->hotspot_y);
    assert(shape->pixels[0] == 0xff000000 && shape->pixels[1] == 0xffffffff);
    assert(!shape->pixels[2] && shape->pixels[3] == 0xff000000);
    installed_driver.pSetCursor((void *)101, NULL);
    assert(native_sent == 2 && !shape->visible);
    /* Unsupported native publication retries, but does not repeat conversion. */
    native_accept = 0; before = sent;
    installed_driver.pSetCursor((void *)101, (void *)4);
    assert(sent == before && native_sent == 2);
    native_accept = 1;
    before = icon_reads;
    installed_driver.pSetCursor((void *)101, (void *)4);
    assert(native_sent == 3 && icon_reads == before);
    width = 65;
    installed_driver.pSetCursor((void *)101, (void *)5);
    assert(native_sent == 3);
    width = 64; height = 64; hotspot_x = hotspot_y = 63; monochrome = 0;
    memset(color, 0xff, sizeof(color));
    installed_driver.pSetCursor((void *)101, (void *)6);
    assert(shape->header.size == PS5_DESKTOP_MAX_MESSAGE && shape->hotspot_y == 63);
    assert(ps5_desktop_cursor_shape_valid(shape, shape->header.size));
    assert(!ps5_desktop_cursor_shape_valid(shape, shape->header.size - 4));
    hub_init(receive_message, disconnect_message, NULL);
    deliver(shape, shape->header.size);
    assert(delivered == 1 && !disconnected);
    shape->width = 65;
    deliver(shape, shape->header.size);
    assert(delivered == 1 && disconnected == 1);
    shape->width = 64;
    shape->hotspot_x = -1;
    assert(!ps5_desktop_cursor_shape_valid(shape, shape->header.size));
    shape->hotspot_x = 64;
    assert(!ps5_desktop_cursor_shape_valid(shape, shape->header.size));
    shape->hotspot_x = 63;
    shape->header.size -= 4;
    deliver(shape, shape->header.size);
    assert(delivered == 1 && disconnected == 2);
    struct ps5_desktop_header unknown = {0xf000, sizeof(unknown)};
    deliver(&unknown, sizeof(unknown));
    assert(delivered == 1 && disconnected == 2); /* unknown bounded frames are skipped */
    shape->header = (struct ps5_desktop_header){PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)};
    shape->visible = 0; shape->width = shape->height = 0; shape->hotspot_x = shape->hotspot_y = 0;
    deliver(shape, sizeof(*shape));
    assert(delivered == 2 && disconnected == 2);
    shape->width = 1;
    assert(!ps5_desktop_cursor_shape_valid(shape, sizeof(*shape)));
    before = icon_reads;
    installed_driver.pDestroyCursorIcon((void *)6);
    color[0] = 0xff13579b;
    installed_driver.pSetCursor((void *)101, (void *)6);
    assert(icon_reads == before + 1 && shape->pixels[0] == 0xff13579b);
    for (unsigned i = 0; i < 128; i++)
    {
        HCURSOR handle = (HCURSOR)(uintptr_t)(1000 + i);
        installed_driver.pSetCursor((void *)101, handle);
        installed_driver.pDestroyCursorIcon(handle);
    }
    unsigned cached = 0;
    for (struct wine_ps5_cursor_entry *entry = ps5_title_cursor_cache.entries; entry; entry = entry->next) cached++;
    assert(cached < 10); /* destroyed handles do not accumulate */
    width = 4; height = 1; hotspot_x = 1; hotspot_y = 0;
    color[0] = 0x80402010; color[1] = 0xff123456; color[2] = 0x00334455; color[3] = 0xffabcdef;
    installed_driver.pSetCursor((void *)101, (void *)9000);
    assert(shape->width == 4 && shape->height == 1 && shape->hotspot_x == 1 && !shape->hotspot_y);
    assert(shape->pixels[0] == 0x80402010 && shape->pixels[1] == 0xff123456 &&
           !shape->pixels[2] && shape->pixels[3] == 0xffabcdef);
    installed_driver.pDestroyCursorIcon((void *)9000);
    puts("desktop + win32u title-GPU cursor tables: PASS (shared conversion/cache, native pixels/hotspot/hide/destroy)");
    return 0;
}
