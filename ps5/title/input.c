/* The title's input devices (input.h). Pad/mouse keep their production path.
 * Keyboard diagnostics deliberately probe user/index/open-parameter variants
 * and both native read APIs without guessing a new record layout. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "input.h"

#define PAD_RECORDS 16
#define KEYBOARD_RECORDS 16
#define MOUSE_RECORDS 16
#define DEVICE_INDEXES 4
#define KEYBOARD_DEVICE_INDEXES 2
#define KEYBOARD_USER_CAPACITY 7
#define KEYBOARD_PARAM_MODES 2
#define KEYBOARD_SLOT_CAPACITY \
    (KEYBOARD_USER_CAPACITY * KEYBOARD_DEVICE_INDEXES * KEYBOARD_PARAM_MODES)
#define USER_SERVICE_ALREADY_INITIALIZED ((int)0x80960003)

int sceUserServiceInitialize(const void *param);
int sceUserServiceGetInitialUser(int *user);
int sceUserServiceGetLoginUserIdList(int users[4]);
int sceSysmoduleLoadModule(uint16_t id);
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void *param);
int scePadRead(int handle, void *records, int count);
int sceKeyboardInit(void);
int sceKeyboardOpen(int user, int type, int index, const void *param);
int sceKeyboardClose(int handle);
int sceKeyboardRead(int handle, void *records, int count);
int sceKeyboardReadState(int handle, void *record);
/* SDK stub + firmware-5.50 export NID K1A7IJp5xYc; disassembly proves an
 * integer/byte argument and integer ioctl return. */
int sceKeyboardSetProcessPrivilege(int enabled);
int sceMouseInit(void);
int sceMouseOpen(int user, int type, int index, const void *param);
int sceMouseRead(int handle, void *records, int count);

/* SDL3 PS5_PadData; touch is PS5_PadTouchData: a count, then two contacts of
 * x, y (16 bits each), an id and three reserved bytes. */
struct pad_touch
{
    uint16_t x, y;
    uint8_t id, reserved[3];
};

struct pad_record
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2, reserved[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserved[7];
    struct pad_touch touch[2];
    int32_t connected;
    uint64_t timestamp;
    uint8_t extension[16], connected_count, reserved2[2], unique_length, unique[12];
};

_Static_assert(sizeof(struct pad_record) == 120, "pad record");

/* Firmware 5.50 sceKeyboardReadState, 96 bytes. */
struct keyboard_record
{
    uint64_t timestamp;
    uint8_t reserved0[8]; /* byte +8 is observed, but has no proven ownership meaning */
    uint8_t connected, reserved1[3];
    int32_t length;
    uint32_t leds, modifiers;
    uint16_t keys[16];
    uint8_t reserved2[32];
};
_Static_assert(sizeof(struct keyboard_record) == INPUT_KEYBOARD_STATE_BYTES, "keyboard record");
_Static_assert(offsetof(struct keyboard_record, reserved0) == 0x08, "keyboard byte 8");
_Static_assert(offsetof(struct keyboard_record, connected) == 0x10, "keyboard connected");
_Static_assert(offsetof(struct keyboard_record, length) == 0x14, "keyboard length");
_Static_assert(offsetof(struct keyboard_record, modifiers) == 0x1c, "keyboard modifiers");
_Static_assert(offsetof(struct keyboard_record, keys) == 0x20, "keyboard keys");

struct keyboard_open_param
{
    uint8_t reserved[8];
};
_Static_assert(sizeof(struct keyboard_open_param) == 8, "keyboard open param");

/* ps5_mouse.hpp ps5::mouse::Data, 40 bytes. */
struct mouse_record
{
    uint64_t timestamp;
    uint8_t connected, reserved0[3];
    uint32_t buttons;
    int32_t x, y, wheel, tilt;
    uint8_t reserved1[8];
};
_Static_assert(sizeof(struct mouse_record) == 40, "mouse record");

struct keyboard
{
    int handle, user, open_rc;
    unsigned device_index, param_null, decode, open_pending;
    uint8_t down[256];   /* HID usages held, modifiers as 0xe0-0xe7 */
    struct input_keyboard_diagnostics diagnostics;
    uint8_t diagnostic_states[INPUT_KEYBOARD_DIAGNOSTIC_SOURCE_COUNT]
                             [INPUT_KEYBOARD_DIAGNOSTIC_STATES]
                             [INPUT_KEYBOARD_STATE_BYTES];
    int diagnostic_handles[INPUT_KEYBOARD_DIAGNOSTIC_SOURCE_COUNT]
                          [INPUT_KEYBOARD_DIAGNOSTIC_STATES];
    unsigned diagnostic_count[INPUT_KEYBOARD_DIAGNOSTIC_SOURCE_COUNT];
    unsigned diagnostic_taken[INPUT_KEYBOARD_DIAGNOSTIC_SOURCE_COUNT];
};

