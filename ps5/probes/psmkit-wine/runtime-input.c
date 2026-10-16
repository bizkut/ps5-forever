/* As WoWPS5 does, read devices on a 4 ms native worker and send hardware
 * input only from a Wine message-pump thread. No compositor is linked here. */
#include <errno.h>
#include <fcntl.h>
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
#include "forever-paths.h"
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
        CURSOR_SLOT_BYTES, CURSOR_SLOT_BYTES, PS5_KERNEL_DIRECT_TYPE_CPU, &physical);
    if (!result)
        result = sceKernelMapDirectMemory((void **)&cursor_image, CURSOR_SLOT_BYTES,
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
           CURSOR_SLOT_BYTES);
    return 0;
}
#else
static int cursor_allocate(void)
{
    return 0;
}
#endif

/* The cursor plane is hidden after CURSOR_IDLE_US without pointer motion, so a
 * static arrow does not sit on an OLED panel; the next motion shows it again. */
#define CURSOR_IDLE_US (3u * 1000000u)

static void cursor_update(uint64_t now_us)
{
    static int last_x = -1, last_y = -1, hidden;
    static uint64_t moved_us;
    int x, y;
    if (!cursor_image) return;
    pointer_position(&x, &y);
    if (x != last_x || y != last_y || !moved_us)
    {
        last_x = x;
        last_y = y;
        moved_us = now_us;
    }
    if (now_us - moved_us >= CURSOR_IDLE_US)
    {
        if (!hidden && !wsi_videoout_cursor_disable()) hidden = 1;
        return;
    }
    if (wsi_videoout_cursor_set(cursor_image, 0, 0)) return;
    hidden = 0;
    /* The pointer works in the 1920x1080 desktop space Battle.net's compositor
     * uses; the cursor plane takes scanout pixels (3840x2160 for WoW). */
    wsi_videoout_cursor_position(x, y, PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT);
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
static int hud_chord_active, hud_chord_fired, hud_chord_dropping;
static uint64_t hud_chord_at, hud_chord_drop_at, hud_chord_held_us;
static unsigned hud_chord_drops, hud_chord_last_drops;
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

/* This file is linked only into the GPU title runtime. File I/O happens only
 * on the one-second chord edge, never while sampling ordinary pad input. */
static int hud_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int hud_toggle_file(const char *path, const char *temporary, int *enabled)
{
    char text[4096], output[sizeof(text) + 8];
    size_t size = 0, begin = 0, end = 0, out_size;
    int fd = open(path, O_RDONLY), found = 0, was_on = 0;
    if (fd < 0 && errno != ENOENT) return -1;
    if (fd >= 0)
    {
        ssize_t count;
        while (size < sizeof(text) &&
               (count = read(fd, text + size, sizeof(text) - size)) != 0)
        {
            if (count < 0)
            {
                if (errno == EINTR) continue;
                close(fd);
                return -1;
            }
            size += count;
        }
        close(fd);
        /* Never truncate user content, including an unexpectedly large file. */
        if (size >= sizeof(text) || memchr(text, 0, size)) return -1;
    }
    for (size_t i = 0; i < size;)
    {
        size_t token;
        while (i < size && hud_space(text[i])) i++;
        token = i;
        while (i < size && !hud_space(text[i])) i++;
        if (i - token >= 4 && !memcmp(text + token, "hud=", 4))
        {
            if (found) return -1; /* vkd3d also rejects duplicate HUD keys. */
            found = 1;
            begin = token + 4;
            end = i;
            was_on = end - begin == 2 && !memcmp(text + begin, "on", 2);
        }
    }
    *enabled = !was_on;
    if (found)
    {
        size_t value_size = *enabled ? 2 : 3;
        memcpy(output, text, begin);
        memcpy(output + begin, *enabled ? "on" : "off", value_size);
        memcpy(output + begin + value_size, text + end, size - end);
        out_size = begin + value_size + size - end;
    }
    else
    {
        memcpy(output, text, size);
        out_size = size;
        if (size && !hud_space(text[size - 1])) output[out_size++] = '\n';
        memcpy(output + out_size, "hud=on\n", 7);
        out_size += 7;
    }
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
    for (size_t written = 0; written < out_size;)
    {
        ssize_t count = write(fd, output + written, out_size - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0)
        {
            close(fd);
            unlink(temporary);
            return -1;
        }
        written += count;
    }
    if (close(fd))
    {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path))
    {
        unlink(temporary);
        return -1;
    }
    return 0;
}

