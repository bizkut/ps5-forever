/* The Wine launcher title, PPSA99030. It owns the TV and input; the Wine
 * payload owns the windows and publishes their surfaces through its hub. */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef PS5_TITLE_EMBEDDED
#include <ps5platform/agc.h>
#include <ps5platform/klog.h>
#endif
#include <wine/ps5_desktop.h>
#include "compositor.h"
#include "display.h"
#include "forever-menu.h"
#include "addonforge-client.h"
#include "hub.h"
#include "ime.h"
#include "input.h"
#include "pointer.h"
#include "installer-continue.h"
#ifdef FOREVER_MENU_DIAG
#include "menu-diag.h"
#endif

#ifndef PS5_TITLE_EMBEDDED
#define WINE_ELF "/app0/wine/lib/wine/x86_64-unix/wine"
#define ELF_LOADER_PORT 9021
#define MAX_ELF_BYTES (4u * 1024u * 1024u)
#define ELF_CHUNK_BYTES 65536u
#define WINE_LINE_BYTES 8192u
#define CONNECT_TIMEOUT_US 3000000u
#define SEND_TIMEOUT_US 10000000u
#endif

static int user = -1;
static pthread_mutex_t message_mutex = PTHREAD_MUTEX_INITIALIZER;
static char pending_message[640];
static int message_pending;
#ifdef PS5_TITLE_EMBEDDED
static struct forever_menu_state menu;
static char pending_menu_status[FOREVER_MENU_STATUS_BYTES];
static struct forever_addon_snapshot pending_addon_snapshot;
static char pending_addon_error[FOREVER_MENU_STATUS_BYTES];
static int pending_menu_status_kind, pending_addon_kind, input_draining;
static int pending_voice_enabled, pending_voice_busy;
static uint8_t physical_keys[256], forwarded_keys[256];
static uint32_t physical_pad, menu_pad, physical_mouse, forwarded_mouse;
static enum forever_menu_command menu_ime_command;
static int menu_hint_started;
static struct forever_installer_continue installer_continue;

int forever_title_menu_load(struct forever_menu_values *);
int forever_title_addons_action(enum forever_menu_command, unsigned,
                                enum forever_addon_tab, const char *);
#ifdef FOREVER_MENU_DIAG
#define MENU_DIAG_SCRIPT "/data/homebrew/PPSA99030/wine/menu-script.txt"
#define MENU_DIAG_DONE "/data/homebrew/PPSA99030/wine/menu-script.done"
static struct
{
    char script[4096], dump_name[49];
    size_t size, offset;
    uint64_t poll_at, wait_until;
    int active, dump_pending;
} menu_diag;
void pw_elf_sha256(const void *, size_t, uint8_t[32]);
#endif
int forever_title_menu_action(enum forever_menu_command, int);
void forever_title_voice_cancel(void);
void io_log(const char *, ...) __attribute__((format(printf, 1, 2)));
#endif

/* Producers never touch compositor state; the desktop consumes the copy. */
void title_desktop_message(const char *text)
{
    pthread_mutex_lock(&message_mutex);
    snprintf(pending_message, sizeof(pending_message), "%s", text ? text : "");
    message_pending = 1;
    pthread_mutex_unlock(&message_mutex);
}

#ifdef PS5_TITLE_EMBEDDED
/* Menu workers publish copies; only the compositor thread mutates menu state. */
void title_desktop_menu_status(const char *text, int notice)
{
    pthread_mutex_lock(&message_mutex);
    snprintf(pending_menu_status, sizeof(pending_menu_status), "%s", text ? text : "");
    pending_menu_status_kind = notice ? 2 : 1;
    pthread_mutex_unlock(&message_mutex);
}

void title_desktop_voice_status(const char *text, int enabled, int busy)
{
    pthread_mutex_lock(&message_mutex);
    snprintf(pending_menu_status, sizeof(pending_menu_status), "%s", text ? text : "");
    pending_menu_status_kind = 3;
    pending_voice_enabled = enabled;
    pending_voice_busy = busy;
    pthread_mutex_unlock(&message_mutex);
}

void title_desktop_addons_snapshot(const struct forever_addon_snapshot *snapshot)
{
    if (!snapshot) return;
    pthread_mutex_lock(&message_mutex);
    if (pending_addon_kind != 3)
    {
        pending_addon_snapshot = *snapshot;
        pending_addon_kind = 1;
    }
    pthread_mutex_unlock(&message_mutex);
}

void title_desktop_addons_error(const char *text)
{
    pthread_mutex_lock(&message_mutex);
    if (pending_addon_kind != 3)
    {
        snprintf(pending_addon_error, sizeof(pending_addon_error), "%s",
                 text ? text : "The add-on operation failed.");
        pending_addon_kind = 2;
    }
    pthread_mutex_unlock(&message_mutex);
}

void title_desktop_addons_progress(const char *text)
{
    pthread_mutex_lock(&message_mutex);
    if (pending_addon_kind != 3)
    {
        snprintf(pending_addon_error, sizeof(pending_addon_error), "%s", text ? text : "");
        pending_addon_kind = 4;
    }
    pthread_mutex_unlock(&message_mutex);
}

void title_desktop_addons_pause(const char *text)
{
    pthread_mutex_lock(&message_mutex);
    snprintf(pending_addon_error, sizeof(pending_addon_error), "%s", text);
    /* An old worker may already have passed its generation check. Keep this
     * event ahead of its late snapshot/error until the compositor consumes it. */
    pending_addon_kind = 3;
    pthread_mutex_unlock(&message_mutex);
}
#endif

#ifndef PS5_TITLE_EMBEDDED
static int32_t agc = -1;   /* sceAgcInit's result: 0 once AGC is ready */
#endif

int sceSystemServiceHideSplashScreen(void);

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#ifdef PS5_TITLE_EMBEDDED
static int installer_continue_armed(enum forever_installer_continue_stage stage)
{
    return stage == FOREVER_INSTALLER_CONTINUE_PROCESS ||
           stage == FOREVER_INSTALLER_CONTINUE_WINDOW ||
           stage == FOREVER_INSTALLER_CONTINUE_SETTLE;
}

static void installer_continue_log(enum forever_installer_continue_result result)
{
    const char *reason;

    switch (result)
    {
    case FOREVER_INSTALLER_CONTINUE_SKIP_USER_INPUT:  reason = "user-input"; break;
    case FOREVER_INSTALLER_CONTINUE_SKIP_FOCUS_LOST:  reason = "focus-lost"; break;
    case FOREVER_INSTALLER_CONTINUE_SKIP_WINDOW_GONE: reason = "window-gone"; break;
    default: return;
    }
    io_log("forever: installer continue skipped reason=%s", reason);
}

