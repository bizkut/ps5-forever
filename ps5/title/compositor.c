/* Wine's shared BGRX surfaces, composed bottom to top into the title's
 * linear image. Only damaged screen rectangles are recomposed. */
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "compositor.h"
#include "display.h"
#include "cursor-arrow.h"

#define MAX_WINDOWS 128u
#define MAX_DAMAGE 32u
#define MAX_SURFACE_BYTES (64u * 1024u * 1024u)
#define MAX_MAPPED_BYTES (256u * 1024u * 1024u)
#define CURSOR_WIDTH PS5_CURSOR_WIDTH
#define CURSOR_HEIGHT PS5_CURSOR_HEIGHT
#define BACKGROUND 0xff120a1eu  /* the picture's dark violet */
/* The launcher's desktop picture at the display's size, 0xAARRGGBB words
 * (ps5/title/build.sh decodes it from sce_sys/pic0.dds, the drawn home-screen
 * background); the plain colour above stands in when the file is missing. */
#define BACKGROUND_PATH "/app0/assets/desktop.bgra"

static uint32_t background[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static int have_background;

static void load_background(void)
{
    size_t done = 0;
    int fd = open(BACKGROUND_PATH, O_RDONLY);

    if (fd < 0) return;
    while (done < sizeof(background))
    {
        ssize_t count = read(fd, (char *)background + done, sizeof(background) - done);
        if (count <= 0) break;
        done += (size_t)count;
    }
    close(fd);
    have_background = done == sizeof(background);
}

struct window
{
    struct ps5_desktop_window state;
    const uint8_t *mapping;
    const uint8_t *pixels;
    size_t mapped_bytes;
    struct ps5_desktop_cursor_shape *cursor;
    int used;
};

static struct window windows[MAX_WINDOWS];
/* Bottom to top; all normal windows precede all topmost windows. */
static unsigned order[MAX_WINDOWS], window_count;
static size_t mapped_bytes;
static struct display_rect dirty[MAX_DAMAGE];
static unsigned dirty_count;
static int bounding_damage, cursor_x, cursor_y;
static uint32_t cursor_pixels[PS5_DESKTOP_CURSOR_SIDE * PS5_DESKTOP_CURSOR_SIDE];
static unsigned cursor_width = CURSOR_WIDTH, cursor_height = CURSOR_HEIGHT;
static int cursor_hotspot_x, cursor_hotspot_y, cursor_visible = 1, cursor_custom;
static struct ps5_desktop_cursor_shape *desktop_cursor, *cursor_selected;
static uint64_t cursor_owner = UINT64_MAX;

static struct display_rect cursor_rect(void)
{
    int left = cursor_x - cursor_hotspot_x, top = cursor_y - cursor_hotspot_y;
    return (struct display_rect){left, top, left + (int)cursor_width, top + (int)cursor_height};
}

#define FONT_WIDTH 8
#define FONT_HEIGHT 16
#define MESSAGE_PADDING 16
#define MESSAGE_COLUMNS ((DISPLAY_WIDTH - 4 * MESSAGE_PADDING) / FONT_WIDTH)
static char message[640];
static struct display_rect message_rect;

/* Font and message panel adapted from ps5-forever examples/battlenet/cpu_desktop.c
 * (commit 6cf23f2d, Copyright (C) 2026 Mihawk, MIT) */
/* An embedded 5x7 bitmap in an 8x16 cell (each row drawn twice). Printable
 * ASCII is enough for launcher_message's paths, download progress and errors.
 * Bit 4 is the leftmost pixel; no font files or runtime rasterizer are needed. */
static const uint8_t font[95][7] = {
    ['!' - 32] = {4, 4, 4, 4, 4, 0, 4},
    ['"' - 32] = {10, 10, 10, 0, 0, 0, 0},
    ['#' - 32] = {10, 10, 31, 10, 31, 10, 10},
    ['$' - 32] = {4, 15, 20, 14, 5, 30, 4},
    ['%' - 32] = {24, 25, 2, 4, 8, 19, 3},
    ['&' - 32] = {12, 18, 20, 8, 21, 18, 13},
    ['\'' - 32] = {4, 4, 8, 0, 0, 0, 0},
    ['(' - 32] = {2, 4, 8, 8, 8, 4, 2},
    [')' - 32] = {8, 4, 2, 2, 2, 4, 8},
    ['*' - 32] = {0, 21, 14, 31, 14, 21, 0},
    ['+' - 32] = {0, 4, 4, 31, 4, 4, 0},
    [',' - 32] = {0, 0, 0, 0, 4, 4, 8},
    ['-' - 32] = {0, 0, 0, 31, 0, 0, 0},
    ['.' - 32] = {0, 0, 0, 0, 0, 0, 4},
    ['/' - 32] = {1, 2, 2, 4, 8, 8, 16},
    ['0' - 32] = {14, 17, 19, 21, 25, 17, 14},
    ['1' - 32] = {4, 12, 4, 4, 4, 4, 14},
    ['2' - 32] = {14, 17, 1, 2, 4, 8, 31},
    ['3' - 32] = {30, 1, 1, 14, 1, 1, 30},
    ['4' - 32] = {2, 6, 10, 18, 31, 2, 2},
    ['5' - 32] = {31, 16, 16, 30, 1, 1, 30},
    ['6' - 32] = {14, 16, 16, 30, 17, 17, 14},
    ['7' - 32] = {31, 1, 2, 4, 8, 8, 8},
    ['8' - 32] = {14, 17, 17, 14, 17, 17, 14},
    ['9' - 32] = {14, 17, 17, 15, 1, 1, 14},
    [':' - 32] = {0, 4, 0, 0, 4, 0, 0},
    [';' - 32] = {0, 4, 0, 0, 4, 4, 8},
    ['<' - 32] = {1, 2, 4, 8, 4, 2, 1},
    ['=' - 32] = {0, 0, 31, 0, 31, 0, 0},
    ['>' - 32] = {16, 8, 4, 2, 4, 8, 16},
    ['?' - 32] = {14, 17, 1, 2, 4, 0, 4},
    ['@' - 32] = {14, 17, 23, 21, 23, 16, 14},
    ['A' - 32] = {14, 17, 17, 31, 17, 17, 17},
    ['B' - 32] = {30, 17, 17, 30, 17, 17, 30},
    ['C' - 32] = {14, 17, 16, 16, 16, 17, 14},
    ['D' - 32] = {30, 17, 17, 17, 17, 17, 30},
    ['E' - 32] = {31, 16, 16, 30, 16, 16, 31},
    ['F' - 32] = {31, 16, 16, 30, 16, 16, 16},
    ['G' - 32] = {14, 17, 16, 23, 17, 17, 15},
    ['H' - 32] = {17, 17, 17, 31, 17, 17, 17},
    ['I' - 32] = {14, 4, 4, 4, 4, 4, 14},
    ['J' - 32] = {7, 2, 2, 2, 2, 18, 12},
    ['K' - 32] = {17, 18, 20, 24, 20, 18, 17},
    ['L' - 32] = {16, 16, 16, 16, 16, 16, 31},
    ['M' - 32] = {17, 27, 21, 21, 17, 17, 17},
    ['N' - 32] = {17, 25, 21, 19, 17, 17, 17},
    ['O' - 32] = {14, 17, 17, 17, 17, 17, 14},
    ['P' - 32] = {30, 17, 17, 30, 16, 16, 16},
    ['Q' - 32] = {14, 17, 17, 17, 21, 18, 13},
    ['R' - 32] = {30, 17, 17, 30, 20, 18, 17},
    ['S' - 32] = {15, 16, 16, 14, 1, 1, 30},
    ['T' - 32] = {31, 4, 4, 4, 4, 4, 4},
    ['U' - 32] = {17, 17, 17, 17, 17, 17, 14},
    ['V' - 32] = {17, 17, 17, 17, 17, 10, 4},
    ['W' - 32] = {17, 17, 17, 21, 21, 21, 10},
    ['X' - 32] = {17, 17, 10, 4, 10, 17, 17},
    ['Y' - 32] = {17, 17, 10, 4, 4, 4, 4},
    ['Z' - 32] = {31, 1, 2, 4, 8, 16, 31},
    ['[' - 32] = {14, 8, 8, 8, 8, 8, 14},
    ['\\' - 32] = {16, 8, 8, 4, 2, 2, 1},
    [']' - 32] = {14, 2, 2, 2, 2, 2, 14},
    ['^' - 32] = {4, 10, 17, 0, 0, 0, 0},
    ['_' - 32] = {0, 0, 0, 0, 0, 0, 31},
    ['`' - 32] = {8, 4, 2, 0, 0, 0, 0},
    ['a' - 32] = {0, 0, 14, 1, 15, 17, 15},
    ['b' - 32] = {16, 16, 30, 17, 17, 17, 30},
    ['c' - 32] = {0, 0, 14, 17, 16, 17, 14},
    ['d' - 32] = {1, 1, 15, 17, 17, 17, 15},
    ['e' - 32] = {0, 0, 14, 17, 31, 16, 14},
    ['f' - 32] = {6, 8, 8, 30, 8, 8, 8},
    ['g' - 32] = {0, 15, 17, 17, 15, 1, 14},
    ['h' - 32] = {16, 16, 30, 17, 17, 17, 17},
    ['i' - 32] = {4, 0, 12, 4, 4, 4, 14},
    ['j' - 32] = {2, 0, 6, 2, 2, 18, 12},
    ['k' - 32] = {16, 16, 18, 20, 24, 20, 18},
    ['l' - 32] = {12, 4, 4, 4, 4, 4, 14},
    ['m' - 32] = {0, 0, 26, 21, 21, 21, 21},
    ['n' - 32] = {0, 0, 30, 17, 17, 17, 17},
    ['o' - 32] = {0, 0, 14, 17, 17, 17, 14},
    ['p' - 32] = {0, 0, 30, 17, 30, 16, 16},
    ['q' - 32] = {0, 0, 15, 17, 15, 1, 1},
    ['r' - 32] = {0, 0, 22, 25, 16, 16, 16},
    ['s' - 32] = {0, 0, 15, 16, 14, 1, 30},
    ['t' - 32] = {8, 8, 30, 8, 8, 9, 6},
    ['u' - 32] = {0, 0, 17, 17, 17, 19, 13},
    ['v' - 32] = {0, 0, 17, 17, 17, 10, 4},
    ['w' - 32] = {0, 0, 17, 17, 21, 21, 10},
    ['x' - 32] = {0, 0, 17, 10, 4, 10, 17},
    ['y' - 32] = {0, 0, 17, 17, 15, 1, 14},
    ['z' - 32] = {0, 0, 31, 2, 4, 8, 31},
    ['{' - 32] = {3, 4, 4, 8, 4, 4, 3},
    ['|' - 32] = {4, 4, 4, 4, 4, 4, 4},
    ['}' - 32] = {24, 4, 4, 2, 4, 4, 24},
    ['~' - 32] = {0, 0, 8, 21, 2, 0, 0},
};

static int intersect(struct display_rect *rect, const struct display_rect *clip)
{
    if (rect->left < clip->left) rect->left = clip->left;
    if (rect->top < clip->top) rect->top = clip->top;
    if (rect->right > clip->right) rect->right = clip->right;
    if (rect->bottom > clip->bottom) rect->bottom = clip->bottom;
    return rect->left < rect->right && rect->top < rect->bottom;
}

static void unite(struct display_rect *rect, const struct display_rect *other)
{
    if (rect->left > other->left) rect->left = other->left;
    if (rect->top > other->top) rect->top = other->top;
    if (rect->right < other->right) rect->right = other->right;
    if (rect->bottom < other->bottom) rect->bottom = other->bottom;
}

static void add_damage(struct display_rect rect)
{
    const struct display_rect screen = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};

    if (!intersect(&rect, &screen)) return;
    if (bounding_damage)
    {
        unite(&dirty[0], &rect);
        return;
    }
    /* Merging can make the result overlap a previously visited rectangle. */
    for (unsigned i = 0; i < dirty_count;)
    {
        if (rect.left <= dirty[i].right && rect.right >= dirty[i].left &&
            rect.top <= dirty[i].bottom && rect.bottom >= dirty[i].top)
        {
            unite(&rect, &dirty[i]);
            dirty[i] = dirty[--dirty_count];
            i = 0;
        }
        else
            i++;
    }
    if (dirty_count == MAX_DAMAGE)
    {
        for (unsigned i = 0; i < dirty_count; i++) unite(&rect, &dirty[i]);
        dirty_count = 0;
        bounding_damage = 1;
    }
    dirty[dirty_count++] = rect;
}

