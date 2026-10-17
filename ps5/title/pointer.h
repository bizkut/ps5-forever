/* One pointer and one key stream for the desktop, from the input devices:
 * a mouse moves it and clicks; with the DualSense alone the left stick or a
 * finger on the touchpad moves it, two fingers on the touchpad scroll,
 * pressing the touchpad clicks (the left button with one finger on it, the
 * right with two; a tap does nothing), Cross is the left button, Circle the
 * right, the right stick scrolls, the D-pad gives the arrow keys, R1/L1
 * Tab/Shift+Tab, Triangle Enter, L2 Escape and Square Backspace. Options is
 * reserved for the maintenance menu, R2 is unmapped, and L3+R3 pressed
 * together toggle the on-screen keyboard. Keyboard keys pass through. Held
 * D-pad keys repeat (350 ms, then every 110 ms; research CONTROLLER-DATA.md). */
#ifndef PS5_TITLE_POINTER_H
#define PS5_TITLE_POINTER_H

#include <stdint.h>
#include "input.h"

enum pointer_kind
{
    POINTER_MOVE,   /* x, y: new position on the display */
    POINTER_BUTTON, /* code: MOUSE_*; down */
    POINTER_WHEEL,  /* dy, dx: in 120ths of a detent, as WHEEL_DELTA */
    POINTER_KEY,    /* code: USB HID usage; down; repeat: an autorepeat */
    POINTER_TEXT,   /* the user asked for the on-screen keyboard */
};

struct pointer_event
{
    enum pointer_kind kind;
    uint32_t code;
    int down, repeat;
    int x, y, dx, dy;
};

typedef void (*pointer_handler)(const struct pointer_event *event, void *context);

void pointer_init(int width, int height);
/* GPU fallback keeps the historical Options -> Escape mapping. Maintenance
 * leaves it disabled because the title consumes Options for its menu. */
void pointer_options_escape(int enabled);
/* Feeds one input event. */
void pointer_input(const struct input_event *event, pointer_handler handler, void *context);
/* Once a frame, after the frame's input: stick motion, scrolling and key
 * repeat; now_us is a monotonic time. */
void pointer_frame(uint64_t now_us, pointer_handler handler, void *context);
void pointer_position(int *x, int *y);
/* A program's cursor warp, clamped to the display; it does not emit input. */
void pointer_set_position(int x, int y);
/* Forget every held button/key and motion accumulator without emitting an
 * event. The owner uses this while switching input away from Wine. */
void pointer_cancel(void);

#endif
