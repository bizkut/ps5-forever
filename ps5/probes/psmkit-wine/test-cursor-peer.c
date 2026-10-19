/* Complete hub/transport functions run on the host; only OS/title boundaries differ. */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wine/ps5_desktop.h>
#define PW_HUB_MAX_FRAME PS5_DESKTOP_MAX_MESSAGE
#define PS5_HUB_MAX_FRAME PS5_DESKTOP_MAX_MESSAGE
#define PS5_HUB_MAX_OUTPUT (1024 * 1024)
#define PS5_TITLE_SURFACE_UNLINK 3
#define io_log(...) ((void)0)
#include "cursor-native-types.inc"
#include "cursor-payload-types.inc"
static unsigned char output[1024 * 1024];
static size_t output_size;
static void *last_peer;
static int capture(void *peer, const void *message, size_t size)
{
    assert(output_size + size <= sizeof(output));
    memcpy(output + output_size, message, size);
    output_size += size;
    last_peer = peer;
    return 1;
}
static int hub_queue_title(const void *message, size_t size) { return capture(NULL, message, size); }
static size_t hub_title_space(void) { return sizeof(output) - output_size; }
static int hub_queue_peer(struct pw_hub_peer *peer, const void *message, size_t size)
{ return capture(peer, message, size); }
static int ps5_hub_queue(struct ps5_hub_peer *peer, const void *message, size_t size)
{
    if (peer) peer->output_end += size;
    return capture(peer, message, size);
}
static int pw_desktop_surface(const char *name, uint64_t size, int op)
{ (void)name; (void)size; assert(op == PS5_TITLE_SURFACE_UNLINK); return 0; }
static int shm_unlink(const char *name) { (void)name; return 0; }
#include "cursor-native-functions.inc"
#include "cursor-payload-functions.inc"

static void check_replay(void)
{
    size_t offset = 0;
    unsigned windows = 0, shapes = 0, desktop = 0, hidden = 0;
    while (offset < output_size)
    {
        struct ps5_desktop_header header;
        memcpy(&header, output + offset, sizeof(header));
        assert(header.size >= sizeof(header) && header.size <= output_size - offset);
        if (header.type == PS5_DESKTOP_WINDOW) windows++;
        if (header.type == PS5_DESKTOP_CURSOR_SHAPE)
        {
            struct ps5_desktop_cursor_shape shape;
            assert(windows == 2); /* replay windows before their cursor states */
            assert(ps5_desktop_cursor_shape_valid(output + offset, header.size));
            memcpy(&shape, output + offset, sizeof(shape));
            shapes++;
            desktop += !shape.id;
            hidden += shape.id == 200 && !shape.visible;
            if (shape.id == 100)
            {
                uint32_t pixel;
                memcpy(&pixel, output + offset + sizeof(shape), sizeof(pixel));
                assert(pixel == 0xff112233); /* replay the latest accepted image */
            }
        }
        offset += header.size;
    }
    assert(windows == 2 && shapes == 3 && desktop == 1 && hidden == 1);
}
#define TEST_HUB(HUB, PEER, DRIVER, REPLAY, REMOVE, SETUP) do { \
    struct HUB hub = {0}; \
    struct PEER a = {.fd = 123}, b = {.fd = 124}, title = {.fd = 125}; \
    SETUP; \
    a.input.hello = (struct ps5_desktop_hello){{PS5_DESKTOP_HELLO, sizeof(a.input.hello)}, 10, PS5_DESKTOP_CURSOR_CAPABLE}; \
    output_size = 0; \
    assert(DRIVER(&hub, &a)); \
    assert(output_size == sizeof(struct ps5_desktop_hello) && last_peer == &a); \
    struct ps5_desktop_hello reply; memcpy(&reply, output, sizeof(reply)); \
    assert(reply.header.type == PS5_DESKTOP_HELLO && !reply.pid && (reply.flags & PS5_DESKTOP_CURSOR_CAPABLE)); \
    b.input.hello = (struct ps5_desktop_hello){{PS5_DESKTOP_HELLO, sizeof(b.input.hello)}, 20, 0}; \
    output_size = 0; assert(DRIVER(&hub, &b)); assert(!output_size); /* legacy driver gets no new reply */ \
    a.input.window = (struct ps5_desktop_window){.header = {PS5_DESKTOP_WINDOW, sizeof(a.input.window)}, .id = 100}; \
    b.input.window = a.input.window; b.input.window.id = 200; \
    assert(DRIVER(&hub, &a) && DRIVER(&hub, &b)); \
    struct ps5_desktop_cursor_shape *shape = (void *)a.input.bytes; \
    *shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape) + 4}, \
        .id = 100, .width = 1, .height = 1, .visible = 1}; \
    shape->pixels[0] = 0xffff0000; assert(DRIVER(&hub, &a)); \
    shape->pixels[0] = 0xff112233; assert(DRIVER(&hub, &a)); \
    shape = (void *)b.input.bytes; \
    *shape = (struct ps5_desktop_cursor_shape){.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(*shape)}, .id = 200}; \
    assert(DRIVER(&hub, &b)); \
    shape = (void *)a.input.bytes; shape->id = 0; \
    assert(DRIVER(&hub, &a)); \
    output_size = 0; REPLAY(&hub); check_replay(); \
    shape->width = 65; output_size = 0; assert(!DRIVER(&hub, &a) && !output_size); \
    shape->width = 1; shape->id = 999; output_size = 0; assert(DRIVER(&hub, &a) && !output_size); \
    while (hub.top) REMOVE(&hub, hub.top); \
    free(hub.desktop_cursor); \
} while (0)

