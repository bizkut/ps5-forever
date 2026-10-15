/* Parent-only PsmKit supervisor. The launch handle is never a Unix pid. */
#include "runtime-parent.h"
#include "protocol.h"
#include "runtime-relaunch.h"
#include "../localproc/probe.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
#include <wine/ps5_desktop.h>
#ifdef PW_RUNTIME_B1
#include <netinet/in.h>
#include <strings.h>

int sceSystemServiceLaunchWebBrowser(const char *, const void *);
int sceUserServiceInitialize(const void *);
#if !PW_RUNTIME_HEADLESS
void title_desktop_message(const char *);
#endif
#endif

int32_t sceSystemServiceAddLocalProcessForPsmKit(
    const char *, const char *const [], const struct local_process_param *);
int32_t sceSystemServiceKillLocalProcessForPsmKit(int32_t);
_Static_assert(sizeof(struct local_process_param) == 0x90, "launch parameter size");
static const char child_path[] = PW_RUNTIME_CHILD_PATH;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t reaper;
static struct pw_slot slots[PW_CHILDREN];
static uint64_t generation;
static int stopping, started;
static int64_t shutdown_grace, shutdown_deadline;

#if defined(PW_RUNTIME_B1) && !PW_RUNTIME_HEADLESS

#define PW_HUB_MAX_FRAME (sizeof(struct ps5_desktop_text) + PS5_DESKTOP_MAX_TEXT * sizeof(uint16_t))
#define PW_HUB_MAX_OUTPUT (1024u * 1024u)
#define PW_HUB_TITLE_INPUT (64u * 1024u)
#define PW_HUB_TITLE_OUTPUT (1024u * 1024u)

struct pw_hub_peer
{
    struct pw_hub_peer *next;
    int fd, hello;
    unsigned int sink_order;
    uint32_t pid;
    unsigned int poll_index, input_size;
    union
    {
        struct ps5_desktop_header header;
        struct ps5_desktop_hello hello;
        struct ps5_desktop_window window;
        struct ps5_desktop_id id;
        uint64_t alignment;
        unsigned char bytes[PW_HUB_MAX_FRAME];
    } input;
    char *output;
    size_t output_begin, output_end, output_capacity;
};

struct pw_hub_window
{
    struct pw_hub_window *above, *below;
    struct pw_hub_peer *owner;
    struct ps5_desktop_window state;
};

struct pw_desktop_hub
{
    struct pw_hub_peer *peers, *input_sink;
    unsigned int sink_serial;
    int input_dropped;
    struct pw_hub_window *top, *bottom;
};

struct pw_ring
{
    unsigned char *data;
    size_t capacity, start, size;
};

static pthread_mutex_t desktop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t desktop_thread;
static struct pw_hub_peer *desktop_pending;
static struct pw_desktop_hub desktop_hub;
static unsigned char desktop_title_input_bytes[PW_HUB_TITLE_INPUT];
static unsigned char desktop_title_output_bytes[PW_HUB_TITLE_OUTPUT];
static struct pw_ring desktop_title_input = {
    .data = desktop_title_input_bytes, .capacity = sizeof(desktop_title_input_bytes)
};
static struct pw_ring desktop_title_output = {
    .data = desktop_title_output_bytes, .capacity = sizeof(desktop_title_output_bytes)
};
static int desktop_started, desktop_stopping, desktop_title_connected, desktop_replay_requested;

int title_desktop_start(void);
void title_desktop_stop(void);

_Static_assert(PW_DESKTOP_NAME_BYTES == PS5_DESKTOP_SHM_NAME, "desktop name wire size");

static void ring_reset(struct pw_ring *ring)
{
    ring->start = ring->size = 0;
}

static int ring_write(struct pw_ring *ring, const void *data, size_t size)
{
    size_t end, first;

    if (size > ring->capacity - ring->size) return 0;
    end = (ring->start + ring->size) % ring->capacity;
    first = ring->capacity - end;
    if (first > size) first = size;
    memcpy(ring->data + end, data, first);
    memcpy(ring->data, (const unsigned char *)data + first, size - first);
    ring->size += size;
    return 1;
}

static void ring_peek(const struct pw_ring *ring, void *data, size_t size)
{
    size_t first = ring->capacity - ring->start;
    if (first > size) first = size;
    memcpy(data, ring->data + ring->start, first);
    memcpy((unsigned char *)data + first, ring->data, size - first);
}

static void ring_discard(struct pw_ring *ring, size_t size)
{
    ring->start = (ring->start + size) % ring->capacity;
    ring->size -= size;
    if (!ring->size) ring->start = 0;
}

static int ring_read_message(struct pw_ring *ring, void *message, size_t capacity)
{
    struct ps5_desktop_header header;

    if (!ring->size) return 0;
    if (ring->size < sizeof(header)) return -1;
    ring_peek(ring, &header, sizeof(header));
    if (header.size < sizeof(header) || header.size > capacity || header.size > ring->size)
        return -1;
    ring_peek(ring, message, header.size);
    ring_discard(ring, header.size);
    return (int)header.size;
}

int pw_desktop_title_connect(void)
{
    int result = 0;

    pthread_mutex_lock(&desktop_lock);
    if (desktop_started && !desktop_stopping)
    {
        ring_reset(&desktop_title_output);
        desktop_title_connected = 1;
        desktop_replay_requested = 1;
        result = 1;
    }
    pthread_mutex_unlock(&desktop_lock);
    return result;
}

int pw_desktop_title_receive(void *message, size_t capacity)
{
    int result;

    pthread_mutex_lock(&desktop_lock);
    result = ring_read_message(&desktop_title_output, message, capacity);
    if (!result && !desktop_title_connected) result = -1;
    pthread_mutex_unlock(&desktop_lock);
    return result;
}

int pw_desktop_title_send(const void *message, size_t size)
{
    struct ps5_desktop_header header;
    int result = 0;

    if (!message || size < sizeof(header) || size > PW_HUB_MAX_FRAME) return 0;
    memcpy(&header, message, sizeof(header));
    if (header.size != size) return 0;
    pthread_mutex_lock(&desktop_lock);
    if (desktop_started && !desktop_stopping && desktop_title_connected)
        result = ring_write(&desktop_title_input, message, size);
    pthread_mutex_unlock(&desktop_lock);
    return result;
}

static int hub_queue_title(const void *data, size_t size)
{
    int result;

    pthread_mutex_lock(&desktop_lock);
    result = desktop_title_connected && ring_write(&desktop_title_output, data, size);
    if (!result && desktop_title_connected)
    {
        desktop_title_connected = 0;
        ring_reset(&desktop_title_output);
        io_log("b1 desktop title queue overflow bytes=%zu", size);
    }
    pthread_mutex_unlock(&desktop_lock);
    return result;
}

static void hub_drop_peer(struct pw_hub_peer *peer)
{
    if (peer->fd >= 0) close(peer->fd);
    peer->fd = -1;
}

static int hub_queue_peer(struct pw_hub_peer *peer, const void *data, size_t size)
{
    size_t pending, capacity;
    char *output;

    if (!peer || peer->fd < 0) return 0;
    pending = peer->output_end - peer->output_begin;
    if (pending + size > PW_HUB_MAX_OUTPUT) goto failed;
    if (peer->output_capacity - peer->output_end < size)
    {
        if (pending + size > peer->output_capacity)
        {
            capacity = peer->output_capacity ? peer->output_capacity : 4096;
            while (capacity < pending + size) capacity *= 2;
            if (!(output = realloc(peer->output, capacity))) goto failed;
            peer->output = output;
            peer->output_capacity = capacity;
        }
        if (pending) memmove(peer->output, peer->output + peer->output_begin, pending);
        peer->output_begin = 0;
        peer->output_end = pending;
    }
    memcpy(peer->output + peer->output_end, data, size);
    peer->output_end += size;
    return 1;

failed:
    hub_drop_peer(peer);
    return 0;
}