void title_desktop_installer_mode(int enabled)
{
    enum forever_installer_continue_stage stage;
    pthread_mutex_lock(&message_mutex);
    forever_installer_continue_mode(&installer_continue, enabled);
    stage = installer_continue.stage;
    pthread_mutex_unlock(&message_mutex);
    io_log("forever: installer continue mode enabled=%d stage=%u",
           !!enabled, (unsigned)stage);
}

void title_desktop_installer_process(uint32_t pid)
{
    enum forever_installer_continue_stage before, after;
    uint32_t expected;
    pthread_mutex_lock(&message_mutex);
    before = installer_continue.stage;
    forever_installer_continue_process(&installer_continue, pid);
    after = installer_continue.stage;
    expected = installer_continue.pid;
    pthread_mutex_unlock(&message_mutex);
    io_log("forever: installer continue process pid=%u stage=%u->%u expected_pid=%u",
           pid, (unsigned)before, (unsigned)after, expected);
}
void title_desktop_installer_agent(uint32_t pid)
{
    enum forever_installer_continue_stage stage;
    uint64_t now, window_deadline, agent_deadline, deadline;
    int recorded;
    now = now_us();
    pthread_mutex_lock(&message_mutex);
    recorded = forever_installer_continue_agent(&installer_continue, now);
    stage = installer_continue.stage;
    window_deadline = installer_continue.window_deadline_us;
    agent_deadline = installer_continue.agent_deadline_us;
    deadline = installer_continue.deadline_us;
    pthread_mutex_unlock(&message_mutex);
    io_log("forever: installer continue agent pid=%u now_us=%llu recorded=%d "
           "window_deadline_us=%llu agent_deadline_us=%llu deadline_us=%llu stage=%u",
           pid, (unsigned long long)now, recorded,
           (unsigned long long)window_deadline, (unsigned long long)agent_deadline,
           (unsigned long long)deadline, (unsigned)stage);
}



void title_desktop_installer_foreground(uint32_t pid, uint64_t window, int ready)
{
    enum forever_installer_continue_result result;
    enum forever_installer_continue_stage before, after;
    uint32_t expected_pid;
    uint64_t expected_window, window_deadline, deadline;
    int agent_ready;
    pthread_mutex_lock(&message_mutex);
    before = installer_continue.stage;
    expected_pid = installer_continue.pid;
    expected_window = installer_continue.window;
    result = forever_installer_continue_foreground(&installer_continue, pid, window, ready, now_us());
    after = installer_continue.stage;
    deadline = installer_continue.deadline_us;
    window_deadline = installer_continue.window_deadline_us;
    agent_ready = installer_continue.agent_ready;
    pthread_mutex_unlock(&message_mutex);
    if (installer_continue_armed(before))
        io_log("forever: installer continue foreground pid=%u window=%llx ready=%d "
               "expected_pid=%u expected_window=%llx stage=%u->%u",
               pid, (unsigned long long)window, !!ready, expected_pid,
               (unsigned long long)expected_window, (unsigned)before, (unsigned)after);
    if (before == FOREVER_INSTALLER_CONTINUE_WINDOW &&
        after == FOREVER_INSTALLER_CONTINUE_SETTLE)
        io_log("forever: installer continue settle pid=%u window=%llx "
               "window_deadline_us=%llu agent_ready=%d deadline_us=%llu",
               pid, (unsigned long long)window,
               (unsigned long long)window_deadline,
               agent_ready, (unsigned long long)deadline);
    installer_continue_log(result);
}

void title_desktop_installer_destroyed(uint32_t pid, uint64_t window)
{
    enum forever_installer_continue_result result;
    enum forever_installer_continue_stage before, after;
    pthread_mutex_lock(&message_mutex);
    before = installer_continue.stage;
    result = forever_installer_continue_destroyed(&installer_continue, pid, window);
    after = installer_continue.stage;
    pthread_mutex_unlock(&message_mutex);
    if (installer_continue_armed(before))
        io_log("forever: installer continue destroy pid=%u window=%llx stage=%u->%u",
               pid, (unsigned long long)window, (unsigned)before, (unsigned)after);
    installer_continue_log(result);
}

static void title_desktop_installer_user_input(void)
{
    enum forever_installer_continue_result result;
    pthread_mutex_lock(&message_mutex);
    result = forever_installer_continue_input(&installer_continue);
    pthread_mutex_unlock(&message_mutex);
    installer_continue_log(result);
}
#endif

#ifndef PS5_TITLE_EMBEDDED
struct wine_log
{
    char line[WINE_LINE_BYTES];
    size_t length;
    int truncated;
};

static void wine_log_line(struct wine_log *log)
{
    /* One whole line on stderr, including when a TCP receive split it. The
     * klog reader is asynchronous; its pipe cannot stall the frame thread. */
    flockfile(stderr);
    fputs("[wine] ", stderr);
    fwrite(log->line, 1, log->length, stderr);
    if (log->truncated) fputs(" [line truncated]", stderr);
    fputc('\n', stderr);
    funlockfile(stderr);
    log->length = 0;
    log->truncated = 0;
}

static void wine_log_bytes(struct wine_log *log, const char *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        if (bytes[i] == '\n') wine_log_line(log);
        else if (log->length < sizeof(log->line)) log->line[log->length++] = bytes[i];
        else log->truncated = 1;
    }
}

/* File reads and stderr's klog pipe are allowed to wait, so they live on a
 * single startup worker, never in the vblank/input loop. The loader socket
 * itself is non-blocking in both directions; the ELF is streamed from a
 * fixed buffer and its write side is closed only after every byte is sent. */