static void drain_replay_stage(unsigned *windows, unsigned *shapes, unsigned char seen[128])
{
    size_t offset = 0;
    while (offset < output_size)
    {
        struct ps5_desktop_header header;
        memcpy(&header, output + offset, sizeof(header));
        assert(header.size <= output_size - offset);
        if (header.type == PS5_DESKTOP_WINDOW) (*windows)++;
        if (header.type == PS5_DESKTOP_CURSOR_SHAPE)
        {
            struct ps5_desktop_cursor_shape shape;
            uint32_t pixel;
            memcpy(&shape, output + offset, sizeof(shape));
            assert(ps5_desktop_cursor_shape_valid(output + offset, header.size));
            assert(shape.id >= 1000 && shape.id < 1128 && !seen[shape.id - 1000]++);
            memcpy(&pixel, output + offset + sizeof(shape), sizeof(pixel));
            assert(pixel == (0xff000000u | (unsigned)(shape.id - 1000)));
            (*shapes)++;
        }
        offset += header.size;
    }
    assert(output_size <= 1024 * 1024);
    output_size = 0;
}
static void drain_fair_replay(unsigned char seen[128], unsigned *desktop_seen)
{
    for (size_t offset = 0; offset < output_size; )
    {
        struct ps5_desktop_header header;
        memcpy(&header, output + offset, sizeof(header));
        if (header.type == PS5_DESKTOP_CURSOR_SHAPE)
        {
            struct ps5_desktop_cursor_shape shape;
            memcpy(&shape, output + offset, sizeof(shape));
            if (!shape.id) (*desktop_seen)++;
            else { assert(shape.id >= 1000 && shape.id < 1128); seen[shape.id - 1000] = 1; }
        }
        offset += header.size;
    }
    output_size = 0;
}
#define TEST_FULL_REPLAY(HUB, PEER, WINDOW, REPLAY, CONTINUE, DRIVER, REMOVE, SETUP) do { \
    struct HUB hub = {0}; \
    struct PEER title = {.fd = 125}; \
    SETUP; \
    for (unsigned i = 0; i < 128; i++) { \
        struct WINDOW *window = calloc(1, sizeof(*window)); assert(window); \
        window->state = (struct ps5_desktop_window){ \
            .header = {PS5_DESKTOP_WINDOW, sizeof(window->state)}, .id = 1000 + i, \
            .width = 64, .height = 64, .visible_right = 64, .visible_bottom = 64, \
            .flags = PS5_DESKTOP_VISIBLE}; \
        window->cursor = malloc(PS5_DESKTOP_MAX_MESSAGE); assert(window->cursor); \
        *window->cursor = (struct ps5_desktop_cursor_shape){ \
            .header = {PS5_DESKTOP_CURSOR_SHAPE, PS5_DESKTOP_MAX_MESSAGE}, .id = 1000 + i, \
            .width = 64, .height = 64, .visible = 1}; \
        for (unsigned p = 0; p < 64 * 64; p++) window->cursor->pixels[p] = 0xff000000u | i; \
        window->below = hub.top; \
        if (hub.top) hub.top->above = window; else hub.bottom = window; \
        hub.top = window; \
    } \
    unsigned windows = 0, shapes = 0; unsigned char seen[128] = {0}; \
    output_size = 0; REPLAY(&hub); \
    for (unsigned stalled = 0; stalled < 128; stalled++) CONTINUE(&hub); \
    assert(hub.replay_cursors && title.fd == 125); /* backpressure, not disconnect */ \
    for (unsigned pass = 0; pass < 128; pass++) { \
        drain_replay_stage(&windows, &shapes, seen); \
        title.output_begin = title.output_end; \
        if (!hub.replay_cursors) break; \
        CONTINUE(&hub); \
    } \
    assert(windows == 128 && shapes == 128 && title.fd == 125); \
    hub.desktop_cursor = malloc(PS5_DESKTOP_MAX_MESSAGE); assert(hub.desktop_cursor); \
    *hub.desktop_cursor = (struct ps5_desktop_cursor_shape){ \
        .header = {PS5_DESKTOP_CURSOR_SHAPE, PS5_DESKTOP_MAX_MESSAGE}, \
        .width = 64, .height = 64, .visible = 1}; \
    memset(hub.desktop_cursor->pixels, 0xff, 64 * 64 * 4); \
    memset(seen, 0, sizeof(seen)); \
    struct PEER live = {.fd = 126, .hello = 1}; \
    unsigned desktop_seen = 0, fair_count = 0; \
    REPLAY(&hub); \
    for (unsigned batch = 0; batch < 48; batch++) { \
        drain_fair_replay(seen, &desktop_seen); \
        title.output_begin = title.output_end; \
        fair_count = 0; for (unsigned i = 0; i < 128; i++) fair_count += !!seen[i]; \
        if (fair_count == 128 && desktop_seen) break; \
        struct WINDOW *first = hub.top; \
        for (unsigned hot = 0; hot < 3; hot++, first = first->below) { \
            memcpy(live.input.bytes, first->cursor, PS5_DESKTOP_MAX_MESSAGE); \
            struct ps5_desktop_cursor_shape *changed = (void *)live.input.bytes; \
            changed->pixels[0] = 0xff800000u | (batch + 1); \
            assert(DRIVER(&hub, &live)); \
        } \
        CONTINUE(&hub); \
    } \
    assert(fair_count == 128 && desktop_seen && title.fd == 125); \
    free(hub.desktop_cursor); \
    while (hub.top) REMOVE(&hub, hub.top); \
} while (0)

