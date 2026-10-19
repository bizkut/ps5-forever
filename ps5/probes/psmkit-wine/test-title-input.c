/* Exercise the real PS5 keyboard decoder and the bounded multi-contract probe. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../title/input.c"

struct fake_keyboard
{
    int used, closed, user, index, param_null;
    int state_rc, read_rc;
    struct keyboard_record state;
};

static struct fake_keyboard fake_keyboards[64];
static struct input_event events[64];
static unsigned event_count, keyboard_opens, keyboard_closes, privilege_calls;

static struct fake_keyboard *fake_from_handle(int handle)
{
    assert(handle >= 100 && handle < 100 + (int)(sizeof(fake_keyboards) / sizeof(fake_keyboards[0])));
    assert(fake_keyboards[handle - 100].used && !fake_keyboards[handle - 100].closed);
    return &fake_keyboards[handle - 100];
}

static unsigned find_slot(int user, unsigned index, unsigned param_null)
{
    for (unsigned slot = 0; slot < keyboard_count; slot++)
        if (keyboards[slot].user == user && keyboards[slot].device_index == index &&
            keyboards[slot].param_null == param_null) return slot;
    assert(0 && "keyboard probe slot missing");
    return 0;
}

int sceUserServiceInitialize(const void *param)
{
    assert(!param);
    return 0;
}

int sceUserServiceGetInitialUser(int *user)
{
    *user = 77;
    return 0;
}

int sceUserServiceGetLoginUserIdList(int users[4])
{
    users[0] = 77; /* duplicate initial user */
    users[1] = 88;
    users[2] = users[3] = -1;
    return 0;
}

int sceSysmoduleLoadModule(uint16_t id)
{
    assert(id == 0x0106 || id == 0x00a9);
    return 0;
}

int scePadInit(void) { return 0; }
int scePadOpen(int user, int type, int index, const void *param)
{
    assert(user == 77 && !type && !index && !param);
    return -1;
}
int scePadRead(int handle, void *records, int count)
{
    (void)handle; (void)records; (void)count;
    assert(0 && "closed pad must not be read");
    return -1;
}

int sceKeyboardInit(void) { return 0; }
int sceKeyboardOpen(int user, int type, int index, const void *param)
{
    struct keyboard_open_param zero = {{0}};
    unsigned at = keyboard_opens++;

    assert(!type && index >= 0 && index < KEYBOARD_DEVICE_INDEXES);
    assert(at < sizeof(fake_keyboards) / sizeof(fake_keyboards[0]));
    if (param) assert(!memcmp(param, &zero, sizeof(zero)));
    fake_keyboards[at] = (struct fake_keyboard){
        .used = 1, .user = user, .index = index, .param_null = param == NULL,
    };
    return 100 + at;
}

int sceKeyboardClose(int handle)
{
    struct fake_keyboard *keyboard = fake_from_handle(handle);
    keyboard->closed = 1;
    keyboard_closes++;
    return 0;
}

int sceKeyboardRead(int handle, void *records, int count)
{
    struct fake_keyboard *keyboard = fake_from_handle(handle);
    assert(count == KEYBOARD_RECORDS);
    if (keyboard->read_rc < 0) return keyboard->read_rc;
    memcpy(records, &keyboard->state, sizeof(keyboard->state));
    return keyboard->read_rc ? keyboard->read_rc : 1;
}

int sceKeyboardReadState(int handle, void *record)
{
    struct fake_keyboard *keyboard = fake_from_handle(handle);
    if (keyboard->state_rc) return keyboard->state_rc;
    memcpy(record, &keyboard->state, sizeof(keyboard->state));
    return 0;
}

int sceKeyboardSetProcessPrivilege(int enabled)
{
    assert(enabled == 1);
    privilege_calls++;
    return 123;
}

