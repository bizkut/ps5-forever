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
#include "forever-menu.h"
#include "cursor-arrow.h"
#include "menu-font.h"
#include "bitmap-font.h"
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
static int cursor_idle, cursor_activity = 1;
static uint64_t cursor_active_us;
static struct ps5_desktop_cursor_shape *desktop_cursor, *cursor_selected;
static uint64_t cursor_owner = UINT64_MAX;
#define MENU_PANEL_WIDTH 1100
#define MENU_PANEL_PADDING 40
#define MENU_TITLE_HEIGHT 72
#define MENU_ROW_HEIGHT 52
#define MENU_SEPARATOR_HEIGHT 16
#define MENU_BLANK_HEIGHT 14
#define MENU_FOOTER_HEIGHT 58
#define MENU_MAX_LINES 40
#define MENU_PANEL 0xff12151cu
#define MENU_BORDER 0xff666a74u
#define MENU_SELECTION 0xff34405cu
#define MENU_TEXT 0xffeeeeeeu
#define MENU_MUTED 0xffa9afbau
#define MENU_ACCENT 0xff8fc7ffu
struct menu_layout_line
{
    size_t begin, length;
    struct display_rect rect;
    int separator, blank, selected;
    unsigned rows;
};
static char menu_text[FOREVER_MENU_TEXT_BYTES];
static struct display_rect menu_rect;
static struct menu_layout_line menu_lines[MENU_MAX_LINES];
static unsigned menu_line_count;
static unsigned menu_wrapped_rows(const char *text, size_t length, int bold, int max_width);
static int menu_text_width(const char *text, size_t length, int bold);

static struct display_rect cursor_rect(void)
{
    unsigned width = menu_text[0] ? CURSOR_WIDTH : cursor_width;
    unsigned height = menu_text[0] ? CURSOR_HEIGHT : cursor_height;
    int hotspot_x = menu_text[0] ? 0 : cursor_hotspot_x;
    int hotspot_y = menu_text[0] ? 0 : cursor_hotspot_y;
    int left = cursor_x - hotspot_x, top = cursor_y - hotspot_y;
    return (struct display_rect){left, top, left + (int)width, top + (int)height};
}

#define FONT_WIDTH 8
#define FONT_HEIGHT 16
#define MESSAGE_PADDING 16
#define MESSAGE_COLUMNS ((DISPLAY_WIDTH - 4 * MESSAGE_PADDING) / FONT_WIDTH)
static char message[640];
static struct display_rect message_rect;
#define HINT_TEXT "OPTIONS  Forever menu"
#define HINT_DURATION_US 10000000ull
#define HINT_PADDING_X 28
#define HINT_PADDING_Y 14
#define HINT_BOTTOM_MARGIN 48
#define HINT_MESSAGE_GAP 24
static struct display_rect hint_rect;
static uint64_t hint_deadline;
static int hint_active;

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

static int rects_overlap(const struct display_rect *left, const struct display_rect *right)
{
    return left->left < right->right && left->right > right->left &&
           left->top < right->bottom && left->bottom > right->top;
}

static void layout_hint(void)
{
    struct display_rect old = hint_rect;
    if (!hint_active)
        hint_rect = (struct display_rect){0};
    else
    {
        int width = menu_text_width(HINT_TEXT, sizeof(HINT_TEXT) - 1, 0) +
                    2 * HINT_PADDING_X;
        int height = FOREVER_FONT_REGULAR_ASCENT + FOREVER_FONT_REGULAR_DESCENT +
                     2 * HINT_PADDING_Y;
        int bottom = DISPLAY_HEIGHT - HINT_BOTTOM_MARGIN;
        hint_rect = (struct display_rect){
            (DISPLAY_WIDTH - width) / 2, bottom - height,
            (DISPLAY_WIDTH + width) / 2, bottom};
        if (message[0] && rects_overlap(&hint_rect, &message_rect))
        {
            bottom = message_rect.top - HINT_MESSAGE_GAP;
            hint_rect.top = bottom - height;
            hint_rect.bottom = bottom;
        }
    }
    if (memcmp(&old, &hint_rect, sizeof(old)))
    {
        add_damage(old);
        add_damage(hint_rect);
    }
}

void compositor_hint_start(uint64_t now_us)
{
    hint_active = 1;
    hint_deadline = now_us + HINT_DURATION_US;
    layout_hint();
}

void compositor_hint_tick(uint64_t now_us)
{
    if (hint_active && now_us >= hint_deadline) compositor_hint_stop();
}

