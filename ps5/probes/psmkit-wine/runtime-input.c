/* As WoWPS5 does, read devices on a 4 ms native worker and send hardware
 * input only from a Wine message-pump thread. No compositor is linked here. */
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__PROSPERO__)
#include <ps5platform/kernel.h>
#endif
#include <wine/ps5_title_input.h>
#include <wine/ps5_title_gamepad.h>
#include "../../title/input.h"
#include "../../title/pointer.h"
#include "../../title/ime.h"
#include "../../title/cursor-arrow.h"
#include "../localproc/psmkit-io.h"

#define INPUT_QUEUE_SIZE 256

#define CURSOR_SLOT_BYTES (64u * 1024u)
#define CURSOR_IMAGE_SIDE 64u
/* Exported by the VideoOut WSI in the linked RADV archive. An unopened output
 * returns 1, so the input tick can retry after RADV creates its display. The
 * position is in the given coordinate space; RADV scales it to its scanout. */
int wsi_videoout_cursor_set(const uint32_t *image, int hotspot_x, int hotspot_y);
int wsi_videoout_cursor_position(int x, int y, int space_width, int space_height);
int wsi_videoout_cursor_disable(void);

static uint32_t *cursor_image;
/* Only the input worker accesses direct memory/WSI. Wine's GUI thread copies
 * a complete pending frame under this lock; it never writes the live slot. */
static pthread_mutex_t cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static union
{
    struct ps5_desktop_cursor_shape shape;
    unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
} cursor_pending;
static int cursor_pending_dirty, cursor_visible = 1, cursor_ready;
static unsigned cursor_slot, cursor_next_slot;
static int cursor_hotspot_x, cursor_hotspot_y;

int pw_title_set_cursor(const void *message, unsigned size)
{
    if (!ps5_desktop_cursor_shape_valid(message, size)) return 0;
    pthread_mutex_lock(&cursor_lock);
    if (cursor_pending.shape.header.size != size || memcmp(&cursor_pending, message, size))
    {
        memcpy(&cursor_pending, message, size);
        cursor_pending_dirty = 1;
    }
    pthread_mutex_unlock(&cursor_lock);
    return 1;
}

/* RADV's VideoOut position function uses these fixed scanout dimensions too
 * (0001-videoout-hdr10-and-immediate.patch), independently of render extent.
 * Desktop shapes are 1080p; clamp a uniform integer enlargement to the plane. */
#define CURSOR_SCANOUT_WIDTH 3840u
#define CURSOR_SCANOUT_HEIGHT 2160u
static void cursor_render_shape(uint32_t *image, const struct ps5_desktop_cursor_shape *shape)
{
    unsigned scale = CURSOR_SCANOUT_WIDTH / PS5_DESKTOP_WIDTH;
    unsigned sy = CURSOR_SCANOUT_HEIGHT / PS5_DESKTOP_HEIGHT;
    if (scale > sy) scale = sy;
    if (!scale) scale = 1;
    if (scale > CURSOR_IMAGE_SIDE / shape->width) scale = CURSOR_IMAGE_SIDE / shape->width;
    if (scale > CURSOR_IMAGE_SIDE / shape->height) scale = CURSOR_IMAGE_SIDE / shape->height;
    memset(image, 0, CURSOR_SLOT_BYTES);
    for (unsigned y = 0; y < shape->height * scale; y++)
        for (unsigned x = 0; x < shape->width * scale; x++)
            image[y * CURSOR_IMAGE_SIDE + x] = shape->pixels[(y / scale) * shape->width + x / scale];
    cursor_hotspot_x = shape->hotspot_x * (int)scale;
    cursor_hotspot_y = shape->hotspot_y * (int)scale;
}

static void cursor_build_image(uint32_t *image)
{
    memset(image, 0, CURSOR_SLOT_BYTES);
    for (unsigned y = 0; y < PS5_CURSOR_HEIGHT; y++)
        for (unsigned x = 0; x < PS5_CURSOR_WIDTH; x++)
        {
            char pixel = ps5_cursor_arrow[y][x];
            if (pixel != ' ') image[y * CURSOR_IMAGE_SIDE + x] =
                pixel == '#' ? 0xff000000u : 0xffffffffu;
        }
}