static void hub_flush(struct pw_hub_peer *peer)
{
    size_t size, sent = 0;

    while (peer->fd >= 0 && peer->output_begin < peer->output_end && sent < 65536)
    {
        size = peer->output_end - peer->output_begin;
        if (size > 65536 - sent) size = 65536 - sent;
        ssize_t result = send(peer->fd, peer->output + peer->output_begin, size,
                              MSG_DONTWAIT | MSG_NOSIGNAL);
        if (result > 0)
        {
            peer->output_begin += (size_t)result;
            sent += (size_t)result;
        }
        else if (result < 0 && errno == EINTR) continue;
        else if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        else
        {
            hub_drop_peer(peer);
            return;
        }
    }
    if (peer->output_begin == peer->output_end) peer->output_begin = peer->output_end = 0;
}

static struct pw_hub_window *hub_find_window(struct pw_desktop_hub *hub, uint64_t id)
{
    struct pw_hub_window *window;
    for (window = hub->top; window; window = window->below)
        if (window->state.id == id) return window;
    return NULL;
}

static void hub_unlink_window(struct pw_desktop_hub *hub, struct pw_hub_window *window)
{
    if (window->above) window->above->below = window->below;
    else if (hub->top == window) hub->top = window->below;
    if (window->below) window->below->above = window->above;
    else if (hub->bottom == window) hub->bottom = window->above;
    window->above = window->below = NULL;
}

static void hub_remove_window(struct pw_desktop_hub *hub, struct pw_hub_window *window)
{
    hub_unlink_window(hub, window);
    if (window->state.shm[0])
        pw_desktop_surface(window->state.shm, 0, PS5_TITLE_SURFACE_UNLINK);
    free(window);
}

static void hub_restack(struct pw_desktop_hub *hub, struct pw_hub_window *window, uint64_t after)
{
    struct pw_hub_window *other, *position = NULL, *last_topmost = NULL;
    int topmost = !!(window->state.flags & PS5_DESKTOP_TOPMOST);

    hub_unlink_window(hub, window);
    for (other = hub->top; other; other = other->below)
        if (other->state.flags & PS5_DESKTOP_TOPMOST) last_topmost = other;
    if (!topmost) position = last_topmost;
    if (after == PS5_DESKTOP_Z_BOTTOM)
    {
        for (other = hub->top; other; other = other->below)
            if (!!(other->state.flags & PS5_DESKTOP_TOPMOST) == topmost) position = other;
    }
    else if (after != PS5_DESKTOP_Z_TOP && (other = hub_find_window(hub, after)))
    {
        if (!!(other->state.flags & PS5_DESKTOP_TOPMOST) == topmost) position = other;
        else position = last_topmost;
    }
    window->above = position;
    window->below = position ? position->below : hub->top;
    if (window->below) window->below->above = window;
    else hub->bottom = window;
    if (position) position->below = window;
    else hub->top = window;
}

static void hub_replay(struct pw_desktop_hub *hub)
{
    struct pw_hub_window *window;
    struct ps5_desktop_window state;
    struct ps5_desktop_damage damage = {
        .header = {PS5_DESKTOP_DAMAGE, sizeof(damage)}
    };
    uint64_t previous = PS5_DESKTOP_Z_TOP;

    for (window = hub->top; window; window = window->below)
    {
        state = window->state;
        state.flags |= PS5_DESKTOP_ZORDER;
        state.insert_after = previous;
        if (!hub_queue_title(&state, sizeof(state))) return;
        previous = state.id;
    }
    for (window = hub->top; window; window = window->below)
    {
        damage.id = window->state.id;
        damage.left = damage.top = 0;
        damage.right = window->state.width;
        damage.bottom = window->state.height;
        if (!hub_queue_title(&damage, sizeof(damage))) return;
    }
}

static struct pw_hub_peer *hub_previous_sink(struct pw_desktop_hub *hub)
{
    struct pw_hub_peer *peer, *best = NULL;

    for (peer = hub->peers; peer; peer = peer->next)
        if (peer->fd >= 0 && peer->sink_order &&
            (!best || peer->sink_order > best->sink_order)) best = peer;
    return best;
}

static void hub_set_sink(struct pw_desktop_hub *hub, struct pw_hub_peer *peer)
{
    if (hub->input_sink == peer) return;
    hub->input_sink = peer;
    if (peer)
    {
        hub->input_dropped = 0;
        io_log("b1 desktop input sink pid=%u", peer->pid);
    }
    else io_log("b1 desktop has no input sink");
}

static int hub_driver_message(struct pw_desktop_hub *hub, struct pw_hub_peer *peer)
{
    const struct ps5_desktop_header *header = &peer->input.header;
    const struct ps5_desktop_window *state = &peer->input.window;
    const struct ps5_desktop_hello *hello = &peer->input.hello;
    struct pw_hub_window *window;
    int restack;

    if (header->type == PS5_DESKTOP_HELLO)
    {
        if (header->size != sizeof(*hello) || !hello->pid ||
            (peer->hello && peer->pid != hello->pid)) return 0;
        peer->pid = hello->pid;
        peer->hello = 1;
        if (hello->flags & PS5_DESKTOP_INPUT_SINK)
        {
            peer->sink_order = ++hub->sink_serial;
            hub_set_sink(hub, peer);
        }
        else
        {
            peer->sink_order = 0;
            if (hub->input_sink == peer) hub_set_sink(hub, hub_previous_sink(hub));
        }
        return 1;
    }
    if (!peer->hello) return 0;
    switch (header->type)
    {
    case PS5_DESKTOP_WINDOW:
        if (header->size != sizeof(*state) || state->id <= PS5_DESKTOP_Z_BOTTOM ||
            !memchr(state->shm, 0, sizeof(state->shm)) ||
            (state->shm[0] && strncmp(state->shm, "/wine-ps5-", 10))) return 0;
        window = hub_find_window(hub, state->id);
        restack = !window || (state->flags & PS5_DESKTOP_ZORDER);
        if (!window)
        {
            if (!(window = calloc(1, sizeof(*window)))) return 0;
        }
        else
        {
            restack |= !!((window->state.flags ^ state->flags) & PS5_DESKTOP_TOPMOST);
            if (strcmp(window->state.shm, state->shm) && window->state.shm[0])
                pw_desktop_surface(window->state.shm, 0, PS5_TITLE_SURFACE_UNLINK);
        }
        window->state = *state;
        window->owner = peer;
        if (restack) hub_restack(hub, window, state->insert_after);
        hub_queue_title(header, header->size);
        return 1;
    case PS5_DESKTOP_DESTROY:
        if (header->size != sizeof(struct ps5_desktop_id)) return 0;
        if ((window = hub_find_window(hub, peer->input.id.id)) && window->owner == peer)
        {
            hub_queue_title(header, header->size);
            hub_remove_window(hub, window);
        }
        return 1;
    case PS5_DESKTOP_DAMAGE:
        if (header->size != sizeof(struct ps5_desktop_damage)) return 0;
        if (hub_find_window(hub, peer->input.id.id)) hub_queue_title(header, header->size);
        return 1;
    case PS5_DESKTOP_CURSOR_POS:
        if (header->size != sizeof(struct ps5_desktop_point)) return 0;
        hub_queue_title(header, header->size);
        return 1;
    case PS5_DESKTOP_TEXT_INPUT:
        if (header->size != sizeof(struct ps5_desktop_text_input)) return 0;
        hub_queue_title(header, header->size);
        return 1;
    default:
        return 0;
    }
}

static int hub_title_message(struct pw_desktop_hub *hub, const void *message)
{
    const struct ps5_desktop_header *header = message;
    const struct ps5_desktop_text *text = message;

    switch (header->type)
    {
    case PS5_DESKTOP_MOUSE:
        if (header->size != sizeof(struct ps5_desktop_mouse)) return 0;
        break;
    case PS5_DESKTOP_KEY:
        if (header->size != sizeof(struct ps5_desktop_key)) return 0;
        break;
    case PS5_DESKTOP_TEXT:
        if (header->size < sizeof(*text) || text->length > PS5_DESKTOP_MAX_TEXT ||
            header->size != sizeof(*text) + text->length * sizeof(text->text[0])) return 0;
        break;
    default:
        return 0;
    }
    if (!hub->input_sink && !hub->input_dropped)
    {
        io_log("b1 desktop input dropped without sink");
        hub->input_dropped = 1;
    }
    hub_queue_peer(hub->input_sink, header, header->size);
    return 1;
}