void compositor_hint_stop(void)
{
    if (!hint_active) return;
    hint_active = 0;
    hint_deadline = 0;
    layout_hint();
}

int compositor_hint_active(void)
{
    return hint_active;
}

int compositor_hint_bounds(struct display_rect *rect)
{
    if (!rect || !hint_active) return 0;
    *rect = hint_rect;
    return 1;
}

int compositor_message_bounds(struct display_rect *rect)
{
    if (!rect || !message[0]) return 0;
    *rect = message_rect;
    return 1;
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
        layout_hint();
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
    layout_hint();
}

static int menu_line_separator(const char *text, size_t length)
{
    size_t at = 0, marks = 0;
    while (at < length && text[at] == ' ') at++;
    while (at < length && text[at] == '-') { at++; marks++; }
    while (at < length && text[at] == ' ') at++;
    return marks >= 3 && at == length;
}

static int menu_line_blank(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') return 0;
    return 1;
}

static void layout_menu(void)
{
    const char *at = menu_text;
    int content_height = 0;

    menu_line_count = 0;
    while (*at && menu_line_count < MENU_MAX_LINES)
    {
        const char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) : strlen(at);
        struct menu_layout_line *line = &menu_lines[menu_line_count];
        *line = (struct menu_layout_line){
            .begin = (size_t)(at - menu_text), .length = length,
            .separator = menu_line_separator(at, length),
            .blank = menu_line_blank(at, length),
            .selected = length && at[0] == '>',
            .rows = 1,
        };
        if (!menu_line_count) content_height += MENU_TITLE_HEIGHT;
        else if (line->separator) content_height += MENU_SEPARATOR_HEIGHT;
        else if (line->blank) content_height += MENU_BLANK_HEIGHT;
        else
        {
            const char *visible = at;
            size_t visible_length = length;
            while (visible_length && (*visible == ' ' || *visible == '>'))
            {
                visible++;
                visible_length--;
            }
            const char *tab = memchr(visible, '\t', visible_length);
            if (tab) visible_length = (size_t)(tab - visible);
            line->rows = menu_wrapped_rows(
                visible, visible_length, 0,
                MENU_PANEL_WIDTH - MENU_PANEL_PADDING - 48);
            content_height += MENU_ROW_HEIGHT * (int)line->rows;
        }
        menu_line_count++;
        if (!end || !end[1]) break;
        at = end + 1;
    }
    int height = 2 * MENU_PANEL_PADDING + content_height + MENU_FOOTER_HEIGHT;
    int left = (DISPLAY_WIDTH - MENU_PANEL_WIDTH) / 2;
    int top = (DISPLAY_HEIGHT - height) / 2;
    int y = top + MENU_PANEL_PADDING;
    menu_rect = (struct display_rect){left, top, left + MENU_PANEL_WIDTH, top + height};
    for (unsigned i = 0; i < menu_line_count; i++)
    {
        struct menu_layout_line *line = &menu_lines[i];
        int line_height = !i ? MENU_TITLE_HEIGHT :
            line->separator ? MENU_SEPARATOR_HEIGHT :
            line->blank ? MENU_BLANK_HEIGHT : MENU_ROW_HEIGHT * (int)line->rows;
        line->rect = (struct display_rect){
            menu_rect.left + MENU_PANEL_PADDING / 2, y,
            menu_rect.right - MENU_PANEL_PADDING / 2, y + line_height};
        y += line_height;
    }
}

void compositor_menu(const char *text)
{
    if (!text) text = "";
    if (!strncmp(menu_text, text, sizeof(menu_text) - 1)) return;
    add_damage(cursor_rect());
    add_damage(menu_rect);
    strncpy(menu_text, text, sizeof(menu_text) - 1);
    menu_text[sizeof(menu_text) - 1] = 0;
    if (!menu_text[0])
    {
        menu_rect = (struct display_rect){0};
        menu_line_count = 0;
        add_damage(cursor_rect());
        return;
    }
    layout_menu();
    add_damage(menu_rect);
    add_damage(cursor_rect());
}

int compositor_menu_line_at(int x, int y)
{
    if (!menu_text[0] || x < menu_rect.left || x >= menu_rect.right ||
        y < menu_rect.top || y >= menu_rect.bottom) return -1;
    for (unsigned i = 0; i < menu_line_count; i++)
        if (x >= menu_lines[i].rect.left && x < menu_lines[i].rect.right &&
            y >= menu_lines[i].rect.top && y < menu_lines[i].rect.bottom) return (int)i;
    return -1;
}