void compositor_message(const char *text)
{
    unsigned columns = 0, longest = 0, lines = 1;

    if (!text) text = "";
    if (!strncmp(message, text, sizeof(message) - 1)) return;
    add_damage(message_rect);
    strncpy(message, text, sizeof(message) - 1);
    message[sizeof(message) - 1] = 0;
    if (!message[0])
    {
        message_rect = (struct display_rect){0};
        return;
    }
    for (const char *p = message; *p; p++)
    {
        if (*p == '\n')
        {
            if (columns > longest) longest = columns;
            columns = 0;
            lines++;
        }
        else
        {
            if (columns == MESSAGE_COLUMNS)
            {
                if (columns > longest) longest = columns;
                columns = 0;
                lines++;
            }
            columns++;
        }
    }
    if (columns > longest) longest = columns;
    int width = (int)longest * FONT_WIDTH + 2 * MESSAGE_PADDING;
    int height = (int)lines * FONT_HEIGHT + 2 * MESSAGE_PADDING;
    int left = (DISPLAY_WIDTH - width) / 2, top = (DISPLAY_HEIGHT - height) / 2;
    message_rect = (struct display_rect){left, top, left + width, top + height};
    add_damage(message_rect);
}

static int find_window(uint64_t id)
{
    for (unsigned i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].state.id == id) return (int)i;
    return -1;
}