static const struct keyboard_open_param keyboard_zero_param;
static int pad = -1;
static struct keyboard keyboards[KEYBOARD_SLOT_CAPACITY];
static unsigned keyboard_count, keyboard_user_count;
static int keyboard_login_rc;
static int keyboard_privilege_called, keyboard_privilege_rc;
static int keyboard_zero_phase_active, keyboard_zero_phase_requested;
static int mice[DEVICE_INDEXES];
static uint32_t mouse_buttons[DEVICE_INDEXES];
static char description[256];
static int keyboard_activity_seen, keyboard_activity_pending;
static unsigned keyboard_activity_index, keyboard_activity_length;
static unsigned pad_batches_multi, pad_batches_reordered;

static void add_keyboard_user(int users[KEYBOARD_USER_CAPACITY], unsigned *count, int user)
{
    if (user < 0) return;
    for (unsigned i = 0; i < *count; i++)
        if (users[i] == user) return;
    if (*count < KEYBOARD_USER_CAPACITY) users[(*count)++] = user;
}

static void open_keyboard_phase(unsigned param_null)
{
    for (unsigned i = 0; i < keyboard_count; i++)
    {
        struct keyboard *keyboard = &keyboards[i];
        if (keyboard->param_null != param_null) continue;
        keyboard->open_rc = sceKeyboardOpen(keyboard->user, 0, keyboard->device_index,
                                             param_null ? NULL : &keyboard_zero_param);
        keyboard->handle = keyboard->open_rc >= 0 ? keyboard->open_rc : -1;
        keyboard->open_pending = 1;
    }
}

int input_open(int *status)
{
    int users[KEYBOARD_USER_CAPACITY] = {-1, -1, -1, -1, -1, -1, -1};
    int login_users[4] = {-1, -1, -1, -1};
    unsigned user_count = 0;
    int user = -1, r, n = 0;

    r = sceUserServiceInitialize(NULL);
    if (r && r != USER_SERVICE_ALREADY_INITIALIZED)
    {
        *status = r;
        return -1;
    }
    if ((r = sceUserServiceGetInitialUser(&user)))
    {
        *status = r;
        return -1;
    }
    keyboard_login_rc = sceUserServiceGetLoginUserIdList(login_users);
    add_keyboard_user(users, &user_count, user);
    add_keyboard_user(users, &user_count, 0xff);
    add_keyboard_user(users, &user_count, 0xfe);
    if (!keyboard_login_rc)
        for (unsigned i = 0; i < 4; i++) add_keyboard_user(users, &user_count, login_users[i]);
    keyboard_user_count = user_count;

    scePadInit();
    pad = scePadOpen(user, 0, 0, NULL);
    sceSysmoduleLoadModule(0x0106);
    sceKeyboardInit();
    keyboard_count = 0;
    for (unsigned u = 0; u < user_count; u++)
        for (unsigned index = 0; index < KEYBOARD_DEVICE_INDEXES; index++)
            for (unsigned param_null = 0; param_null < KEYBOARD_PARAM_MODES; param_null++)
            {
                struct keyboard *keyboard = &keyboards[keyboard_count++];
                memset(keyboard, 0, sizeof(*keyboard));
                keyboard->handle = keyboard->open_rc = -1;
                keyboard->user = users[u];
                keyboard->device_index = index;
                keyboard->param_null = param_null;
                keyboard->decode = users[u] == user;
            }
    /* Start with the original NULL-parameter contract. Runtime requests one
     * transition to zero parameters after pre/post privilege windows. */
    open_keyboard_phase(1);

    sceSysmoduleLoadModule(0x00a9);
    sceMouseInit();
    n += snprintf(description + n, sizeof(description) - n,
                  "user=%d pad=%#x keyboard_users=%u login_rc=%#x slots=%u"
                  " param=null privilege=deferred focus=skipped_unknown_pointer_abi",
                  user, pad, keyboard_user_count, (unsigned)keyboard_login_rc, keyboard_count);
    for (int i = 0; i < DEVICE_INDEXES; i++)
    {
        mice[i] = sceMouseOpen(user, 0, i, NULL);
        mouse_buttons[i] = 0;
        n += snprintf(description + n, sizeof(description) - n, " mouse%d=%#x", i, mice[i]);
    }
    *status = 0;
    return user;
}

