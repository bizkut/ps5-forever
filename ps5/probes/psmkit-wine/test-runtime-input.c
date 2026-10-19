/* Exercise the real bounded queue and native routing without console devices. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
static int test_clock_gettime(clockid_t clock, struct timespec *now);
static int test_usleep(useconds_t usecs);
static FILE *test_voice_fopen(const char *, const char *);
#define fopen test_voice_fopen
#define clock_gettime test_clock_gettime
#define usleep test_usleep
#include "runtime-input.c"
#undef clock_gettime
#undef usleep
#undef fopen

static int osk_calls, osk_open, vibration_calls, vibration_result;
static int open_diagnostic_pending, raw_diagnostic_pending, raw_diagnostic_calls;
static int aggregate_diagnostic_pending, testing_poll, poll_clock_calls;
static jmp_buf poll_done;
static int privilege_calls, phase_switch_calls;
static uint8_t motors[2];
static char logged_line[512];
static char logged_output[8192];
static const char *voice_test_setting;
static unsigned voice_setting_reads, voice_captures, voice_starts, voice_finishes, voice_discards;
static unsigned voice_warmup_requests;
static int voice_result_ready;
static struct pw_voice_result voice_result;
static char ime_initial_text[VOICE_TEXT_BYTES];
static int ime_show_result;
static int voice_fopen_errno = ENOENT;
static unsigned voice_setting_line_count, voice_hold_line_count;
static FILE *test_voice_fopen(const char *path, const char *mode)
{
    assert(!strcmp(path, "/data/forever/prefix/drive_c/wineps5-voice.txt") && !strcmp(mode, "r"));
    voice_setting_reads++;
    if (!voice_test_setting) { errno = voice_fopen_errno; return NULL; }
    FILE *file = tmpfile();
    assert(file && fputs(voice_test_setting, file) >= 0);
    rewind(file);
    return file;
}
static int test_clock_gettime(clockid_t clock, struct timespec *now)
{
    if (!testing_poll) return clock_gettime(clock, now);
    assert(clock == CLOCK_MONOTONIC);
    *now = (struct timespec){.tv_sec = poll_clock_calls++ ? 10 : 0};
    return 0;
}
static int test_usleep(useconds_t usecs)
{
    if (!testing_poll) return usleep(usecs);
    assert(usecs == 4000);
    longjmp(poll_done, 1);
}
int input_open(int *status) { *status = 0; return 1; }
const char *input_describe(void) { return "host test"; }
int input_pad_handle(void) { return 7; }
unsigned input_keyboard_diagnostic_count(void) { return 1; }
void input_take_pad_order_stats(unsigned *multi, unsigned *reordered) { *multi = *reordered = 0; }
int input_take_keyboard_open_diagnostic(unsigned slot, struct input_keyboard_open_diagnostic *out)
{
    assert(!slot);
    if (!open_diagnostic_pending) return 0;
    open_diagnostic_pending = 0;
    *out = (struct input_keyboard_open_diagnostic){100, 77, 100, 1, 1};
    return 1;
}
int input_take_keyboard_diagnostics(unsigned index, struct input_keyboard_diagnostics *out)
{
    assert(!index);
    if (!aggregate_diagnostic_pending) return 0;
    aggregate_diagnostic_pending = 0;
    *out = (struct input_keyboard_diagnostics){
        .handle = 100, .user = 77, .device_index = 1, .param_null = 1,
        .reads = 10, .rc_nonzero = 2, .last_rc = -3, .connected = 8,
        .byte8_nonzero = 1, .last_byte8 = 0xa5, .length_nonzero = 4, .max_length = 2,
        .usage_nonzero = 11, .queue_reads = 12, .queue_errors = 3, .last_queue_rc = -4,
        .queue_records = 9, .queue_connected = 7, .queue_usage_nonzero = 6,
    };
    return 1;
}
int input_take_keyboard_activity(unsigned *index, unsigned *length)
{ (void)index; (void)length; return 0; }
int input_take_keyboard_raw_diagnostic(unsigned slot, unsigned source, unsigned *ordinal,
                                       int *handle, uint8_t state[INPUT_KEYBOARD_STATE_BYTES])
{
    assert(!slot);
    raw_diagnostic_calls++;
    if (source != INPUT_KEYBOARD_DIAGNOSTIC_STATE || !raw_diagnostic_pending) return 0;
    raw_diagnostic_pending = 0;
    *ordinal = 1;
    *handle = 100;
    for (unsigned i = 0; i < INPUT_KEYBOARD_STATE_BYTES; i++) state[i] = i;
    return 1;
}
int input_keyboard_set_process_privilege(void) { privilege_calls++; return 123; }
void input_keyboard_request_zero_param_phase(void) { phase_switch_calls++; }
int scePadSetVibration(int handle, const void *value)
{
    assert(handle == 7);
    memcpy(motors, value, sizeof(motors));
    vibration_calls++;
    return vibration_result;
}
void input_poll(void (*handler)(const struct input_event *, void *), void *context)
{ (void)handler; (void)context; }
int ime_start(int id) { assert(id == 1); osk_calls++; return 0; }
int ime_resume(int id) { assert(id == 1); osk_calls++; osk_open = 1; return 0; }
int ime_is_open(void) { return osk_open; }
int ime_poll(struct ime_edit *edit) { (void)edit; return 0; }
int ime_show_text(int id, const char *text)
{
    assert(id == 1);
    snprintf(ime_initial_text, sizeof(ime_initial_text), "%s", text);
    osk_calls++;
    if (ime_show_result) return ime_show_result;
    osk_open = 1; return 0;
}
size_t ime_copy_text(char *text, size_t size)
{
    int length = snprintf(text, size, "%s", ime_initial_text);
    assert(length >= 0 && (size_t)length < size);
    return length;
}
void pw_voice_capture(int id) { assert(id == 1); voice_captures++; }
void pw_voice_start(void) { voice_starts++; }
void pw_voice_finish(void) { voice_finishes++; }
void pw_voice_discard(void) { voice_discards++; }
void voice_overlay_vk_request_warmup(void) { voice_warmup_requests++; }
int pw_voice_poll(struct pw_voice_result *out)
{
    if (!voice_result_ready) return 0;
    *out = voice_result; voice_result_ready = 0; return 1;
}
void io_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(logged_line, sizeof(logged_line), format, args);
    va_end(args);
    if (strstr(logged_line, "voice setting path=") == logged_line) voice_setting_line_count++;
    if (strstr(logged_line, "voice hold enabled=") == logged_line) voice_hold_line_count++;
    if (testing_poll)
    {
        size_t length = strlen(logged_output);
        int written = snprintf(logged_output + length, sizeof(logged_output) - length,
                               "%s\n", logged_line);
        assert(written >= 0 && (size_t)written < sizeof(logged_output) - length);
    }
}
static int text_events;
static void count_text(const struct pointer_event *pointer_event, void *context)
{
    (void)context;
    if (pointer_event->kind == POINTER_TEXT) text_events++;
}
static int escape_down, escape_up;
static void count_shortcuts(const struct pointer_event *pointer_event, void *context)
{
    (void)context;
    if (pointer_event->kind == POINTER_TEXT) text_events++;
    else if (pointer_event->kind == POINTER_KEY && pointer_event->code == 0x29)
    {
        if (pointer_event->down) escape_down++;
        else escape_up++;
    }
}
static int cursor_shown, cursor_open = 0;
static const uint32_t *shown_image;
static int shown_hotspot_x, shown_hotspot_y, shown_x, shown_y;
int wsi_videoout_cursor_set(const uint32_t *image, int hotspot_x, int hotspot_y)
{
    if (!cursor_open) return 1;
    shown_image = image;
    shown_hotspot_x = hotspot_x;
    shown_hotspot_y = hotspot_y;
    cursor_shown = 1;
    return 0;
}
int wsi_videoout_cursor_position(int x, int y, int space_width, int space_height)
{
    assert(space_width == PS5_DESKTOP_WIDTH && space_height == PS5_DESKTOP_HEIGHT);
    shown_x = x; shown_y = y;
    return 0;
}
int wsi_videoout_cursor_disable(void)
{
    if (!cursor_open) return 1;
    cursor_shown = 0;
    return 0;
}

static void test_keyboard_logging(void)
{
    pad_sample = (struct input_event){.kind = INPUT_PAD,
        .lx = 128, .ly = 128, .rx = 128, .ry = 128};
    open_diagnostic_pending = raw_diagnostic_pending = aggregate_diagnostic_pending = 1;
    atomic_store(&stat_queued, 7);
    atomic_store(&stat_read, 5);
    atomic_store(&stat_resets, 2);
    testing_poll = 1;
    if (!setjmp(poll_done)) poll_devices(NULL);
    testing_poll = 0;
    assert(!open_diagnostic_pending && !aggregate_diagnostic_pending);
    assert(raw_diagnostic_pending && !raw_diagnostic_calls);
    assert(strstr(logged_output, "title keyboard diagnostic=open slot=0 user=77 index=1"));
    assert(strstr(logged_output, "param=null rc=0x64 handle=0x64"));
    assert(strstr(logged_output, "title input window_s=10 queued=7 read=5 resets=2"));
    assert(strstr(logged_output, "privilege=pre calls=10 rc_nonzero=2"));
    assert(strstr(logged_output, "connected=8 byte8_nonzero=1 last_byte8=0xa5"));
    assert(strstr(logged_output, "length_nonzero=4 max_length=2 usage_nonzero=11"));
    assert(strstr(logged_output, "privilege=pre calls=12 errors=3"));
    assert(strstr(logged_output, "records=9 connected=7 usage_nonzero=6"));
    assert(strstr(logged_output, "diagnostic=privilege"));
    assert(!strstr(logged_output, "raw96"));
    assert(!strstr(logged_output, "bytes="));
    assert(!strstr(logged_output, "00010203"));
    assert(!strstr(logged_output, "5e5f"));
    assert(!strstr(logged_output, "usage="));
    assert(!strstr(logged_output, "modifiers="));
    atomic_store(&stat_queued, 0);
    atomic_store(&stat_read, 0);
}

static void *publish_cursor_shapes(void *unused)
{
    union {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message = {.shape = {.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(message.shape) + 16},
        .width = 2, .height = 2, .hotspot_x = 1, .hotspot_y = 1, .visible = 1}};
    (void)unused;
    for (unsigned i = 0; i < 4096; i++)
    {
        for (unsigned pixel = 0; pixel < 4; pixel++) message.shape.pixels[pixel] = 0xff000000u | i;
        assert(pw_title_set_cursor(&message, message.shape.header.size));
    }
    return NULL;
}

static void test_cursor_shapes(void)
{
    static uint32_t slots[2 * CURSOR_SLOT_BYTES / sizeof(uint32_t)];
    union {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message = {.shape = {.header = {PS5_DESKTOP_CURSOR_SHAPE, 0},
        .width = 2, .height = 2, .hotspot_x = 1, .hotspot_y = 1, .visible = 1}};
    uint32_t *pixels = message.shape.pixels;
    uint64_t t = 1000000000000000ull;
    const uint32_t *first_image;
    cursor_image = slots;
    cursor_build_image(slots);
    cursor_open = 1;
    pointer_set_position(200, 300);
    pixels[0] = 0xffff0000; pixels[1] = 0xff00ff00;
    pixels[2] = 0xff0000ff; pixels[3] = 0xffffffff;
    message.shape.header.size = sizeof(message.shape) + 4 * sizeof(uint32_t);
    assert(pw_title_set_cursor(&message, message.shape.header.size));
    cursor_update(t);
    assert(cursor_shown && shown_hotspot_x == 2 && shown_hotspot_y == 2);
    assert(shown_x == 200 && shown_y == 300); /* position remains the click hotspot */
    /* The VideoOut cursor plane takes Windows ARGB unchanged (WoW's dumped
     * gauntlet has a red-brown cuff, which showed red only without a swap). */
    assert(shown_image[0] == 0xffff0000 && shown_image[1] == 0xffff0000);
    assert(shown_image[2] == 0xff00ff00 && shown_image[2 * 64] == 0xff0000ff);
    assert(!shown_image[4] && !shown_image[4 * 64]);
    first_image = shown_image;
    assert(pw_title_set_cursor(&message, message.shape.header.size));
    cursor_update(t);
    assert(shown_image == first_image); /* republishing identical pixels needs no plane write */
    pixels[0] = 0xff123456;
    assert(pw_title_set_cursor(&message, message.shape.header.size));
    assert(first_image[0] == 0xffff0000); /* receiver cannot touch live direct memory */
    cursor_open = 0;
    cursor_update(t + 1);
    assert(shown_image == first_image && first_image[0] == 0xffff0000);
    cursor_open = 1;
    cursor_update(t + 1);
    assert(shown_image != first_image && shown_image[0] == 0xff123456);
    pixels[0] = 0xffabcdef;
    assert(pw_title_set_cursor(&message, message.shape.header.size));
    cursor_update(t + 2);
    assert(shown_image == first_image && shown_image[0] == 0xffabcdef);
    /* Clamp the integer scale, not the image edges or the hotspot. */
    message.shape.width = 33; message.shape.height = 64;
    message.shape.hotspot_x = 32; message.shape.hotspot_y = 63;
    message.shape.header.size = sizeof(message.shape) + 33 * 64 * 4;
    for (unsigned i = 0; i < 33 * 64; i++) pixels[i] = 0xff000000u | i;
    assert(pw_title_set_cursor(&message, message.shape.header.size));
    cursor_update(t + 3);
    assert(shown_hotspot_x == 32 && shown_hotspot_y == 63);
    assert(shown_image[63 * 64 + 32] == pixels[33 * 64 - 1]);
    assert(!shown_image[33]);
    assert(!pw_title_set_cursor(&message, message.shape.header.size - 1));
    message.shape.width = 65;
    assert(!pw_title_set_cursor(&message, message.shape.header.size));
    message.shape.width = 33;
    message.shape.header.type = 0xf000;
    assert(!pw_title_set_cursor(&message, message.shape.header.size));
    message.shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(message.shape)}};
    assert(pw_title_set_cursor(&message, sizeof(message.shape)));
    cursor_update(t + 4);
    assert(!cursor_shown);
    pointer_set_position(201, 300);
    cursor_update(t + 5);
    assert(!cursor_shown); /* motion cannot override app hide / mouselook */
    message.shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(message.shape) + 4},
        .width = 1, .height = 1, .visible = 1};
    assert(pw_title_set_cursor(&message, message.shape.header.size));
    cursor_update(t + 6);
    assert(cursor_shown);
    cursor_update(t + 3000006);
    assert(!cursor_shown); /* shape changes preserve the idle protection */
    pointer_set_position(202, 300);
    cursor_update(t + 3000007);
    assert(cursor_shown);
    {
        pthread_t publisher;
        message.shape = (struct ps5_desktop_cursor_shape){
            .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(message.shape) + 16},
            .width = 2, .height = 2, .hotspot_x = 1, .hotspot_y = 1, .visible = 1};
        for (unsigned i = 0; i < 4; i++) pixels[i] = 0xff000000u;
        assert(pw_title_set_cursor(&message, message.shape.header.size));
        assert(!pthread_create(&publisher, NULL, publish_cursor_shapes, NULL));
        for (unsigned i = 0; i < 4096; i++)
        {
            cursor_update(t + 3000008 + i);
            assert(shown_hotspot_x == 2 && shown_hotspot_y == 2);
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 4; x++)
                    assert(shown_image[y * 64 + x] == shown_image[0]);
            assert(!shown_image[4]);
        }
        assert(!pthread_join(publisher, NULL));
        cursor_update(t + 3005000);
        assert(shown_image[0] == (0xff000000u | 4095));
    }
    cursor_image = NULL;
}