int sceMouseInit(void) { return 0; }
int sceMouseOpen(int user, int type, int index, const void *param)
{
    assert(user == 77 && !type && index >= 0 && index < DEVICE_INDEXES && !param);
    return -1;
}
int sceMouseRead(int handle, void *records, int count)
{
    (void)handle; (void)records; (void)count;
    assert(0 && "closed mouse must not be read");
    return -1;
}

static void capture(const struct input_event *event, void *context)
{
    (void)context;
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = *event;
}

static void expect(unsigned at, unsigned usage, int down)
{
    assert(at < event_count);
    assert(events[at].kind == INPUT_KEY);
    assert(events[at].code == usage);
    assert(events[at].down == down);
}

static void check_open_results(unsigned param_null)
{
    unsigned found = 0;
    for (unsigned slot = 0; slot < input_keyboard_diagnostic_count(); slot++)
    {
        struct input_keyboard_open_diagnostic opened;
        if (!input_take_keyboard_open_diagnostic(slot, &opened)) continue;
        assert(opened.param_null == param_null);
        assert(opened.user == 77 || opened.user == 0xff || opened.user == 0xfe || opened.user == 88);
        assert(opened.device_index < 2 && opened.open_rc == opened.handle && opened.handle >= 100);
        found++;
    }
    assert(found == 8); /* four unique users, two indexes, one active parameter mode */
}

static void check_raw_source(unsigned slot, unsigned source)
{
    struct keyboard_record raw;
    unsigned ordinal = 0;
    int handle = -1;

    assert(input_take_keyboard_raw_diagnostic(slot, source, &ordinal, &handle, (uint8_t *)&raw));
    assert(ordinal == 1 && handle >= 100 && raw.timestamp == 0 && raw.keys[0] == 0x04);
    assert(input_take_keyboard_raw_diagnostic(slot, source, &ordinal, &handle, (uint8_t *)&raw));
    assert(ordinal == 2 && raw.timestamp == 1 && raw.keys[0] == 0x4f);
    assert(input_take_keyboard_raw_diagnostic(slot, source, &ordinal, &handle, (uint8_t *)&raw));
    assert(ordinal == 3 && raw.keys[0] == 1);
    assert(input_take_keyboard_raw_diagnostic(slot, source, &ordinal, &handle, (uint8_t *)&raw));
    assert(ordinal == 4 && raw.reserved0[0] == 0xa5 && raw.keys[0] == 0x4f);
    assert(!input_take_keyboard_raw_diagnostic(slot, source, &ordinal, &handle, (uint8_t *)&raw));
}