typedef int BOOL;
typedef unsigned DWORD;
typedef void *HWND;
typedef void *HANDLE;
typedef int NTSTATUS;
typedef uint16_t WCHAR;
typedef struct { unsigned short Length; const WCHAR *Buffer; } UNICODE_STRING;
struct test_params { UNICODE_STRING ImagePathName; };
struct test_peb { struct test_params *ProcessParameters; };
struct test_teb { struct test_peb *Peb; };
static struct test_params process_params;
static struct test_peb process_peb = {&process_params};
static struct test_teb process_teb = {&process_peb};
static struct test_teb *NtCurrentTeb(void) { return &process_teb; }
#define UOI_NAME 2
#define TRUE 1
#define FALSE 0
static pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;
static int desktop_fd, cursor_capable, cursor_capability_checked, writes;
static DWORD desktop_tid, event_tid;
static unsigned registrations, delivered_input, registered_threads, sink_hellos;
#include "cursor-queue-types.inc"
static struct queue_thread *queue_threads;
static unsigned cursor_pending_size;
static union {
    struct ps5_desktop_cursor_shape shape;
    unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
} cursor_pending_message;
static _Thread_local DWORD current_tid = 42;
static DWORD GetCurrentThreadId(void) { return current_tid; }
static DWORD GetCurrentProcessId(void) { return 10; }
static DWORD NtUserGetWindowThread(HWND hwnd, DWORD *pid)
{ (void)hwnd; *pid = GetCurrentProcessId(); return GetCurrentThreadId(); }
static HWND NtUserGetThreadDesktop(DWORD tid) { return (HWND)(uintptr_t)tid; }
static int NtUserGetObjectInformation(HWND hwnd, int kind, void *buffer, unsigned size, void *needed)
{ (void)hwnd; (void)kind; (void)buffer; (void)size; (void)needed; return 1; }
static const char *debugstr_wn(const WCHAR *text, unsigned size) { (void)text; (void)size; return "host"; }
static const char *debugstr_w(const WCHAR *text) { (void)text; return "host"; }
#define GENERIC_READ 1
#define SYNCHRONIZE 2
#define ERR(...) ((void)0)
struct queue_request { uintptr_t handle; };
#define SERVER_START_REQ(name) do { struct queue_request request = {0}, *req = &request;
#define SERVER_END_REQ } while (0)
static NTSTATUS wine_server_fd_to_handle(int fd, unsigned access, unsigned attributes, HANDLE *handle)
{
    assert(fd == desktop_fd && access == (GENERIC_READ | SYNCHRONIZE) && !attributes);
    *handle = (HANDLE)1;
    return 0;
}
static uintptr_t wine_server_obj_handle(HANDLE handle) { assert(handle == (HANDLE)1); return 1; }
static NTSTATUS wine_server_call(struct queue_request *request)
{
    unsigned bit = GetCurrentThreadId() == 42 ? 1 : 2;
    assert(request->handle == 1);
    registered_threads |= bit;
    registrations++;
    return 0;
}
static void NtClose(HANDLE handle) { assert(handle == (HANDLE)1); }
static void NtTerminateProcess(HANDLE process, unsigned status) { (void)process; (void)status; abort(); }
static void send_mouse(const struct ps5_desktop_mouse *mouse) { (void)mouse; delivered_input++; }
static void send_key(const struct ps5_desktop_key *key) { (void)key; delivered_input++; }
static void send_text(const struct ps5_desktop_text *text) { (void)text; delivered_input++; }
static unsigned voice_delivered;
static void send_voice_edit(const struct ps5_desktop_voice_edit *edit)
{
    assert(ps5_desktop_voice_edit_valid(edit, edit->header.size));
    assert(edit->erase == PS5_DESKTOP_MAX_TEXT && edit->length == PS5_DESKTOP_MAX_TEXT);
    assert(edit->text[PS5_DESKTOP_MAX_TEXT - 1] == 'a');
    delivered_input++;
    voice_delivered++;
}
static BOOL processing_events;
static union {
    struct ps5_desktop_header header;
    struct ps5_desktop_hello hello;
    struct ps5_desktop_mouse mouse;
    struct ps5_desktop_key key;
    unsigned char bytes[PS5_DESKTOP_VOICE_MAX_MESSAGE];
} input_event;
static unsigned input_size;
static uint64_t written_cursor_id;
static BOOL connect_desktop(void) { return TRUE; }
static BOOL write_message(int fd, const void *message, unsigned size)
{
    const struct ps5_desktop_header *header = message;
    if (header->type == PS5_DESKTOP_HELLO)
    {
        const struct ps5_desktop_hello *hello = message;
        assert(size == sizeof(*hello) && (hello->flags & PS5_DESKTOP_INPUT_SINK));
        sink_hellos++;
        return TRUE;
    }
    assert(fd == desktop_fd && ps5_desktop_cursor_shape_valid(message, size));
    struct ps5_desktop_cursor_shape shape;
    memcpy(&shape, message, sizeof(shape));
    written_cursor_id = shape.id;
    writes++;
    return TRUE;
}
static void connection_lost(void) { abort(); }
#include "cursor-transport.inc"
static BOOL registered_here(void)
{
    BOOL registered;
    pthread_mutex_lock(&connection_mutex);
    registered = queue_is_registered(GetCurrentThreadId());
    pthread_mutex_unlock(&connection_mutex);
    return registered;
}