static void expect_voice(unsigned flags, unsigned erase, const char *value)
{
    union ps5_title_input event;
    uint16_t expected[VOICE_TEXT_UNITS];
    unsigned length = voice_utf16(value, expected, VOICE_TEXT_UNITS);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.header.type == PS5_DESKTOP_VOICE_EDIT);
    struct ps5_desktop_voice_edit *edit = (void *)&event;
    assert(edit->flags == flags && edit->erase == erase && edit->length == length);
    assert(!memcmp(edit->text, expected, length * sizeof(*expected)));
}
static void test_voice_routing(void)
{
    struct input_event pad = {.kind = INPUT_PAD, .connected = 1, .touches = 1,
        .touch_x = 100, .touch_y = 100, .code = PAD_TOUCHPAD,
        .lx = 128, .ly = 128, .rx = 128, .ry = 128};
    union ps5_title_input event;
    uint64_t now = input_now_us() + 400000000;
    while (pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0)) {}
    pointer_init(PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT);
    user = 1;
    voice_hold = (struct voice_hold){0};
    voice_pending = (struct voice_pending){0};
    voice_setting_logged = 0;
    voice_setting_line_count = voice_hold_line_count = 0;
    voice_test_setting = "voice=on";
    unsigned reads = voice_setting_reads, captures = voice_captures, discards = voice_discards;
    unsigned warmups = voice_warmup_requests;
    route_pad(&pad, now, NULL);
    assert(voice_captures == captures + 1 && voice_setting_reads == reads + 1);
    assert(voice_warmup_requests == warmups + 1); /* Warm during deferral, not at visibility. */
    assert(!pw_title_voice_overlay()); /* Deferring a press is not recording. */
    assert(voice_setting_line_count == 1 && voice_setting_rc == 0);
    assert(strstr(logged_line, "voice setting path=/data/forever/prefix/drive_c/wineps5-voice.txt rc=0"));
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    pad.touch_x = 180; /* Still held: the complete 80-unit roll is deferred. */
    route_pad(&pad, now + 399000, NULL);
    assert(!voice_starts);
    assert(!pw_title_voice_overlay());
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    pad.code = 0; pad.touches = 0; pad.touch_x = pad.touch_y = 0;
    route_pad(&pad, now + 399000, NULL);
    assert(voice_discards == discards + 1 && !voice_finishes);
    assert(voice_hold_line_count == 1);
    assert(!strcmp(logged_line, "voice hold enabled=1 result=click held_ms=399 max_move=80 setting_rc=0"));
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && event.mouse.down);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.mouse.down);
    pad.code = PAD_TOUCHPAD; pad.touches = 1; pad.touch_x = pad.touch_y = 100;
    route_pad(&pad, now + 500000, NULL);
    route_pad(&pad, now + 900000, NULL);
    assert(voice_starts == 1);
    unsigned overlay_session = pw_title_voice_overlay();
    assert(overlay_session);
    voice_tick(now + 900000);
    expect_voice(PS5_DESKTOP_VOICE_ENTER_BEFORE, 0, "");
    pad.touch_x = 1800; pad.touch_y = 900;
    route_pad(&pad, now + 950000, NULL);
    assert(voice_hold.phase == VOICE_RECORDING);
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    pad.touch_id++; pad.touches = 2;
    route_pad(&pad, now + 951000, NULL);
    assert(voice_hold.phase == VOICE_RECORDING);
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    pad.touch_id++; pad.touches = 1;
    route_pad(&pad, now + 952000, NULL);
    assert(voice_hold.phase == VOICE_RECORDING);
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(pw_title_voice_overlay() == overlay_session); /* Recording stays visible despite motion. */
    assert(voice_setting_line_count == 1); /* First-press path log is lifetime-once. */
    voice_result = (struct pw_voice_result){.text = "hello"};
    voice_result_ready = 1; voice_tick(now + 901000);
    expect_voice(0, 0, "hello");
    /* Saturation and lock contention retain the desired edit and mirror.
     * Hardware queue overflow/reset cannot evict any admitted voice batch. */
    union ps5_title_input empty;
    struct ps5_desktop_voice_edit *batch = (void *)&empty;
    memset(batch, 0, sizeof(*batch));
    batch->header = (struct ps5_desktop_header){PS5_DESKTOP_VOICE_EDIT, sizeof(*batch)};
    for (unsigned i = 0; i < VOICE_QUEUE_SIZE; i++) assert(voice_admit(batch));
    voice_result = (struct pw_voice_result){.text = "helloworld"};
    voice_result_ready = 1; voice_tick(now + 902000);
    assert(voice_dirty && voice_typed_length == 5 && voice_count == VOICE_QUEUE_SIZE);
    struct ps5_desktop_key hardware = {{PS5_DESKTOP_KEY, sizeof(hardware)}, 4, 1, 0};
    for (unsigned i = 0; i <= INPUT_QUEUE_SIZE; i++) enqueue(&hardware);
    assert(voice_count == VOICE_QUEUE_SIZE);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && event.header.type == PS5_TITLE_INPUT_RESET);
    expect_voice(0, 0, "");
    pthread_mutex_lock(&input_lock);
    voice_tick(now + 903000);
    assert(voice_dirty && voice_typed_length == 5);
    pthread_mutex_unlock(&input_lock);
    voice_tick(now + 904000);
    assert(!voice_dirty && voice_typed_length == 10);
    for (unsigned i = 1; i < VOICE_QUEUE_SIZE; i++) expect_voice(0, 0, "");
    expect_voice(0, 0, "world");
    while (pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0)) {}
    pad.code = 0; route_pad(&pad, now + 1000000, NULL);
    assert(voice_finishes == 1);
    assert(!pw_title_voice_overlay()); /* Release hides before final text admission. */
    voice_result = (struct pw_voice_result){.final = 1, .text = "help"};
    voice_result_ready = 1; voice_tick(now + 1001000);
    expect_voice(0, 7, "p");
    assert(voice_pending.active);
    assert(!pw_title_voice_overlay()); /* Pending text is never a visible cue. */
    pad.code = PAD_R1; route_pad(&pad, now + 1002000, NULL);
    voice_tick(now + 1002000);
    expect_voice(PS5_DESKTOP_VOICE_ESCAPE_BEFORE | PS5_DESKTOP_VOICE_ENTER_BEFORE, 0, "/s help");
    ime_show_result = -1;
    pad.code = PAD_SQUARE; route_pad(&pad, now + 1003000, NULL);
    voice_tick(now + 1003000);
    assert(!voice_ime && !osk_open && voice_pending.active && voice_pending.prefix == 1);
    pad.code = 0; route_pad(&pad, now + 1003100, NULL);
    ime_show_result = 0;
    pad.code = PAD_SQUARE; route_pad(&pad, now + 1003200, NULL);
    voice_tick(now + 1003200);
    assert(voice_ime && osk_open && !strcmp(ime_initial_text, "/s help"));
    struct ime_edit capacity_edit = {.length = VOICE_TEXT_UNITS};
    memset(ime_initial_text, 'a', VOICE_TEXT_UNITS);
    assert(!pw_title_voice_overlay());
    ime_initial_text[VOICE_TEXT_UNITS] = 0;
    route_ime(&capacity_edit); voice_tick(now + 1003300);
    expect_voice(0, 7, ime_initial_text);
    assert(voice_typed_length == VOICE_TEXT_UNITS);
    memcpy(ime_initial_text + VOICE_TEXT_UNITS - 2, "\xf0\x9f\x98\x80", 5);
    route_ime(&capacity_edit); voice_tick(now + 1003400);
    expect_voice(0, 2, "\xf0\x9f\x98\x80");
    assert(voice_typed_length == VOICE_TEXT_UNITS && voice_typed[511] == 0xde00);
    strcpy(ime_initial_text, "/s help");
    route_ime(&capacity_edit); voice_tick(now + 1003500);
    expect_voice(0, 511, "/s help");
    strcpy(ime_initial_text, "/s hel!");
    struct ime_edit edit = {.erase = 1, .length = 1, .text = {'!'}, .enter = 1};
    route_ime(&edit); voice_tick(now + 1004000);
    expect_voice(PS5_DESKTOP_VOICE_ENTER_AFTER, 1, "!");
    assert(!voice_chat_open && !voice_ime);
    osk_open = 0;
    pad.code = 0; route_pad(&pad, now + 1100000, NULL);
    pad.code = PAD_TOUCHPAD; route_pad(&pad, now + 1200000, NULL);
    route_pad(&pad, now + 1600000, NULL); voice_tick(now + 1600000);
    expect_voice(PS5_DESKTOP_VOICE_ENTER_BEFORE, 0, "");
    voice_result = (struct pw_voice_result){.final = 1, .muted = 1};
    voice_result_ready = 1; voice_tick(now + 1601000);
    expect_voice(PS5_DESKTOP_VOICE_ESCAPE_AFTER, 0, "");
    assert(!voice_pending.active && !voice_chat_open && voice_pulses == 3);
    assert(!pw_title_voice_overlay()); /* A failed/muted session does not leave the cue visible. */
    pad.code = 0; route_pad(&pad, now + 1700000, NULL);
    /* A physical key takes over a live draft: no stale voice mutation or
     * Escape is allowed to clobber the user's text, including queued edits. */
    pad.code = PAD_TOUCHPAD; route_pad(&pad, now + 1800000, NULL);
    route_pad(&pad, now + 2200000, NULL); voice_tick(now + 2200000);
    expect_voice(PS5_DESKTOP_VOICE_ENTER_BEFORE, 0, "");
    voice_result = (struct pw_voice_result){.text = "hello"};
    voice_result_ready = 1; voice_tick(now + 2201000);
    assert(voice_count == 1);
    struct input_event key = {.kind = INPUT_KEY, .code = 0x2a, .down = 1};
    on_input(&key, NULL);
    assert(!voice_chat_open && !voice_pending.active);
    assert(!pw_title_voice_overlay()); /* Physical keyboard takeover hides immediately. */
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.header.type == PS5_DESKTOP_KEY && event.key.usage == 0x2a);
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    key.down = 0; on_input(&key, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.key.down);
    pad.code = 0; route_pad(&pad, now + 2300000, NULL);
    /* Crossing the movement threshold replays down THEN all deferred motion. */
    pad.touch_x = pad.touch_y = 100; pad.touch_id = 7;
    pad.code = PAD_TOUCHPAD; route_pad(&pad, now + 2400000, NULL);
    pad.touch_x = 181; route_pad(&pad, now + 2500000, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && event.mouse.down);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_MOVE | PS5_TITLE_MOUSE_RELATIVE) && event.mouse.x == 162);
    pad.code = 0; route_pad(&pad, now + 2501000, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.mouse.down);
    assert(!strcmp(logged_line, "voice hold enabled=1 result=drag held_ms=101 max_move=81 setting_rc=0"));
    /* Voice disabled preserves the raw touchpad click even with IME open. */
    voice_test_setting = "voice=off"; osk_open = 1;
    pad.code = PAD_TOUCHPAD; route_pad(&pad, now + 2600000, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && event.mouse.down);
    pad.code = 0; route_pad(&pad, now + 2601000, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.mouse.down);
    osk_open = 0; voice_test_setting = NULL;
    /* The errno diagnostics distinguish a game-parent open failure from
     * a valid voice=off file, without exposing any setting file content. */
    voice_setting_logged = 0;
    voice_fopen_errno = EACCES;
    pad.code = PAD_TOUCHPAD; route_pad(&pad, now + 2700000, NULL);
    assert(voice_setting_rc == EACCES && voice_setting_line_count == 2);
    assert(strstr(logged_line, "voice setting path="));
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && event.mouse.down);
    pad.code = 0; route_pad(&pad, now + 2701000, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.mouse.down);
    assert(strstr(logged_line, "voice hold enabled=0 result=passthrough held_ms=1 max_move=0 setting_rc="));
    int setting_rc = -1;
    voice_fopen_errno = ENOENT;
    assert(!voice_enabled(&setting_rc) && setting_rc == ENOENT);
}