static void hub_read(struct pw_desktop_hub *hub, struct pw_hub_peer *peer)
{
    unsigned int wanted, count = 0;

    while (count < 64)
    {
        wanted = sizeof(peer->input.header);
        if (peer->input_size >= wanted)
        {
            wanted = peer->input.header.size;
            if (wanted < sizeof(peer->input.header) || wanted > sizeof(peer->input.bytes)) break;
            if (peer->input_size == wanted)
            {
                if (!hub_driver_message(hub, peer)) break;
                peer->input_size = 0;
                count++;
                continue;
            }
        }
        ssize_t result = recv(peer->fd, peer->input.bytes + peer->input_size,
                              wanted - peer->input_size, MSG_DONTWAIT);
        if (result > 0) peer->input_size += (unsigned int)result;
        else if (result < 0 && errno == EINTR) continue;
        else if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        else break;
    }
    if (count < 64)
    {
        if (peer->hello && peer->input_size >= sizeof(peer->input.header))
            io_log("b1 desktop pid=%u invalid type=%u size=%u", peer->pid,
                   peer->input.header.type, peer->input.header.size);
        else if (peer->hello) io_log("b1 desktop pid=%u disconnected", peer->pid);
        hub_drop_peer(peer);
    }
}

static void hub_cleanup_peers(struct pw_desktop_hub *hub)
{
    struct ps5_desktop_id destroy = {
        .header = {PS5_DESKTOP_DESTROY, sizeof(destroy)}
    };
    struct pw_hub_peer **ptr, *peer;
    struct pw_hub_window *window, *next;

    for (ptr = &hub->peers; (peer = *ptr); )
    {
        if (peer->fd >= 0)
        {
            ptr = &peer->next;
            continue;
        }
        if (hub->input_sink == peer) hub_set_sink(hub, hub_previous_sink(hub));
        for (window = hub->top; window; window = next)
        {
            next = window->below;
            if (window->owner != peer) continue;
            destroy.id = window->state.id;
            hub_queue_title(&destroy, sizeof(destroy));
            hub_remove_window(hub, window);
        }
        *ptr = peer->next;
        free(peer->output);
        free(peer);
    }
}

static int take_title_message(void *message, size_t capacity)
{
    int result;
    pthread_mutex_lock(&desktop_lock);
    result = ring_read_message(&desktop_title_input, message, capacity);
    pthread_mutex_unlock(&desktop_lock);
    return result;
}

static int take_replay_request(void)
{
    int result;
    pthread_mutex_lock(&desktop_lock);
    result = desktop_replay_requested;
    desktop_replay_requested = 0;
    pthread_mutex_unlock(&desktop_lock);
    return result;
}

static int desktop_should_stop(void)
{
    int result;
    pthread_mutex_lock(&desktop_lock);
    result = desktop_stopping;
    pthread_mutex_unlock(&desktop_lock);
    return result;
}

static void hub_add_pending(struct pw_desktop_hub *hub)
{
    struct pw_hub_peer *pending, *next;

    pthread_mutex_lock(&desktop_lock);
    pending = desktop_pending;
    desktop_pending = NULL;
    pthread_mutex_unlock(&desktop_lock);
    for (; pending; pending = next)
    {
        next = pending->next;
        pending->next = hub->peers;
        hub->peers = pending;
        io_log("b1 desktop driver peer connected fd=%d", pending->fd);
    }
}

static void *desktop_hub_loop(void *unused)
{
    struct pw_desktop_hub *hub = &desktop_hub;
    struct pw_hub_peer *peer;
    struct pollfd *pollfds = NULL, *new_pollfds;
    union
    {
        struct ps5_desktop_header header;
        uint64_t alignment;
        unsigned char bytes[PW_HUB_MAX_FRAME];
    } title_message;
    unsigned int count, capacity = 0;

    (void)unused;
    while (!desktop_should_stop())
    {
        hub_add_pending(hub);
        if (take_replay_request()) hub_replay(hub);
        for (unsigned int i = 0; i < 128; i++)
        {
            int size = take_title_message(title_message.bytes, sizeof(title_message.bytes));
            if (!size) break;
            if (size < 0 || !hub_title_message(hub, title_message.bytes))
            {
                pthread_mutex_lock(&desktop_lock);
                desktop_title_connected = 0;
                ring_reset(&desktop_title_input);
                pthread_mutex_unlock(&desktop_lock);
                break;
            }
        }
        count = 0;
        for (peer = hub->peers; peer; peer = peer->next) count++;
        if (count > capacity)
        {
            capacity = capacity ? capacity * 2 : 16;
            while (capacity < count) capacity *= 2;
            if (!(new_pollfds = realloc(pollfds, capacity * sizeof(*pollfds))))
            {
                pthread_mutex_lock(&desktop_lock);
                desktop_stopping = 1;
                desktop_title_connected = 0;
                pthread_mutex_unlock(&desktop_lock);
                io_log("b1 desktop hub poll allocation failed");
                break;
            }
            pollfds = new_pollfds;
        }
        count = 0;
        for (peer = hub->peers; peer; peer = peer->next)
        {
            peer->poll_index = count;
            pollfds[count++] = (struct pollfd){
                .fd = peer->fd,
                .events = POLLIN | (peer->output_end > peer->output_begin ? POLLOUT : 0),
            };
        }
        int result = poll(pollfds, count, 16);
        if (result < 0 && errno != EINTR)
        {
            pthread_mutex_lock(&desktop_lock);
            desktop_stopping = 1;
            desktop_title_connected = 0;
            pthread_mutex_unlock(&desktop_lock);
            io_log("b1 desktop hub poll failed errno=%d", errno);
            break;
        }
        if (result > 0)
            for (peer = hub->peers; peer; peer = peer->next)
            {
                if (peer->fd < 0) continue;
                int events = pollfds[peer->poll_index].revents;
                if (events & (POLLIN | POLLHUP)) hub_read(hub, peer);
                if (peer->fd >= 0 && (events & POLLOUT)) hub_flush(peer);
                if (events & (POLLERR | POLLNVAL)) hub_drop_peer(peer);
            }
        hub_cleanup_peers(hub);
    }
    for (peer = hub->peers; peer; peer = peer->next) hub_drop_peer(peer);
    hub_cleanup_peers(hub);
    free(pollfds);
    pthread_mutex_lock(&desktop_lock);
    while ((peer = desktop_pending))
    {
        desktop_pending = peer->next;
        hub_drop_peer(peer);
        free(peer);
    }
    desktop_title_connected = 0;
    ring_reset(&desktop_title_input);
    pthread_mutex_unlock(&desktop_lock);
    return NULL;
}

static int surface_name_valid(const char *name)
{
    size_t length;

    if (!name || strncmp(name, "/wine-ps5-", 10)) return 0;
    length = strnlen(name, PW_DESKTOP_NAME_BYTES);
    if (length <= 10 || length >= PW_DESKTOP_NAME_BYTES) return 0;
    return !memchr(name + 1, '/', length - 1);
}