int compositor_menu_line_bounds(int line, struct display_rect *rect)
{
    if (!rect || line < 0 || (unsigned)line >= menu_line_count) return 0;
    *rect = menu_lines[line].rect;
    return 1;
}

int compositor_menu_value_bounds(int line, struct display_rect *rect)
{
    const struct menu_layout_line *layout;
    const char *text, *tab;
    size_t value_length;
    int right;
    if (!rect || line < 0 || (unsigned)line >= menu_line_count) return 0;
    layout = &menu_lines[line];
    text = menu_text + layout->begin;
    tab = memchr(text, '\t', layout->length);
    if (!tab) return 0;
    value_length = layout->length - (size_t)(tab + 1 - text);
    right = layout->rect.right - 24;
    *rect = (struct display_rect){
        right - menu_text_width(tab + 1, value_length, 0),
        layout->rect.top, right, layout->rect.top + MENU_ROW_HEIGHT};
    return 1;
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
    if (cursor_visible) compositor_cursor_wake();
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
    cursor_idle = 0;
    cursor_activity = 1;
    cursor_active_us = 0;
    cursor_selected = NULL;
    cursor_owner = UINT64_MAX;
    message[0] = menu_text[0] = 0;
    message_rect = menu_rect = hint_rect = (struct display_rect){0};
    hint_active = 0;
    hint_deadline = 0;
    menu_line_count = 0;
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
    cursor_idle = 0;
    cursor_activity = 1;
    cursor_active_us = 0;
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
    compositor_cursor_wake();
    select_cursor(0);
    add_damage(cursor_rect());
}

void compositor_cursor_wake(void)
{
    cursor_activity = 1;
}

void compositor_cursor_tick(uint64_t now_us)
{
    int idle;
    if (cursor_activity)
    {
        cursor_active_us = now_us;
        cursor_activity = 0;
    }
    idle = now_us - cursor_active_us >= CURSOR_IDLE_US;
    if (idle == cursor_idle) return;
    cursor_idle = idle;
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
    unsigned width = menu_text[0] ? CURSOR_WIDTH : cursor_width;
    unsigned height = menu_text[0] ? CURSOR_HEIGHT : cursor_height;
    int hotspot_x = menu_text[0] ? 0 : cursor_hotspot_x;
    int hotspot_y = menu_text[0] ? 0 : cursor_hotspot_y;
    int left = cursor_x - hotspot_x, top = cursor_y - hotspot_y;
    struct display_rect rect = {left, top, left + (int)width, top + (int)height};

    if (cursor_idle || !cursor_visible || !intersect(&rect, damage)) return;
    for (int y = rect.top; y < rect.bottom; y++)
        for (int x = rect.left; x < rect.right; x++)
        {
            uint32_t pixel;
            if (!menu_text[0] && cursor_custom)
                pixel = cursor_pixels[(y - top) * width + x - left];
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
        uint8_t bits = title_bitmap_font[character - 32][(py - y - 1) / 2];
        for (int px = rect.left; px < rect.right; px++)
            if (bits & (1u << (5 - (px - x)))) image[(size_t)py * DISPLAY_WIDTH + px] = 0xffeeeeeeu;
    }
}

static void draw_text_panel(const char *text, const struct display_rect *panel,
                            const struct display_rect *damage, uint32_t *image)
{
    struct display_rect rect = *panel;
    unsigned column = 0, line = 0;

    if (!text[0] || !intersect(&rect, damage)) return;
    for (int y = rect.top; y < rect.bottom; y++)
        for (int x = rect.left; x < rect.right; x++)
        {
            int border = x == panel->left || x == panel->right - 1 ||
                         y == panel->top || y == panel->bottom - 1;
            image[(size_t)y * DISPLAY_WIDTH + x] = border ? 0xff666a74u : 0xff12151cu;
        }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
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
        draw_character(*p, panel->left + MESSAGE_PADDING + (int)column * FONT_WIDTH,
                       panel->top + MESSAGE_PADDING + (int)line * FONT_HEIGHT, &rect, image);
        column++;
    }
}

