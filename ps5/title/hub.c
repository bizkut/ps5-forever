/* The title's connection to the payload's desktop hub (hub.h). A title
 * cannot listen or use AF_UNIX, but can connect to TCP loopback. */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wine/ps5_desktop.h>
#include "hub.h"

#ifdef PS5_TITLE_EMBEDDED

#define MAX_MESSAGE_BYTES (sizeof(struct ps5_desktop_text) + PS5_DESKTOP_MAX_TEXT * sizeof(uint16_t))
#define MESSAGES_PER_FRAME 128u

int pw_desktop_title_receive(void *message, size_t capacity);
int pw_desktop_title_connect(void);
int pw_desktop_title_send(const void *message, size_t size);

static hub_message_handler on_message;
static hub_disconnect_handler on_disconnect;
static void *handler_context;
static int connected;

static void disconnect_hub(void)
{
    if (!connected) return;
    connected = 0;
    if (on_disconnect) on_disconnect(handler_context);
}

void hub_init(hub_message_handler message, hub_disconnect_handler disconnect, void *context)
{
    on_message = message;
    on_disconnect = disconnect;
    handler_context = context;
    connected = pw_desktop_title_connect();
}

static int message_size(uint32_t type, uint32_t size)
{
    switch (type)
    {
    case PS5_DESKTOP_WINDOW:     return size == sizeof(struct ps5_desktop_window) ? 1 : -1;
    case PS5_DESKTOP_DESTROY:    return size == sizeof(struct ps5_desktop_id) ? 1 : -1;
    case PS5_DESKTOP_DAMAGE:     return size == sizeof(struct ps5_desktop_damage) ? 1 : -1;
    case PS5_DESKTOP_CURSOR_POS: return size == sizeof(struct ps5_desktop_point) ? 1 : -1;
    default:                     return 0;
    }
}

void hub_poll(uint64_t now_us)
{
    union
    {
        struct ps5_desktop_header header;
        uint64_t alignment;
        uint8_t bytes[MAX_MESSAGE_BYTES];
    } message;
    unsigned int count;

    (void)now_us;
    if (!connected) connected = pw_desktop_title_connect();
    for (count = 0; connected && count < MESSAGES_PER_FRAME; count++)
    {
        int size = pw_desktop_title_receive(message.bytes, sizeof(message.bytes));
        if (!size) break;
        if (size < (int)sizeof(message.header) || (uint32_t)size != message.header.size ||
            message.header.size > sizeof(message.bytes) ||
            message_size(message.header.type, message.header.size) < 0)
        {
            disconnect_hub();
            break;
        }
        if (message_size(message.header.type, message.header.size) && on_message)
            on_message(message.bytes, handler_context);
    }
}

int hub_send(const void *message, size_t size)
{
    struct ps5_desktop_header header;

    if (!connected || size < sizeof(header) || size > MAX_MESSAGE_BYTES) return 0;
    memcpy(&header, message, sizeof(header));
    if (header.size != size || !pw_desktop_title_send(message, size))
    {
        disconnect_hub();
        return 0;
    }
    return 1;
}

#else


#define RETRY_US 1000000u
#define RECEIVE_BYTES 8192u
#define SEND_BYTES 65536u
#define IO_BYTES_PER_FRAME 65536u
#define MESSAGES_PER_FRAME 128u
#define CALLS_PER_FRAME 32u
#define MAX_MESSAGE_BYTES (sizeof(struct ps5_desktop_text) + PS5_DESKTOP_MAX_TEXT * sizeof(uint16_t))

static int socket_fd = -1, connecting, connected;
static uint64_t retry_at, connect_until, last_poll;
static uint8_t received[RECEIVE_BYTES], outgoing[SEND_BYTES];
static size_t received_size, outgoing_start, outgoing_size;
static hub_message_handler on_message;
static hub_disconnect_handler on_disconnect;
static void *handler_context;

static void disconnect_hub(void)
{
    int was_connected = connected;

    if (socket_fd >= 0) close(socket_fd);
    socket_fd = -1;
    connecting = connected = 0;
    received_size = outgoing_start = outgoing_size = 0;
    retry_at = last_poll + RETRY_US;
    if (was_connected && on_disconnect) on_disconnect(handler_context);
}

void hub_init(hub_message_handler message, hub_disconnect_handler disconnect, void *context)
{
    if (socket_fd >= 0) disconnect_hub();
    on_message = message;
    on_disconnect = disconnect;
    handler_context = context;
    retry_at = 0;
}

static void connect_hub(uint64_t now_us)
{
    struct sockaddr_in address = {
        .sin_len = sizeof(address),
        .sin_family = AF_INET,
        .sin_port = htons(PS5_DESKTOP_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    retry_at = connect_until = now_us + RETRY_US;
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        static int reported;
        if (reported != errno) fprintf(stderr, "hub: socket failed errno=%d\n", errno);
        reported = errno;
        return;
    }
    /* A title may not make a socket non-blocking (fcntl F_SETFL: EACCES):
     * the connect is blocking (loopback answers or refuses at once) and every
     * send and receive is non-blocking on its own (MSG_DONTWAIT). */
    if (!connect(socket_fd, (struct sockaddr *)&address, sizeof(address)))
        connected = 1;
    else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EINTR)
        connecting = 1;
    else
        disconnect_hub();
}