int pw_desktop_surface(const char *name, uint64_t size, int op)
{
    struct stat status;
    int fd, error;

    if (!surface_name_valid(name) || size > PW_DESKTOP_SURFACE_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    if (op == PS5_TITLE_SURFACE_UNLINK)
    {
        if (size)
        {
            errno = EINVAL;
            return -1;
        }
        return shm_unlink(name);
    }
    if (op != PS5_TITLE_SURFACE_CREATE && op != PS5_TITLE_SURFACE_OPEN)
    {
        errno = EINVAL;
        return -1;
    }
    if (op == PS5_TITLE_SURFACE_CREATE)
    {
        if (!size)
        {
            errno = EINVAL;
            return -1;
        }
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0) return -1;
        if (ftruncate(fd, (off_t)size) || fchmod(fd, 0666))
        {
            error = errno;
            close(fd);
            shm_unlink(name);
            errno = error;
            return -1;
        }
        return fd;
    }
    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return -1;
    if (fstat(fd, &status))
    {
        error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    if (status.st_size < 0 || (uint64_t)status.st_size < size)
    {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

int pw_desktop_connect(void)
{
    struct pw_hub_peer *peer;
    int pair[2], error;

    if (!(peer = calloc(1, sizeof(*peer)))) return -1;
    peer->fd = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
    {
        free(peer);
        return -1;
    }
    peer->fd = pair[0];
    pthread_mutex_lock(&desktop_lock);
    if (!desktop_started || desktop_stopping)
    {
        error = ENOSYS;
        pthread_mutex_unlock(&desktop_lock);
        close(pair[0]);
        close(pair[1]);
        free(peer);
        errno = error;
        return -1;
    }
    peer->next = desktop_pending;
    desktop_pending = peer;
    pthread_mutex_unlock(&desktop_lock);
    return pair[1];
}

static int pw_desktop_start(void)
{
    int error;

    pthread_mutex_lock(&desktop_lock);
    memset(&desktop_hub, 0, sizeof(desktop_hub));
    ring_reset(&desktop_title_input);
    ring_reset(&desktop_title_output);
    desktop_pending = NULL;
    desktop_stopping = 0;
    desktop_title_connected = 1;
    desktop_replay_requested = 0;
    desktop_started = 1;
    pthread_mutex_unlock(&desktop_lock);
    if ((error = pthread_create(&desktop_thread, NULL, desktop_hub_loop, NULL)))
    {
        pthread_mutex_lock(&desktop_lock);
        desktop_started = desktop_title_connected = 0;
        pthread_mutex_unlock(&desktop_lock);
        return error;
    }
    if ((error = title_desktop_start()))
    {
        pthread_mutex_lock(&desktop_lock);
        desktop_stopping = 1;
        pthread_mutex_unlock(&desktop_lock);
        pthread_join(desktop_thread, NULL);
        pthread_mutex_lock(&desktop_lock);
        desktop_started = 0;
        pthread_mutex_unlock(&desktop_lock);
        return error;
    }
    io_log("b1 desktop started topology=IN_PROCESS_HUB_CPU_COMPOSITOR");
    return 0;
}

static void pw_desktop_stop(void)
{
    pthread_mutex_lock(&desktop_lock);
    if (!desktop_started)
    {
        pthread_mutex_unlock(&desktop_lock);
        return;
    }
    desktop_stopping = 1;
    pthread_mutex_unlock(&desktop_lock);
    pthread_join(desktop_thread, NULL);
    title_desktop_stop();
    pthread_mutex_lock(&desktop_lock);
    desktop_started = 0;
    ring_reset(&desktop_title_output);
    pthread_mutex_unlock(&desktop_lock);
    io_log("b1 desktop stopped after child supervisor");
}

#endif /* PW_RUNTIME_B1 && !PW_RUNTIME_HEADLESS */

#if defined(PW_RUNTIME_B1) && PW_RUNTIME_HEADLESS
int pw_desktop_connect(void)
{
    errno = ENOSYS;
    return -1;
}

int pw_desktop_surface(const char *name, uint64_t size, int op)
{
    (void)name;
    (void)size;
    (void)op;
    errno = ENOSYS;
    return -1;
}
#endif

static uint64_t random_id(void)
{
    uint64_t id;
    do { arc4random_buf(&id, sizeof(id)); } while (!id);
    return id;
}
/* Caller holds lock; failed/poisoned children remain charged until EOF. */
static unsigned int occupied_slots(void)
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < PW_CHILDREN; ++i) count += !!slots[i].occupied;
    return count;
}
#ifdef PW_RUNTIME_B1
static pthread_cond_t children_changed = PTHREAD_COND_INITIALIZER;
static void kill_occupied_slots(void);
void pw_supervisor_wait_children(void)
{
    pthread_mutex_lock(&lock);
    io_log("b1 parent startup_exit awaiting_children=%u server=KEPT_RUNNING", occupied_slots());
#if PW_RUNTIME_GPU
    /* Game role: WoW has exited and the children left are Battle.net's Agent,
     * which WoW starts itself. Run 192a: the title stayed up 13.3 s after
     * WoW's exit while the Agent ran a self-update (AgentHelper --install,
     * exit -1). Give them 2 s, then end them; the maintenance role (whose
     * children are Battle.net itself) keeps waiting. */
    int64_t grace = io_now_ms() + 2000;
    while (occupied_slots() && io_now_ms() < grace)
    {
        struct timespec pause = {.tv_nsec = 10000000};
        pthread_mutex_unlock(&lock);
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&lock);
    }
    if (occupied_slots())
    {
        io_log("b1 game exit children_left=%u grace_ms=2000; ending them", occupied_slots());
        kill_occupied_slots();
    }
#endif
    while (occupied_slots()) pthread_cond_wait(&children_changed, &lock);
    pthread_mutex_unlock(&lock);
}
#endif

#ifdef PW_RUNTIME_B1
static pthread_mutex_t payload_lock = PTHREAD_MUTEX_INITIALIZER;
struct payload_drain { pthread_mutex_t lock; int fd; };
static const char payload_loader[] = "/data/homebrew/PPSA99030/wine/lib/wine/x86_64-unix/wine";

static int payload_line(int fd, const char *kind, const char *value)
{
    const char *parts[] = {kind, value, "\n"};
    size_t sizes[] = {strlen(kind), strlen(value), 1};
    if (strchr(value, '\n') || strchr(value, '\r')) return EINVAL;
    for (unsigned int i = 0; i < 3; ++i)
    {
        const char *p = parts[i];
        size_t left = sizes[i];
        while (left)
        {
            ssize_t count = write(fd, p, left);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return count < 0 ? errno : EIO;
            p += count;
            left -= count;
        }
    }
    return 0;
}

/* Decode the Windows quote/backslash rules in place, emitting one boot arg
 * per token. No shell quoting survives into the loader's argv. */
static int payload_arguments(int fd, char *command)
{
    char *source = command, *dest = command;
    unsigned int argc = 0;
    while (*source)
    {
        int quoted = 0, error;
        char *argument;
        while (*source == ' ' || *source == '\t') ++source;
        if (!*source) break;
        argument = dest;
        while (*source && (quoted || (*source != ' ' && *source != '\t')))
        {
            unsigned int slashes = 0;
            while (*source == '\\') { ++slashes; ++source; }
            if (*source == '"')
            {
                for (unsigned int i = 0; i < slashes / 2; ++i) *dest++ = '\\';
                if (slashes & 1) { *dest++ = '"'; ++source; }
                else
                {
                    ++source;
                    if (quoted && *source == '"') { *dest++ = '"'; ++source; }
                    else quoted = !quoted;
                }
            }
            else
            {
                while (slashes--) *dest++ = '\\';
                if (*source && (quoted || (*source != ' ' && *source != '\t')))
                    *dest++ = *source++;
            }
        }
        /* Advance before writing NUL: source and dest can coincide. */
        while (*source == ' ' || *source == '\t') ++source;
        *dest++ = 0;
        if (!argc && !*argument) return EINVAL;
        if ((error = payload_line(fd, "arg ", argument))) return error;
        ++argc;
    }
    return argc ? 0 : EINVAL;
}

static void *payload_output(void *arg)
{
    struct payload_drain *drain = arg;
    int fd = drain->fd;
    /* The sender keeps ownership until upload and shutdown complete. */
    pthread_mutex_lock(&drain->lock);
    pthread_mutex_unlock(&drain->lock);
    pthread_mutex_destroy(&drain->lock);
    free(drain);
    char bytes[1024], line[1024];
    size_t used = 0;
    for (;;)
    {
        ssize_t count = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (!count) break;
        if (count < 0)
        {
            struct pollfd wait = {.fd = fd, .events = POLLIN};
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (poll(&wait, 1, -1) < 0 && errno != EINTR) break;
            continue;
        }
        for (ssize_t i = 0; i < count; ++i)
        {
            if (bytes[i] == '\n' || used == sizeof(line) - 1)
            {
                line[used] = 0;
                io_log("b2 payload: %s", line);
                used = 0;
            }
            if (bytes[i] != '\n') line[used++] = bytes[i];
        }
    }
    if (used) { line[used] = 0; io_log("b2 payload: %s", line); }
    close(fd);
    return NULL;
}

static pthread_once_t url_user_once = PTHREAD_ONCE_INIT;
static int url_user_result;