const char *input_describe(void)
{
    return description;
}

int input_pad_handle(void)
{
    return pad;
}

unsigned input_keyboard_diagnostic_count(void)
{
    return keyboard_count;
}

int input_take_keyboard_open_diagnostic(unsigned slot, struct input_keyboard_open_diagnostic *out)
{
    struct keyboard *keyboard;

    if (slot >= keyboard_count || !out) return 0;
    keyboard = &keyboards[slot];
    if (!keyboard->open_pending) return 0;
    keyboard->open_pending = 0;
    *out = (struct input_keyboard_open_diagnostic){
        keyboard->handle, keyboard->user, keyboard->open_rc,
        keyboard->device_index, keyboard->param_null,
    };
    return 1;
}

int input_take_keyboard_diagnostics(unsigned slot, struct input_keyboard_diagnostics *out)
{
    struct keyboard *keyboard;

    if (slot >= keyboard_count || !out) return 0;
    keyboard = &keyboards[slot];
    *out = keyboard->diagnostics;
    out->handle = keyboard->handle;
    out->user = keyboard->user;
    out->open_rc = keyboard->open_rc;
    out->device_index = keyboard->device_index;
    out->param_null = keyboard->param_null;
    memset(&keyboard->diagnostics, 0, sizeof(keyboard->diagnostics));
    return out->reads != 0 || out->queue_reads != 0;
}

int input_take_keyboard_raw_diagnostic(unsigned slot, unsigned source, unsigned *ordinal,
                                       int *handle, uint8_t state[INPUT_KEYBOARD_STATE_BYTES])
{
    struct keyboard *keyboard;
    unsigned at;

    if (slot >= keyboard_count || source >= INPUT_KEYBOARD_DIAGNOSTIC_SOURCE_COUNT || !state)
        return 0;
    keyboard = &keyboards[slot];
    if (keyboard->diagnostic_taken[source] >= keyboard->diagnostic_count[source]) return 0;
    at = keyboard->diagnostic_taken[source]++;
    memcpy(state, keyboard->diagnostic_states[source][at], INPUT_KEYBOARD_STATE_BYTES);
    if (ordinal) *ordinal = at + 1;
    if (handle) *handle = keyboard->diagnostic_handles[source][at];
    return 1;
}

int input_take_keyboard_activity(unsigned *slot, unsigned *length)
{
    if (!keyboard_activity_pending) return 0;
    if (slot) *slot = keyboard_activity_index;
    if (length) *length = keyboard_activity_length;
    keyboard_activity_pending = 0;
    return 1;
}

int input_keyboard_set_process_privilege(void)
{
    if (!keyboard_privilege_called)
    {
        keyboard_privilege_called = 1;
        keyboard_privilege_rc = sceKeyboardSetProcessPrivilege(1);
    }
    return keyboard_privilege_rc;
}

void input_keyboard_request_zero_param_phase(void)
{
    if (!keyboard_zero_phase_active) keyboard_zero_phase_requested = 1;
}

static void key(struct keyboard *k, unsigned usage, int down,
                void (*handler)(const struct input_event *, void *), void *context)
{
    if (usage >= 256 || k->down[usage] == down) return;
    k->down[usage] = down;
    handler(&(struct input_event){.kind = INPUT_KEY, .code = usage, .down = down}, context);
}

/* Releases everything a keyboard holds after physical disconnection. Byte +8
 * is telemetry only until hardware evidence establishes its meaning. */
static void release_all(struct keyboard *k, void (*handler)(const struct input_event *, void *), void *context)
{
    for (unsigned usage = 0; usage < 256; usage++) key(k, usage, 0, handler, context);
}

static int decode_keyboard(const struct keyboard_record *record, uint8_t now[256])
{
    int length = record->length;

    if (!record->connected) return 0;
    if (length < 0) length = 0;
    if (length > 16) length = 16;
    for (int i = 0; i < length; i++)
    {
        unsigned usage = record->keys[i];
        /* Usages 1-3 are HID error/rollover states, not a release. */
        if (usage && usage < 4) return -1;
        if (usage >= 4 && usage < 0xe0) now[usage] = 1;
    }
    for (int bit = 0; bit < 8; bit++)
        if (record->modifiers & (1u << bit)) now[0xe0 + bit] = 1;
    return 1;
}

