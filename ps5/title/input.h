/* The title's input devices: the pad (ScePad), USB keyboards (SceKeyboard)
 * and mice (SceMouse) of the console's initial user. Keyboard state uses
 * sceKeyboardReadState; pad and mouse queues are read in batches. Record
 * layouts: SDL3 src/joystick/ps5/SDL_ps5joystick.h (pad) and
 * ps5-native-gamepad-input-research KEYBOARD-MOUSE.md (keyboard, mouse). */
#ifndef PS5_TITLE_INPUT_H
#define PS5_TITLE_INPUT_H

#include <stdint.h>

/* ScePad button bits (research CONTROLLER-DATA.md). */
#define PAD_CREATE    0x00000001u
#define PAD_L3        0x00000002u
#define PAD_R3        0x00000004u
#define PAD_OPTIONS   0x00000008u
#define PAD_UP        0x00000010u
#define PAD_RIGHT     0x00000020u
#define PAD_DOWN      0x00000040u
#define PAD_LEFT      0x00000080u
#define PAD_L2        0x00000100u
#define PAD_R2        0x00000200u
#define PAD_L1        0x00000400u
#define PAD_R1        0x00000800u
#define PAD_TRIANGLE  0x00001000u
#define PAD_CIRCLE    0x00002000u
#define PAD_CROSS     0x00004000u
#define PAD_SQUARE    0x00008000u
#define PAD_TOUCHPAD  0x00100000u
#define PAD_INTERCEPTED 0x80000000u

/* Mouse buttons. */
#define MOUSE_LEFT    0x01u
#define MOUSE_RIGHT   0x02u
#define MOUSE_MIDDLE  0x04u
#define MOUSE_X1      0x08u
#define MOUSE_X2      0x10u

enum input_kind
{
    INPUT_KEY,          /* code: USB HID usage; down */
    INPUT_MOUSE_MOVE,   /* dx, dy */
    INPUT_MOUSE_BUTTON, /* code: MOUSE_*; down */
    INPUT_MOUSE_WHEEL,  /* dy: vertical, dx: horizontal, in detents */
    INPUT_PAD,          /* code: buttons; lx, ly, rx, ry: 0-255, 128 centred;
                          touches: fingers on the touchpad (0-2), touch_x/y and
                          touch_id of the first, in the pad's touch units */
};

struct input_event
{
    enum input_kind kind;
    uint32_t code;
    int down;
    int connected; /* physical pad connection, even while system-intercepted */
    int dx, dy;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t touches, touch_id;
    uint16_t touch_x, touch_y;
};

/* Aggregate keyboard-reader telemetry. It deliberately contains no usage,
 * modifier, or text values. input_take_keyboard_diagnostics() resets the
 * returned logical handle's window counters. */
struct input_keyboard_diagnostics
{
    int handle, user, open_rc;
    unsigned device_index, param_null;
    /* sceKeyboardReadState. */
    uint64_t reads, rc_nonzero, connected, byte8_nonzero, length_nonzero, usage_nonzero;
    int last_rc;
    unsigned last_byte8, max_length;
    /* Batched sceKeyboardRead. */
    uint64_t queue_reads, queue_errors, queue_records, queue_connected, queue_usage_nonzero;
    int last_queue_rc;
};

struct input_keyboard_open_diagnostic
{
    int handle, user, open_rc;
    unsigned device_index, param_null;
};

#define INPUT_KEYBOARD_STATE_BYTES 96
#define INPUT_KEYBOARD_DIAGNOSTIC_STATES 4
enum input_keyboard_diagnostic_source
{
    INPUT_KEYBOARD_DIAGNOSTIC_STATE,
    INPUT_KEYBOARD_DIAGNOSTIC_READ,
    INPUT_KEYBOARD_DIAGNOSTIC_SOURCE_COUNT,
};

/* The user the devices belong to, or -1 with *status the failing call's. */
int input_open(int *status);
/* What opened, for the log. */
const char *input_describe(void);
/* Handle for GPU-only vibration; -1 when no pad was opened. */
int input_pad_handle(void);
/* Number of logical user/index/parameter probes. */
unsigned input_keyboard_diagnostic_count(void);
/* One open result, once per logical probe. */
int input_take_keyboard_open_diagnostic(unsigned slot, struct input_keyboard_open_diagnostic *out);
/* Nonzero when this slot had reads since the previous take. */
int input_take_keyboard_diagnostics(unsigned slot, struct input_keyboard_diagnostics *out);
/* Takes one of the first distinct raw state or batched-read records. */
int input_take_keyboard_raw_diagnostic(unsigned slot, unsigned source, unsigned *ordinal,
                                       int *handle, uint8_t state[INPUT_KEYBOARD_STATE_BYTES]);
/* One-shot first connected state with a positive reported key count. */
int input_take_keyboard_activity(unsigned *slot, unsigned *length);
/* Process-wide diagnostic, called after the first counter window. */
int input_keyboard_set_process_privilege(void);
/* One-shot transition from NULL to zeroed keyboard open parameters. */
void input_keyboard_request_zero_param_phase(void);
/* Calls handler for every state transition or queue record since the last
 * call. A pad event comes once per poll with the latest state. */
void input_poll(void (*handler)(const struct input_event *event, void *context), void *context);
/* Pad batches with several records, and those not already in time order,
 * since the previous take. */
void input_take_pad_order_stats(unsigned *multi, unsigned *reordered);

#endif