static void url_user_initialize(void)
{
    url_user_result = sceUserServiceInitialize(NULL);
    /* Desktop input may have initialized the service already. Headless B1
     * has no input setup, so the URL hand-off must initialize it itself. */
    if (url_user_result == (int)0x80960003) url_user_result = 0;
}

int pw_open_url(const char *url)
{
    size_t length;
    int result = EINVAL;

    if (!url || !(length = strnlen(url, PW_URL_BYTES)) || length == PW_URL_BYTES ||
        (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)))
        goto done;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)url[i] < 0x20 || (unsigned char)url[i] == 0x7f) goto done;
    result = pthread_once(&url_user_once, url_user_initialize);
    if (!result) result = url_user_result;
    if (!result) result = sceSystemServiceLaunchWebBrowser(url, NULL);
#if !PW_RUNTIME_HEADLESS
    if (!result)
        title_desktop_message("Battle.net opened a sign-in page in the PS5 browser.\n"
                              "Finish there, then press the PS button to come back.");
#endif
done:
    io_log("b1 parent open_url result=%#x", (unsigned int)result);
    /* SDK failures are not errno values on the child wire. */
    return result ? (result > 0 && result <= 4095 ? result : EIO) : 0;
}

int pw_payload_spawn(const char *cmdline, const char *cwd)
{
    static const char title_root[] = "/app0/";
    static const char payload_root[] = "/data/homebrew/PPSA99030/";
    char prefix[sizeof(PW_RUNTIME_PREFIX) + sizeof(payload_root)];
    char home[sizeof(PW_RUNTIME_HOME) + sizeof(payload_root)];
    char *command = NULL, *directory = NULL;
    struct payload_drain *drain = NULL;
    char bytes[16384];
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(9021)};
    pthread_t thread;
    int error = 0, boot = -1, loader = -1, socket_fd = -1, draining = 0;
    int join_drain = 0;
    size_t length;

    if (!cmdline || !(length = strnlen(cmdline, PW_PAYLOAD_CMDLINE_BYTES)) ||
        length == PW_PAYLOAD_CMDLINE_BYTES) return EINVAL;
    if (strncmp(PW_RUNTIME_PREFIX, title_root, sizeof(title_root) - 1) ||
        strncmp(PW_RUNTIME_HOME, title_root, sizeof(title_root) - 1)) return EINVAL;
    snprintf(prefix, sizeof(prefix), "%s%s", payload_root, &PW_RUNTIME_PREFIX[sizeof(title_root) - 1]);
    snprintf(home, sizeof(home), "%s%s", payload_root, &PW_RUNTIME_HOME[sizeof(title_root) - 1]);
    command = malloc(length + 1);
    if (!command) return ENOMEM;
    memcpy(command, cmdline, length + 1);
    if (cwd && *cwd)
    {
        length = strnlen(cwd, PW_PAYLOAD_CWD_BYTES);
        if (length == PW_PAYLOAD_CWD_BYTES || length < 3 || cwd[1] != ':' ||
            (cwd[2] != '\\' && cwd[2] != '/') ||
            !((cwd[0] >= 'A' && cwd[0] <= 'Z') || (cwd[0] >= 'a' && cwd[0] <= 'z')))
        { error = EINVAL; goto done; }
        directory = malloc(strlen(prefix) + length + 32);
        if (!directory) { error = ENOMEM; goto done; }
        char drive = cwd[0] >= 'A' && cwd[0] <= 'Z' ? cwd[0] + ('a' - 'A') : cwd[0];
        snprintf(directory, strlen(prefix) + length + 32, "%s/dosdevices/%c:%s", prefix, drive, cwd + 2);
        for (char *p = directory; *p; ++p) if (*p == '\\') *p = '/';
    }
    pthread_mutex_lock(&payload_lock);
    boot = open("/app0/wine/boot.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (boot < 0) { error = errno; goto unlock; }
    if ((error = payload_line(boot, "arg ", payload_loader)) ||
        (error = payload_arguments(boot, command)) ||
        (error = payload_line(boot, "env WINEPREFIX=", prefix)) ||
        (error = payload_line(boot, "env HOME=", home)) ||
        (error = payload_line(boot, "env USER=", "ps5")) ||
        (error = payload_line(boot, "env PATH=", "/usr/bin:/bin")) ||
        (error = payload_line(boot, "env WINEDLLOVERRIDES=", PW_RUNTIME_PAYLOAD_DLL_OVERRIDES)) ||
        (error = payload_line(boot, "cwd ", directory ? directory : ""))) goto unlock;
    if (close(boot)) { boot = -1; error = errno; goto unlock; }
    boot = -1;
    loader = open("/app0/wine/lib/wine/x86_64-unix/wine", O_RDONLY);
    if (loader < 0) { error = errno; goto unlock; }
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) { error = errno; goto unlock; }
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)))
    { error = errno; goto unlock; }
    drain = malloc(sizeof(*drain));
    if (!drain) { error = ENOMEM; goto unlock; }
    drain->fd = socket_fd;
    if ((error = pthread_mutex_init(&drain->lock, NULL))) { free(drain); drain = NULL; goto unlock; }
    pthread_mutex_lock(&drain->lock);
    error = pthread_create(&thread, NULL, payload_output, drain);
    if (error) goto unlock;
    draining = 1;
    if ((error = pthread_detach(thread))) { join_drain = 1; goto unlock; }
    for (;;)
    {
        ssize_t count = read(loader, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (!count) break;
        if (io_transfer(socket_fd, bytes, count, 1, io_now_ms() + PW_OVERALL_MS))
        { error = errno ? errno : EIO; break; }
    }
    if (!error && shutdown(socket_fd, SHUT_WR)) error = errno;
unlock:
    if (boot >= 0) close(boot);
    if (loader >= 0) close(loader);
    if (socket_fd >= 0)
    {
        if (draining)
        {
            if (error) shutdown(socket_fd, SHUT_RDWR);
            /* The detached drain thread owns the socket until loader EOF. */
        }
        else close(socket_fd);
    }
    if (drain)
    {
        pthread_mutex_unlock(&drain->lock);
        if (!draining) { pthread_mutex_destroy(&drain->lock); free(drain); }
    }
    if (join_drain) pthread_join(thread, NULL);
    pthread_mutex_unlock(&payload_lock);
done:
    free(directory);
    free(command);
    return error;
}
#endif

#ifdef PW_RUNTIME_B1
/* A child cannot admit PsmKit processes, use the parent's native network
 * modules, connect to the desktop hub, or enter the shared-memory namespace.
 * It sends typed requests over a dedicated channel for the parent to perform. */
struct spawn_service { int fd; uint64_t nonce; };
static int serve_spawn(const struct spawn_service *service, struct pw_p1c_bootstrap *request)
{
    int64_t deadline = io_now_ms() + PW_OVERALL_MS;
    if (io_transfer(service->fd, request, sizeof(*request), 0, deadline)) return -1;
    int32_t error = pw_p1c_bootstrap_validate(request, generation, service->nonce);
    int server = pw_receive_fd(service->fd, deadline);
    if (server < 0) return -1;
    if (!error)
    {
        char *args[PW_ARGC + 1];
        pw_p1c_argv(request, args);
        struct wine_ps5_title_spawn spawn = {.size = sizeof(spawn),
            .windows_pid = request->windows_pid, .server_socket = server, .argv = args,
            .unix_cwd = request->cwd[0] ? request->cwd : NULL};
        error = pw_supervisor_spawn(&spawn);
    }
    close(server);
    io_log("b1 supervisor child spawn request windows_pid=%u error=%d",
           request->windows_pid, (int)error);
    return io_transfer(service->fd, &error, sizeof(error), 1, io_now_ms() + PW_OVERALL_MS) ? -1 : 0;
}
static int serve_native(const struct spawn_service *service)
{
    struct pw_native_request request;
    struct pw_native_reply reply;
    if (io_transfer(service->fd, &request, sizeof(request), 0, io_now_ms() + PW_OVERALL_MS)) return -1;
    memset(&reply, 0, sizeof(reply));
    if (pw_native_execute(&request, &reply)) return -1; /* unknown op: drop the child channel */
    return io_transfer(service->fd, &reply, sizeof(reply), 1, io_now_ms() + PW_OVERALL_MS) ? -1 : 0;
}
static int serve_payload(const struct spawn_service *service)
{
    struct pw_payload_request request;
    char *data;
    int32_t error;
    int64_t deadline = io_now_ms() + PW_OVERALL_MS;
    if (io_transfer(service->fd, &request, sizeof(request), 0, deadline)) return -1;
    if (!request.cmdline_bytes || request.cmdline_bytes > PW_PAYLOAD_CMDLINE_BYTES ||
        request.cwd_bytes > PW_PAYLOAD_CWD_BYTES) return -1;
    data = malloc(request.cmdline_bytes + request.cwd_bytes);
    if (!data) return -1;
    if (io_transfer(service->fd, data, request.cmdline_bytes + request.cwd_bytes, 0, deadline))
    { free(data); return -1; }
    if (data[request.cmdline_bytes - 1] ||
        strlen(data) != request.cmdline_bytes - 1 ||
        (request.cwd_bytes && (data[request.cmdline_bytes + request.cwd_bytes - 1] ||
         strlen(data + request.cmdline_bytes) != request.cwd_bytes - 1)))
    { free(data); return -1; }
    error = pw_payload_spawn(data, request.cwd_bytes ? data + request.cmdline_bytes : NULL);
    free(data);
    return io_transfer(service->fd, &error, sizeof(error), 1, io_now_ms() + PW_OVERALL_MS) ? -1 : 0;
}

