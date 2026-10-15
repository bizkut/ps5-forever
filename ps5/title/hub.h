/* The title's non-blocking TCP connection to Wine's desktop hub. Messages
 * use wine/ps5_desktop.h; callbacks receive complete, size-checked frames. */
#ifndef PS5_TITLE_HUB_H
#define PS5_TITLE_HUB_H

#include <stddef.h>
#include <stdint.h>

typedef void (*hub_message_handler)(const void *message, void *context);
typedef void (*hub_disconnect_handler)(void *context);

void hub_init(hub_message_handler message, hub_disconnect_handler disconnect, void *context);
/* Once a frame: connects/reconnects, receives frames and drains queued input.
 * now_us is monotonic. Socket calls never wait; work per call is bounded. */
void hub_poll(uint64_t now_us);
/* Queues a complete frame, or returns 0 if disconnected. Queue exhaustion
 * disconnects rather than dropping an individual button/key transition. */
int hub_send(const void *message, size_t size);

#endif