/* Clip in surface coordinates first, then add the origin in 64 bits: window
 * origins may be negative, and protocol dimensions need not fit an int. */
static int visible_rect(const struct window *window, const struct ps5_desktop_damage *damage,
                        struct display_rect *rect)
{
    const struct ps5_desktop_window *state = &window->state;
    int64_t left = state->visible_left, top = state->visible_top;
    int64_t right = state->visible_right, bottom = state->visible_bottom;

    if (!(state->flags & PS5_DESKTOP_VISIBLE)) return 0;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > state->width) right = state->width;
    if (bottom > state->height) bottom = state->height;
    if (damage)
    {
        if (left < damage->left) left = damage->left;
        if (top < damage->top) top = damage->top;
        if (right > damage->right) right = damage->right;
        if (bottom > damage->bottom) bottom = damage->bottom;
    }
    if (left >= right || top >= bottom) return 0;
    left += state->x;
    right += state->x;
    top += state->y;
    bottom += state->y;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > DISPLAY_WIDTH) right = DISPLAY_WIDTH;
    if (bottom > DISPLAY_HEIGHT) bottom = DISPLAY_HEIGHT;
    if (left >= right || top >= bottom) return 0;
    *rect = (struct display_rect){(int)left, (int)top, (int)right, (int)bottom};
    return 1;
}