static void *wine_launch(void *unused)
{
    static uint8_t elf[ELF_CHUNK_BYTES];
    static struct wine_log log;
    struct sockaddr_in address = {
        .sin_len = sizeof(address),
        .sin_family = AF_INET,
        .sin_port = htons(ELF_LOADER_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    struct stat status;
    struct pollfd descriptor;
    size_t start = 0, size = 0;
    uint64_t remaining, deadline;
    int fd = -1, socket_fd = -1, connecting = 0, sent = 0;
    const char *step = "open Wine ELF";

    (void)unused;
    fd = open(WINE_ELF, O_RDONLY);
    if (fd < 0) goto failed;
    step = "stat Wine ELF";
    if (fstat(fd, &status) < 0) goto failed;
    if (!S_ISREG(status.st_mode) || status.st_size <= 0 || (uint64_t)status.st_size > MAX_ELF_BYTES)
    {
        errno = EFBIG;
        goto failed;
    }
    remaining = (uint64_t)status.st_size;
    /* A title may not make a socket non-blocking (fcntl F_SETFL: EACCES, M5
     * runs): it stays blocking, connects at once (loopback answers or
     * refuses immediately), and each send and receive below is non-blocking
     * on its own (MSG_DONTWAIT). */
    step = "create ELF-loader socket";
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) goto failed;
    step = "connect ELF loader";
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)))
    {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EINTR) goto failed;
        connecting = 1;
    }
    deadline = now_us() + CONNECT_TIMEOUT_US;
    descriptor = (struct pollfd){.fd = socket_fd, .events = POLLOUT};
    while (connecting)
    {
        int result = poll(&descriptor, 1, 100);
        if (result < 0)
        {
            if (errno == EINTR) continue;
            goto failed;
        }
        if (result > 0 && descriptor.revents)
        {
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) goto failed;
            if (error || !(descriptor.revents & POLLOUT))
            {
                errno = error ? error : ECONNRESET;
                goto failed;
            }
            connecting = 0;
        }
        else if (now_us() >= deadline)
        {
            errno = ETIMEDOUT;
            goto failed;
        }
    }
    deadline = now_us() + SEND_TIMEOUT_US;
    for (;;)
    {
        if (!sent && start == size)
        {
            start = size = 0;
            if (remaining)
            {
                size_t chunk = remaining < sizeof(elf) ? (size_t)remaining : sizeof(elf);
                ssize_t count;
                step = "read Wine ELF";
                do count = read(fd, elf, chunk); while (count < 0 && errno == EINTR);
                if (count <= 0)
                {
                    if (!count) errno = EIO;
                    goto failed;
                }
                size = (size_t)count;
                remaining -= size;
            }
            else
            {
                step = "finish ELF upload";
                if (shutdown(socket_fd, SHUT_WR) < 0) goto failed;
                close(fd);
                fd = -1;
                sent = 1;
                fprintf(stderr, "Wine ELF sent (%lld bytes)\n", (long long)status.st_size);
            }
        }
        descriptor.events = POLLIN | (sent ? 0 : POLLOUT);
        step = "poll Wine output";
        int result = poll(&descriptor, 1, 100);
        if (result < 0)
        {
            if (errno == EINTR) continue;
            goto failed;
        }
        if (descriptor.revents & POLLNVAL)
        {
            errno = EBADF;
            goto failed;
        }
        if (descriptor.revents & (POLLIN | POLLHUP | POLLERR))
        {
            for (unsigned i = 0; i < 16; i++)
            {
                char output[4096];
                step = "read Wine output";
                ssize_t count = recv(socket_fd, output, sizeof(output), MSG_DONTWAIT);
                if (count > 0) wine_log_bytes(&log, output, (size_t)count);
                else if (!count) goto done;
                else if (errno == EINTR) continue;
                else if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    if (descriptor.revents & POLLHUP) goto done;
                    break;
                }
                else goto failed;
            }
        }
        if (!sent && (descriptor.revents & POLLOUT))
        {
            step = "send Wine ELF";
            ssize_t count = send(socket_fd, elf + start, size - start, MSG_DONTWAIT);
            if (count > 0) start += (size_t)count;
            else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) goto failed;
            else if (!count)
            {
                errno = EPIPE;
                goto failed;
            }
        }
        if (!sent && now_us() >= deadline)
        {
            step = "send Wine ELF";
            errno = ETIMEDOUT;
            goto failed;
        }
    }

failed:
    {
        int error = errno;
        if (log.length || log.truncated) wine_log_line(&log);
        fprintf(stderr, "%s failed: errno=%d\n", step, error);
    }
done:
    if (log.length || log.truncated) wine_log_line(&log);
    if (fd >= 0) close(fd);
    if (socket_fd >= 0) close(socket_fd);
    return NULL;
}
#endif

#ifdef PS5_TITLE_EMBEDDED
static void maybe_start_menu_hint(uint64_t now)
{
    if (menu_hint_started) return;
    menu_hint_started = 1;
    compositor_hint_start(now);
}
#endif

static void on_hub_message(const void *message, void *context)
{
    struct ps5_desktop_header header;
    (void)context;
    memcpy(&header, message, sizeof(header));
    switch (header.type)
    {
    case PS5_DESKTOP_WINDOW:
    {
        struct ps5_desktop_window window;
        memcpy(&window, message, sizeof(window));
        compositor_window(&window);
#ifdef PS5_TITLE_EMBEDDED
        if ((window.flags & PS5_DESKTOP_VISIBLE) && window.width && window.height)
            maybe_start_menu_hint(now_us());
#endif
        break;
    }
    case PS5_DESKTOP_DESTROY:
    {
        struct ps5_desktop_id destroyed;
        memcpy(&destroyed, message, sizeof(destroyed));
        compositor_destroy(destroyed.id);
        break;
    }
    case PS5_DESKTOP_DAMAGE:
    {
        struct ps5_desktop_damage damage;
        memcpy(&damage, message, sizeof(damage));
        compositor_damage(&damage);
        break;
    }
    case PS5_DESKTOP_CURSOR_POS:
    {
        struct ps5_desktop_point position;
        memcpy(&position, message, sizeof(position));
        pointer_set_position(position.x, position.y);
        break;
    }
    case PS5_DESKTOP_CURSOR_SHAPE:
        compositor_cursor_shape(message);
        break;
    case PS5_DESKTOP_TEXT_INPUT:
    {
        /* A text field took (active) or lost the focus. Chromium does not
         * report password fields: L3+R3 open the keyboard there. */
        struct ps5_desktop_text_input input;
        memcpy(&input, message, sizeof(input));
#ifdef PS5_TITLE_EMBEDDED
        if (user >= 0 && !forever_menu_is_open(&menu) && !input_draining)
#else
        if (user >= 0)
#endif
            ime_show(user, input.active != 0);
        break;
    }
}
}

static void on_hub_disconnect(void *context)
{
    (void)context;
#ifdef PS5_TITLE_EMBEDDED
    memset(forwarded_keys, 0, sizeof(forwarded_keys));
    forwarded_mouse = 0;
#endif
    compositor_reset();
}

static uint32_t mouse_button(uint32_t code)
{
    switch (code)
    {
    case MOUSE_LEFT:   return PS5_DESKTOP_BUTTON_LEFT;
    case MOUSE_RIGHT:  return PS5_DESKTOP_BUTTON_RIGHT;
    case MOUSE_MIDDLE: return PS5_DESKTOP_BUTTON_MIDDLE;
    case MOUSE_X1:     return PS5_DESKTOP_BUTTON_X1;
    case MOUSE_X2:     return PS5_DESKTOP_BUTTON_X2;
    default:          return 0;
    }
}

#ifdef PS5_TITLE_EMBEDDED
#define KEY_ENTER 0x28u
#define KEY_ESCAPE 0x29u
#define KEY_BACKSPACE 0x2au
#define KEY_F12 0x45u
#define KEY_RIGHT 0x4fu
#define KEY_LEFT 0x50u
#define KEY_DOWN 0x51u
#define KEY_UP 0x52u
static void render_menu(void);
static int physical_input_neutral(void);
static void close_menu(void);
static void open_menu(void);
static void apply_menu_result(struct forever_menu_result result);
static void menu_key(uint32_t usage);
static void menu_ime_finish(const char *text, int accepted);
#ifdef FOREVER_MENU_DIAG
static void menu_diag_log_state(const char *event);
#endif
#endif