static int serve_open_url(const struct spawn_service *service)
{
    struct pw_url_request request;
    char *url;
    int32_t error;
    int64_t deadline = io_now_ms() + PW_OVERALL_MS;

    if (io_transfer(service->fd, &request, sizeof(request), 0, deadline)) return -1;
    if (!request.url_bytes || request.url_bytes > PW_URL_BYTES) return -1;
    url = malloc(request.url_bytes);
    if (!url) return -1;
    if (io_transfer(service->fd, url, request.url_bytes, 0, deadline))
    { free(url); return -1; }
    if (url[request.url_bytes - 1] || strlen(url) != request.url_bytes - 1)
    { free(url); return -1; }
    error = pw_open_url(url);
    free(url);
    return io_transfer(service->fd, &error, sizeof(error), 1, io_now_ms() + PW_OVERALL_MS) ? -1 : 0;
}

static int serve_game_launch(const struct spawn_service *service)
{
    struct pw_game_request request;
    int32_t error;
#if !PW_RUNTIME_GPU
    char *argv[PW_GAME_WIRE_ARGV + 1];
    int accepted = 0;
#endif
    int64_t deadline = io_now_ms() + PW_OVERALL_MS;

    if (io_transfer(service->fd, &request, sizeof(request), 0, deadline)) return -1;
#if PW_RUNTIME_GPU
    error = ENOTSUP;
#else
    error = pw_game_request_unpack(&request, argv);
    if (!error)
    {
        error = pw_parent_stage_game_launch(argv + 7, argv[5], argv[3]);
        accepted = !error;
    }
#endif
    io_log("b1 supervisor child game launch image=Wow*.exe result=%d argv=REDACTED cwd=REDACTED",
           (int)error);
    if (io_transfer(service->fd, &error, sizeof(error), 1, io_now_ms() + PW_OVERALL_MS))
        return -1;
#if !PW_RUNTIME_GPU
    if (accepted) pw_parent_request_game_launch();
#endif
    return 0;
}

static int serve_desktop(const struct spawn_service *service)
{
    struct pw_desktop_reply reply;
    int64_t deadline = io_now_ms() + PW_OVERALL_MS;
    int descriptor = pw_desktop_connect();

    reply.result = descriptor < 0 ? -1 : 0;
    reply.error = descriptor < 0 ? (errno ? errno : EIO) : 0;
    if (io_transfer(service->fd, &reply, sizeof(reply), 1, deadline)) goto failed;
    if (descriptor >= 0 && pw_send_fd(service->fd, descriptor, deadline)) goto failed;
    if (descriptor >= 0) close(descriptor);
    return 0;

failed:
    if (descriptor >= 0) close(descriptor);
    return -1;
}

static int serve_surface(const struct spawn_service *service)
{
    struct pw_surface_request request;
    struct pw_desktop_reply reply;
    int64_t deadline = io_now_ms() + PW_OVERALL_MS;
    int descriptor;

    if (io_transfer(service->fd, &request, sizeof(request), 0, deadline)) return -1;
    if (request.reserved)
    {
        descriptor = -1;
        errno = EPROTO;
    }
    else descriptor = pw_desktop_surface(request.name, request.size, request.op);
    reply.result = descriptor < 0 ? -1 : 0;
    reply.error = descriptor < 0 ? (errno ? errno : EIO) : 0;
    if (io_transfer(service->fd, &reply, sizeof(reply), 1, deadline)) goto failed;
    if (!reply.result && request.op != PS5_TITLE_SURFACE_UNLINK &&
        pw_send_fd(service->fd, descriptor, deadline)) goto failed;
    if (!reply.result && request.op != PS5_TITLE_SURFACE_UNLINK) close(descriptor);
    return 0;

failed:
    if (descriptor >= 0 && request.op == PS5_TITLE_SURFACE_CREATE)
        pw_desktop_surface(request.name, 0, PS5_TITLE_SURFACE_UNLINK);
    if (descriptor >= 0 && request.op != PS5_TITLE_SURFACE_UNLINK) close(descriptor);
    return -1;
}

static void *serve_child_spawns(void *arg)
{
    struct spawn_service service = *(struct spawn_service *)arg;
    struct pw_p1c_bootstrap *request = malloc(sizeof(*request));
    free(arg);
    if (!request) { close(service.fd); return NULL; }
    for (;;)
    {
        struct pollfd wait = {.fd = service.fd, .events = POLLIN};
        uint32_t kind;
        if (poll(&wait, 1, -1) < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (io_transfer(service.fd, &kind, sizeof(kind), 0, io_now_ms() + PW_OVERALL_MS)) break;
        if (kind == PW_CHILD_SPAWN ? serve_spawn(&service, request) :
            kind == PW_CHILD_NATIVE ? serve_native(&service) :
            kind == PW_CHILD_DESKTOP ? serve_desktop(&service) :
            kind == PW_CHILD_PAYLOAD_SPAWN ? serve_payload(&service) :
            kind == PW_CHILD_OPEN_URL ? serve_open_url(&service) :
            kind == PW_CHILD_SURFACE ? serve_surface(&service) :
            kind == PW_CHILD_GAME_LAUNCH ? serve_game_launch(&service) : -1) break;
    }
    close(service.fd);
    free(request);
    return NULL;
}
#endif
static int kill_slot(struct pw_slot *slot)
{
    slot->kill_requested = 1;
    int32_t result = sceSystemServiceKillLocalProcessForPsmKit(slot->handle);
    io_log("p1c supervisor terminate handle=%#x pid=%d result=%#x generation=%llx",
           (unsigned int)slot->handle, slot->unix_pid, (unsigned int)result,
           (unsigned long long)slot->generation);
    if (result < 0) { errno = EIO; return -1; }
    return 0;
}
/* Shutdown kills: the shell's PsmKit kill takes ~0.1-0.3 s per child and the
 * reaper used to issue them one by one (run 189b: four kills, 1.2 s). Issue
 * them from one thread each; a thread that cannot start falls back to a
 * direct call. Caller holds lock. */
struct pw_kill_request { struct pw_slot *slot; int32_t result; pthread_t thread; int started; };
static void *kill_slot_thread(void *arg)
{
    struct pw_kill_request *request = arg;
    request->result = sceSystemServiceKillLocalProcessForPsmKit(request->slot->handle);
    return NULL;
}
static void kill_slots_parallel(struct pw_kill_request *requests, unsigned int count)
{
    for (unsigned int i = 0; i < count; ++i)
    {
        requests[i].slot->kill_requested = 1;
        requests[i].started = !pthread_create(&requests[i].thread, NULL, kill_slot_thread, &requests[i]);
    }
    for (unsigned int i = 0; i < count; ++i)
    {
        if (requests[i].started) pthread_join(requests[i].thread, NULL);
        else kill_slot_thread(&requests[i]);
        io_log("p1c supervisor terminate handle=%#x pid=%d result=%#x generation=%llx parallel=%u",
               (unsigned int)requests[i].slot->handle, requests[i].slot->unix_pid,
               (unsigned int)requests[i].result, (unsigned long long)requests[i].slot->generation, count);
    }
}
#ifdef PW_RUNTIME_B1
/* Caller holds lock. */
static void kill_occupied_slots(void)
{
    struct pw_kill_request kills[PW_CHILDREN];
    unsigned int count = 0;
    for (unsigned int i = 0; i < PW_CHILDREN; ++i)
        if (slots[i].occupied && !slots[i].kill_requested)
            kills[count++] = (struct pw_kill_request){.slot = &slots[i]};
    if (count) kill_slots_parallel(kills, count);
}

/* First-start setup: wineboot -i leaves services.exe and explorer.exe
 * running, which pw_supervisor_wait_children would wait for forever. */
void pw_supervisor_end_children(void)
{
    pthread_mutex_lock(&lock);
    io_log("b1 parent ending children=%u", occupied_slots());
    kill_occupied_slots();
    pthread_mutex_unlock(&lock);
}
#endif
/* The title's budget allows 256 open files (FD_FILE) for the whole title;
 * the in-process wineserver holds one for every file any Wine process has
 * open. Count this process's descriptors by kind (public fstat only). */
static void count_descriptors(unsigned int *files, unsigned int *sockets, unsigned int *other)
{
    struct stat st;
    *files = *sockets = *other = 0;
    for (int fd = 0; fd < 4096; ++fd)
    {
        if (fstat(fd, &st)) continue;
        if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) ++*files;
        else if (S_ISSOCK(st.st_mode)) ++*sockets;
        else ++*other;
    }
}
/* What the regular-file descriptors are: distinct files (one file opened
 * twice shares a dev/ino), directories, and large files (DLL images and
 * Chromium's .pak/.dat files) versus small ones (caches, logs, databases). */
