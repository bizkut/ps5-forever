#ifndef PSMKIT_WINE_PROTOCOL_H
#define PSMKIT_WINE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "../localproc/psmkit-io.h"

/* Transport prerequisite only: these frames never represent a Wine result. */
#define PW_MAGIC UINT32_C(0x50575731)
#define PW_VERSION 1
#define PW_COOKIE "PsmWine-FD3-550!"
#define PW_NONCE UINT64_C(0x6a02946ce03817b5)
#define PW_OVERALL_MS 20000
#define PW_LOADER "/app0/wine/lib/wine/x86_64-unix/wine"
#define PW_PREFIX "/app0/wine/psmkit-wine-disposable/prefix"
#define PW_SERVER "/app0/wine/psmkit-wine-disposable/server-title"
#define PW_PROGRAM "/app0/wine/tests/psmkit-wine/console.exe"
#define PW_OUTPUT "psmkit-wine PE: cpu-only works=1\r\n"

struct pw_bootstrap
{
    uint32_t magic, version, size, reserved;
    uint64_t nonce;
    unsigned char cookie[16];
    char loader[96], prefix[96], server[96], program[96];
};
struct pw_challenge
{
    uint32_t magic, version;
    uint64_t nonce;
};
struct pw_reply
{
    struct pw_challenge echo;
    uint32_t pid, descriptor_only;
};
_Static_assert(sizeof(PW_COOKIE) - 1 == 16, "cookie wire bytes");
_Static_assert(sizeof(struct pw_bootstrap) == 424, "bootstrap wire size");
_Static_assert(sizeof(struct pw_challenge) == 16, "challenge wire size");
_Static_assert(sizeof(struct pw_reply) == 24, "reply wire size");

void pw_bootstrap_init(struct pw_bootstrap *bootstrap);
int pw_bootstrap_valid(const struct pw_bootstrap *bootstrap);
int pw_socket_valid(int fd);
int pw_send_fd(int control, int descriptor, int64_t deadline);
int pw_receive_descriptor(int control, int64_t deadline);
int pw_receive_fd(int control, int64_t deadline);
int pw_duplicate_bridge_init(int channel[2]);
void pw_duplicate_bridge_close(int channel[2]);
int pw_duplicate_fd(int descriptor, int channel[2], int64_t deadline);
int pw_child_session(int control, int64_t deadline);
int pw_within(int64_t deadline);

/* P1c control plane. Fixed-width wire records, never native pointers/argv. */
#define PW_P1C_VERSION 3
#define PW_CHILDREN 7
#define PW_ARGC 64
#define PW_ARG_BYTES 8192
#define PW_CWD_BYTES 1024
#define PW_PARENT_OUTPUT "p1w parent: pe32 seh=ok threads=2 fiber=ok context=ok win32k=ok child exit=5\r\n"
#define PW_CHILD_ENTRY "p1w child: pe32 entry ptr=4\r\n"
#define PW_CHILD_OUTPUT "p1w child: pe32 seh=ok threads=2 fiber=ok context=fact win32k=ok ptr=4 exit=5\r\n"
/* Each X is exactly one lowercase hex digit, not an arbitrary suffix. */
#define PW_PARENT_FIBER "p1w parent: fiber data=XXXXXXXX current=XXXXXXXX fs10=XXXXXXXX converted=XXXXXXXX error=XXXXXXXX\r\n"
#define PW_CHILD_FIBER "p1w child: fiber data=XXXXXXXX current=XXXXXXXX fs10=XXXXXXXX converted=XXXXXXXX error=XXXXXXXX\r\n"
#define PW_CHILD_CONTEXT "p1w child: context suspend=XXXXXXXX get=XXXXXXXX resume=XXXXXXXX error=XXXXXXXX\r\n"
#define PW_STALE_BOOTSTRAP_TOKEN "--pw-p1c-stale-bootstrap"
struct pw_p1c_bootstrap
{
    uint32_t magic, version, size, argc;
    uint64_t generation, nonce;
    uint32_t windows_pid, argv_bytes;
    char argv[PW_ARG_BYTES], cwd[PW_CWD_BYTES];
};
enum pw_control_type {
    PW_HELLO = 1, PW_SIGNAL = 2, PW_SIGNAL_REPLY = 3, PW_BOOTSTRAP_REPLY = 4
};
struct pw_control
{
    uint32_t magic, version, size, type;
    uint64_t generation, nonce, teb;
    uint32_t pid, thread_id;
    int32_t sig, error;
};
struct pw_slot
{
    uint64_t generation, nonce;
    int control, handle, unix_pid;
    unsigned int windows_pid;
    int occupied, failed, kill_requested;
};
_Static_assert(sizeof(struct pw_p1c_bootstrap) == 9256, "v3 bootstrap wire size");
_Static_assert(sizeof(struct pw_control) == 56, "control wire size");
int pw_p1c_argv_identity(int, char *const [], uint64_t *, uint64_t *);
int pw_p1c_bootstrap_init(struct pw_p1c_bootstrap *, uint64_t, uint64_t,
                         unsigned int, char *const [], const char *);