static void on_pointer(const struct pointer_event *event, void *context)
{
    int x, y;
    struct ps5_desktop_mouse mouse = {.header = {PS5_DESKTOP_MOUSE, sizeof(mouse)}};
    (void)context;
    if (event->kind == POINTER_BUTTON && event->down && mouse_button(event->code))
        compositor_cursor_wake();

#ifdef PS5_TITLE_EMBEDDED
    if (event->kind == POINTER_KEY && event->code == KEY_F12)
    {
        if (event->down && !event->repeat)
        {
            if (forever_menu_is_open(&menu)) close_menu();
            else if (!input_draining) open_menu();
        }
        return;
    }
    if (forever_menu_is_open(&menu))
    {
        if (event->kind == POINTER_KEY && event->down) menu_key(event->code);
        else if (event->kind == POINTER_BUTTON && event->down)
        {
            if (event->code == MOUSE_LEFT)
            {
                pointer_position(&x, &y);
                apply_menu_result(forever_menu_click_line(&menu, compositor_menu_line_at(x, y)));
            }
            else if (event->code == MOUSE_RIGHT)
                apply_menu_result(forever_menu_back(&menu));
        }
        return;
    }
#endif
    switch (event->kind)
    {
    case POINTER_KEY:
    {
        struct ps5_desktop_key key = {
            .header = {PS5_DESKTOP_KEY, sizeof(key)},
            .usage = event->code,
            .down = !!event->down,
            .repeat = !!event->repeat,
        };
        if (hub_send(&key, sizeof(key)))
        {
#ifdef PS5_TITLE_EMBEDDED
            if (event->code < sizeof(forwarded_keys)) forwarded_keys[event->code] = !!event->down;
#endif
        }
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
        mouse.button = mouse_button(event->code);
        if (!mouse.button) return;
        mouse.down = !!event->down;
        break;
    case POINTER_WHEEL:
        mouse.kind = PS5_DESKTOP_MOUSE_WHEEL;
        mouse.wheel_x = event->dx;
        mouse.wheel_y = event->dy;
        break;
    }
    pointer_position(&x, &y);
    mouse.x = x;
    mouse.y = y;
    if (hub_send(&mouse, sizeof(mouse)))
    {
#ifdef PS5_TITLE_EMBEDDED
        if (event->kind == POINTER_BUTTON)
        {
            if (event->down) forwarded_mouse |= event->code;
            else forwarded_mouse &= ~event->code;
        }
#endif
    }
}

static void on_input(const struct input_event *event, void *context)
{
#ifdef PS5_TITLE_EMBEDDED
    uint32_t previous_pad = physical_pad;
    if (event->kind == INPUT_KEY && event->code < sizeof(physical_keys))
        physical_keys[event->code] = !!event->down;
    else if (event->kind == INPUT_MOUSE_BUTTON)
    {
        if (event->down) physical_mouse |= event->code;
        else physical_mouse &= ~event->code;
    }
    else if (event->kind == INPUT_PAD)
        physical_pad = event->connected ? event->code & ~PAD_INTERCEPTED : 0;
    if ((event->kind == INPUT_KEY && event->down) ||
        (event->kind == INPUT_MOUSE_BUTTON && event->down) ||
        (event->kind == INPUT_PAD && (physical_pad & ~previous_pad)))
        title_desktop_installer_user_input();
    if (input_draining)
    {
        if (physical_input_neutral())
        {
            pointer_cancel();
            input_draining = 0;
        }
        return;
    }
    if (menu_ime_command) return;
    if (event->kind == INPUT_PAD && (physical_pad & PAD_OPTIONS) &&
        !(previous_pad & PAD_OPTIONS))
    {
        if (!forever_menu_is_open(&menu)) open_menu();
        else if (menu.page == FOREVER_MENU_MAIN) close_menu();
        else apply_menu_result(forever_menu_back(&menu));
        return;
    }
    if (event->kind == INPUT_PAD && forever_menu_is_open(&menu))
    {
        uint32_t changed = physical_pad ^ menu_pad;
        struct input_event pointer = *event;
        menu_pad = physical_pad;
        if (changed & physical_pad & PAD_UP) forever_menu_move(&menu, -1);
        if (changed & physical_pad & PAD_DOWN) forever_menu_move(&menu, 1);
#ifdef FOREVER_MENU_DIAG
        if (changed & physical_pad & (PAD_UP | PAD_DOWN))
            menu_diag_log_state("selection");
#endif
        if (menu.page == FOREVER_MENU_ADDON_PAGE)
        {
            if (changed & physical_pad & PAD_LEFT)
                apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_LEFT));
            else if (changed & physical_pad & PAD_RIGHT)
                apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_RIGHT));
            else if (changed & physical_pad & PAD_L1)
                apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_L1));
            else if (changed & physical_pad & PAD_R1)
                apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_R1));
            else if (changed & physical_pad & PAD_SQUARE)
                apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_SQUARE));
            else if (changed & physical_pad & PAD_TRIANGLE)
                apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_TRIANGLE));
            else if (changed & physical_pad & PAD_R3)
                apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_R3));
        }
        if (changed & physical_pad & PAD_CROSS)
            apply_menu_result(forever_menu_select(&menu));
        else if (changed & physical_pad & PAD_CIRCLE)
            apply_menu_result(forever_menu_back(&menu));
        if (!forever_menu_is_open(&menu)) return;
        pointer.code &= PAD_TOUCHPAD;
        pointer_input(&pointer, on_pointer, context);
        render_menu();
        return;
    }
#endif
    pointer_input(event, on_pointer, context);
}
#if defined(PS5_TITLE_EMBEDDED) && defined(FOREVER_MENU_DIAG)
static void menu_diag_log_state(const char *event)
{
    io_log("menu diag menu %s page=%d selected=%d",
           event, menu.page, menu.selected);
}

static void menu_diag_load(uint64_t now)
{
    int fd;
    ssize_t count;
    size_t used = 0;

    menu_diag.poll_at = now + 1000000;
    if (rename(MENU_DIAG_SCRIPT, MENU_DIAG_DONE))
    {
        if (errno != ENOENT)
            io_log("menu diag input script rename failed errno=%d", errno);
        return;
    }
    if ((fd = open(MENU_DIAG_DONE, O_RDONLY)) < 0)
    {
        io_log("menu diag input script open failed errno=%d", errno);
        return;
    }
    while (used < sizeof(menu_diag.script) &&
           (count = read(fd, menu_diag.script + used, sizeof(menu_diag.script) - used)) != 0)
    {
        if (count < 0 && errno == EINTR) continue;
        if (count < 0)
        {
            io_log("menu diag input script read failed errno=%d", errno);
            close(fd);
            return;
        }
        used += (size_t)count;
    }
    close(fd);
    if (used == sizeof(menu_diag.script) || memchr(menu_diag.script, 0, used))
    {
        io_log("menu diag input script refused bytes=%zu", used);
        return;
    }
    menu_diag.size = used;
    menu_diag.offset = 0;
    menu_diag.active = 1;
    io_log("menu diag input script bytes=%zu", used);
}