static void damage_window(const struct window *window)
{
    struct display_rect rect;
    if (visible_rect(window, NULL, &rect)) add_damage(rect);
}

static void unmap_window(struct window *window)
{
    if (!window->pixels) return;
    munmap((void *)window->mapping, window->mapped_bytes);
    mapped_bytes -= window->mapped_bytes;
    window->pixels = NULL;
    window->mapping = NULL;
    window->mapped_bytes = 0;
}

static size_t surface_bytes(const struct ps5_desktop_window *state)
{
    uint64_t bytes = (uint64_t)state->stride * state->height;

    if (!state->shm[0] || !state->width || !state->height ||
        (state->stride & 3) || state->stride < (uint64_t)state->width * 4 ||
        bytes > MAX_SURFACE_BYTES) return 0;
    return 2 * (size_t)bytes;
}

static void map_window(struct window *window, size_t bytes)
{
    struct stat status;
    int fd;
    void *pixels;

    if (!bytes || bytes > MAX_MAPPED_BYTES - mapped_bytes) return;
    fd = shm_open(window->state.shm, O_RDONLY, 0);
    if (fd < 0) return;
    if (fstat(fd, &status) < 0 || status.st_size < 0 || (uint64_t)status.st_size < bytes)
    {
        close(fd);
        return;
    }
    pixels = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (pixels == MAP_FAILED) return;
    window->mapping = pixels;
    /* The first stride*height bytes are the live draw plane. Only flushes
     * update the second plane; window moves and cursor redraws read it too. */
    window->pixels = (const uint8_t *)pixels + bytes / 2;
    window->mapped_bytes = bytes;
    mapped_bytes += bytes;
}