static void log_file_breakdown(const char *when)
{
    static struct { dev_t dev; ino_t ino; } seen[4096];
    unsigned int count = 0, descriptors = 0, dirs = 0, large = 0, large_distinct = 0;
    struct stat st;
    for (int fd = 0; fd < 4096; ++fd)
    {
        if (fstat(fd, &st)) continue;
        if (S_ISDIR(st.st_mode)) { ++dirs; continue; }
        if (!S_ISREG(st.st_mode)) continue;
        ++descriptors;
        if (st.st_size >= 1 << 20) ++large;
        unsigned int i;
        for (i = 0; i < count; ++i)
            if (seen[i].dev == st.st_dev && seen[i].ino == st.st_ino) break;
        if (i == count)
        {
            seen[count].dev = st.st_dev;
            seen[count++].ino = st.st_ino;
            if (st.st_size >= 1 << 20) ++large_distinct;
        }
    }
    io_log("p1c supervisor files %s descriptors=%u distinct=%u dirs=%u large=%u large_distinct=%u",
           when, descriptors, count, dirs, large, large_distinct);
}
static void reap_slot(unsigned int i)
{
    uint64_t nonce = slots[i].nonce;
    unsigned int files, sockets, other;
    count_descriptors(&files, &sockets, &other);
    io_log("p1c supervisor reaped slot=%u generation=%llx pid=%d windows_pid=%u fd_files=%u fd_sockets=%u fd_other=%u",
           i, (unsigned long long)generation, slots[i].unix_pid, slots[i].windows_pid, files, sockets, other);
    log_file_breakdown("at-reap");
    pw_slot_release(&slots[i], generation, nonce);
    #ifdef PW_RUNTIME_B1
    pthread_cond_broadcast(&children_changed);
    #endif
}
static void *reap_children(void *unused)
{
    (void)unused;
    for (;;)
    {
        pthread_mutex_lock(&lock);
        int live = 0;
        int64_t now = stopping ? io_now_ms() : 0;
        for (unsigned int i = 0; i < PW_CHILDREN; ++i)
        {
            if (!slots[i].occupied) continue;
            struct pw_control unexpected;
            /* Signal transactions hold lock, so any idle input is invalid.
             * Drain it, rather than peeking forever at bytes queued before EOF. */
            ssize_t count = recv(slots[i].control, &unexpected, sizeof(unexpected), MSG_DONTWAIT);
            if (!count || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                reap_slot(i);
            else
            {
                if (count > 0)
                {
                    io_log("p1c supervisor unexpected control slot=%u bytes=%zd", i, count);
                    pw_slot_poison(&slots[i]);
                    if (!slots[i].kill_requested) kill_slot(&slots[i]);
                }
                ++live;
            }
        }
        if (stopping)
        {
            struct pw_kill_request kills[PW_CHILDREN];
            unsigned int kill_count = 0;
            for (unsigned int i = 0; i < PW_CHILDREN; ++i)
            {
                switch (pw_slot_shutdown_action(&slots[i], now, shutdown_grace, shutdown_deadline))
                {
                case PW_SHUTDOWN_KILL:
                    kills[kill_count++] = (struct pw_kill_request){.slot = &slots[i]};
                    break;
                case PW_SHUTDOWN_ABANDON:
                    io_log("p1c supervisor abandoned slot=%u generation=%llx pid=%d failed=1",
                           i, (unsigned long long)generation, slots[i].unix_pid);
                    pw_slot_poison(&slots[i]);
                    pw_slot_release(&slots[i], generation, slots[i].nonce);
                    --live;
                    break;
                case PW_SHUTDOWN_WAIT:
                    break;
                }
            }
            if (kill_count) kill_slots_parallel(kills, kill_count);
        }
        int done = stopping && !live;
        pthread_mutex_unlock(&lock);
        if (done) return NULL;
        struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
}
int pw_supervisor_init(void)
{
    int error;

    generation = random_id();
#if defined(PW_RUNTIME_B1) && !PW_RUNTIME_HEADLESS
    if ((error = pw_desktop_start())) return error;
#endif
    if ((error = pthread_create(&reaper, NULL, reap_children, NULL)))
    {
#if defined(PW_RUNTIME_B1) && !PW_RUNTIME_HEADLESS
        pw_desktop_stop();
#endif
        return error;
    }
    started = 1;
    io_log("p1c supervisor session generation=%llx slots=%d", (unsigned long long)generation, PW_CHILDREN);
    return 0;
}
/* A Chromium subprocess (renderer, GPU, utility: "--type=" in its command
 * line), which may not take the last free slot: Battle.net's own restarter
 * after a self-update ("Battle.net.exe --updatepid=") was refused with all
 * seven slots held, mostly by renderers, and the launcher never came back
 * (run 20261009-b1-installed-41). Chromium starts another renderer later;
 * the last slot stays for programs like that, and for the game. */
static int is_chromium_subprocess(const struct wine_ps5_title_spawn *spawn)
{
    for (char *const *arg = spawn->argv ? spawn->argv + 1 : NULL; arg && *arg; arg++)
        if (!strncmp(*arg, "--type=", 7)) return 1;
    return 0;
}
/* The Windows command line of a spawn request, shortened, for the log:
 * which program takes or is refused a slot. Arguments may carry account
 * values (a game's login, a session), so only the program and option names
 * are shown; a value is shown only for Chromium's --type=. */
static const char *spawn_command(const struct wine_ps5_title_spawn *spawn, char *text, size_t size)
{
    return pw_redact_spawn_argv(spawn->argv, text, size);
}
int pw_supervisor_spawn(const struct wine_ps5_title_spawn *spawn)
{
    struct pw_p1c_bootstrap bootstrap;
    struct pw_control hello;
    int pair[2] = {-1, -1}, index = -1, error;
    uint64_t nonce = random_id();
    if (!spawn || spawn->size != sizeof(*spawn) || !pw_socket_valid(spawn->server_socket)) return EINVAL;
    error = pw_p1c_bootstrap_init(&bootstrap, generation, nonce, spawn->windows_pid, spawn->argv, spawn->unix_cwd);
    if (error) return error;
    pthread_mutex_lock(&lock);
    if (stopping) { error = EAGAIN; goto complete; }
    if (is_chromium_subprocess(spawn) && occupied_slots() >= PW_CHILDREN - 1)
    {
        char command[160];
        error = EAGAIN;
        io_log("p1c supervisor refuse reason=reserve occupied=%u windows_pid=%u command=%s", occupied_slots(),
               spawn->windows_pid, spawn_command(spawn, command, sizeof(command)));
        goto complete;
    }
    index = pw_slot_reserve(slots, generation, nonce, spawn->windows_pid);
    if (index < 0)
    {
        error = errno;
        char command[160];
        io_log("p1c supervisor refuse reason=full occupied=%u windows_pid=%u command=%s", occupied_slots(),
               spawn->windows_pid, spawn_command(spawn, command, sizeof(command)));
        goto complete;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { error = errno; goto failed; }
    slots[index].control = pair[0]; pair[0] = -1;
    char gen[17], token[17];
    snprintf(gen, sizeof(gen), "%llx", (unsigned long long)generation);
    snprintf(token, sizeof(token), "%llx", (unsigned long long)nonce);
    const char *const argv[] = {child_path, gen, token, NULL};
    const struct local_process_param param = {.size = sizeof(param), .socket_fd = pair[1],
        .field_8 = 1, .field_c = -1, .field_10 = 1};
    int64_t deadline = io_now_ms() + PW_OVERALL_MS;
    slots[index].handle = sceSystemServiceAddLocalProcessForPsmKit(child_path, argv, &param);
    close(pair[1]); pair[1] = -1;
    if (slots[index].handle < 0) { error = EIO; goto failed; }
    int fd = slots[index].control;
#ifdef PW_P1C_FAULT_INJECTION
    if (pw_p1c_bootstrap_fault(&bootstrap, spawn->argv))
        io_log("p1c supervisor fault stale-bootstrap slot=%d generation=%llx sent=%llx",
               index, (unsigned long long)generation, (unsigned long long)bootstrap.generation);
#endif
    if (io_transfer(fd, &bootstrap, sizeof(bootstrap), 1, deadline) ||
        io_transfer(fd, &hello, sizeof(hello), 0, deadline))
    { error = errno ? errno : EPIPE; goto failed; }
    error = pw_slot_bootstrap_reply(&slots[index], &hello);
    if (error) goto failed;
    if (pw_send_fd(fd, spawn->server_socket, deadline) ||
        io_transfer(fd, &hello, sizeof(hello), 0, deadline))
    { error = errno ? errno : EPIPE; goto failed; }
    error = pw_slot_hello(&slots[index], &hello);
    if (error) goto failed;
    if (slots[index].unix_pid == (int)getpid()) { error = EPROTO; goto failed; }
#ifdef PW_RUNTIME_B1
    {
        int channel[2];
        pthread_t thread;
        struct spawn_service *service;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel)) { error = errno; goto failed; }
        if (pw_send_fd(fd, channel[1], deadline))
        {
            error = errno ? errno : EPIPE;
            close(channel[0]); close(channel[1]);
            goto failed;
        }
        close(channel[1]);
        if (!(service = malloc(sizeof(*service)))) { close(channel[0]); error = ENOMEM; goto failed; }
        service->fd = channel[0]; service->nonce = nonce;
        if ((error = pthread_create(&thread, NULL, serve_child_spawns, service)))
        {
            close(channel[0]); free(service);
            goto failed;
        }
        pthread_detach(thread);
    }
#endif
    size_t flexible = 0;
    int32_t flexible_status = sceKernelAvailableFlexibleMemorySize(&flexible);
    int flexible_error = errno;
    unsigned int fd_files, fd_sockets, fd_other;
    count_descriptors(&fd_files, &fd_sockets, &fd_other);
    if (!flexible_status)
        io_log("p1c supervisor admit slot=%d occupied=%u flexible_free=%llu fd_files=%u fd_sockets=%u fd_other=%u",
               index, occupied_slots(), (unsigned long long)flexible, fd_files, fd_sockets, fd_other);
    else
        io_log("p1c supervisor admit slot=%d occupied=%u flexible_free=unavailable status=%#x errno=%d",
               index, occupied_slots(), (unsigned int)flexible_status, flexible_error);
    log_file_breakdown("at-admit");
    char command[160];
    io_log("p1c supervisor spawn slot=%d generation=%llx pid=%d windows_pid=%u handle=%#x command=%s",
           index, (unsigned long long)generation, slots[index].unix_pid, spawn->windows_pid,
           (unsigned int)slots[index].handle, spawn_command(spawn, command, sizeof(command)));
    error = 0;
    goto complete;
failed:
    io_log("p1c supervisor spawn rejected slot=%d generation=%llx error=%d", index,
           (unsigned long long)generation, error);
    if (slots[index].handle >= 0)
    {
        /* Failed streams stop routing immediately, but remain charged
         * until EOF (or bounded supervisor shutdown abandonment). */
        pw_slot_poison(&slots[index]);
        kill_slot(&slots[index]);
    }
    else pw_slot_release(&slots[index], generation, nonce);
complete:
    if (pair[0] >= 0) close(pair[0]);
    if (pair[1] >= 0) close(pair[1]);
    pthread_mutex_unlock(&lock);
    return error;
}
static struct pw_slot *find_child(int pid)
{
    for (unsigned int i = 0; i < PW_CHILDREN; ++i)
        if (pw_slot_routable(&slots[i], generation, pid))
            return &slots[i];
    return NULL;
}
int pw_supervisor_signal(int pid, unsigned int thread_id, void *teb, int sig)
{
    int error = ESRCH;
    pthread_mutex_lock(&lock);
    struct pw_slot *slot = find_child(pid);
    if (slot)
    {
        struct pw_control request, reply;
        int framing_error = 0;
        pw_control_init(&request, PW_SIGNAL, generation, slot->nonce);
        request.pid = (unsigned int)pid; request.thread_id = thread_id;
        request.teb = (uint64_t)(uintptr_t)teb; request.sig = sig;
        int64_t deadline = io_now_ms() + IO_OPERATION_MS;
        if (io_transfer(slot->control, &request, sizeof(request), 1, deadline) ||
            io_transfer(slot->control, &reply, sizeof(reply), 0, deadline))
        { error = errno ? errno : ESRCH; framing_error = 1; }
        else if ((error = pw_control_validate(&reply, PW_SIGNAL_REPLY, generation, slot->nonce)))
            framing_error = 1;
        else if (reply.pid != request.pid || reply.thread_id != thread_id ||
                 reply.teb != request.teb || reply.sig != sig || reply.error < 0 || reply.error > 4095)
        { error = EPROTO; framing_error = 1; }
        else error = reply.error;
        if (framing_error)
        {
            pw_slot_poison(slot);
            kill_slot(slot);
        }
    }
    pthread_mutex_unlock(&lock);
    if (error) { errno = error; return -1; }
    return 0;
}
int pw_supervisor_terminate(int pid)
{
    pthread_mutex_lock(&lock);
    struct pw_slot *slot = find_child(pid);
    int result;
    if (!slot) { errno = ESRCH; result = -1; }
    else result = kill_slot(slot);
    int error = errno;
    pthread_mutex_unlock(&lock);
    errno = error;
    return result;
}
void pw_supervisor_shutdown(void)
{
    if (!started) return;
    pthread_mutex_lock(&lock);
    io_log("p1c supervisor shutdown occupied=%u", occupied_slots());
    stopping = 1;
    /* The wineserver every child serves has already stopped (Wine's exit
     * joins it before this runs), so no child can finish anything useful:
     * kill at once instead of after a grace. Run 189b: no child exited
     * during the old 2-s grace, which cost 2 s on every Play. */
    int64_t now = io_now_ms();
    shutdown_grace = now;
    shutdown_deadline = shutdown_grace + 2000;
    io_log("p1c supervisor shutdown grace_ms=0 kill_wait_ms=2000");
    pthread_mutex_unlock(&lock);
    pthread_join(reaper, NULL);
#if defined(PW_RUNTIME_B1) && !PW_RUNTIME_HEADLESS
    pw_desktop_stop();
#endif
    started = 0;
}