static void menu_diag_press_pad(uint32_t code)
{
    struct input_event event = {
        .kind = INPUT_PAD, .connected = 1, .code = code,
        .lx = 128, .ly = 128, .rx = 128, .ry = 128,
    };
    on_input(&event, NULL);
    event.code = 0;
    on_input(&event, NULL);
}

static void menu_diag_press_key(uint32_t code)
{
    struct input_event event = {.kind = INPUT_KEY, .code = code, .down = 1};
    on_input(&event, NULL);
    event.down = 0;
    on_input(&event, NULL);
}

static void menu_diag_click(int x, int y)
{
    int current_x, current_y;
    pointer_position(&current_x, &current_y);
    struct input_event event = {
        .kind = INPUT_MOUSE_MOVE, .dx = x - current_x, .dy = y - current_y};
    on_input(&event, NULL);
    event = (struct input_event){.kind = INPUT_MOUSE_BUTTON, .code = MOUSE_LEFT, .down = 1};
    on_input(&event, NULL);
    event.down = 0;
    on_input(&event, NULL);
}

static void menu_diag_step(uint64_t now)
{
    if (menu_diag.dump_pending) return;
    if (menu_diag.wait_until)
    {
        if (now < menu_diag.wait_until) return;
        menu_diag.wait_until = 0;
    }
    if (!menu_diag.active)
    {
        if (now < menu_diag.poll_at) return;
        menu_diag_load(now);
        if (!menu_diag.active) return;
    }
    while (menu_diag.offset < menu_diag.size)
    {
        size_t start = menu_diag.offset, end = start;
        struct forever_menu_diag_command command;
        while (end < menu_diag.size && menu_diag.script[end] != '\n') end++;
        menu_diag.offset = end < menu_diag.size ? end + 1 : end;
        size_t length = end - start;
        if (forever_menu_diag_parse(menu_diag.script + start, length, &command))
        {
            io_log("menu diag input invalid");
            continue;
        }
        {
            char summary[96];
            forever_menu_diag_summary(&command, summary, sizeof(summary));
            io_log("%s", summary);
        }
        switch (command.kind)
        {
        case FOREVER_MENU_DIAG_PAD: menu_diag_press_pad(command.code); break;
        case FOREVER_MENU_DIAG_KEY: menu_diag_press_key(command.code); break;
        case FOREVER_MENU_DIAG_CLICK: menu_diag_click(command.x, command.y); break;
        case FOREVER_MENU_DIAG_WAIT:
            menu_diag.wait_until = now_us() + (uint64_t)command.milliseconds * 1000;
            return;
        case FOREVER_MENU_DIAG_DUMP:
            if (menu_ime_command == FOREVER_MENU_COMMAND_WAGO_IME)
            {
                io_log("menu diag dump skipped private keyboard");
                break;
            }
            snprintf(menu_diag.dump_name, sizeof(menu_diag.dump_name), "%s", command.name);
            menu_diag.dump_pending = 1;
            return;
        case FOREVER_MENU_DIAG_TEXT:
            if (menu_ime_command)
            {
                ime_show(user, 0);
                menu_ime_finish(command.text, 1);
                io_log("menu diag text confirmed bytes=%zu", strlen(command.text));
            }
            else
                io_log("menu diag text ignored no-menu-ime");
            memset(command.text, 0, sizeof(command.text));
            break;
        case FOREVER_MENU_DIAG_ADDON:
        {
            size_t selected;
            for (selected = 0; selected < menu.addon_count; selected++)
                if (!strcmp(menu.addon_rows[selected].key, command.name)) break;
            if (selected < menu.addon_count || !strcmp(command.name, "menu:wago"))
            {
                menu.addon_selected = selected;
                menu.addon_scroll = selected >= 5 ? selected - 4 : 0;
                render_menu();
            }
            io_log("menu diag addon key=%.48s found=%d index=%zu",
                   command.name, selected < menu.addon_count ||
                       !strcmp(command.name, "menu:wago"), selected);
            break;
        }
        }
    }
    menu_diag.active = 0;
}

static void menu_diag_dump(void)
{
    static const char root[] = "/data/homebrew/PPSA99030/wine/menu-";
    char path[256], temporary[272], hex[65];
    uint8_t digest[32];
    const void *pixels = display_image();
    const size_t bytes = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint32_t);
    int fd, error = 0;

    if (!menu_diag.dump_pending) return;
    snprintf(path, sizeof(path), "%s%s.bgra", root, menu_diag.dump_name);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    pw_elf_sha256(pixels, bytes, digest);
    for (unsigned i = 0; i < sizeof(digest); i++)
        snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02x", digest[i]);
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)
        error = errno;
    else
    {
        size_t done = 0;
        while (done < bytes)
        {
            ssize_t count = write(fd, (const char *)pixels + done, bytes - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { error = count < 0 ? errno : EIO; break; }
            done += (size_t)count;
        }
        if (close(fd) && !error) error = errno;
        if (!error && rename(temporary, path)) error = errno;
    }
    if (error)
    {
        unlink(temporary);
        io_log("menu diag dump name=%s failed errno=%d",
               menu_diag.dump_name, error);
    }
    else
        io_log("menu diag dump name=%s bytes=%zu sha256=%s",
               menu_diag.dump_name, bytes, hex);
    menu_diag.dump_pending = 0;
}
#endif

#ifdef PS5_TITLE_EMBEDDED

static void render_menu(void)
{
    char text[FOREVER_MENU_TEXT_BYTES];
    forever_menu_render(&menu, text, sizeof(text));
    compositor_menu(text);
}

static int physical_input_neutral(void)
{
    if (physical_pad || physical_mouse) return 0;
    for (unsigned i = 0; i < sizeof(physical_keys); i++)
        if (physical_keys[i]) return 0;
    return 1;
}

static void release_forwarded_input(void)
{
    int x, y;
    for (unsigned usage = 0; usage < sizeof(forwarded_keys); usage++)
        if (forwarded_keys[usage])
        {
            struct ps5_desktop_key key = {
                .header = {PS5_DESKTOP_KEY, sizeof(key)}, .usage = usage};
            hub_send(&key, sizeof(key));
        }
    pointer_position(&x, &y);
    for (unsigned code = MOUSE_LEFT; code <= MOUSE_X2; code <<= 1)
        if (forwarded_mouse & code)
        {
            struct ps5_desktop_mouse mouse = {
                .header = {PS5_DESKTOP_MOUSE, sizeof(mouse)},
                .kind = PS5_DESKTOP_MOUSE_BUTTON, .x = x, .y = y,
                .button = mouse_button(code),
            };
            hub_send(&mouse, sizeof(mouse));
        }
    memset(forwarded_keys, 0, sizeof(forwarded_keys));
    forwarded_mouse = 0;
}