static void test_capabilities(void)
{
    int pair[2];
    struct ps5_desktop_cursor_shape hidden = {.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(hidden)}};
    struct ps5_desktop_hello reply = {{PS5_DESKTOP_HELLO, sizeof(reply)}, 0, PS5_DESKTOP_CURSOR_CAPABLE};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    desktop_fd = pair[0];
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && !writes); /* old hub: no answer */
    struct ps5_desktop_mouse mouse = {.header = {PS5_DESKTOP_MOUSE, sizeof(mouse)}};
    assert(send(pair[1], &mouse, sizeof(mouse), 0) == sizeof(mouse));
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && !writes);
    assert(!input_size); /* probing must not consume an input wakeup */
    struct ps5_desktop_mouse pending_mouse;
    assert(recv(desktop_fd, &pending_mouse, sizeof(pending_mouse), MSG_PEEK | MSG_DONTWAIT) == sizeof(mouse));
    assert(pending_mouse.header.type == PS5_DESKTOP_MOUSE);
    assert(read_event() == 1 && input_size == sizeof(mouse));
    input_size = 0; /* the normal message pump, not the probe, consumes it */
    assert(send(pair[1], &reply, 5, 0) == 5);
    processing_events = TRUE;
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && !input_size && !writes);
    processing_events = FALSE;
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && !writes); /* fragmented answer */
    assert(send(pair[1], (char *)&reply + 5, sizeof(reply) - 5, 0) == sizeof(reply) - 5);
    assert(ps5_send_cursor_message(&hidden, sizeof(hidden)) && writes == 1);
    assert(ps5_send_cursor_message(&hidden, sizeof(hidden)) && writes == 2);
    close(pair[0]); close(pair[1]);
}
static void test_late_capability_publication(void)
{
    int pair[2];
    struct ps5_desktop_cursor_shape hidden = {.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(hidden)}, .id = 200};
    struct ps5_desktop_hello reply = {{PS5_DESKTOP_HELLO, sizeof(reply)}, 0, PS5_DESKTOP_CURSOR_CAPABLE};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    desktop_fd = pair[0];
    cursor_capable = FALSE; cursor_capability_checked = TRUE;
    input_size = writes = 0;
    PS5_ThreadDetach();
    event_tid = registrations = 0;
    registered_threads = 0;
    processing_events = TRUE;
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && !writes);
    assert(cursor_pending_size == sizeof(hidden) && registrations == 1 && registered_here());
    hidden.id = 300;
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && !writes);
    assert(send(pair[1], &reply, 5, 0) == 5);
    processing_events = FALSE;
    assert(PS5_ProcessEvents(0));
    assert(!writes && input_size == 5); /* incomplete reply retains pending cursor */
    assert(send(pair[1], (char *)&reply + 5, sizeof(reply) - 5, 0) == sizeof(reply) - 5);
    assert(PS5_ProcessEvents(0));
    assert(writes == 1 && !cursor_pending_size && written_cursor_id == 300); /* latest pending state */
    close(pair[0]); close(pair[1]);
}