#ifdef PW_P1C_FAULT_INJECTION
int pw_p1c_bootstrap_fault(struct pw_p1c_bootstrap *, char *const []);
#endif
int pw_p1c_bootstrap_validate(const struct pw_p1c_bootstrap *, uint64_t, uint64_t);
void pw_p1c_argv(struct pw_p1c_bootstrap *, char *[PW_ARGC + 1]);
void pw_control_init(struct pw_control *, unsigned int, uint64_t, uint64_t);
int pw_control_validate(const struct pw_control *, unsigned int, uint64_t, uint64_t);
int pw_slot_reserve(struct pw_slot [PW_CHILDREN], uint64_t, uint64_t, unsigned int);
int pw_slot_hello(struct pw_slot *, const struct pw_control *);
int pw_slot_release(struct pw_slot *, uint64_t, uint64_t);
int pw_slot_bootstrap_reply(const struct pw_slot *, const struct pw_control *);
int pw_slot_routable(const struct pw_slot *, uint64_t, int);
void pw_slot_poison(struct pw_slot *);
enum pw_shutdown_action { PW_SHUTDOWN_WAIT, PW_SHUTDOWN_KILL, PW_SHUTDOWN_ABANDON };
enum pw_shutdown_action pw_slot_shutdown_action(const struct pw_slot *, int64_t, int64_t, int64_t);
const char *pw_redact_spawn_argv(char *const [], char *, size_t);
/* B1: requests a PsmKit child sends on its parent channel. A local child has no
 * libSceNet, libSceNetCtl, libSceSystemService, desktop hub or shared-memory
 * namespace, so the parent performs those operations. */
enum pw_child_request {
    PW_CHILD_SPAWN = 1, PW_CHILD_NATIVE = 2, PW_CHILD_DESKTOP = 3,
    PW_CHILD_SURFACE = 4, PW_CHILD_PAYLOAD_SPAWN = 5, PW_CHILD_OPEN_URL = 6,
    PW_CHILD_GAME_LAUNCH = 7,
};
enum pw_native_op {
    PW_NET_POOL_CREATE = 1, PW_NET_POOL_DESTROY, PW_NET_RESOLVER_CREATE,
    PW_NET_RESOLVER_DESTROY, PW_NET_RESOLVER_NTOA, PW_NET_RESOLVER_ATON,
    PW_NET_CTL_INIT, PW_NET_CTL_GET_INFO, PW_SS_PARAM_GET_STRING,
};
#define PW_NATIVE_BYTES 256
#define PW_DESKTOP_NAME_BYTES 64
/* Both planes, preserving the compositor's 64 MiB per-plane limit. */
#define PW_DESKTOP_SURFACE_MAX (128u * 1024u * 1024u)
struct pw_native_request { uint32_t op; int32_t arg[6]; char text[PW_NATIVE_BYTES]; };
struct pw_native_reply { int32_t result, net_errno; uint8_t data[PW_NATIVE_BYTES]; };
struct pw_surface_request {
    uint64_t size; /* total shared object bytes: draw plane + published plane */
    int32_t op;
    uint32_t reserved;
    char name[PW_DESKTOP_NAME_BYTES];
};
struct pw_desktop_reply { int32_t result, error; };
_Static_assert(sizeof(struct pw_native_request) == 284, "native request wire size");
_Static_assert(sizeof(struct pw_native_reply) == 264, "native reply wire size");
_Static_assert(sizeof(struct pw_surface_request) == 80, "surface request wire size");
_Static_assert(sizeof(struct pw_desktop_reply) == 8, "desktop reply wire size");
/* UTF-8 can use four bytes per Windows UTF-16 character. */
#define PW_PAYLOAD_CMDLINE_BYTES (4u * 32768u)
#define PW_PAYLOAD_CWD_BYTES (4u * 32768u)
struct pw_payload_request { uint32_t cmdline_bytes, cwd_bytes; };
_Static_assert(sizeof(struct pw_payload_request) == 8, "payload request wire size");
/* NUL-terminated UTF-8 URL, followed by an int32 errno reply. */
#define PW_URL_BYTES (16u * 1024u)
struct pw_url_request { uint32_t url_bytes; };
_Static_assert(sizeof(struct pw_url_request) == 4, "URL request wire size");
#define PW_GAME_WIRE_BYTES 4096
#define PW_GAME_WIRE_ARGV 72
struct pw_game_request {
    uint32_t argc, bytes;
    uint32_t offsets[PW_GAME_WIRE_ARGV];
    char data[PW_GAME_WIRE_BYTES];
};
_Static_assert(sizeof(struct pw_game_request) == 4392, "game request wire size");
int pw_native_execute(const struct pw_native_request *, struct pw_native_reply *);
#endif
