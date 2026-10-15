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
#include "hub.h"
#include "ime.h"
#include "input.h"
#include "pointer.h"

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

/* Producers never touch compositor state; the desktop consumes the copy. */
void title_desktop_message(const char *text)
{
    pthread_mutex_lock(&message_mutex);
    snprintf(pending_message, sizeof(pending_message), "%s", text ? text : "");
    message_pending = 1;
    pthread_mutex_unlock(&message_mutex);
}

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
    case PS5_DESKTOP_TEXT_INPUT:
    {
        /* A text field took (active) or lost the focus. Chromium does not
         * report password fields: L3+R3 open the keyboard there. */
        struct ps5_desktop_text_input input;
        memcpy(&input, message, sizeof(input));
        if (user >= 0) ime_show(user, input.active != 0);
        break;
    }
    }
}

static void on_hub_disconnect(void *context)
{
    (void)context;
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

static void on_pointer(const struct pointer_event *event, void *context)
{
    int x, y;
    struct ps5_desktop_mouse mouse = {.header = {PS5_DESKTOP_MOUSE, sizeof(mouse)}};
    (void)context;

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
        hub_send(&key, sizeof(key));
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
    hub_send(&mouse, sizeof(mouse));
}

static void on_input(const struct input_event *event, void *context)
{
    pointer_input(event, on_pointer, context);
}

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

static void send_key(uint32_t usage)
{
    struct ps5_desktop_key key = {.header = {PS5_DESKTOP_KEY, sizeof(key)}, .usage = usage};
    key.down = 1;
    hub_send(&key, sizeof(key));
    key.down = 0;
    hub_send(&key, sizeof(key));
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
    pthread_mutex_unlock(&message_mutex);

    input_poll(on_input, NULL);
    pointer_frame(now, on_pointer, NULL);
    if (ime_poll(&edit))
    {
        for (unsigned i = 0; i < edit.erase; i++) send_key(0x2a); /* Backspace */
        send_text(edit.text, edit.length);
        if (edit.enter) send_key(0x28);                            /* Enter */
    }
    hub_poll(now);
    pointer_position(&x, &y);
    compositor_cursor(x, y);
    compositor_compose();
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
    atomic_store(&desktop_running, 0);
    pthread_join(desktop_thread, NULL);
    compositor_reset();
    desktop_started = 0;
}
#endif