int main(void)
{
    int status = -1;
    unsigned slot;
    struct fake_keyboard *keyboard;

    assert(input_open(&status) == 77 && !status);
    assert(input_keyboard_diagnostic_count() == 16); /* initial/system/everyone/login x 2 x 2 */
    assert(keyboard_opens == 8); /* NULL phase only */
    assert(strstr(input_describe(), "keyboard_users=4") && strstr(input_describe(), "slots=16"));
    assert(strstr(input_describe(), "focus=skipped_unknown_pointer_abi"));
    check_open_results(1);

    slot = find_slot(77, 1, 1);
    keyboard = fake_from_handle(keyboards[slot].handle);
    keyboard->state.connected = 1;
    keyboard->state.length = 5;
    keyboard->state.modifiers = (1u << 0) | (1u << 5); /* left Ctrl, right Shift */
    keyboard->state.keys[0] = 0x04; /* A */
    keyboard->state.keys[1] = 0x28; /* Return */
    keyboard->state.keys[2] = 0x29; /* Escape */
    keyboard->state.keys[3] = 0x4f; /* Right */
    keyboard->state.keys[4] = 0x45; /* F12 */
    input_poll(capture, NULL);
    assert(event_count == 7);
    expect(0, 0xe0, 1); expect(1, 0xe5, 1); expect(2, 0x04, 1);
    expect(3, 0x28, 1); expect(4, 0x29, 1); expect(5, 0x45, 1); expect(6, 0x4f, 1);
    {
        unsigned activity_slot = 99, length = 0;
        assert(input_take_keyboard_activity(&activity_slot, &length));
        assert(activity_slot == slot && length == 5);
        assert(!input_take_keyboard_activity(&activity_slot, &length));
    }

    event_count = 0;
    keyboard->state.timestamp = 1; /* Timestamp alone is not distinct. */
    input_poll(capture, NULL);
    assert(!event_count);

    memset(keyboard->state.keys, 0, sizeof(keyboard->state.keys));
    keyboard->state.length = 1;
    keyboard->state.keys[0] = 0x4f;
    keyboard->state.modifiers = 0;
    input_poll(capture, NULL);
    assert(event_count == 6);
    expect(0, 0x04, 0); expect(1, 0x28, 0); expect(2, 0x29, 0);
    expect(3, 0x45, 0); expect(4, 0xe0, 0); expect(5, 0xe5, 0);

    event_count = 0;
    keyboard->state.keys[0] = 1; /* rollover preserves Right */
    input_poll(capture, NULL);
    assert(!event_count);
    keyboard->state.keys[0] = 0x4f;
    keyboard->state.reserved0[0] = 0xa5;
    input_poll(capture, NULL);
    assert(!event_count && keyboards[slot].down[0x4f]);

    check_raw_source(slot, INPUT_KEYBOARD_DIAGNOSTIC_STATE);
    check_raw_source(slot, INPUT_KEYBOARD_DIAGNOSTIC_READ);
    {
        struct input_keyboard_diagnostics diagnostic;
        assert(input_take_keyboard_diagnostics(slot, &diagnostic));
        assert(diagnostic.user == 77 && diagnostic.device_index == 1 && diagnostic.param_null);
        assert(diagnostic.reads == 5 && diagnostic.queue_reads == 5 && diagnostic.queue_records == 5);
        assert(diagnostic.connected == 5 && diagnostic.queue_connected == 5);
        assert(diagnostic.usage_nonzero == 13 && diagnostic.queue_usage_nonzero == 13);
        assert(diagnostic.byte8_nonzero == 1 && diagnostic.last_byte8 == 0xa5);
    }

    keyboard->state.connected = 0;
    keyboard->state.reserved0[0] = 0;
    event_count = 0;
    input_poll(capture, NULL);
    assert(event_count == 1);
    expect(0, 0x4f, 0);

    keyboard->state_rc = -7;
    keyboard->read_rc = -9;
    input_poll(capture, NULL);
    {
        struct input_keyboard_diagnostics diagnostic;
        assert(input_take_keyboard_diagnostics(slot, &diagnostic));
        assert(diagnostic.rc_nonzero == 1 && diagnostic.last_rc == -7);
        assert(diagnostic.queue_errors == 1 && diagnostic.last_queue_rc == -9);
    }
    keyboard->state_rc = keyboard->read_rc = 0;

    assert(input_keyboard_set_process_privilege() == 123);
    assert(input_keyboard_set_process_privilege() == 123 && privilege_calls == 1);

    input_keyboard_request_zero_param_phase();
    input_poll(capture, NULL);
    assert(keyboard_closes == 8 && keyboard_opens == 16);
    check_open_results(0);
    for (unsigned i = 0; i < keyboard_count; i++)
    {
        if (keyboards[i].param_null) assert(keyboards[i].handle < 0);
        else assert(keyboards[i].handle >= 0);
    }

    slot = find_slot(77, 1, 0);
    keyboard = fake_from_handle(keyboards[slot].handle);
    keyboard->state.connected = 1;
    keyboard->state.length = 1;
    keyboard->state.keys[0] = 0x28;
    event_count = 0;
    input_poll(capture, NULL);
    assert(event_count == 1);
    expect(0, 0x28, 1);

    puts("title keyboard users/params/ReadState/Read/privilege diagnostic: PASS");
    return 0;
}