static const struct forever_raster_glyph *menu_glyph(uint32_t codepoint, int bold)
{
    const struct forever_raster_glyph *glyphs =
        bold ? forever_font_bold_glyphs : forever_font_regular_glyphs;
    size_t count = bold ? sizeof(forever_font_bold_glyphs) / sizeof(forever_font_bold_glyphs[0]) :
                          sizeof(forever_font_regular_glyphs) / sizeof(forever_font_regular_glyphs[0]);
    for (size_t i = 0; i < count; i++)
        if (glyphs[i].codepoint == codepoint) return &glyphs[i];
    for (size_t i = 0; i < count; i++)
        if (glyphs[i].codepoint == '?') return &glyphs[i];
    return NULL;
}

static uint32_t menu_utf8(const unsigned char **at, const unsigned char *end)
{
    const unsigned char *p = *at;
    uint32_t value;
    if (p >= end) return 0;
    if (*p < 0x80) { *at = p + 1; return *p; }
    if ((*p & 0xe0) == 0xc0 && p + 1 < end && (p[1] & 0xc0) == 0x80)
    {
        value = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        *at = p + 2;
        return value;
    }
    if ((*p & 0xf0) == 0xe0 && p + 2 < end &&
        (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80)
    {
        value = ((uint32_t)(p[0] & 0x0f) << 12) |
                ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *at = p + 3;
        return value;
    }
    if ((*p & 0xf8) == 0xf0 && p + 3 < end &&
        (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80)
    {
        value = ((uint32_t)(p[0] & 7) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
                ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        *at = p + 4;
        return value;
    }
    *at = p + 1;
    return '?';
}

static int menu_text_width(const char *text, size_t length, int bold)
{
    const unsigned char *at = (const unsigned char *)text;
    const unsigned char *end = at + length;
    int width = 0;
    while (at < end)
    {
        const struct forever_raster_glyph *glyph = menu_glyph(menu_utf8(&at, end), bold);
        if (glyph) width += glyph->advance;
    }
    return width;
}

static const char *menu_footer_text(void)
{
    static const char main_footer[] = "\xe2\x9c\x95 Select   \xe2\x97\x8b Back   OPTIONS Close";
    static const char addon_footer[] =
        "\xe2\x9c\x95 Details   \xe2\x96\xa1 Update   \xe2\x96\xb3 All   R3 Remove   \xe2\x97\x8b Back";
    static const char find_footer[] =
        "\xe2\x9c\x95 Install   \xe2\x96\xa1 Sort   L1/R1 Channel   \xe2\x97\x8b Back";
    static const char packs_footer[] =
        "\xe2\x9c\x95 Confirm   L1/R1 Channel   \xe2\x97\x8b Back";
    if (!strstr(menu_text, " Add-ons ")) return main_footer;
    if (strstr(menu_text, "[Find]")) return find_footer;
    if (strstr(menu_text, "[Starter packs]")) return packs_footer;
    return addon_footer;
}

int compositor_menu_text_supported(const char *text)
{
    const unsigned char *at = (const unsigned char *)(text ? text : "");
    const unsigned char *end = at + strlen((const char *)at);
    while (at < end)
    {
        uint32_t codepoint = menu_utf8(&at, end);
        if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t') continue;
        const struct forever_raster_glyph *regular = menu_glyph(codepoint, 0);
        const struct forever_raster_glyph *bold = menu_glyph(codepoint, 1);
        if (!regular || regular->codepoint != codepoint ||
            !bold || bold->codepoint != codepoint) return 0;
    }
    return 1;
}

int compositor_menu_footer_fits(void)
{
    const char *footer = menu_footer_text();
    return menu_text_width(footer, strlen(footer), 0) <=
        MENU_PANEL_WIDTH - 2 * MENU_PANEL_PADDING;
}

int compositor_menu_title_fits(void)
{
    const char *end = strchr(menu_text, '\n');
    size_t length = end ? (size_t)(end - menu_text) : strlen(menu_text);
    return menu_text_width(menu_text, length, 1) <=
        MENU_PANEL_WIDTH - 2 * MENU_PANEL_PADDING;
}

static size_t menu_wrap_length(const char *text, size_t length, int bold, int max_width)
{
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *at = start, *end = start + length;
    size_t last_space = (size_t)-1;
    int width = 0;

    while (at < end)
    {
        const unsigned char *before = at;
        uint32_t codepoint = menu_utf8(&at, end);
        const struct forever_raster_glyph *glyph = menu_glyph(codepoint, bold);
        int advance = glyph ? glyph->advance : 0;
        if (codepoint == ' ') last_space = (size_t)(before - start);
        if (width + advance > max_width)
        {
            if (last_space != (size_t)-1 && last_space) return last_space;
            return before > start ? (size_t)(before - start) : (size_t)(at - start);
        }
        width += advance;
    }
    return length;
}

static unsigned menu_wrapped_rows(const char *text, size_t length, int bold, int max_width)
{
    unsigned rows = 0;
    while (length)
    {
        while (length && *text == ' ') { text++; length--; }
        if (!length) break;
        size_t part = menu_wrap_length(text, length, bold, max_width);
        if (!part) break;
        rows++;
        text += part;
        length -= part;
    }
    return rows ? rows : 1;
}

static void draw_menu_text(const char *text, size_t length, int bold, int x, int baseline,
                           uint32_t color, const struct display_rect *clip,
                           const struct display_rect *damage, uint32_t *image)
{
    const unsigned char *at = (const unsigned char *)text;
    const unsigned char *end = at + length;
    const uint8_t *coverage = bold ? forever_font_bold_coverage : forever_font_regular_coverage;

    while (at < end)
    {
        const struct forever_raster_glyph *glyph = menu_glyph(menu_utf8(&at, end), bold);
        if (!glyph) continue;
        int left = x + glyph->left, top = baseline + glyph->top;
        struct display_rect rect = {
            left, top, left + glyph->width, top + glyph->height};
        struct display_rect clipped = rect;
        if (glyph->width && glyph->height && intersect(&clipped, damage) &&
            intersect(&clipped, clip))
            for (int py = clipped.top; py < clipped.bottom; py++)
                for (int px = clipped.left; px < clipped.right; px++)
                {
                    unsigned alpha = coverage[glyph->offset +
                        (size_t)(py - top) * glyph->width + (size_t)(px - left)];
                    if (alpha)
                    {
                        uint32_t *destination = &image[(size_t)py * DISPLAY_WIDTH + px];
                        *destination = blend(color, *destination, alpha);
                    }
                }
        x += glyph->advance;
    }
}

static void draw_menu_wrapped_text(
    const char *text, size_t length, const struct display_rect *line,
    uint32_t color, const struct display_rect *damage, uint32_t *image)
{
    unsigned row = 0;
    int max_width = line->right - line->left - 48;
    while (length)
    {
        while (length && *text == ' ') { text++; length--; }
        if (!length) break;
        size_t part = menu_wrap_length(text, length, 0, max_width);
        if (!part) break;
        int baseline = line->top + (int)row * MENU_ROW_HEIGHT +
            (MENU_ROW_HEIGHT - FOREVER_FONT_REGULAR_ASCENT -
             FOREVER_FONT_REGULAR_DESCENT) / 2 + FOREVER_FONT_REGULAR_ASCENT;
        draw_menu_text(text, part, 0, line->left + 24, baseline, color,
                       &menu_rect, damage, image);
        text += part;
        length -= part;
        row++;
    }
}

static int menu_line_contains(const struct menu_layout_line *line, const char *needle)
{
    size_t length = strlen(needle);
    if (length > line->length) return 0;
    const char *text = menu_text + line->begin;
    for (size_t i = 0; i + length <= line->length; i++)
        if (!memcmp(text + i, needle, length)) return 1;
    return 0;
}

static void draw_menu(const struct display_rect *damage, uint32_t *image)
{
    const char *footer = menu_footer_text();
    size_t footer_length = strlen(footer);
    struct display_rect panel = menu_rect;
    if (!menu_text[0] || !intersect(&panel, damage)) return;

    for (int y = panel.top; y < panel.bottom; y++)
        for (int x = panel.left; x < panel.right; x++)
        {
            int border = x < menu_rect.left + 2 || x >= menu_rect.right - 2 ||
                         y < menu_rect.top + 2 || y >= menu_rect.bottom - 2;
            image[(size_t)y * DISPLAY_WIDTH + x] = border ? MENU_BORDER : MENU_PANEL;
        }
    for (unsigned i = 0; i < menu_line_count; i++)
    {
        const struct menu_layout_line *line = &menu_lines[i];
        if (line->selected)
        {
            struct display_rect bar = line->rect;
            if (intersect(&bar, damage))
                for (int y = bar.top; y < bar.bottom; y++)
                    for (int x = bar.left; x < bar.right; x++)
                        image[(size_t)y * DISPLAY_WIDTH + x] = MENU_SELECTION;
        }
        if (line->separator)
        {
            int y = (line->rect.top + line->rect.bottom) / 2;
            if (y >= damage->top && y < damage->bottom)
                for (int x = line->rect.left + 20; x < line->rect.right - 20; x++)
                    if (x >= damage->left && x < damage->right)
                        image[(size_t)y * DISPLAY_WIDTH + x] = MENU_BORDER;
            continue;
        }
        if (line->blank) continue;
        const char *text = menu_text + line->begin;
        size_t length = line->length, value_length = 0;
        const char *value = NULL;
        while (length && (*text == ' ' || *text == '>')) { text++; length--; }
        const char *tab = memchr(text, '\t', length);
        if (tab)
        {
            value = tab + 1;
            value_length = length - (size_t)(value - text);
            length = (size_t)(tab - text);
        }
        int bold = i == 0;
        uint32_t color = menu_line_contains(line, "Coming soon") ||
                         menu_line_contains(line, "CurseForge only") ? MENU_MUTED : MENU_TEXT;
        if (bold)
        {
            int baseline = line->rect.top +
                (line->rect.bottom - line->rect.top - FOREVER_FONT_BOLD_ASCENT -
                 FOREVER_FONT_BOLD_DESCENT) / 2 + FOREVER_FONT_BOLD_ASCENT;
            int x = (menu_rect.left + menu_rect.right -
                     menu_text_width(text, length, 1)) / 2;
            draw_menu_text(text, length, 1, x, baseline, color,
                           &menu_rect, damage, image);
        }
        else
            draw_menu_wrapped_text(text, length, &line->rect, color, damage, image);
        if (value)
        {
            struct display_rect bounds;
            compositor_menu_value_bounds((int)i, &bounds);
            int baseline = bounds.top +
                (MENU_ROW_HEIGHT - FOREVER_FONT_REGULAR_ASCENT -
                 FOREVER_FONT_REGULAR_DESCENT) / 2 + FOREVER_FONT_REGULAR_ASCENT;
            uint32_t value_color = menu_line_contains(line, "Coming soon") ||
                                   menu_line_contains(line, "CurseForge only") ?
                MENU_MUTED : MENU_ACCENT;
            draw_menu_text(value, value_length, 0, bounds.left, baseline,
                           value_color, &menu_rect, damage, image);
        }
    }
    int footer_top = menu_rect.bottom - MENU_PANEL_PADDING - MENU_FOOTER_HEIGHT;
    if (footer_top >= damage->top && footer_top < damage->bottom)
        for (int x = menu_rect.left + MENU_PANEL_PADDING; x < menu_rect.right - MENU_PANEL_PADDING; x++)
            if (x >= damage->left && x < damage->right)
                image[(size_t)footer_top * DISPLAY_WIDTH + x] = MENU_BORDER;
    int footer_width = menu_text_width(footer, footer_length, 0);
    int footer_baseline = footer_top +
        (MENU_FOOTER_HEIGHT - FOREVER_FONT_REGULAR_ASCENT -
         FOREVER_FONT_REGULAR_DESCENT) / 2 + FOREVER_FONT_REGULAR_ASCENT;
    draw_menu_text(footer, footer_length, 0,
                   (menu_rect.left + menu_rect.right - footer_width) / 2,
                   footer_baseline, MENU_MUTED, &menu_rect, damage, image);
}

static void draw_hint(const struct display_rect *damage, uint32_t *image)
{
    struct display_rect panel = hint_rect;
    if (!hint_active || !intersect(&panel, damage)) return;
    for (int y = panel.top; y < panel.bottom; y++)
        for (int x = panel.left; x < panel.right; x++)
        {
            int border = x < hint_rect.left + 2 || x >= hint_rect.right - 2 ||
                         y < hint_rect.top + 2 || y >= hint_rect.bottom - 2;
            image[(size_t)y * DISPLAY_WIDTH + x] = border ? MENU_BORDER : MENU_PANEL;
        }
    int width = menu_text_width(HINT_TEXT, sizeof(HINT_TEXT) - 1, 0);
    int baseline = hint_rect.top +
        (hint_rect.bottom - hint_rect.top - FOREVER_FONT_REGULAR_ASCENT -
         FOREVER_FONT_REGULAR_DESCENT) / 2 + FOREVER_FONT_REGULAR_ASCENT;
    draw_menu_text(HINT_TEXT, sizeof(HINT_TEXT) - 1, 0,
                   (hint_rect.left + hint_rect.right - width) / 2,
                   baseline, MENU_TEXT, &hint_rect, damage, image);
}

static void draw_message(const struct display_rect *damage, uint32_t *image)
{
    draw_text_panel(message, &message_rect, damage, image);
    draw_hint(damage, image);
    draw_menu(damage, image);
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
