/* Pad, touchpad, mouse and keyboard into one pointer and key stream
 * (pointer.h). */
#include <stdlib.h>
#include "pointer.h"

#define DEADZONE 24          /* of 128, before the stick moves anything */
#define STICK_SPEED 1400.0f  /* pixels per second at full deflection */
#define TOUCH_SCALE 1.0f     /* display pixels per touchpad unit */
#define TOUCH_SCROLL 2.0f    /* wheel units (120 a detent) per touchpad unit */
#define SCROLL_RATE 8.0f     /* detents per second at full right-stick deflection */
#define REPEAT_DELAY_US 350000
#define REPEAT_INTERVAL_US 110000

/* HID usages. */
#define KEY_ENTER 0x28
#define KEY_BACKSPACE 0x2a
#define KEY_ESCAPE 0x29
#define KEY_TAB 0x2b
#define KEY_RIGHT 0x4f
#define KEY_LEFT 0x50
#define KEY_DOWN 0x51
#define KEY_UP 0x52
#define KEY_LEFT_SHIFT 0xe1

static const struct { uint32_t button; uint32_t usage; int shift; } pad_keys[] = {
    {PAD_UP, KEY_UP, 0}, {PAD_DOWN, KEY_DOWN, 0}, {PAD_LEFT, KEY_LEFT, 0}, {PAD_RIGHT, KEY_RIGHT, 0},
    {PAD_R1, KEY_TAB, 0}, {PAD_L1, KEY_TAB, 1}, {PAD_TRIANGLE, KEY_ENTER, 0}, {PAD_OPTIONS, KEY_ESCAPE, 0},
    /* as PS5_WineLauncher's: Square erases in the field; L3+R3 opens the
     * keyboard (pad_input), as in the GPU game */
    {PAD_SQUARE, KEY_BACKSPACE, 0},
};
#define PAD_KEYS (sizeof(pad_keys) / sizeof(pad_keys[0]))

static int width, height;
static float px, py;           /* position, fractional for slow stick motion */
static uint32_t pad_buttons;
static uint8_t lx = 128, ly = 128, ry = 128, rx = 128;
static int touching, touch_id;
static uint16_t touch_x, touch_y;
static float scroll_x, scroll_y;
static uint64_t last_frame;
static int repeat_key = -1;     /* index in pad_keys */
static uint64_t repeat_at;
static float scroll_touch_x, scroll_touch_y;
/* The pad's buttons as mouse buttons: which a touchpad press is, and what
 * the pad holds down. */
static uint32_t touch_click;
static int held_left, held_right;
static int text_chord;         /* L3+R3 held together, not yet released */

static void emit_move(pointer_handler handler, void *context)
{
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > width - 1) px = width - 1;
    if (py > height - 1) py = height - 1;
    handler(&(struct pointer_event){.kind = POINTER_MOVE, .x = (int)px, .y = (int)py}, context);
}

static void emit_key(uint32_t usage, int down, int repeat, pointer_handler handler, void *context)
{
    handler(&(struct pointer_event){.kind = POINTER_KEY, .code = usage, .down = down, .repeat = repeat},
            context);
}

static void pad_key(unsigned index, int down, int repeat, pointer_handler handler, void *context)
{
    if (pad_keys[index].shift && (down || !repeat)) emit_key(KEY_LEFT_SHIFT, down, 0, handler, context);
    emit_key(pad_keys[index].usage, down, repeat, handler, context);
}

void pointer_init(int w, int h)
{
    width = w;
    height = h;
    px = w / 2.0f;
    py = h / 2.0f;
}

void pointer_position(int *x, int *y)
{
    *x = (int)px;
    *y = (int)py;
}

void pointer_set_position(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    px = x;
    py = y;
}