static void close_menu(void)
{
    forever_title_voice_cancel();
    pthread_mutex_lock(&message_mutex);
    if (pending_menu_status_kind == 3) pending_menu_status_kind = 0;
    pthread_mutex_unlock(&message_mutex);
    if (menu.page == FOREVER_MENU_ADDON_PAGE || menu.page == FOREVER_MENU_ADDON_CONFIRM ||
        menu.page == FOREVER_MENU_WAGO_KEY)
        forever_title_addons_action(FOREVER_MENU_COMMAND_ADDONS_CLOSE,
                                    menu.addon_channel, menu.addon_tab, NULL);
    if (menu_ime_command && user >= 0) ime_show(user, 0);
    menu_ime_command = FOREVER_MENU_COMMAND_NONE;
    release_forwarded_input();
    forever_menu_close(&menu);
    compositor_menu(NULL);
    pointer_cancel();
    menu_pad = 0;
    input_draining = !physical_input_neutral();
#ifdef FOREVER_MENU_DIAG
    menu_diag_log_state("close");
#endif
}

static void open_menu(void)
{
    struct forever_menu_values values = {FOREVER_FG_ON, 1, 0, 1, 0};
    int error;
    compositor_hint_stop();
    menu_hint_started = 1;
    if (user >= 0) ime_show(user, 0);
    menu_ime_command = FOREVER_MENU_COMMAND_NONE;
    release_forwarded_input();
    pointer_cancel();
    error = forever_title_menu_load(&values);
    pointer_set_invert_scroll(values.invert_scroll);
    forever_menu_open(&menu, &values);
    if (error)
    {
        char status[FOREVER_MENU_STATUS_BYTES];
        snprintf(status, sizeof(status), "Could not read the settings (error %d).", -error);
        forever_menu_status(&menu, status);
    }
    menu_pad = physical_pad;
    input_draining = 0;
    render_menu();
#ifdef FOREVER_MENU_DIAG
    menu_diag_log_state("open");
    io_log("menu diag action load result=%d", error);
#endif
}

static void apply_menu_result(struct forever_menu_result result)
{
    int error;
    char status[FOREVER_MENU_STATUS_BYTES];
    if (result.command == FOREVER_MENU_COMMAND_NONE)
    {
        render_menu();
#ifdef FOREVER_MENU_DIAG
        menu_diag_log_state("selection");
#endif
        return;
    }
    if (result.command == FOREVER_MENU_COMMAND_CLOSE)
    {
        close_menu();
        return;
    }
    if (result.command == FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME ||
        result.command == FOREVER_MENU_COMMAND_ADDONS_GITHUB_IME ||
        result.command == FOREVER_MENU_COMMAND_WAGO_IME)
    {
        const char *initial = result.command == FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME ?
            menu.addon_query : result.command == FOREVER_MENU_COMMAND_WAGO_IME ?
            "" : "https://github.com/";
        error = user >= 0 ? ime_show_text(user, initial) : -ENODEV;
        if (error)
        {
            snprintf(status, sizeof(status),
                     "Could not open the on-screen keyboard (error %d).", -error);
            forever_menu_status(&menu, status);
        }
        else
            menu_ime_command = result.command;
        render_menu();
        return;
    }
    if (result.command >= FOREVER_MENU_COMMAND_ADDONS_OPEN &&
        result.command <= FOREVER_MENU_COMMAND_WAGO_REMOVE)
    {
        error = forever_title_addons_action(result.command, result.channel, result.tab,
                                            result.key[0] ? result.key : NULL);
#ifdef FOREVER_MENU_DIAG
        io_log("menu diag addons command=%d channel=%d tab=%d key=%.80s result=%d",
               result.command, result.channel, result.tab,
               result.key[0] ? result.key : "(none)", error);
#endif
        if (error)
        {
            snprintf(status, sizeof(status), "Could not start the add-on operation (error %d).",
                     -error);
            forever_menu_addon_error(&menu, status);
        }
        else if (result.command == FOREVER_MENU_COMMAND_WAGO_FILE ||
                 result.command == FOREVER_MENU_COMMAND_WAGO_REMOVE)
        {
            menu.addon_loading = 1;
            forever_menu_status(&menu, result.command == FOREVER_MENU_COMMAND_WAGO_REMOVE ?
                "Removing Wago key..." : "Saving Wago key...");
        }
        render_menu();
        return;
    }
    if (result.command == FOREVER_MENU_COMMAND_REINSTALL ||
        result.command == FOREVER_MENU_COMMAND_UNINSTALL ||
        result.command == FOREVER_MENU_COMMAND_EXIT)
    {
        forever_menu_progress(&menu, result.command == FOREVER_MENU_COMMAND_REINSTALL ?
                              "Stopping Battle.net before reinstalling..." :
                              result.command == FOREVER_MENU_COMMAND_UNINSTALL ?
                              "Stopping Battle.net before removing it..." : "Closing Battle.net...");
        render_menu();
        error = forever_title_menu_action(result.command, 0);
#ifdef FOREVER_MENU_DIAG
        io_log("menu diag action command=%d enabled=0 result=%d",
               result.command, error);
#endif
        if (!error) return;
        if (error == -EBUSY && result.command == FOREVER_MENU_COMMAND_EXIT)
            snprintf(status, sizeof(status),
                     "Another operation is still running. Wait for it to finish before exiting.");
        else
            snprintf(status, sizeof(status), "Could not start the operation (error %d).", -error);
        forever_menu_notice(&menu, status);
        render_menu();
        return;
    }
    error = forever_title_menu_action(result.command, result.enabled);
#ifdef FOREVER_MENU_DIAG
    io_log("menu diag action command=%d enabled=%d result=%d",
           result.command, result.enabled, error);
#endif
    if (result.command == FOREVER_MENU_COMMAND_SET_VOICE && error > 0)
    {
        menu.voice_downloading = 1;
        forever_menu_status(&menu, "Checking voice model\xe2\x80\xa6");
    }
    else if (!error)
    {
        forever_menu_commit(&menu, result.command, result.enabled);
        if (result.command == FOREVER_MENU_COMMAND_SET_INVERT_SCROLL)
            pointer_set_invert_scroll(result.enabled);
    }
    else
    {
        snprintf(status, sizeof(status), "Could not save the setting (error %d).", -error);
        forever_menu_status(&menu, status);
    }
    render_menu();
}