#if defined(__PROSPERO__)
static void cursor_flush(void *memory)
{
    for (size_t i = 0; i < CURSOR_SLOT_BYTES; i += 64)
        __asm__ volatile("clflush (%0)" : : "r"((uint8_t *)memory + i) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static int cursor_allocate(void)
{
    int64_t physical = -1;
    int result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
        2 * CURSOR_SLOT_BYTES, CURSOR_SLOT_BYTES, PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
    if (!result)
        result = sceKernelMapDirectMemory((void **)&cursor_image, 2 * CURSOR_SLOT_BYTES,
            PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE |
            PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE,
            0, physical, CURSOR_SLOT_BYTES);
    if (result)
    {
        io_log("title cursor direct memory failed status=%#x", (unsigned)result);
        return result;
    }
    cursor_build_image(cursor_image);
    cursor_flush(cursor_image);
    io_log("title cursor image=%p bytes=%u format=linear-AARRGGBB", cursor_image,
           2 * CURSOR_SLOT_BYTES);
    return 0;
}
#else
static void cursor_flush(void *memory) { (void)memory; }
static int cursor_allocate(void)
{
    return 0;
}
#endif

/* The cursor plane is hidden after CURSOR_IDLE_US without pointer motion, so a
 * static arrow does not sit on an OLED panel; the next motion shows it again. */
static void cursor_update(uint64_t now_us)
{
    static int last_x = -1, last_y = -1, hidden;
    static uint64_t moved_us;
    int x, y;
    uint32_t *image;
    if (!cursor_image) return;
    pointer_position(&x, &y);
    if (x != last_x || y != last_y || !moved_us)
    {
        last_x = x;
        last_y = y;
        moved_us = now_us;
    }
    pthread_mutex_lock(&cursor_lock);
    if (cursor_pending_dirty)
    {
        if (!cursor_visible && cursor_pending.shape.visible) moved_us = now_us;
        cursor_visible = cursor_pending.shape.visible;
        cursor_ready = 0;
        if (cursor_visible)
        {
            cursor_next_slot = 1 - cursor_slot;
            image = (uint32_t *)((uint8_t *)cursor_image + cursor_next_slot * CURSOR_SLOT_BYTES);
            cursor_render_shape(image, &cursor_pending.shape);
            cursor_flush(image);
            cursor_ready = 1;
        }
        cursor_pending_dirty = 0;
    }
    if (!cursor_visible || now_us - moved_us >= CURSOR_IDLE_US)
    {
        if (!hidden && !wsi_videoout_cursor_disable()) hidden = 1;
    }
    else
    {
        unsigned slot = cursor_ready ? cursor_next_slot : cursor_slot;
        image = (uint32_t *)((uint8_t *)cursor_image + slot * CURSOR_SLOT_BYTES);
        if (!wsi_videoout_cursor_set(image, cursor_hotspot_x, cursor_hotspot_y))
        {
            cursor_slot = slot;
            cursor_ready = hidden = 0;
            /* Pointer position is the click hotspot, never the image origin. */
            wsi_videoout_cursor_position(x, y, PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT);
        }
    }
    pthread_mutex_unlock(&cursor_lock);
}
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static union ps5_title_input queue[INPUT_QUEUE_SIZE];
static unsigned first, count;
static int reset_pending, user;
static int cursor_x = PS5_DESKTOP_WIDTH / 2, cursor_y = PS5_DESKTOP_HEIGHT / 2;
static int usb_mouse;
static int cursor_changed;
static atomic_int reset_requested;
/* Counts for the 10 s "title input" klog line: events queued by the device
 * thread, taken by Wine, and resets (overflow or contention). */
static atomic_uint stat_queued, stat_read, stat_resets;

/* As WoWPS5 does, retain a real XInput snapshot beside the desktop queue.
 * This separate lock never covers queue/Wine calls. Only GetState refreshes
 * the one-second mode lease; discovery/capabilities queries do not. */
static pthread_mutex_t gamepad_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wine_ps5_gamepad_state gamepad;
static int gamepad_connected, gamepad_enabled = 1;
static unsigned vibration_left, vibration_right;
static atomic_int gamepad_active = 1;
static atomic_uint_fast64_t gamepad_poll_us;
static struct input_event pad_sample = {.kind = INPUT_PAD, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
static unsigned chord_previous, chord_pending, chord_swallowed, chord_taps;
static uint64_t chord_at[2], chord_tap_until[2];
static int osk_chord_active, osk_chord_dropping;
static uint64_t osk_chord_drop_at;
static int touch_active;
static uint8_t touch_id;
static uint16_t touch_x, touch_y;

_Static_assert(sizeof(struct wine_ps5_gamepad_state) == 16, "XInput state ABI");
int scePadSetVibration(int handle, const void *motors);

static uint64_t input_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static int gamepad_mode(uint64_t now)
{
    uint64_t polled = atomic_load(&gamepad_poll_us);
    return atomic_load(&gamepad_active) && polled && now >= polled && now - polled < 1000000;
}

static int16_t gamepad_axis(unsigned value, int invert)
{
    int offset = (int)value - 128;
    /* Neutral tolerance and asymmetric full range, as WoWPS5 does. */
    int result = offset >= -1 && offset <= 1 ? 0 :
                 offset < 0 ? offset * 256 : offset * 32767 / 127;
    if (invert) result = result == -32768 ? 32767 : -result;
    return result;
}

static void gamepad_update(const struct input_event *sample, unsigned buttons)
{
    static const uint32_t native[] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_OPTIONS, PAD_CREATE,
        PAD_L3, PAD_R3, PAD_L1, PAD_R1, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE};
    static const uint16_t xbox[] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200,
        0x1000, 0x2000, 0x4000, 0x8000};
    struct wine_ps5_gamepad next = {0};
    pthread_mutex_lock(&gamepad_lock);
    if (sample->connected && gamepad_enabled)
    {
        for (unsigned i = 0; i < sizeof(native) / sizeof(native[0]); i++)
            if (buttons & native[i]) next.buttons |= xbox[i];
        next.left_trigger = sample->l2;
        next.right_trigger = sample->r2;
        next.lx = gamepad_axis(sample->lx, 0);
        next.ly = gamepad_axis(sample->ly, 1);
        next.rx = gamepad_axis(sample->rx, 0);
        next.ry = gamepad_axis(sample->ry, 1);
    }
    if (gamepad_connected != sample->connected || memcmp(&next, &gamepad.pad, sizeof(next)))
    {
        gamepad.pad = next;
        if (!++gamepad.packet) ++gamepad.packet;
    }
    gamepad_connected = sample->connected;
    pthread_mutex_unlock(&gamepad_lock);
}

unsigned pw_title_get_gamepad(unsigned index, struct wine_ps5_gamepad_state *out, int poll)
{
    unsigned result;
    if (index >= 4 || !out) return 160; /* ERROR_BAD_ARGUMENTS */
    if (!index && poll) atomic_store(&gamepad_poll_us, input_now_us());
    pthread_mutex_lock(&gamepad_lock);
    result = !index && gamepad_connected ? 0 : 1167;
    if (!result) *out = gamepad;
    pthread_mutex_unlock(&gamepad_lock);
    return result;
}

static int gamepad_vibration(unsigned left, unsigned right)
{
    uint8_t motors[2] = {left >> 8, right >> 8};
    int handle = input_pad_handle();
    return handle >= 0 && scePadSetVibration(handle, motors) >= 0 ? 0 : 50;
}

unsigned pw_title_set_gamepad_vibration(unsigned index, unsigned left, unsigned right)
{
    unsigned result;
    if (index >= 4 || left > 65535 || right > 65535) return 160;
    pthread_mutex_lock(&gamepad_lock);
    result = !index && gamepad_connected ? 0 : 1167;
    if (!result)
    {
        vibration_left = left;
        vibration_right = right;
        if (gamepad_enabled) result = gamepad_vibration(left, right);
    }
    pthread_mutex_unlock(&gamepad_lock);
    return result;
}

void pw_title_enable_gamepad(int enabled)
{
    pthread_mutex_lock(&gamepad_lock);
    gamepad_enabled = !!enabled;
    atomic_store(&gamepad_active, gamepad_enabled);
    if (!enabled)
    {
        struct wine_ps5_gamepad neutral = {0};
        if (memcmp(&neutral, &gamepad.pad, sizeof(neutral)))
        {
            gamepad.pad = neutral;
            if (!++gamepad.packet) ++gamepad.packet;
        }
    }
    if (gamepad_connected)
        gamepad_vibration(enabled ? vibration_left : 0, enabled ? vibration_right : 0);
    pthread_mutex_unlock(&gamepad_lock);
}

static void enqueue(const void *message)
{
    const struct ps5_desktop_header *header = message;
    /* The device thread never waits for the consumer. Treat contention like
     * overflow, releasing delivered state before the next consumed event. */
    if (pthread_mutex_trylock(&input_lock))
    {
        atomic_store(&reset_requested, 1);
        atomic_fetch_add(&stat_resets, 1);
        return;
    }
    /* Drop the oldest prefix (the entire stale queue) on overflow. A reset
     * precedes the retained suffix so a discarded key/button up cannot leave
     * Wine stuck down. No capacity wait or I/O while holding the mutex. */
    if (atomic_exchange(&reset_requested, 0) || count == INPUT_QUEUE_SIZE)
    {
        first = count = 0;
        reset_pending = 1;
    }
    memcpy(&queue[(first + count++) % INPUT_QUEUE_SIZE], message, header->size);
    atomic_fetch_add(&stat_queued, 1);
    pthread_mutex_unlock(&input_lock);
}

int pw_title_read_input(union ps5_title_input *out, unsigned capacity,
                        int x, int y, int width, int height)
{
    int result = 0;
    if (capacity < sizeof(*out)) return 0;
    pthread_mutex_lock(&input_lock);
    if (atomic_exchange(&reset_requested, 0))
    {
        first = count = 0;
        reset_pending = 1;
    }
    if (width > 0 && height > 0)
    {
        int next_x = (int)((int64_t)x * PS5_DESKTOP_WIDTH / width);
        int next_y = (int)((int64_t)y * PS5_DESKTOP_HEIGHT / height);
        if (next_x != cursor_x || next_y != cursor_y)
        {
            cursor_x = next_x;
            cursor_y = next_y;
            cursor_changed = 1;
        }
    }
    if (reset_pending)
    {
        out->header = (struct ps5_desktop_header){PS5_TITLE_INPUT_RESET, sizeof(out->header)};
        reset_pending = 0;
        result = 1;
    }
    else if (count)
    {
        memcpy(out, &queue[first], queue[first].header.size);
        first = (first + 1) % INPUT_QUEUE_SIZE;
        count--;
        result = 1;
        atomic_fetch_add(&stat_read, 1);
    }
    pthread_mutex_unlock(&input_lock);
    return result;
}

static void relative_motion(int dx, int dy)
{
    struct ps5_desktop_mouse mouse = {
        .header = {PS5_DESKTOP_MOUSE, sizeof(mouse)},
        .kind = PS5_DESKTOP_MOUSE_MOVE | PS5_TITLE_MOUSE_RELATIVE,
        .x = dx, .y = dy,
    };
    enqueue(&mouse);
}

static void touchpad_motion(const struct input_event *event)
{
    /* As WoWPS5 does, raw relative motion makes the game see a mouse rather
     * than a cursor warp. Lifting/replacing a finger rebases without motion. */
    if (event->connected && event->touches)
    {
        if (touch_active && touch_id == event->touch_id)
        {
            int dx = ((int)event->touch_x - touch_x) * 2;
            int dy = ((int)event->touch_y - touch_y) * 2;
            if (dx || dy) relative_motion(dx, dy);
        }
        touch_active = 1;
        touch_id = event->touch_id;
        touch_x = event->touch_x;
        touch_y = event->touch_y;
    }
    else touch_active = 0;
}

static void on_pointer(const struct pointer_event *event, void *context)
{
    struct ps5_desktop_mouse mouse = {.header = {PS5_DESKTOP_MOUSE, sizeof(mouse)}};
    (void)context;
    switch (event->kind)
    {
    case POINTER_KEY:
    {
        struct ps5_desktop_key key = {{PS5_DESKTOP_KEY, sizeof(key)}, event->code,
                                      !!event->down, !!event->repeat};
        enqueue(&key);
        return;
    }
    case POINTER_TEXT:
        if (user >= 0) ime_start(user);
        return;
    case POINTER_MOVE:
        mouse.kind = PS5_DESKTOP_MOUSE_MOVE;
        break;
    case POINTER_BUTTON:
        mouse.kind = PS5_DESKTOP_MOUSE_BUTTON;
        switch (event->code)
        {
        case MOUSE_LEFT: mouse.button = PS5_DESKTOP_BUTTON_LEFT; break;
        case MOUSE_RIGHT: mouse.button = PS5_DESKTOP_BUTTON_RIGHT; break;
        case MOUSE_MIDDLE: mouse.button = PS5_DESKTOP_BUTTON_MIDDLE; break;
        case MOUSE_X1: mouse.button = PS5_DESKTOP_BUTTON_X1; break;
        case MOUSE_X2: mouse.button = PS5_DESKTOP_BUTTON_X2; break;
        default: return;
        }
        mouse.down = !!event->down;
        break;
    case POINTER_WHEEL:
        mouse.kind = PS5_DESKTOP_MOUSE_WHEEL;
        mouse.wheel_x = event->dx;
        mouse.wheel_y = event->dy;
        break;
    }
    if (usb_mouse) mouse.kind |= PS5_TITLE_MOUSE_RELATIVE;
    else pointer_position(&mouse.x, &mouse.y);
    enqueue(&mouse);
}

/* A one-sample drop shorter than this is controller flicker, not release. */
#define OSK_CHORD_RELEASE_US 40000

/* L3+R3 remains the on-screen keyboard chord. It acts only after a stable
 * release, so neither thumb button reaches WoW while the chord is held. */
static int osk_chord_update(unsigned raw, int connected, uint64_t now)
{
    int both = connected && (raw & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3);
    int action = 0;

    if (!connected)
    {
        osk_chord_active = osk_chord_dropping = 0;
        return 0;
    }
    if (osk_chord_active && osk_chord_dropping &&
        now - osk_chord_drop_at >= OSK_CHORD_RELEASE_US)
    {
        action = 1;
        osk_chord_active = osk_chord_dropping = 0;
    }
    if (!both)
    {
        if (osk_chord_active && !osk_chord_dropping)
        {
            osk_chord_dropping = 1;
            osk_chord_drop_at = now;
        }
        return action;
    }
    if (!osk_chord_active) osk_chord_active = 1;
    if (osk_chord_dropping) osk_chord_dropping = 0;
    return action;
}

static unsigned gamepad_chord(unsigned raw, uint64_t now)
{
    const unsigned chord = PAD_L3 | PAD_R3;
    unsigned held = raw & chord;
    chord_swallowed &= held;
    for (unsigned i = 0; i < 2; i++)
    {
        unsigned bit = i ? PAD_R3 : PAD_L3;
        if ((held & ~chord_previous & ~chord_swallowed) & bit)
        {
            chord_pending |= bit;
            chord_at[i] = now;
        }
    }
    chord_previous = held;
    /* Delay isolated thumb clicks for 80 ms, as WoWPS5 does, so both
     * buttons of a completed shortcut are swallowed before the game sees
     * either. A short isolated click is replayed as an 80 ms tap. */
    if (held == chord)
    {
        chord_swallowed |= chord;
        chord_pending = chord_taps = 0;
    }
    for (unsigned i = 0; i < 2; i++)
    {
        unsigned bit = i ? PAD_R3 : PAD_L3;
        if (chord_pending & bit)
        {
            if (!(held & bit))
            {
                chord_pending &= ~bit;
                chord_taps |= bit;
                chord_tap_until[i] = now + 80000;
            }
            else if (now - chord_at[i] >= 80000) chord_pending &= ~bit;
        }
        if ((chord_taps & bit) && now >= chord_tap_until[i]) chord_taps &= ~bit;
    }
    return (raw & ~chord_pending & ~chord_swallowed) | chord_taps;
}

static void route_pad(const struct input_event *event, uint64_t now, void *context)
{
    struct input_event pointer = *event, state = *event;
    unsigned buttons;
    if (osk_chord_update(event->code, event->connected, now))
    {
        io_log("title pad chord action=osk");
        if (user >= 0) ime_resume(user);
    }
    buttons = gamepad_chord(event->code, now);
    if (!event->connected)
    {
        chord_previous = chord_pending = chord_swallowed = chord_taps = 0;
        buttons = 0;
    }
    if (ime_is_open())
    {
        state.code = state.l2 = state.r2 = 0;
        state.lx = state.ly = state.rx = state.ry = 128;
        buttons = 0;
    }
    gamepad_update(&state, buttons);
    /* The thumb chord is handled above for the OSK. L2's maintenance-desktop
     * Escape mapping is also excluded here: WoW receives L2 only as a game
     * trigger. Filtering a complete sample releases previously held pointer
     * buttons/keys when GetState starts polling or XInputEnable changes mode. */
    pointer.code = buttons & ~(PAD_L3 | PAD_R3 | PAD_L2);
    if (gamepad_mode(now) || ime_is_open())
    {
        pointer.code = event->code & PAD_TOUCHPAD;
        pointer.lx = pointer.ly = pointer.rx = pointer.ry = 128;
    }
    touchpad_motion(event);
    /* GPU touchpad motion is handled above, not the product's absolute
     * trackpad/two-finger path. Zero touches makes every pad press a left
     * click; retaining PAD_TOUCHPAD also keeps that movie-skip escape hatch
     * active while XInput owns all the other pad buttons/sticks. */
    pointer.touches = 0;
    usb_mouse = 1; /* Button transitions must not warp a relative mouse. */
    pointer_input(&pointer, on_pointer, context);
    usb_mouse = 0;
}

static void on_input(const struct input_event *event, void *context)
{
    if (event->kind == INPUT_PAD)
    {
        pad_sample = *event;
        route_pad(event, input_now_us(), context);
        return;
    }
    if (event->kind == INPUT_MOUSE_MOVE)
    {
        /* Do not clamp USB deltas: WoW camera drag needs relative raw input,
         * as WoWPS5 does. Buttons/wheels do not warp the pointer either. */
        relative_motion(event->dx, event->dy);
        return;
    }
    usb_mouse = event->kind == INPUT_MOUSE_BUTTON || event->kind == INPUT_MOUSE_WHEEL;
    pointer_input(event, on_pointer, context);
    usb_mouse = 0;
}

static void tap_key(unsigned usage)
{
    struct ps5_desktop_key key = {{PS5_DESKTOP_KEY, sizeof(key)}, usage, 1, 0};
    enqueue(&key);
    key.down = 0;
    enqueue(&key);
}

static void route_ime(const struct ime_edit *edit)
{
    union ps5_title_input message;
    struct ps5_desktop_text *text = (void *)&message;

    for (unsigned i = 0; i < edit->erase; i++) tap_key(0x2a);
    if (edit->length)
    {
        text->header = (struct ps5_desktop_header){PS5_DESKTOP_TEXT,
            sizeof(*text) + edit->length * sizeof(uint16_t)};
        text->length = edit->length;
        memcpy(text->text, edit->text, edit->length * sizeof(uint16_t));
        enqueue(text);
    }
    /* Confirmation is ordered after the final text frame so Return submits
     * the text that the user just committed. Cancellation leaves enter clear. */
    if (edit->enter) tap_key(0x28);
}

static void log_keyboard_open_diagnostics(void)
{
    for (unsigned slot = 0; slot < input_keyboard_diagnostic_count(); slot++)
    {
        struct input_keyboard_open_diagnostic opened;
        if (input_take_keyboard_open_diagnostic(slot, &opened))
            io_log("title keyboard diagnostic=open slot=%u user=%d index=%u param=%s"
                   " rc=%#x handle=%#x",
                   slot, opened.user, opened.device_index, opened.param_null ? "null" : "zero",
                   (unsigned)opened.open_rc, (unsigned)opened.handle);
    }
}

static void *poll_devices(void *unused)
{
    struct ime_edit edit;
    unsigned logged_queued = 0, logged_read = 0;
    unsigned keyboard_windows = 0;
    struct timespec log_at;
    clock_gettime(CLOCK_MONOTONIC, &log_at);
    (void)unused;
    for (;;)
    {
        struct timespec now;
        if (!pthread_mutex_trylock(&input_lock))
        {
            int x = cursor_x, y = cursor_y, changed = cursor_changed;
            cursor_changed = 0;
            pthread_mutex_unlock(&input_lock);
            if (changed) pointer_set_position(x, y);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        input_poll(on_input, NULL);
        log_keyboard_open_diagnostics();
        {
            unsigned index, length;
            if (input_take_keyboard_activity(&index, &length))
                io_log("title keyboard state connected=1 active_keys>=1 index=%u length=%u",
                       index, length);
        }
        /* Expire the XInput lease, pending isolated thumb clicks, and the
         * OSK release debounce even when scePadRead has no new records. */
        route_pad(&pad_sample, input_now_us(), NULL);
        pointer_frame((uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000, on_pointer, NULL);
        /* One coalesced hardware-plane update per input/display tick, after
         * pointer_frame has applied the latest relative or absolute motion. */
        cursor_update((uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000);
        if (now.tv_sec - log_at.tv_sec >= 10)
        {
            unsigned queued = atomic_load(&stat_queued), read = atomic_load(&stat_read);
            if (queued != logged_queued || read != logged_read)
                io_log("title input window_s=10 queued=%u read=%u resets=%u",
                       queued - logged_queued, read - logged_read, atomic_exchange(&stat_resets, 0));
            {
                unsigned multi, reordered;
                input_take_pad_order_stats(&multi, &reordered);
                if (multi)
                    io_log("title pad records window_s=10 multi=%u reordered=%u", multi, reordered);
            }
            for (unsigned slot = 0; slot < input_keyboard_diagnostic_count(); slot++)
            {
                struct input_keyboard_diagnostics diagnostic;
                if (!input_take_keyboard_diagnostics(slot, &diagnostic)) continue;
                io_log("title keyboard diagnostic=state slot=%u user=%d index=%u param=%s"
                       " handle=%#x window_s=10 privilege=%s calls=%llu rc_nonzero=%llu"
                       " last_rc=%#x connected=%llu byte8_nonzero=%llu last_byte8=%#x"
                       " length_nonzero=%llu max_length=%u usage_nonzero=%llu",
                       slot, diagnostic.user, diagnostic.device_index,
                       diagnostic.param_null ? "null" : "zero", (unsigned)diagnostic.handle,
                       keyboard_windows ? "post" : "pre",
                       (unsigned long long)diagnostic.reads,
                       (unsigned long long)diagnostic.rc_nonzero, (unsigned)diagnostic.last_rc,
                       (unsigned long long)diagnostic.connected,
                       (unsigned long long)diagnostic.byte8_nonzero, diagnostic.last_byte8,
                       (unsigned long long)diagnostic.length_nonzero, diagnostic.max_length,
                       (unsigned long long)diagnostic.usage_nonzero);
                io_log("title keyboard diagnostic=read slot=%u user=%d index=%u param=%s"
                       " handle=%#x window_s=10 privilege=%s calls=%llu errors=%llu"
                       " last_rc=%#x records=%llu connected=%llu usage_nonzero=%llu",
                       slot, diagnostic.user, diagnostic.device_index,
                       diagnostic.param_null ? "null" : "zero", (unsigned)diagnostic.handle,
                       keyboard_windows ? "post" : "pre",
                       (unsigned long long)diagnostic.queue_reads,
                       (unsigned long long)diagnostic.queue_errors,
                       (unsigned)diagnostic.last_queue_rc,
                       (unsigned long long)diagnostic.queue_records,
                       (unsigned long long)diagnostic.queue_connected,
                       (unsigned long long)diagnostic.queue_usage_nonzero);
            }
            keyboard_windows++;
            if (keyboard_windows == 1)
            {
                int privilege = input_keyboard_set_process_privilege();
                io_log("title keyboard diagnostic=privilege nid=K1A7IJp5xYc arg=1"
                       " rc=%#x boundary=after_pre_window", (unsigned)privilege);
                io_log("title keyboard diagnostic=focus nid=jqRcNtZmLuE"
                       " action=skipped_unknown_pointer_abi");
            }
            else if (keyboard_windows == 6)
            {
                input_keyboard_request_zero_param_phase();
                io_log("title keyboard diagnostic=param_switch from=null to=zero"
                       " boundary=after_window_6 tester=repeat_A_Shift_Return");
            }
            logged_queued = queued;
            logged_read = read;
            log_at = now;
        }
        if (ime_poll(&edit)) route_ime(&edit);
        usleep(4000);
    }
    return NULL;
}

int pw_title_input_start(void)
{
    pthread_t thread;
    int status;
    user = input_open(&status);
    io_log("input user=%d status=%#x %s", user, status, input_describe());
    pointer_init(PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT);
    pointer_options_escape(1);
    if ((status = cursor_allocate())) return status;
    status = pthread_create(&thread, NULL, poll_devices, NULL);
    if (!status) pthread_detach(thread);
    return status;
}