static void remove_order(unsigned slot)
{
    for (unsigned i = 0; i < window_count; i++)
    {
        if (order[i] != slot) continue;
        memmove(order + i, order + i + 1, (window_count - i - 1) * sizeof(order[0]));
        window_count--;
        return;
    }
}

static void insert_order(unsigned slot)
{
    const struct ps5_desktop_window *state = &windows[slot].state;
    unsigned normal = 0, low, high, position;

    while (normal < window_count && !(windows[order[normal]].state.flags & PS5_DESKTOP_TOPMOST))
        normal++;
    low = state->flags & PS5_DESKTOP_TOPMOST ? normal : 0;
    high = state->flags & PS5_DESKTOP_TOPMOST ? window_count : normal;
    position = high;
    if (state->flags & PS5_DESKTOP_ZORDER)
    {
        if (state->insert_after == PS5_DESKTOP_Z_BOTTOM)
            position = low;
        else if (state->insert_after != PS5_DESKTOP_Z_TOP)
        {
            for (unsigned i = 0; i < window_count; i++)
                if (windows[order[i]].state.id == state->insert_after)
                {
                    position = i; /* Directly below the named window. */
                    break;
                }
        }
    }
    /* An insertion in the other band is limited to this band's boundary. */
    if (position < low) position = low;
    if (position > high) position = high;
    memmove(order + position + 1, order + position, (window_count - position) * sizeof(order[0]));
    order[position] = slot;
    window_count++;
}

/* Pointer ownership follows the compositor's actual top-to-bottom visible
 * window order, not the process which last happened to publish a cursor. */
static void select_cursor(int force)
{
    struct ps5_desktop_cursor_shape *shape = desktop_cursor;
    uint64_t owner = 0;
    for (unsigned i = window_count; i; i--)
    {
        const struct window *window = &windows[order[i - 1]];
        const struct ps5_desktop_window *state = &window->state;
        struct display_rect visible;
        if (!visible_rect(window, NULL, &visible) || cursor_x < visible.left || cursor_y < visible.top ||
            cursor_x >= visible.right || cursor_y >= visible.bottom) continue;
        owner = state->id;
        shape = window->cursor;
        break;
    }
    if (!force && owner == cursor_owner && shape == cursor_selected) return;
    add_damage(cursor_rect());
    cursor_owner = owner;
    cursor_selected = shape;
    cursor_custom = shape != NULL;
    cursor_width = shape ? shape->width : CURSOR_WIDTH;
    cursor_height = shape ? shape->height : CURSOR_HEIGHT;
    cursor_hotspot_x = shape ? shape->hotspot_x : 0;
    cursor_hotspot_y = shape ? shape->hotspot_y : 0;
    cursor_visible = shape ? shape->visible : 1;
    if (shape) memcpy(cursor_pixels, shape->pixels, cursor_width * cursor_height * sizeof(uint32_t));
    add_damage(cursor_rect());
}