static void pad_input(const struct input_event *e, pointer_handler handler, void *context)
{
    uint32_t changed = e->code ^ pad_buttons;
    int left, right;

    /* A touchpad press is the left button with one finger on it and the right
     * button with two, as on a laptop trackpad; which one is decided when it
     * goes down and held until it comes up. */
    if (changed & e->code & PAD_TOUCHPAD) touch_click = e->touches >= 2 ? MOUSE_RIGHT : MOUSE_LEFT;
    if (!(e->code & PAD_TOUCHPAD)) touch_click = 0;
    left = (e->code & PAD_CROSS) || touch_click == MOUSE_LEFT;
    right = (e->code & PAD_CIRCLE) || touch_click == MOUSE_RIGHT;
    if (left != held_left)
        handler(&(struct pointer_event){.kind = POINTER_BUTTON, .code = MOUSE_LEFT, .down = left}, context);
    if (right != held_right)
        handler(&(struct pointer_event){.kind = POINTER_BUTTON, .code = MOUSE_RIGHT, .down = right}, context);
    held_left = left;
    held_right = right;
    /* L3+R3 toggle the keyboard when either is released, as the GPU game's
     * short chord does (runtime-input.c, where a 1-s hold toggles the HUD
     * instead); a disconnect cancels. */
    if ((e->code & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3)) text_chord = e->connected;
    else if (text_chord)
    {
        text_chord = 0;
        if (e->connected) handler(&(struct pointer_event){.kind = POINTER_TEXT}, context);
    }
    for (unsigned i = 0; i < PAD_KEYS; i++)
    {
        if (!(changed & pad_keys[i].button)) continue;
        int down = !!(e->code & pad_keys[i].button);
        pad_key(i, down, 0, handler, context);
        if (down)
        {
            repeat_key = i;
            repeat_at = last_frame + REPEAT_DELAY_US;
        }
        else if (repeat_key == (int)i) repeat_key = -1;
    }
    pad_buttons = e->code;
    lx = e->lx; ly = e->ly; rx = e->rx; ry = e->ry;

    /* The touchpad as a trackpad: a finger's motion, not its position; two
     * fingers scroll instead. Taps do nothing: only a press clicks. */
    if (e->touches && touching && e->touch_id == touch_id)
    {
        int dx = (int)e->touch_x - touch_x, dy = (int)e->touch_y - touch_y;
        if (e->touches == 2 && (dx || dy))
        {
            /* Fingers moving up scroll down, as on a laptop trackpad. */
            scroll_touch_y += dy * TOUCH_SCROLL;
            scroll_touch_x -= dx * TOUCH_SCROLL;
            if ((int)scroll_touch_y || (int)scroll_touch_x)
            {
                handler(&(struct pointer_event){.kind = POINTER_WHEEL, .dx = (int)scroll_touch_x,
                                                .dy = (int)scroll_touch_y}, context);
                scroll_touch_x -= (int)scroll_touch_x;
                scroll_touch_y -= (int)scroll_touch_y;
            }
        }
        else if (dx || dy)
        {
            px += dx * TOUCH_SCALE;
            py += dy * TOUCH_SCALE;
            emit_move(handler, context);
        }
    }
    touching = e->touches > 0;
    touch_id = e->touch_id;
    touch_x = e->touch_x;
    touch_y = e->touch_y;
}

void pointer_input(const struct input_event *e, pointer_handler handler, void *context)
{
    switch (e->kind)
    {
    case INPUT_KEY:
        emit_key(e->code, e->down, 0, handler, context);
        break;
    case INPUT_MOUSE_MOVE:
        px += e->dx;
        py += e->dy;
        emit_move(handler, context);
        break;
    case INPUT_MOUSE_BUTTON:
        handler(&(struct pointer_event){.kind = POINTER_BUTTON, .code = e->code, .down = e->down}, context);
        break;
    case INPUT_MOUSE_WHEEL:
        handler(&(struct pointer_event){.kind = POINTER_WHEEL, .dx = e->dx * 120, .dy = -e->dy * 120},
                context);
        break;
    case INPUT_PAD:
        pad_input(e, handler, context);
        break;
    }
}

/* -1..1 past the dead zone, squared for fine control near the centre. */
static float axis(uint8_t value)
{
    int v = (int)value - 128;
    if (abs(v) < DEADZONE) return 0;
    float f = (abs(v) - DEADZONE) / (float)(128 - DEADZONE);
    if (f > 1) f = 1;
    f *= f;
    return v < 0 ? -f : f;
}

void pointer_frame(uint64_t now_us, pointer_handler handler, void *context)
{
    float seconds = last_frame ? (now_us - last_frame) / 1e6f : 0;
    float ax = axis(lx), ay = axis(ly), sx = axis(rx), sy = axis(ry);

    if (seconds > 0.1f) seconds = 0.1f;
    last_frame = now_us;
    if (ax || ay)
    {
        px += ax * STICK_SPEED * seconds;
        py += ay * STICK_SPEED * seconds;
        emit_move(handler, context);
    }
    /* Pushing the stick up scrolls up: a positive wheel. */
    scroll_y += -sy * SCROLL_RATE * seconds * 120;
    scroll_x += sx * SCROLL_RATE * seconds * 120;
    if (!sy) scroll_y = 0;
    if (!sx) scroll_x = 0;
    if ((int)scroll_y || (int)scroll_x)
    {
        handler(&(struct pointer_event){.kind = POINTER_WHEEL, .dx = (int)scroll_x, .dy = (int)scroll_y},
                context);
        scroll_x -= (int)scroll_x;
        scroll_y -= (int)scroll_y;
    }
    if (repeat_key >= 0 && now_us >= repeat_at)
    {
        pad_key(repeat_key, 1, 1, handler, context);
        repeat_at = now_us + REPEAT_INTERVAL_US;
    }
}