static void finish_connect(uint64_t now_us)
{
    struct pollfd descriptor = {.fd = socket_fd, .events = POLLOUT};
    int error = 0, result;
    socklen_t size = sizeof(error);

    result = poll(&descriptor, 1, 0);
    if (result < 0 && errno != EINTR)
        disconnect_hub();
    else if (result > 0 && descriptor.revents)
    {
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error ||
            !(descriptor.revents & POLLOUT))
            disconnect_hub();
        else
        {
            connecting = 0;
            connected = 1;
        }
    }
    else if (now_us >= connect_until)
        disconnect_hub();
}

/* Only messages the title consumes are delivered. A malformed known frame
 * ends the connection; unknown bounded frames can be skipped safely. */
static int message_size(uint32_t type, uint32_t size)
{
    switch (type)
    {
    case PS5_DESKTOP_WINDOW:     return size == sizeof(struct ps5_desktop_window) ? 1 : -1;
    case PS5_DESKTOP_DESTROY:    return size == sizeof(struct ps5_desktop_id) ? 1 : -1;
    case PS5_DESKTOP_DAMAGE:     return size == sizeof(struct ps5_desktop_damage) ? 1 : -1;
    case PS5_DESKTOP_CURSOR_POS: return size == sizeof(struct ps5_desktop_point) ? 1 : -1;
    default:                    return 0;
    }
}

static void receive_messages(void)
{
    size_t bytes = 0;
    unsigned messages = 0, calls = 0;

    while (connected && messages < MESSAGES_PER_FRAME && calls < CALLS_PER_FRAME)
    {
        size_t consumed = 0;

        while (received_size - consumed >= sizeof(struct ps5_desktop_header) &&
               messages < MESSAGES_PER_FRAME)
        {
            struct ps5_desktop_header header;
            int deliver;

            memcpy(&header, received + consumed, sizeof(header));
            if (header.size < sizeof(header) || header.size > MAX_MESSAGE_BYTES ||
                (deliver = message_size(header.type, header.size)) < 0)
            {
                disconnect_hub();
                return;
            }
            if (received_size - consumed < header.size) break;
            if (deliver && on_message) on_message(received + consumed, handler_context);
            consumed += header.size;
            messages++;
        }
        if (consumed)
        {
            received_size -= consumed;
            memmove(received, received + consumed, received_size);
        }
        if (messages == MESSAGES_PER_FRAME || bytes == IO_BYTES_PER_FRAME) break;
        size_t space = sizeof(received) - received_size;
        if (space > IO_BYTES_PER_FRAME - bytes) space = IO_BYTES_PER_FRAME - bytes;
        if (!space)
        {
            disconnect_hub();
            return;
        }
        ssize_t count = recv(socket_fd, received + received_size, space, MSG_DONTWAIT);
        calls++;
        if (count > 0)
        {
            received_size += (size_t)count;
            bytes += (size_t)count;
        }
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        else
        {
            disconnect_hub();
            return;
        }
    }
}

static void send_messages(void)
{
    size_t bytes = 0;
    unsigned calls = 0;

    while (connected && outgoing_size && bytes < IO_BYTES_PER_FRAME && calls < CALLS_PER_FRAME)
    {
        size_t size = sizeof(outgoing) - outgoing_start;
        if (size > outgoing_size) size = outgoing_size;
        if (size > IO_BYTES_PER_FRAME - bytes) size = IO_BYTES_PER_FRAME - bytes;
        ssize_t count = send(socket_fd, outgoing + outgoing_start, size, MSG_DONTWAIT);
        calls++;
        if (count > 0)
        {
            outgoing_start = (outgoing_start + (size_t)count) % sizeof(outgoing);
            outgoing_size -= (size_t)count;
            bytes += (size_t)count;
        }
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        else
        {
            disconnect_hub();
            return;
        }
    }
}

void hub_poll(uint64_t now_us)
{
    last_poll = now_us;
    if (socket_fd < 0 && now_us >= retry_at) connect_hub(now_us);
    if (connecting) finish_connect(now_us);
    if (!connected) return;
    receive_messages();
    send_messages();
}

int hub_send(const void *message, size_t size)
{
    struct ps5_desktop_header header;
    size_t end, first;

    if (!connected) return 0;
    if (size < sizeof(header) || size > MAX_MESSAGE_BYTES) return 0;
    memcpy(&header, message, sizeof(header));
    if (header.size != size) return 0;
    if (size > sizeof(outgoing) - outgoing_size)
    {
        disconnect_hub();
        return 0;
    }
    end = (outgoing_start + outgoing_size) % sizeof(outgoing);
    first = sizeof(outgoing) - end;
    if (first > size) first = size;
    memcpy(outgoing + end, message, first);
    memcpy(outgoing, (const uint8_t *)message + first, size - first);
    outgoing_size += size;
    return 1;
}

#endif /* PS5_TITLE_EMBEDDED */