int main(void)
{
    union ps5_title_input event;
    struct ps5_desktop_key key = {{PS5_DESKTOP_KEY, sizeof(key)}, 4, 1, 0};
    struct input_event device = {.kind = INPUT_MOUSE_MOVE, .dx = -100, .dy = 75};
    struct pointer_event pointer = {.kind = POINTER_MOVE};
    {
        const unsigned chord = PAD_L3 | PAD_R3;
        assert(!osk_chord_update(PAD_L3 | PAD_CROSS, 1, 0));
        assert(!osk_chord_update(chord | PAD_CROSS, 1, 100));
        assert(!osk_chord_update(chord, 1, 2000000)); /* no one-second HUD action */
        /* A one-sample drop inside the hold is a flicker, not a release. */
        assert(!osk_chord_update(PAD_R3, 1, 2000001));
        assert(!osk_chord_update(chord, 1, 2000002));
        assert(!osk_chord_update(0, 1, 3000000));
        assert(!osk_chord_update(0, 1, 3039999));
        assert(osk_chord_update(0, 1, 3040000) == 1);
        /* Disconnect cancels rather than opening the keyboard. */
        assert(!osk_chord_update(chord, 1, 4000000));
        assert(!osk_chord_update(chord, 0, 5000000));
        assert(!osk_chord_update(0, 1, 5100000));
    }
    pointer_init(PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT);
    test_keyboard_logging();
    {
        uint32_t image[CURSOR_SLOT_BYTES / sizeof(uint32_t)];
        cursor_build_image(image);
        for (unsigned y = 0; y < CURSOR_IMAGE_SIDE; y++)
            for (unsigned x = 0; x < CURSOR_IMAGE_SIDE; x++)
            {
                uint32_t expected = 0;
                if (y < PS5_CURSOR_HEIGHT && x < PS5_CURSOR_WIDTH)
                {
                    char pixel = ps5_cursor_arrow[y][x];
                    expected = pixel == '#' ? 0xff000000u :
                               pixel == '.' ? 0xffffffffu : 0;
                }
                assert(image[y * CURSOR_IMAGE_SIDE + x] == expected);
            }
    }
    assert(!pw_title_read_input(&event, sizeof(event), 1920, 1080, 3840, 2160));
    assert(cursor_x == 960 && cursor_y == 540);
    enqueue(&key);
    assert(!pw_title_read_input(&event, sizeof(event) - 1, 0, 0, 0, 0));
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.key.usage == 4 && event.key.down == 1);
    for (unsigned i = 0; i <= INPUT_QUEUE_SIZE; i++) enqueue(&key);
    assert(count == 1);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.header.type == PS5_TITLE_INPUT_RESET);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.header.type == PS5_DESKTOP_KEY);
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    enqueue(&key);
    pthread_mutex_lock(&input_lock);
    enqueue(&key); /* must return without waiting on our held mutex */
    pthread_mutex_unlock(&input_lock);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.header.type == PS5_TITLE_INPUT_RESET);
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    on_input(&device, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_MOVE | PS5_TITLE_MOUSE_RELATIVE));
    assert(event.mouse.x == -100 && event.mouse.y == 75);
    on_pointer(&pointer, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.mouse.kind == PS5_DESKTOP_MOUSE_MOVE);
    device = (struct input_event){.kind = INPUT_KEY, .code = 0x45, .down = 1};
    on_input(&device, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.key.usage == 0x45 && event.key.down);
    device.down = 0;
    on_input(&device, NULL);
    assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    assert(event.key.usage == 0x45 && !event.key.down);
    user = 1;
    device = (struct input_event){.kind = INPUT_PAD, .connected = 1, .code = PAD_L2,
        .lx = 128, .ly = 128, .rx = 128, .ry = 128, .l2 = 201};
    on_input(&device, NULL);
    assert(!osk_calls); /* L2 is a real trigger, never the GPU shortcut. */
    assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));

    {
        /* The shared desktop pointer (Battle.net): L3+R3 toggle the
         * keyboard once on release. L2 owns Escape; Options and R2 are
         * unmapped so the maintenance title can consume Options itself. */
        struct input_event pad = {.kind = INPUT_PAD, .connected = 1,
            .lx = 128, .ly = 128, .rx = 128, .ry = 128};
        static const unsigned sequence[] = {PAD_L2, PAD_L2 | PAD_R2, PAD_R2, 0,
            PAD_OPTIONS, 0, PAD_L3, PAD_L3 | PAD_R3, PAD_L3 | PAD_R3, PAD_R3, 0};
        text_events = escape_down = escape_up = 0;
        for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++)
        {
            pad.code = sequence[i];
            pointer_input(&pad, count_shortcuts, NULL);
            assert(text_events == (i >= 9));
        }
        pointer_options_escape(1);
        pad.code = PAD_OPTIONS;
        pointer_input(&pad, count_shortcuts, NULL);
        pad.code = 0;
        pointer_input(&pad, count_shortcuts, NULL);
        assert(escape_down == 2 && escape_up == 2);
        pointer_options_escape(0);
        pad.code = PAD_L3 | PAD_R3;
        pointer_input(&pad, count_text, NULL);
        pad = (struct input_event){.kind = INPUT_PAD, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
        pointer_input(&pad, count_text, NULL);
        assert(text_events == 1);
        pad.connected = 1;
        pointer_input(&pad, count_text, NULL);
        assert(text_events == 1);
    }

    {
        struct ime_edit edit = {.erase = 1, .length = 2, .text = {'o', 'k'}, .enter = 1};
        route_ime(&edit);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.header.type == PS5_DESKTOP_KEY && event.key.usage == 0x2a && event.key.down);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.header.type == PS5_DESKTOP_KEY && event.key.usage == 0x2a && !event.key.down);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.header.type == PS5_DESKTOP_TEXT);
        {
            const struct ps5_desktop_text *text = (const void *)&event;
            assert(text->length == 2 && text->text[0] == 'o' && text->text[1] == 'k');
        }
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.header.type == PS5_DESKTOP_KEY && event.key.usage == 0x28 && event.key.down);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.header.type == PS5_DESKTOP_KEY && event.key.usage == 0x28 && !event.key.down);
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));

        edit = (struct ime_edit){.length = 1, .text = {'x'}};
        route_ime(&edit);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.header.type == PS5_DESKTOP_TEXT);
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
    }

    {
        struct wine_ps5_gamepad_state state, same;
        unsigned packet;
        uint64_t lease, now = 2000000;
        assert(sizeof(state) == 16);
        assert(gamepad_axis(0, 0) == -32768 && gamepad_axis(255, 0) == 32767);
        assert(gamepad_axis(0, 1) == 32767 && gamepad_axis(255, 1) == -32767);
        for (unsigned v = 127; v <= 129; v++) assert(!gamepad_axis(v, 0));
        assert(gamepad_axis(126, 0) == -512 && gamepad_axis(130, 0) > 0);
        assert(!pw_title_get_gamepad(0, &state, 0));
        assert(state.pad.left_trigger == 201 && !atomic_load(&gamepad_poll_us));
        assert(pw_title_get_gamepad(1, &state, 1) == 1167);
        assert(pw_title_get_gamepad(4, &state, 1) == 160);
        assert(pw_title_get_gamepad(0, NULL, 1) == 160);
        assert(!atomic_load(&gamepad_poll_us));

        device.code = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_OPTIONS | PAD_CREATE |
                      PAD_L3 | PAD_L1 | PAD_R1 | PAD_CROSS | PAD_CIRCLE | PAD_SQUARE | PAD_TRIANGLE;
        device.lx = 0; device.ly = 255; device.rx = 255; device.ry = 0;
        device.r2 = 255;
        /* Mapping itself has no OSK gating; isolated thumb clicks retain
         * their native XInput bits once the chord window expires. */
        gamepad_update(&device, device.code);
        assert(!pw_title_get_gamepad(0, &state, 0));
        assert(state.pad.buttons == 0xf37f);
        assert(state.pad.lx == -32768 && state.pad.ly == -32767);
        assert(state.pad.rx == 32767 && state.pad.ry == 32767);
        assert(state.pad.left_trigger == 201 && state.pad.right_trigger == 255);
        packet = state.packet;
        gamepad_update(&device, device.code);
        assert(!pw_title_get_gamepad(0, &same, 0) && same.packet == packet);
        gamepad_update(&device, PAD_R3);
        assert(!pw_title_get_gamepad(0, &state, 0) && state.pad.buttons == 0x80);
        assert(state.packet != packet);

        assert(!pw_title_set_gamepad_vibration(0, 65535, 32768));
        assert(motors[0] == 255 && motors[1] == 128);
        assert(pw_title_set_gamepad_vibration(1, 1, 1) == 1167);
        assert(pw_title_set_gamepad_vibration(4, 1, 1) == 160);
        assert(pw_title_set_gamepad_vibration(0, 65536, 1) == 160);
        vibration_result = -1;
        assert(pw_title_set_gamepad_vibration(0, 1, 1) == 50);
        vibration_result = 0;
        assert(!pw_title_set_gamepad_vibration(0, 65535, 32768));
        pw_title_enable_gamepad(0);
        assert(!motors[0] && !motors[1]);
        assert(!pw_title_get_gamepad(0, &state, 0) && !state.pad.buttons && !state.pad.lx);
        packet = vibration_calls;
        assert(!pw_title_set_gamepad_vibration(0, 256, 512));
        assert(vibration_calls == (int)packet);
        pw_title_enable_gamepad(1);
        assert(motors[0] == 1 && motors[1] == 2);

        /* Real pointer routing: fallback click, XInput takeover release,
         * touchpad click/motion preserved, then timeout restores fallback. */
        device = (struct input_event){.kind = INPUT_PAD, .connected = 1, .code = PAD_CROSS,
            .lx = 128, .ly = 128, .rx = 128, .ry = 128};
        pointer_init(PS5_DESKTOP_WIDTH, PS5_DESKTOP_HEIGHT);
        route_pad(&device, now, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.mouse.button == PS5_DESKTOP_BUTTON_LEFT && event.mouse.down);
        atomic_store(&gamepad_poll_us, now);
        route_pad(&device, now + 1, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.mouse.button == PS5_DESKTOP_BUTTON_LEFT && !event.mouse.down);
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(!pw_title_get_gamepad(0, &state, 0) && state.pad.buttons == 0x1000);
        device.code |= PAD_TOUCHPAD;
        device.touches = 1; device.touch_id = 1; device.touch_x = 200; device.touch_y = 200;
        route_pad(&device, now + 2, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && event.mouse.down);
        device.touch_x += 20;
        route_pad(&device, now + 3, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_MOVE | PS5_TITLE_MOUSE_RELATIVE));
        assert(event.mouse.x == 40 && !event.mouse.y);
        device.touches = 0;
        route_pad(&device, now + 4, NULL);
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        device.touches = 1; device.touch_x = 1500; device.touch_y = 600;
        route_pad(&device, now + 5, NULL); /* Lift/re-touch rebases even with the same id. */
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        device.touch_id = 2; device.touch_x = 100; device.touch_y = 100;
        route_pad(&device, now + 6, NULL); /* Replacement finger also rebases. */
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        device.touch_x -= 7; device.touch_y += 3;
        route_pad(&device, now + 7, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_MOVE | PS5_TITLE_MOUSE_RELATIVE));
        assert(event.mouse.x == -14 && event.mouse.y == 6);
        route_pad(&device, now + 8, NULL); /* Replaying the cached sample must not move. */
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        device.code = PAD_CROSS; device.touches = 0;
        route_pad(&device, now + 9, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.mouse.down);
        assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_BUTTON | PS5_TITLE_MOUSE_RELATIVE));
        /* Two fingers still move the raw mouse, and a press is always left:
         * WoW's movie can use this one escape hatch during an XInput lease. */
        device.touches = 2; device.code |= PAD_TOUCHPAD;
        route_pad(&device, now + 10, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_BUTTON | PS5_TITLE_MOUSE_RELATIVE));
        assert(event.mouse.button == PS5_DESKTOP_BUTTON_LEFT && event.mouse.down);
        device.touch_x += 10; device.touch_y -= 5;
        route_pad(&device, now + 11, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_MOVE | PS5_TITLE_MOUSE_RELATIVE));
        assert(event.mouse.x == 20 && event.mouse.y == -10);
        device.touches = 0; device.code = PAD_CROSS;
        route_pad(&device, now + 12, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.mouse.down);
        route_pad(&device, now + 1000000, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && event.mouse.down);
        device.touches = 1; device.touch_id = 3; device.touch_x = 500; device.touch_y = 500;
        route_pad(&device, now + 1000001, NULL);
        assert(!pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        device.touch_x += 2; device.touch_y += 4;
        route_pad(&device, now + 1000002, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0));
        assert(event.mouse.kind == (PS5_DESKTOP_MOUSE_MOVE | PS5_TITLE_MOUSE_RELATIVE));
        assert(event.mouse.x == 4 && event.mouse.y == 8); /* Also relative outside XInput mode. */
        device.touches = 0;
        pw_title_get_gamepad(0, &state, 1);
        lease = atomic_load(&gamepad_poll_us);
        assert(lease && gamepad_mode(lease) && !gamepad_mode(lease + 1000000));
        pw_title_enable_gamepad(0);
        assert(!gamepad_mode(lease));
        pw_title_enable_gamepad(1);
        device.code = 0;
        route_pad(&device, lease, NULL);
        assert(pw_title_read_input(&event, sizeof(event), 0, 0, 0, 0) && !event.mouse.down);

        /* Short chord opens OSK only on release; both thumb buttons remain
         * swallowed, other buttons/triggers are unchanged before OSK opens. */
        now = lease + 2000000;
        device.code = PAD_L3;
        route_pad(&device, now, NULL);
        assert(!pw_title_get_gamepad(0, &state, 0) && !state.pad.buttons);
        device.code |= PAD_R3 | PAD_CROSS;
        device.l2 = 255; device.lx = 0;
        route_pad(&device, now + 4000, NULL);
        assert(!osk_calls && !osk_open);
        assert(!pw_title_get_gamepad(0, &state, 0));
        assert(state.pad.buttons == 0x1000 && state.pad.left_trigger == 255);
        device.code = PAD_R3;
        route_pad(&device, now + 8000, NULL);
        assert(!osk_calls && !osk_open);
        route_pad(&device, now + 48000, NULL);
        assert(osk_calls == 1 && osk_open);
        assert(!strcmp(logged_line, "title pad chord action=osk"));
        assert(!pw_title_get_gamepad(0, &state, 0));
        assert(!state.pad.buttons && !state.pad.left_trigger && !state.pad.lx);
        osk_open = 0;
        device.code = 0;
        route_pad(&device, now + 52000, NULL);
        device.code = PAD_L3;
        route_pad(&device, now + 60000, NULL);
        route_pad(&device, now + 140000, NULL);
        assert(!pw_title_get_gamepad(0, &state, 0) && state.pad.buttons == 0x40);
        device.code = 0;
        route_pad(&device, now + 144000, NULL);
        device.code = PAD_R3;
        route_pad(&device, now + 148000, NULL);
        device.code = 0;
        route_pad(&device, now + 152000, NULL);
        assert(!pw_title_get_gamepad(0, &state, 0) && state.pad.buttons == 0x80);
        route_pad(&device, now + 232000, NULL);
        assert(!pw_title_get_gamepad(0, &state, 0) && !state.pad.buttons);
        device.connected = 0;
        route_pad(&device, now + 236000, NULL);
        assert(pw_title_get_gamepad(0, &state, 0) == 1167);
        assert(pw_title_set_gamepad_vibration(0, 0, 0) == 1167);

        /* Hardware cursor: shown on motion, hidden after 3 s idle, shown
         * again on motion; gamepad use alone does not hide it. */
        {
            static uint32_t image[2 * CURSOR_SLOT_BYTES / sizeof(uint32_t)];
            uint64_t t = now + 300000000ull;
            cursor_image = image;
            cursor_open = 1;
            pointer_set_position(100, 100);
            cursor_update(t);
            assert(cursor_shown);
            cursor_update(t + 2999999);
            assert(cursor_shown);
            cursor_update(t + 3000000);
            assert(!cursor_shown);
            pointer_set_position(101, 100);
            cursor_update(t + 3000001);
            assert(cursor_shown);
            atomic_store(&gamepad_poll_us, t + 3000002);
            cursor_update(t + 3000003);
            assert(cursor_shown);
            cursor_update(t + 6000001);
            assert(!cursor_shown);
            cursor_image = NULL;
        }
    }
    test_cursor_shapes();
    test_voice_routing();
    puts("runtime GPU input queue/routing/XInput/IME: PASS");
    return 0;
}