void compositor_init(int x, int y)
{
    cursor_x = x;
    cursor_y = y;
    cursor_width = CURSOR_WIDTH;
    cursor_height = CURSOR_HEIGHT;
    cursor_hotspot_x = cursor_hotspot_y = cursor_custom = 0;
    cursor_visible = 1;
    cursor_selected = NULL;
    cursor_owner = UINT64_MAX;
    select_cursor(0);
    load_background();
    add_damage((struct display_rect){0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT});
}

void compositor_reset(void)
{
    for (unsigned i = 0; i < MAX_WINDOWS; i++)
    {
        unmap_window(&windows[i]);
        free(windows[i].cursor);
        windows[i].cursor = NULL;
        windows[i].used = 0;
    }
    window_count = 0;
    free(desktop_cursor);
    desktop_cursor = cursor_selected = NULL;
    cursor_owner = UINT64_MAX;
    cursor_width = CURSOR_WIDTH;
    cursor_height = CURSOR_HEIGHT;
    cursor_hotspot_x = cursor_hotspot_y = cursor_custom = 0;
    cursor_visible = 1;
    add_damage((struct display_rect){0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT});
}

void compositor_window(const struct ps5_desktop_window *state)
{
    int slot = find_window(state->id), reorder;
    size_t bytes;
    struct window *window;

    if (!memchr(state->shm, 0, sizeof(state->shm))) return;
    if (slot < 0)
    {
        for (unsigned i = 0; i < MAX_WINDOWS; i++)
            if (!windows[i].used)
            {
                slot = (int)i;
                break;
            }
        if (slot < 0) return;
        window = &windows[slot];
        *window = (struct window){0};
        window->used = 1;
        reorder = 1;
    }
    else
    {
        window = &windows[slot];
        damage_window(window);
        reorder = (state->flags & PS5_DESKTOP_ZORDER) ||
                  ((state->flags ^ window->state.flags) & PS5_DESKTOP_TOPMOST);
        if (reorder) remove_order((unsigned)slot);
    }
    bytes = surface_bytes(state);
    if (strcmp(window->state.shm, state->shm) || window->mapped_bytes != bytes)
        unmap_window(window);
    window->state = *state;
    /* A failed open/map is retried on the next WINDOW, not every frame. */
    if (!window->pixels) map_window(window, bytes);
    if (reorder) insert_order((unsigned)slot);
    damage_window(window);
    select_cursor(0);
}

void compositor_destroy(uint64_t id)
{
    int slot = find_window(id);
    if (slot < 0) return;
    damage_window(&windows[slot]);
    unmap_window(&windows[slot]);
    free(windows[slot].cursor);
    windows[slot].cursor = NULL;
    remove_order((unsigned)slot);
    windows[slot].used = 0;
    select_cursor(0);
}

void compositor_damage(const struct ps5_desktop_damage *damage)
{
    struct display_rect rect;
    int slot = find_window(damage->id);

    if (slot >= 0 && windows[slot].pixels && visible_rect(&windows[slot], damage, &rect))
        add_damage(rect);
}

void compositor_cursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= DISPLAY_WIDTH) x = DISPLAY_WIDTH - 1;
    if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;
    if (x == cursor_x && y == cursor_y) return;
    add_damage(cursor_rect());
    cursor_x = x;
    cursor_y = y;
    select_cursor(0);
    add_damage(cursor_rect());
}