static void menu_ime_finish(const char *text, int accepted)
{
    enum forever_menu_command command = menu_ime_command;
    struct forever_menu_result result = {0};
    menu_ime_command = FOREVER_MENU_COMMAND_NONE;
    /* IME owns the pad while open. Forget the stale Cross edge used to open
     * it, including the release consumed by the system keyboard. */
    menu_pad = physical_pad;
    if (command == FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME)
        forever_menu_find_ime(&menu, text, accepted);
    else if (command == FOREVER_MENU_COMMAND_ADDONS_GITHUB_IME)
        result = forever_menu_github_ime(&menu, text, accepted);
    else if (command == FOREVER_MENU_COMMAND_WAGO_IME)
    {
        if (accepted)
        {
            int error = strlen(text) >= ADDONFORGE_WAGO_KEY_BYTES ? -EFBIG :
                forever_title_addons_action(FOREVER_MENU_COMMAND_WAGO_SET,
                    menu.addon_channel, menu.addon_tab, text);
            if (error)
                forever_menu_addon_error(&menu, error == -EFBIG ?
                    "Wago key is too long (maximum 256 characters)." : "Could not save the Wago key.");
            else
            {
                menu.addon_loading = 1;
                forever_menu_status(&menu, "Saving Wago key...");
            }
        }
        ime_clear_text();
    }
    apply_menu_result(result);
}

static char menu_usb_character(uint32_t usage)
{
    int shift = physical_keys[0xe1] || physical_keys[0xe5];
    if (physical_keys[0xe0] || physical_keys[0xe2] || physical_keys[0xe3] ||
        physical_keys[0xe4] || physical_keys[0xe6] || physical_keys[0xe7]) return 0;
    if (usage >= 0x04 && usage <= 0x1d)
        return (char)((shift ? 'A' : 'a') + usage - 0x04);
    if (usage >= 0x1e && usage <= 0x27)
    {
        static const char plain[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return shift ? shifted[usage - 0x1e] : plain[usage - 0x1e];
    }
    switch (usage)
    {
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x32: return shift ? '~' : '#';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}

static void menu_key(uint32_t usage)
{
    if (menu.page == FOREVER_MENU_ADDON_PAGE &&
        menu.addon_tab == FOREVER_ADDON_FIND && !menu.addon_loading)
    {
        char character = menu_usb_character(usage);
        if (usage == KEY_BACKSPACE)
        {
            forever_menu_find_backspace(&menu);
            render_menu();
            return;
        }
        if (character)
        {
            char text[2] = {character, 0};
            forever_menu_find_append(&menu, text);
            render_menu();
            return;
        }
    }
    if (usage == KEY_UP) forever_menu_move(&menu, -1);
    else if (usage == KEY_DOWN) forever_menu_move(&menu, 1);
    else if (usage == KEY_LEFT)
        apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_LEFT));
    else if (usage == KEY_RIGHT)
        apply_menu_result(forever_menu_addon_button(&menu, FOREVER_ADDON_RIGHT));
    else if (usage == KEY_ENTER) apply_menu_result(forever_menu_select(&menu));
    else if (usage == KEY_ESCAPE) apply_menu_result(forever_menu_back(&menu));
#ifdef FOREVER_MENU_DIAG
    if (usage == KEY_UP || usage == KEY_DOWN) menu_diag_log_state("selection");
#endif
    if (forever_menu_is_open(&menu)) render_menu();
}
#endif


static void send_text(const uint16_t *text, size_t length)
{
    uint8_t message[sizeof(struct ps5_desktop_text) + PS5_DESKTOP_MAX_TEXT * sizeof(uint16_t)];

    while (length)
    {
        size_t units = length < PS5_DESKTOP_MAX_TEXT ? length : PS5_DESKTOP_MAX_TEXT;
        /* Do not split a UTF-16 surrogate pair across frames. */
        if (units < length && text[units - 1] >= 0xd800 && text[units - 1] <= 0xdbff &&
            text[units] >= 0xdc00 && text[units] <= 0xdfff) units--;
        struct ps5_desktop_text header = {
            .header = {PS5_DESKTOP_TEXT, sizeof(header) + units * sizeof(uint16_t)},
            .length = (uint32_t)units,
        };
        memcpy(message, &header, sizeof(header));
        memcpy(message + sizeof(header), text, units * sizeof(uint16_t));
        if (!hub_send(message, header.header.size)) return;
        text += units;
        length -= units;
    }
}

static int send_key(uint32_t usage)
{
    struct ps5_desktop_key key = {.header = {PS5_DESKTOP_KEY, sizeof(key)}, .usage = usage};
    int down, up;
    key.down = 1;
    down = hub_send(&key, sizeof(key));
    key.down = 0;
    up = hub_send(&key, sizeof(key));
    return down && up;
}

#ifndef PS5_TITLE_EMBEDDED
/* Offers the graphics pipe at a suspend point. The call sleeps while a Wine
 * process holds the pipe, and the kernel gives the pipe back to the title at
 * that process's next suspend point: called again only a frame later, it kept
 * the pipe for up to 16 ms each time while the Wine process slept
 * (m9d3d11 under this title, 2026-10-03). A call that slept means a Wine
 * process is drawing, so the pipe goes straight back; one that returned at once
 * means nobody waits, and the next offer comes a frame later. */
static void *agc_yield(void *unused)
{
    (void)unused;
    for (;;)
    {
        uint64_t start = now_us();
        sceAgcSuspendPoint();
        if (now_us() - start < 2000) usleep(16000);
    }
    return NULL;
}
#endif

#ifdef PS5_TITLE_EMBEDDED
static void title_desktop_installer_tick(uint64_t now)
{
    enum forever_installer_continue_result result;
    uint32_t pid;
    uint64_t window;
    pthread_mutex_lock(&message_mutex);
    result = forever_installer_continue_tick(&installer_continue, now);
    pid = installer_continue.pid;
    window = installer_continue.window;
    pthread_mutex_unlock(&message_mutex);
    if (result != FOREVER_INSTALLER_CONTINUE_SEND) return;
    io_log("forever: installer continue tick send pid=%u window=%llx usage=%u",
           pid, (unsigned long long)window, KEY_ENTER);
    if (send_key(KEY_ENTER)) io_log("forever: installer continue sent");
    else io_log("forever: installer continue skipped reason=input-unavailable");
}
#endif