/* A drop of the chord shorter than this is a flicker inside the hold, not a
 * release; the per-tick call ends a longer one even without new records. */
#define HUD_CHORD_RELEASE_US 40000

/* Returns 1 for HUD, 2 for the short OSK chord, zero otherwise. Releasing
 * either thumb (for HUD_CHORD_RELEASE_US) re-arms; disconnect cancels rather
 * than opening the OSK. hud_chord_held_us/hud_chord_last_drops describe the
 * hold that produced an action. */
static int hud_chord_update(unsigned raw, int connected, uint64_t now)
{
    int both = connected && (raw & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3);
    int action = 0;

    if (!connected)
    {
        hud_chord_active = hud_chord_fired = hud_chord_dropping = 0;
        return 0;
    }
    if (hud_chord_active && hud_chord_dropping
        && now - hud_chord_drop_at >= HUD_CHORD_RELEASE_US)
    {
        hud_chord_held_us = hud_chord_drop_at - hud_chord_at;
        hud_chord_last_drops = hud_chord_drops;
        if (!hud_chord_fired) action = hud_chord_held_us >= 1000000 ? 1 : 2;
        hud_chord_active = hud_chord_fired = hud_chord_dropping = 0;
    }
    if (!both)
    {
        if (hud_chord_active && !hud_chord_dropping)
        {
            hud_chord_dropping = 1;
            hud_chord_drop_at = now;
        }
        return action;
    }
    if (!hud_chord_active)
    {
        hud_chord_active = 1;
        hud_chord_at = now;
        hud_chord_drops = 0;
    }
    if (hud_chord_dropping)
    {
        hud_chord_dropping = 0;
        hud_chord_drops++;
    }
    if (!action && !hud_chord_fired && now - hud_chord_at >= 1000000)
    {
        hud_chord_fired = 1;
        hud_chord_held_us = now - hud_chord_at;
        hud_chord_last_drops = hud_chord_drops;
        return 1;
    }
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
    int chord_action = hud_chord_update(event->code, event->connected, now);
    if (chord_action)
        io_log("title pad chord action=%s held_ms=%u drops=%u", chord_action == 1 ? "hud" : "osk",
               (unsigned)(hud_chord_held_us / 1000), hud_chord_last_drops);
    if (chord_action == 1)
    {
        int enabled;
        if (!hud_toggle_file(FOREVER_DRIVE_C "/wineps5-hud.txt",
                             FOREVER_DRIVE_C "/wineps5-hud.txt.chord-tmp", &enabled))
            io_log("wine: title hud chord toggled hud=%s", enabled ? "on" : "off");
        else
            io_log("title hud chord write failed");
    }
    else if (chord_action == 2 && user >= 0)
        ime_resume(user);
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
    /* The thumb chord is handled above (HUD/OSK), never by the shared
     * pointer's desktop L3+R3 keyboard shortcut. Filtering a complete
     * sample also releases previously held pointer buttons/keys when
     * GetState starts polling or XInputEnable changes the mode. */
    pointer.code = buttons & ~(PAD_L3 | PAD_R3);
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
        /* Expire the XInput lease, pending isolated thumb clicks, and HUD
         * hold timer even when scePadRead has no new records. */
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
    if ((status = cursor_allocate())) return status;
    status = pthread_create(&thread, NULL, poll_devices, NULL);
    if (!status) pthread_detach(thread);
    return status;
}