static void test_split_old_input(void)
{
    int pair[2];
    struct ps5_desktop_cursor_shape hidden = {.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(hidden)}};
    /* Its payload deliberately resembles a complete capability HELLO. */
    struct ps5_desktop_mouse mouse = {
        .header = {PS5_DESKTOP_MOUSE, sizeof(mouse)}, .kind = PS5_DESKTOP_HELLO,
        .x = sizeof(struct ps5_desktop_hello), .button = PS5_DESKTOP_CURSOR_CAPABLE};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    desktop_fd = pair[0]; input_size = writes = delivered_input = 0;
    cursor_capable = FALSE; cursor_capability_checked = TRUE;
    assert(send(pair[1], &mouse, sizeof(mouse.header), 0) == sizeof(mouse.header));
    assert(!read_event() && input_size == sizeof(mouse.header));
    assert(send(pair[1], (char *)&mouse + sizeof(mouse.header), sizeof(mouse) - sizeof(mouse.header), 0) ==
           sizeof(mouse) - sizeof(mouse.header));
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && !writes && !cursor_capable);
    assert(input_size == sizeof(mouse.header));
    assert(PS5_ProcessEvents(0) && delivered_input == 1 && !input_size && !cursor_capable);
    close(pair[0]); close(pair[1]);
}