static void apply_keyboard(struct keyboard *keyboard, const struct keyboard_record *record,
                           void (*handler)(const struct input_event *, void *), void *context)
{
    uint8_t now[256] = {0};
    int usable = decode_keyboard(record, now);

    if (usable < 0) return;
    if (!usable)
    {
        release_all(keyboard, handler, context);
        return;
    }
    /* Release ordinary keys before modifiers, then press modifiers before
     * ordinary keys, preserving the useful edge ordering of every chord. */
    for (unsigned usage = 4; usage < 0xe0; usage++)
        if (keyboard->down[usage] && !now[usage]) key(keyboard, usage, 0, handler, context);
    for (unsigned usage = 0xe0; usage <= 0xe7; usage++)
        if (keyboard->down[usage] && !now[usage]) key(keyboard, usage, 0, handler, context);
    for (unsigned usage = 0xe0; usage <= 0xe7; usage++)
        if (!keyboard->down[usage] && now[usage]) key(keyboard, usage, 1, handler, context);
    for (unsigned usage = 4; usage < 0xe0; usage++)
        if (!keyboard->down[usage] && now[usage]) key(keyboard, usage, 1, handler, context);
}

static unsigned keyboard_usage_count(const struct keyboard_record *record)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 16; i++)
        if (record->keys[i]) count++;
    return count;
}

static void retain_keyboard_diagnostic(struct keyboard *keyboard, unsigned source,
                                       const struct keyboard_record *record)
{
    const uint8_t *bytes = (const void *)record;
    unsigned count = keyboard->diagnostic_count[source];

    if (count >= INPUT_KEYBOARD_DIAGNOSTIC_STATES) return;
    /* The timestamp may advance without an input change. Distinct means the
     * remaining 88 raw bytes differ; every retained dump still includes it. */
    for (unsigned i = 0; i < count; i++)
        if (!memcmp(bytes + sizeof(record->timestamp),
                    keyboard->diagnostic_states[source][i] + sizeof(record->timestamp),
                    INPUT_KEYBOARD_STATE_BYTES - sizeof(record->timestamp))) return;
    memcpy(keyboard->diagnostic_states[source][count], record, INPUT_KEYBOARD_STATE_BYTES);
    keyboard->diagnostic_handles[source][count] = keyboard->handle;
    keyboard->diagnostic_count[source]++;
}

static void poll_keyboard(unsigned slot, struct keyboard *keyboard,
                          void (*handler)(const struct input_event *, void *), void *context)
{
    struct input_keyboard_diagnostics *diagnostics = &keyboard->diagnostics;
    struct keyboard_record records[KEYBOARD_RECORDS], state;
    int count, rc;

    diagnostics->queue_reads++;
    count = sceKeyboardRead(keyboard->handle, records, KEYBOARD_RECORDS);
    if (count < 0)
    {
        diagnostics->queue_errors++;
        diagnostics->last_queue_rc = count;
    }
    else
    {
        if (count > KEYBOARD_RECORDS) count = KEYBOARD_RECORDS;
        diagnostics->queue_records += count;
        for (int i = 0; i < count; i++)
        {
            retain_keyboard_diagnostic(keyboard, INPUT_KEYBOARD_DIAGNOSTIC_READ, &records[i]);
            if (records[i].connected) diagnostics->queue_connected++;
            diagnostics->queue_usage_nonzero += keyboard_usage_count(&records[i]);
        }
    }

    diagnostics->reads++;
    rc = sceKeyboardReadState(keyboard->handle, &state);
    if (rc)
    {
        diagnostics->rc_nonzero++;
        diagnostics->last_rc = rc;
        return;
    }
    retain_keyboard_diagnostic(keyboard, INPUT_KEYBOARD_DIAGNOSTIC_STATE, &state);
    if (state.connected) diagnostics->connected++;
    diagnostics->usage_nonzero += keyboard_usage_count(&state);
    if (state.reserved0[0])
    {
        diagnostics->byte8_nonzero++;
        diagnostics->last_byte8 = state.reserved0[0];
    }
    if (state.length > 0)
    {
        unsigned length = (unsigned)state.length;
        diagnostics->length_nonzero++;
        if (length > diagnostics->max_length) diagnostics->max_length = length;
        if (state.connected && !keyboard_activity_seen)
        {
            keyboard_activity_seen = keyboard_activity_pending = 1;
            keyboard_activity_index = slot;
            keyboard_activity_length = length;
        }
    }
    if (keyboard->decode) apply_keyboard(keyboard, &state, handler, context);
}