void compositor_cursor_shape(const void *message)
{
    struct ps5_desktop_cursor_shape state, **saved;
    int slot, force;
    memcpy(&state, message, sizeof(state));
    if (!ps5_desktop_cursor_shape_valid(message, state.header.size)) return;
    if (state.id)
    {
        if ((slot = find_window(state.id)) < 0) return;
        saved = &windows[slot].cursor;
    }
    else saved = &desktop_cursor;
    force = *saved && *saved == cursor_selected;
    if (*saved && (*saved)->header.size == state.header.size &&
        !memcmp(*saved, message, state.header.size)) return;
    if (!*saved && !(*saved = malloc(PS5_DESKTOP_MAX_MESSAGE))) return;
    memcpy(*saved, message, state.header.size);
    select_cursor(force);
}

static uint32_t blend(uint32_t source, uint32_t destination, unsigned alpha)
{
    unsigned inverse = 255 - alpha;
    unsigned red = (((source >> 16) & 255) * alpha + ((destination >> 16) & 255) * inverse + 127) / 255;
    unsigned green = (((source >> 8) & 255) * alpha + ((destination >> 8) & 255) * inverse + 127) / 255;
    unsigned blue = ((source & 255) * alpha + (destination & 255) * inverse + 127) / 255;
    return 0xff000000u | red << 16 | green << 8 | blue;
}

/* A pixel of an UpdateLayeredWindow surface over the desktop: its colour is
 * already multiplied by its own alpha, and the window's constant alpha scales
 * both. Without this the transparent edge of a window with a drop shadow (the
 * Battle.net installer) was drawn solid black. */
static uint32_t blend_premultiplied(uint32_t source, uint32_t destination, unsigned alpha)
{
    unsigned source_alpha = ((source >> 24) * alpha + 127) / 255;
    unsigned inverse = 255 - source_alpha;
    unsigned red = (((source >> 16) & 255) * alpha + ((destination >> 16) & 255) * inverse + 127) / 255;
    unsigned green = (((source >> 8) & 255) * alpha + ((destination >> 8) & 255) * inverse + 127) / 255;
    unsigned blue = ((source & 255) * alpha + (destination & 255) * inverse + 127) / 255;
    return 0xff000000u | (red > 255 ? 255 : red) << 16 | (green > 255 ? 255 : green) << 8 | (blue > 255 ? 255 : blue);
}

static void draw_window(const struct window *window, const struct display_rect *damage, uint32_t *image)
{
    const struct ps5_desktop_window *state = &window->state;
    struct display_rect rect;
    unsigned alpha = 255;
    uint32_t key = ~0u;

    if (!window->pixels || !visible_rect(window, NULL, &rect) || !intersect(&rect, damage)) return;
    if (state->flags & PS5_DESKTOP_LAYERED)
    {
        alpha = state->alpha > 255 ? 255 : state->alpha;
        key = state->color_key;
    }
    if (!alpha) return;
    if (state->flags & PS5_DESKTOP_PREMULTIPLIED)
    {
        size_t px = (size_t)((int64_t)rect.left - state->x), py = (size_t)((int64_t)rect.top - state->y);
        const uint8_t *line = window->pixels + py * state->stride;
        for (int y = rect.top; y < rect.bottom; y++, line += state->stride)
        {
            const uint32_t *source = (const uint32_t *)line + px;
            uint32_t *destination = image + (size_t)y * DISPLAY_WIDTH + rect.left;
            for (int x = 0; x < rect.right - rect.left; x++)
                destination[x] = blend_premultiplied(source[x], destination[x], alpha);
        }
        return;
    }
    size_t source_x = (size_t)((int64_t)rect.left - state->x);
    size_t source_y = (size_t)((int64_t)rect.top - state->y);
    const uint8_t *row = window->pixels + source_y * state->stride;
    for (int y = rect.top; y < rect.bottom; y++, row += state->stride)
    {
        const uint32_t *source = (const uint32_t *)row + source_x;
        uint32_t *destination = image + (size_t)y * DISPLAY_WIDTH + rect.left;
        int count = rect.right - rect.left;
        if (alpha == 255 && key == ~0u)
        {
            for (int x = 0; x < count; x++) destination[x] = source[x] | 0xff000000u;
        }
        else if (alpha == 255)
        {
            for (int x = 0; x < count; x++)
                if ((source[x] & 0x00ffffffu) != key) destination[x] = source[x] | 0xff000000u;
        }
        else if (key == ~0u)
        {
            for (int x = 0; x < count; x++) destination[x] = blend(source[x], destination[x], alpha);
        }
        else
        {
            for (int x = 0; x < count; x++)
                if ((source[x] & 0x00ffffffu) != key) destination[x] = blend(source[x], destination[x], alpha);
        }
    }
}