static void test_split_voice_input(void)
{
    int pair[2];
    unsigned char bytes[PS5_DESKTOP_VOICE_MAX_MESSAGE];
    struct ps5_desktop_voice_edit *edit = (void *)bytes;
    memset(edit, 0, sizeof(*edit));
    edit->header = (struct ps5_desktop_header){PS5_DESKTOP_VOICE_EDIT, sizeof(bytes)};
    edit->flags = PS5_DESKTOP_VOICE_ESCAPE_BEFORE | PS5_DESKTOP_VOICE_ENTER_AFTER;
    edit->erase = edit->length = PS5_DESKTOP_MAX_TEXT;
    for (unsigned i = 0; i < edit->length; i++) edit->text[i] = 'a';
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    desktop_fd = pair[0]; input_size = delivered_input = voice_delivered = 0;
    processing_events = FALSE;
    assert(send(pair[1], bytes, 9, 0) == 9);
    assert(!read_event() && input_size == 9);
    assert(send(pair[1], bytes + 9, sizeof(bytes) - 9, 0) == sizeof(bytes) - 9);
    assert(PS5_ProcessEvents(0) && delivered_input == 1 && voice_delivered == 1 && !input_size);
    close(pair[0]); close(pair[1]);
}

static void *activate_other_desktop_thread(void *unused)
{
    (void)unused;
    current_tid = 77;
    assert(!registered_here());
    PS5_SetDesktopWindow((HWND)200);
    assert(registered_here());
    return NULL;
}

static void test_cursor_before_desktop(void)
{
    int pair[2];
    struct ps5_desktop_cursor_shape hidden = {.header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(hidden)}};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    desktop_fd = pair[0]; current_tid = 42; desktop_tid = event_tid = 0;
    PS5_ThreadDetach();
    registered_threads = registrations = sink_hellos = input_size = 0;
    cursor_capable = FALSE; cursor_capability_checked = TRUE;
    assert(!ps5_send_cursor_message(&hidden, sizeof(hidden)) && registered_here() && !desktop_tid);
    PS5_SetDesktopWindow((HWND)100);
    assert(desktop_tid == 42 && sink_hellos == 1 && registered_threads == 1);
    PS5_SetDesktopWindow((HWND)100);
    assert(sink_hellos == 1);
    pthread_t other;
    assert(!pthread_create(&other, NULL, activate_other_desktop_thread, NULL));
    assert(!pthread_join(other, NULL));
    assert(desktop_tid == 77 && event_tid == 42 && sink_hellos == 2 &&
           registered_threads == 3 && registrations == 2);
    current_tid = 77;
    PS5_ThreadDetach();
    assert(!registered_here() && !desktop_tid && event_tid == 42);
    /* Wine may recycle a thread id after detach: the new queue must register. */
    assert(!pthread_create(&other, NULL, activate_other_desktop_thread, NULL));
    assert(!pthread_join(other, NULL));
    assert(registrations == 3 && registered_here() && desktop_tid == 77);
    PS5_ThreadDetach();
    current_tid = 42;
    PS5_ThreadDetach();
    assert(!queue_threads && !event_tid && !desktop_tid);
    close(pair[0]); close(pair[1]);
}

int main(void)
{
    TEST_HUB(pw_desktop_hub, pw_hub_peer, hub_driver_message, hub_replay, hub_remove_window, (void)title);
    TEST_HUB(ps5_desktop_hub, ps5_hub_peer, ps5_hub_driver_message, ps5_hub_replay, ps5_hub_remove_window, hub.title = &title);
    TEST_FULL_REPLAY(pw_desktop_hub, pw_hub_peer, pw_hub_window,
        hub_replay, hub_replay_cursors, hub_driver_message, hub_remove_window, (void)title);
    TEST_FULL_REPLAY(ps5_desktop_hub, ps5_hub_peer, ps5_hub_window,
        ps5_hub_replay, ps5_hub_replay_cursors, ps5_hub_driver_message, ps5_hub_remove_window, hub.title = &title);
    test_capabilities();
    test_late_capability_publication();
    test_split_old_input();
    test_split_voice_input();
    test_cursor_before_desktop();
    puts("cursor peers: PASS (bounded/fair 128x64x64 replay, split input, late HELLO, per-thread desktop activation)");
    return 0;
}