static void switch_keyboard_to_zero_param(void (*handler)(const struct input_event *, void *),
                                          void *context)
{
    for (unsigned i = 0; i < keyboard_count; i++)
    {
        struct keyboard *keyboard = &keyboards[i];
        if (keyboard->handle < 0) continue;
        if (keyboard->decode) release_all(keyboard, handler, context);
        sceKeyboardClose(keyboard->handle);
        keyboard->handle = -1;
    }
    keyboard_zero_phase_active = 1;
    keyboard_zero_phase_requested = 0;
    open_keyboard_phase(0);
}

static void poll_mouse(int index, void (*handler)(const struct input_event *, void *), void *context)
{
    struct mouse_record records[MOUSE_RECORDS];
    int count = sceMouseRead(mice[index], records, MOUSE_RECORDS);

    for (int i = 0; i < count; i++)
    {
        const struct mouse_record *r = &records[i];
        uint32_t buttons = r->connected && !(r->buttons & 0x80000000u) ? r->buttons & 0x1f : 0;

        if (buttons == r->buttons && (r->x || r->y))
            handler(&(struct input_event){.kind = INPUT_MOUSE_MOVE, .dx = r->x, .dy = r->y}, context);
        for (uint32_t bit = 1; bit <= MOUSE_X2; bit <<= 1)
            if ((buttons ^ mouse_buttons[index]) & bit)
                handler(&(struct input_event){.kind = INPUT_MOUSE_BUTTON, .code = bit, .down = !!(buttons & bit)},
                        context);
        mouse_buttons[index] = buttons;
        if (buttons == r->buttons && (r->wheel || r->tilt))
            handler(&(struct input_event){.kind = INPUT_MOUSE_WHEEL, .dx = r->tilt, .dy = r->wheel}, context);
    }
}

void input_poll(void (*handler)(const struct input_event *event, void *context), void *context)
{
    if (pad >= 0)
    {
        struct pad_record records[PAD_RECORDS];
        int count = scePadRead(pad, records, PAD_RECORDS);

        /* Every state, so a press and release between polls both arrive, in
         * time order: a reversed batch would end a held chord or button. */
        int reordered = 0;
        for (int i = 1; i < count; i++)
        {
            struct pad_record record = records[i];
            int j = i;
            while (j > 0 && records[j - 1].timestamp > record.timestamp)
            {
                records[j] = records[j - 1];
                j--;
            }
            records[j] = record;
            reordered |= j != i;
        }
        if (count > 1) pad_batches_multi++;
        pad_batches_reordered += reordered;
        for (int i = 0; i < count; i++)
        {
            const struct pad_record *r = &records[i];
            int live = r->connected && !(r->buttons & PAD_INTERCEPTED);
            int touches = live && r->touch_count <= 2 ? r->touch_count : 0;
            handler(&(struct input_event){.kind = INPUT_PAD, .code = live ? r->buttons : 0,
                                          .connected = !!r->connected,
                                          .lx = live ? r->lx : 128, .ly = live ? r->ly : 128,
                                          .rx = live ? r->rx : 128, .ry = live ? r->ry : 128,
                                          .l2 = live ? r->l2 : 0, .r2 = live ? r->r2 : 0,
                                          .touches = touches,
                                          .touch_id = touches ? r->touch[0].id : 0,
                                          .touch_x = touches ? r->touch[0].x : 0,
                                          .touch_y = touches ? r->touch[0].y : 0},
                    context);
        }
    }
    if (keyboard_zero_phase_requested && !keyboard_zero_phase_active)
        switch_keyboard_to_zero_param(handler, context);
    for (unsigned i = 0; i < keyboard_count; i++)
        if (keyboards[i].handle >= 0) poll_keyboard(i, &keyboards[i], handler, context);
    for (int i = 0; i < DEVICE_INDEXES; i++)
        if (mice[i] >= 0) poll_mouse(i, handler, context);
}

void input_take_pad_order_stats(unsigned *multi, unsigned *reordered)
{
    *multi = pad_batches_multi;
    *reordered = pad_batches_reordered;
    pad_batches_multi = pad_batches_reordered = 0;
}