static void draw_cursor(const struct display_rect *damage, uint32_t *image)
{
    struct display_rect rect = cursor_rect();
    int left = rect.left, top = rect.top;

    if (!cursor_visible || !intersect(&rect, damage)) return;
    for (int y = rect.top; y < rect.bottom; y++)
        for (int x = rect.left; x < rect.right; x++)
        {
            uint32_t pixel;
            if (cursor_custom)
                pixel = cursor_pixels[(y - top) * cursor_width + x - left];
            else
            {
                char value = ps5_cursor_arrow[y - top][x - left];
                pixel = value == '#' ? 0xff000000u : value == '.' ? 0xffffffffu : 0;
            }
            if (pixel >> 24)
            {
                uint32_t *destination = &image[(size_t)y * DISPLAY_WIDTH + x];
                *destination = blend(pixel, *destination, pixel >> 24);
            }
        }
}

static void draw_character(unsigned char character, int x, int y, const struct display_rect *damage, uint32_t *image)
{
    struct display_rect rect = {x + 1, y + 1, x + 6, y + 15};

    if (character < 32 || character > 126) character = '?';
    if (!intersect(&rect, damage)) return;
    for (int py = rect.top; py < rect.bottom; py++)
    {
        uint8_t bits = font[character - 32][(py - y - 1) / 2];
        for (int px = rect.left; px < rect.right; px++)
            if (bits & (1u << (5 - (px - x)))) image[(size_t)py * DISPLAY_WIDTH + px] = 0xffeeeeeeu;
    }
}

static void draw_message(const struct display_rect *damage, uint32_t *image)
{
    struct display_rect rect = message_rect;
    unsigned column = 0, line = 0;

    if (!message[0] || !intersect(&rect, damage)) return;
    for (int y = rect.top; y < rect.bottom; y++)
        for (int x = rect.left; x < rect.right; x++)
        {
            int border = x == message_rect.left || x == message_rect.right - 1 ||
                         y == message_rect.top || y == message_rect.bottom - 1;
            image[(size_t)y * DISPLAY_WIDTH + x] = border ? 0xff666a74u : 0xff12151cu;
        }
    for (const unsigned char *p = (const unsigned char *)message; *p; p++)
    {
        if (*p == '\n')
        {
            column = 0;
            line++;
            continue;
        }
        if (column == MESSAGE_COLUMNS)
        {
            column = 0;
            line++;
        }
        draw_character(*p, message_rect.left + MESSAGE_PADDING + (int)column * FONT_WIDTH,
                       message_rect.top + MESSAGE_PADDING + (int)line * FONT_HEIGHT, &rect, image);
        column++;
    }
}

void compositor_compose(void)
{
    uint32_t *image = display_image();

    for (unsigned i = 0; i < dirty_count; i++)
    {
        const struct display_rect *rect = &dirty[i];
        for (int y = rect->top; y < rect->bottom; y++)
        {
            uint32_t *row = image + (size_t)y * DISPLAY_WIDTH;
            if (have_background)
                memcpy(row + rect->left, background + (size_t)y * DISPLAY_WIDTH + rect->left,
                       (size_t)(rect->right - rect->left) * sizeof(*row));
            else
                for (int x = rect->left; x < rect->right; x++) row[x] = BACKGROUND;
        }
        for (unsigned j = 0; j < window_count; j++) draw_window(&windows[order[j]], rect, image);
        draw_message(rect, image);
        draw_cursor(rect, image);
        display_damage(rect);
    }
    dirty_count = 0;
    bounding_damage = 0;
}
