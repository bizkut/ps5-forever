/* Exercise the real TCP stream decoder with local socketpairs, never a console. */
#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
/* FreeBSD's sockaddr length initializer is immaterial to socketpair tests. */
#define sin_len sin_zero[0]
#include "../../title/hub.c"
#undef sin_len

static unsigned delivered, disconnected;
static void message_handler(const void *message, void *context)
{
    struct ps5_desktop_header header;
    (void)context;
    memcpy(&header, message, sizeof(header));
    assert(header.type == PS5_DESKTOP_CURSOR_SHAPE || header.type == PS5_DESKTOP_CURSOR_POS);
    delivered++;
}
static void disconnect_handler(void *context) { (void)context; disconnected++; }
static void send_bytes(int fd, const void *message, size_t size)
{
    assert(send(fd, message, size, 0) == (ssize_t)size);
    receive_messages();
}
static int connect_pair(void)
{
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    socket_fd = pair[0];
    connected = 1;
    return pair[1];
}
int main(void)
{
    union {
        struct ps5_desktop_cursor_shape shape;
        unsigned char bytes[PS5_DESKTOP_MAX_MESSAGE];
    } message = {.shape = {.header = {PS5_DESKTOP_CURSOR_SHAPE, PS5_DESKTOP_MAX_MESSAGE},
        .width = 64, .height = 64, .hotspot_x = 63, .hotspot_y = 63, .visible = 1}};
    struct ps5_desktop_point point = {{PS5_DESKTOP_CURSOR_POS, sizeof(point)}, 10, 20};
    struct ps5_desktop_header unknown = {0xf000, sizeof(unknown)};
    int peer;
    memset(message.shape.pixels, 0xff, 64 * 64 * 4);
    hub_init(message_handler, disconnect_handler, NULL);
    peer = connect_pair();
    send_bytes(peer, &message, 3);
    assert(!delivered && !disconnected);
    send_bytes(peer, message.bytes + 3, sizeof(message.bytes) - 7);
    assert(!delivered && !disconnected); /* complete dimensions, incomplete pixels */
    send_bytes(peer, message.bytes + sizeof(message.bytes) - 4, 4);
    assert(delivered == 1 && !disconnected);
    send_bytes(peer, &unknown, sizeof(unknown));
    send_bytes(peer, &point, sizeof(point));
    assert(delivered == 2 && !disconnected); /* unknown frame did not desynchronize */
    message.shape.width = 65;
    send_bytes(peer, &message, sizeof(message));
    assert(delivered == 2 && disconnected == 1 && !connected);
    close(peer);
    peer = connect_pair();
    message.shape.width = 64;
    message.shape.header.size -= 4;
    send_bytes(peer, &message, message.shape.header.size);
    assert(delivered == 2 && disconnected == 2); /* truncated image rejected */
    close(peer);
    peer = connect_pair();
    unknown.size = PS5_DESKTOP_MAX_MESSAGE + 1;
    send_bytes(peer, &unknown, sizeof(unknown));
    assert(disconnected == 3 && !connected); /* oversized envelope rejected before buffering */
    close(peer);
    peer = connect_pair();
    message.shape = (struct ps5_desktop_cursor_shape){
        .header = {PS5_DESKTOP_CURSOR_SHAPE, sizeof(message.shape)}};
    send_bytes(peer, &message, sizeof(message.shape));
    assert(delivered == 3 && disconnected == 3);
    close(peer);
    receive_messages();
    assert(disconnected == 4);
    puts("cursor socket hub: PASS (fragmentation, maximum image, hide, unknown, malformed)");
    return 0;
}