static int desktop_open(void)
{
    const char *what;
    int result, x, y;

    if ((result = display_open(&what)))
    {
        fprintf(stderr, "display: %s failed %#x\n", what, result);
        return result;
    }
    user = input_open(&result);
    fprintf(stderr, "input user=%d status=%#x %s\n", user, result, input_describe());
    pointer_init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
#ifdef PS5_TITLE_EMBEDDED
    struct forever_menu_values values = {FOREVER_FG_ON, 1, 0, 1, 0};
    forever_title_menu_load(&values);
    pointer_set_invert_scroll(values.invert_scroll);
    forever_menu_init(&menu);
    memset(physical_keys, 0, sizeof(physical_keys));
    memset(forwarded_keys, 0, sizeof(forwarded_keys));
    physical_pad = menu_pad = physical_mouse = forwarded_mouse = 0;
    input_draining = pending_menu_status_kind = pending_addon_kind = 0;
    menu_ime_command = FOREVER_MENU_COMMAND_NONE;
    menu_hint_started = 0;
    title_desktop_installer_mode(0);
#ifdef FOREVER_MENU_DIAG
    memset(&menu_diag, 0, sizeof(menu_diag));
#endif
#endif
    pointer_position(&x, &y);
    compositor_init(x, y);
    hub_init(on_hub_message, on_hub_disconnect, NULL);
    compositor_compose();
    display_present();
    fprintf(stderr, "splash dismissed: %d\n", sceSystemServiceHideSplashScreen());
    return 0;
}

static void desktop_frame(void)
{
    static struct ime_edit edit;
    int x, y;
    uint64_t now = now_us();

    pthread_mutex_lock(&message_mutex);
    if (message_pending)
    {
        compositor_message(pending_message);
        message_pending = 0;
    }
#ifdef PS5_TITLE_EMBEDDED
    if (pending_menu_status_kind)
    {
        if (pending_menu_status_kind == 3)
        {
            if (forever_menu_is_open(&menu))
            {
                forever_menu_commit(&menu, FOREVER_MENU_COMMAND_SET_VOICE, pending_voice_enabled);
                menu.voice_downloading = pending_voice_busy;
                forever_menu_status(&menu, pending_menu_status);
            }
        }
        else if (pending_menu_status_kind == 2) forever_menu_notice(&menu, pending_menu_status);
        else forever_menu_progress(&menu, pending_menu_status);
        pending_menu_status_kind = 0;
        render_menu();
    }
    if (pending_addon_kind)
    {
        if (pending_addon_kind == 1)
            forever_menu_addon_snapshot(&menu, &pending_addon_snapshot);
        else if (pending_addon_kind == 3)
        {
            if (forever_menu_addon_pause(&menu, pending_addon_error))
            {
                if (menu_ime_command && user >= 0) ime_show(user, 0);
                menu_ime_command = FOREVER_MENU_COMMAND_NONE;
                ime_clear_text();
            }
        }
        else if (pending_addon_kind == 4)
            forever_menu_addon_progress(&menu, pending_addon_error);
        else
            forever_menu_addon_error(&menu, pending_addon_error);
        pending_addon_kind = 0;
        render_menu();
    }
#endif
    pthread_mutex_unlock(&message_mutex);
#ifdef PS5_TITLE_EMBEDDED
    compositor_hint_tick(now);
#endif

    input_poll(on_input, NULL);
#if defined(PS5_TITLE_EMBEDDED) && defined(FOREVER_MENU_DIAG)
    menu_diag_step(now);
#endif
    pointer_frame(now, on_pointer, NULL);
    if (ime_poll(&edit))
    {
#ifdef PS5_TITLE_EMBEDDED
        if (menu_ime_command)
        {
            char text[IME_MAX_TEXT * 4 + 1];
            ime_copy_text(text, sizeof(text));
            if (edit.enter || edit.cancel)
                menu_ime_finish(text, edit.enter && !edit.cancel);
            /* Search filters on each keystroke, as the inline panel reports it. */
            else if (menu_ime_command == FOREVER_MENU_COMMAND_ADDONS_SEARCH_IME &&
                     forever_menu_find_set_query(&menu, text))
                render_menu();
            memset(text, 0, sizeof(text));
        }
        else if (!forever_menu_is_open(&menu) && !input_draining)
#endif
        {
            for (unsigned i = 0; i < edit.erase; i++) send_key(0x2a); /* Backspace */
            send_text(edit.text, edit.length);
            if (edit.enter) send_key(0x28);                            /* Enter */
        }
    }
    hub_poll(now);
#ifdef PS5_TITLE_EMBEDDED
    title_desktop_installer_tick(now);
#endif
    pointer_position(&x, &y);
    compositor_cursor(x, y);
    compositor_cursor_tick(now);
    compositor_compose();
#if defined(PS5_TITLE_EMBEDDED) && defined(FOREVER_MENU_DIAG)
    menu_diag_dump();
#endif
    display_present();
}

#ifndef PS5_TITLE_EMBEDDED
int main(void)
{
    pthread_t wine_thread, agc_thread;
    int result;

    ps5_klog_capture_stderr("[title] ");
    /* A peer that goes away must not end the title: writes return EPIPE. */
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    fprintf(stderr, "Wine desktop launcher\n");
    if (desktop_open()) return 1;
    /* A title owns the GPU's graphics pipe from its launch even when, like
     * this one, it never submits work: a Wine process that opened the GPU
     * then slept until the title's exit ("The other pcocess(163) uses gfx,
     * and current(175) sleeps", 2026-10-03). The kernel hands the pipe over
     * at a suspend point, and the caller then sleeps there until the pipe
     * comes back: a thread of its own reaches one every frame, so the frame
     * loop (input, cursor, windows) never waits. */
    agc = sceAgcInit(PS5_AGC_INIT_VERSION);
    fprintf(stderr, "agc init: %#x\n", (unsigned)agc);
    if (!agc && (result = pthread_create(&agc_thread, NULL, agc_yield, NULL)))
        fprintf(stderr, "AGC thread failed: %d\n", result);
    result = pthread_create(&wine_thread, NULL, wine_launch, NULL);
    if (result) fprintf(stderr, "Wine startup thread failed: %d\n", result);
    else pthread_detach(wine_thread);
    for (;;) desktop_frame();
}
#else
static pthread_t desktop_thread;
static _Atomic int desktop_running;
static int desktop_started;

static void *desktop_loop(void *unused)
{
    (void)unused;
    while (atomic_load(&desktop_running)) desktop_frame();
    return NULL;
}

int title_desktop_start(void)
{
    int result;

    if (desktop_started) return 0;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    fprintf(stderr, "B1 in-process Wine desktop\n");
    if ((result = desktop_open())) return result;
    atomic_store(&desktop_running, 1);
    if ((result = pthread_create(&desktop_thread, NULL, desktop_loop, NULL)))
    {
        atomic_store(&desktop_running, 0);
        compositor_reset();
        return result;
    }
    desktop_started = 1;
    return 0;
}

void title_desktop_stop(void)
{
    if (!desktop_started) return;
    forever_title_voice_cancel();
    atomic_store(&desktop_running, 0);
    pthread_join(desktop_thread, NULL);
    compositor_reset();
    desktop_started = 0;
}
#endif
