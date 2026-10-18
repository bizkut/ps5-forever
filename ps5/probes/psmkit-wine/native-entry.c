/* One title-safe runtime: parent server host or admitted shared-server child. */
#include "elf-runtime.h"
#include "protocol.h"
#include "runtime-relaunch.h"
#include "writebroker-autostart.h"
#include "writebroker-menu.h"
#include <wine/ps5_title.h>
#include <wine/ps5_writebroker.h>
#ifdef PW_PARENT_ROLE
#include "runtime-parent.h"
#endif
#ifdef PW_RUNTIME_B1
#include "title-log.h"
#include "forever-paths.h"
#include "forever-setup.h"
#include "forever-menu-settings.h"
#include "../../title/addonforge-client.h"
#include "../../title/addonforge-security.h"
#endif
#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE) && !PW_RUNTIME_GPU
#include "title-icon.h"
#endif
#if defined(PW_PARENT_ROLE) && PW_RUNTIME_GPU
#include <ps5platform/klog.h>
void *pw_title_vulkan_proc(const char *);
int pw_title_input_start(void);
#endif

#if defined(PW_PARENT_ROLE) && PW_RUNTIME_GPU
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index,
                    uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);
int sceAudioOutClose(int32_t port);
int sceSystemServiceGetHdrToneMapLuminance(struct wine_ps5_hdr_luminance *);
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include "runtime-profile.h"

#ifndef SO_NBIO
#define SO_NBIO 0x1200
#endif

extern const char Need_sceLibcInternal;
__attribute__((used)) static const void *const libc_requirement = &Need_sceLibcInternal;
int sceKernelDebugOutText(int, const char *);
void wine_ps5_title_native_request_exit(int);
int wine_public_title_main(int, char **);
void *pw_public_dlopen(const char *, int);
void *pw_public_dlsym(void *, const char *);
int pw_public_dlclose(void *);
char *pw_public_dlerror(void);
int pw_public_dladdr(const void *, Dl_info *);
void *pw_native_tls_get_addr(const size_t *);
static int duplicate_channel[2] = {-1, -1};

static pthread_mutex_t duplicate_mutex = PTHREAD_MUTEX_INITIALIZER;
long pw_native_fail_syscall(long number, ...)
{
    (void)number;
    errno = ENOSYS;
    return -1;
}
pid_t pw_native_fail_fork(void)
{
    errno = ENOSYS;
    return -1;
}
pid_t pw_native_fail_setsid(void)
{
    errno = ENOSYS;
    return -1;
}
long pw_native_fail_minus_one(long argument, ...)
{
    (void)argument;
    errno = ENOSYS;
    return -1;
}
int pw_native_fail_posix_error(int argument, ...)
{
    (void)argument;
    return ENOSYS;
}
unsigned int pw_native_fail_if_nametoindex(const char *name)
{
    (void)name;
    errno = ENOSYS;
    return 0;
}
int pw_native_fail_getmntinfo(void *buffer, int flags)
{
    (void)buffer;
    (void)flags;
    errno = ENOSYS;
    return 0;
}
int pw_native_dup_fd(int descriptor)
{
    int copy, error;
    int64_t now = io_now_ms();
    if (now < 0) return -1;
    pthread_mutex_lock(&duplicate_mutex);
    if (duplicate_channel[0] < 0 || duplicate_channel[1] < 0)
    {
        if (pw_duplicate_bridge_init(duplicate_channel))
        {
            error = errno;
            pthread_mutex_unlock(&duplicate_mutex);
            errno = error;
            return -1;
        }
    }
    copy = pw_duplicate_fd(descriptor, duplicate_channel, now + IO_OPERATION_MS);
    error = errno;
    pthread_mutex_unlock(&duplicate_mutex);
    errno = error;
    return copy;
}
/* The dedicated server preserves its existing platform wrapper ABI. These
 * names are deliberately native calls; the title linker does not wrap them. */
int __real_pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    return pthread_key_create(key, destructor);
}
void *__real_pthread_getspecific(pthread_key_t key)
{
    return pthread_getspecific(key);
}
int __real_pthread_setspecific(pthread_key_t key, const void *value)
{
    return pthread_setspecific(key, value);
}
struct pw_title_module_manifest {
    const char *path;
    uint8_t sha256[32];
    uint64_t size;
};
#ifndef PW_NATIVE_IMPORTS_INCLUDE
#error build-runtime.py must select its generated exact import table
#endif
#include PW_NATIVE_IMPORTS_INCLUDE
_Static_assert(sizeof(title_modules) / sizeof(title_modules[0]) == PW_RUNTIME_MODULE_COUNT,
               "immutable title modules must match the selected profile allow-list");

static struct pw_elf_runtime runtime;
static char from_prefix[PW_ELF_PATH], to_prefix[PW_ELF_PATH];
static const char *const system_modules[] = {"libkernel_web.sprx", "libkernel_sys.sprx",
    "libSceLibcInternal.sprx", "libSceNet.sprx"};
static int output_read = -1, output_write = -1, output_ok, output_bad;
static pthread_t output_thread;
static int output_started;
static _Atomic int output_done;

/* Module wrappers in the staged Unix ntdll call these weak hooks. Keeping
 * this code in the FSELF also gives public pthread/TLS calls a native caller. */
static const struct pw_title_module_manifest *module_manifest(const char *path)
{
    for (size_t i = 0; i < sizeof(title_modules) / sizeof(title_modules[0]); ++i)
        if (!strcmp(path, title_modules[i].path)) return &title_modules[i];
    return NULL;
}
static void *open_module(const char *path, int flags)
{
    if (!path || (flags & ~(RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE | RTLD_NOLOAD))) {
        errno = EINVAL; return NULL;
    }
    char translated[PW_ELF_PATH];
    size_t prefix = strlen(from_prefix);
    if (prefix && !strncmp(path, from_prefix, prefix) && (path[prefix] == '/' || !path[prefix])) {
        int n = snprintf(translated, sizeof(translated), "%s%s", to_prefix, path + prefix);
        if (n < 0 || (size_t)n >= sizeof(translated)) { errno = ENAMETOOLONG; return NULL; }
        path = translated;
    }
    const struct pw_title_module_manifest *manifest = module_manifest(path);
    pthread_mutex_lock(&runtime.lock);
    struct pw_elf_module *m = NULL;
    if (!manifest) {
        snprintf(runtime.error, sizeof(runtime.error), "module is absent from immutable title manifest");
        errno = EACCES;
    } else if (flags & RTLD_NOLOAD) {
        runtime.error[0] = 0;
        for (unsigned i = 0; i < runtime.count; ++i) {
            struct pw_elf_module *candidate = &runtime.modules[i];
            if (strcmp(candidate->path, manifest->path)) continue;
            if (candidate->verified && candidate->verified_size == manifest->size &&
                !memcmp(candidate->verified_sha256, manifest->sha256,
                        sizeof(candidate->verified_sha256))) m = candidate;
            else {
                snprintf(runtime.error, sizeof(runtime.error), "loaded module identity is not manifest identity");
                errno = EACCES;
            }
            break;
        }
        if (!m && !runtime.error[0]) {
            snprintf(runtime.error, sizeof(runtime.error), "manifest module is not loaded");
            errno = ENOENT;
        }
    } else {
        m = pw_elf_load_verified(&runtime, manifest->path, manifest->sha256, manifest->size);
        if (m && pw_elf_start(&runtime)) m = NULL;
    }
    if (!m) io_log("psmkit-wine public-loader failure=%s path=%.200s", runtime.error, path);
    pthread_mutex_unlock(&runtime.lock);
    return m;
}
static void *find_symbol(void *handle, const char *name)
{
    uintptr_t address = 0;
    if (!name) return NULL;
    pthread_mutex_lock(&runtime.lock);
    for (unsigned i=0; i<runtime.count; ++i)
        if (handle == RTLD_DEFAULT || handle == &runtime.modules[i]) {
            address = pw_elf_symbol(&runtime, &runtime.modules[i], name);
            if (address) break;
        }
    if (!address && handle == RTLD_DEFAULT)
        for (size_t i=0; i<sizeof(native_imports)/sizeof(native_imports[0]); ++i)
            if (!strcmp(name,native_imports[i].name)) { address=native_imports[i].address; break; }
    pthread_mutex_unlock(&runtime.lock);
    return (void *)address;
}
static int close_module(void *handle)
{
    /* Wine keeps Unix modules until all its threads and server have joined.
     * Pin them for this native child's lifetime (RTLD_NODELETE semantics). */
    for (unsigned i=0; i<runtime.count; ++i) if (handle == &runtime.modules[i]) return 0;
    errno = EINVAL; return -1;
}
static char *module_error(void) { return runtime.error[0] ? runtime.error : NULL; }
static int address_module(const void *address, Dl_info *info)
{
    if (!info) return 0;
    memset(info,0,sizeof(*info));
    uintptr_t p=(uintptr_t)address;
    for (unsigned i=0; i<runtime.count; ++i) {
        struct pw_elf_module *m=&runtime.modules[i];
        for (unsigned j=0; j<m->segment_count; ++j) {
            struct pw_elf_segment *s=&m->segments[j];
            if (p>=m->bias+s->address && p-(m->bias+s->address)<s->size) {
                info->dli_fname=m->path; info->dli_fbase=m->mapping; return 1;
            }
        }
    }
    return 0;
}
void *pw_public_dlopen(const char *path,int flags) { return open_module(path,flags); }
void *pw_public_dlsym(void *handle,const char *name) { return find_symbol(handle,name); }
int pw_public_dlclose(void *handle) { return close_module(handle); }
char *pw_public_dlerror(void) { return module_error(); }
int pw_public_dladdr(const void *address,Dl_info *info) { return address_module(address,info); }
int ps5_rtld_register_prefix(const char *from,const char *to)
{
    if (!from || !to || strlen(from)>=sizeof(from_prefix) || strlen(to)>=sizeof(to_prefix)) return EINVAL;
    strcpy(from_prefix,from); strcpy(to_prefix,to); return 0;
}
void *pw_native_tls_get_addr(const size_t *index)
{
    return index ? pw_elf_tls(&runtime,index[0],index[1]) : NULL;
}

__attribute__((noinline,disable_tail_calls)) static int create_thread(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *p)
{ return pthread_create(t,a,fn,p); }
__attribute__((noinline,disable_tail_calls)) static int join_thread(pthread_t t,void **p)
{ return pthread_join(t,p); }
__attribute__((noinline,disable_tail_calls)) static int create_key(pthread_key_t *k,void (*d)(void *))
{ return pthread_key_create(k,d); }
__attribute__((noinline,disable_tail_calls)) static void *get_specific(pthread_key_t k)
{ return pthread_getspecific(k); }
__attribute__((noinline,disable_tail_calls)) static int set_specific(pthread_key_t k,const void *p)
{ return pthread_setspecific(k,p); }
__attribute__((noinline,disable_tail_calls)) static int delete_key(pthread_key_t k)
{ return pthread_key_delete(k); }

/*
 * The native port shape is adapted from WoWPS5 v0.1.1
 * app/runtime/wine_audio.c.
 *
 * MIT License
 * Copyright (c) 2026 Brett (BeLikeBrett) and the WoWPS5 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#if defined(PW_PARENT_ROLE) && PW_RUNTIME_GPU
#define PW_AUDIO_ALREADY_INITIALIZED ((int)0x8026000e)
#define PW_AUDIO_USER_SYSTEM 0xff
#define PW_AUDIO_PORT_MAIN 0
#define PW_AUDIO_FORMAT_S16_STEREO 1
static pthread_mutex_t title_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int title_audio_attempted;
static int32_t title_audio_port = -1;
static struct wine_ps5_hdr_luminance title_hdr_luminance;
static int title_hdr_luminance_valid;

static int pw_title_get_hdr_luminance(struct wine_ps5_hdr_luminance *luminance)
{
    if (!luminance) return EINVAL;
    if (!title_hdr_luminance_valid) return ENXIO;
    *luminance = title_hdr_luminance;
    return 0;
}

static int pw_title_hdr_luminance_init(void)
{
    struct wine_ps5_hdr_luminance luminance;
    int result = sceSystemServiceGetHdrToneMapLuminance(&luminance);

    if (result || !(luminance.max_luminance > 0.0f) ||
        !(luminance.max_full_frame_luminance > 0.0f) ||
        !(luminance.min_luminance >= 0.0f) ||
        luminance.max_luminance > 10000.0f ||
        luminance.max_full_frame_luminance > luminance.max_luminance ||
        luminance.min_luminance > luminance.max_luminance / 100.0f)
    {
        io_log("wine: title hdr luminance max=0 min=0 maxfall=0 source=unavailable");
        return -1;
    }
    title_hdr_luminance = luminance;
    title_hdr_luminance_valid = 1;
    io_log("wine: title hdr luminance max=%.3f min=%.6f maxfall=%.3f source=system_service",
           luminance.max_luminance, luminance.min_luminance,
           luminance.max_full_frame_luminance);
    return 0;
}


static int pw_title_audio_open(unsigned int rate, unsigned int grain)
{
    int init, result;

    if (rate != 48000 || grain != 256) return -EINVAL;
    pthread_mutex_lock(&title_audio_lock);
    if (title_audio_attempted)
    {
        result = title_audio_port >= 0 ? 0 : -1;
        pthread_mutex_unlock(&title_audio_lock);
        return result;
    }
    title_audio_attempted = 1;
    init = sceAudioOutInit();
    if (init >= 0 || init == PW_AUDIO_ALREADY_INITIALIZED)
        result = sceAudioOutOpen(PW_AUDIO_USER_SYSTEM, PW_AUDIO_PORT_MAIN, 0, grain, rate,
                                 PW_AUDIO_FORMAT_S16_STEREO);
    else
        result = init;
    if (result >= 0) title_audio_port = result;
    io_log("wine: title audio port open rc=%#x rate=48000 grain=256 format=s16 stereo",
           (unsigned int)result);
    pthread_mutex_unlock(&title_audio_lock);
    return result >= 0 ? 0 : result;
}

static int pw_title_audio_output(const int16_t *frames)
{
    if (title_audio_port < 0 || !frames) return -1;
    return sceAudioOutOutput(title_audio_port, frames);
}

static void pw_title_audio_close(void)
{
    int32_t port;

    pthread_mutex_lock(&title_audio_lock);
    port = title_audio_port;
    title_audio_port = -1;
    pthread_mutex_unlock(&title_audio_lock);
    if (port >= 0) sceAudioOutClose(port);
}
#endif
static struct wine_ps5_title_runtime native_runtime = {
    .create_thread=create_thread,.join_thread=join_thread,.create_key=create_key,
    .get_specific=get_specific,.set_specific=set_specific,.delete_key=delete_key,
    .stdout_fd=1,.server_socket=-1,.version=WINE_PS5_TITLE_RUNTIME_VERSION
};

#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE)
extern int sceSystemServiceLoadExec(const char *, const char *const []);
static atomic_int forever_broker_repair_restart_claimed;
static int pw_title_writebroker_restart(void);
#if !PW_RUNTIME_GPU
static struct pw_game_wire game_wire;
static pthread_mutex_t game_lock = PTHREAD_MUTEX_INITIALIZER;
static int game_relaunch_pending;
#if !PW_RUNTIME_HEADLESS
enum forever_menu_lifetime
{
    FOREVER_MENU_IDLE,
    FOREVER_MENU_ACTIVE,
    FOREVER_MENU_TERMINAL_NOTICE,
    FOREVER_MENU_REINSTALL_READY,
};
static pthread_mutex_t forever_menu_lifetime_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t forever_menu_lifetime_changed = PTHREAD_COND_INITIALIZER;
static int forever_menu_operation;
static atomic_int forever_menu_relaunch_pending;
#endif
/* This start found no initialized Wine prefix in /data/forever: it runs
 * wineboot -i instead of Battle.net, then restarts the app. */
static int forever_prefix_init;
static void forever_say(const char *text);
static void b1_wait(void);
static struct pw_writebroker_outcome pw_title_writebroker_autostart(void);
static int pw_forever_restart_broker(const char *);
static void pw_forever_permission_stop(const char *, const char *);
#if !PW_RUNTIME_HEADLESS
static int pw_writebroker_remove_path(const char *path);
#endif

#if !PW_RUNTIME_HEADLESS
static int forever_title_menu_begin_lifetime(void)
{
    int result = 0;
    pthread_mutex_lock(&forever_menu_lifetime_lock);
    if (forever_menu_operation != FOREVER_MENU_IDLE) result = -EBUSY;
    else forever_menu_operation = FOREVER_MENU_ACTIVE;
    pthread_mutex_unlock(&forever_menu_lifetime_lock);
    return result;
}

static void forever_title_menu_finish_lifetime(enum forever_menu_lifetime state)
{
    pthread_mutex_lock(&forever_menu_lifetime_lock);
    forever_menu_operation = state;
    pthread_cond_broadcast(&forever_menu_lifetime_changed);
    pthread_mutex_unlock(&forever_menu_lifetime_lock);
}

void forever_title_menu_wait_lifetime(void)
{
    pthread_mutex_lock(&forever_menu_lifetime_lock);
    while (forever_menu_operation == FOREVER_MENU_ACTIVE ||
           forever_menu_operation == FOREVER_MENU_TERMINAL_NOTICE)
        pthread_cond_wait(&forever_menu_lifetime_changed, &forever_menu_lifetime_lock);
    pthread_mutex_unlock(&forever_menu_lifetime_lock);
}
#endif

int pw_parent_stage_game_launch(char *const *program, const char *windows_cwd, const char *unix_cwd,
                                int remember)
{
    unsigned separator;
    int error;
    pthread_mutex_lock(&game_lock);
    if (game_relaunch_pending) error = EALREADY;
    else if (!(error = pw_game_wire_encode(&game_wire, program, windows_cwd, unix_cwd)))
    {
        error = -pw_game_wire_decode((int)game_wire.argc, (char **)game_wire.argv,
                                     PW_RUNTIME_PREFIX, wow_channel_resolve_files, NULL, &separator);
        if (!error)
        {
            if (remember)
            {
                int saved = pw_game_record_launch(FOREVER_STATE, (char **)game_wire.argv + 7,
                                                   windows_cwd, unix_cwd);
                io_log("b1 maintenance Play record result=%d argc=%u argv=REDACTED cwd=REDACTED",
                       saved, game_wire.argc - 7);
            }
            game_relaunch_pending = 1;
        }
    }
    pthread_mutex_unlock(&game_lock);
    return error;
}
static void pw_maintenance_loadexec_game(const char *reason)
{
    io_log("b1 maintenance %s; LoadExec path=/app0/game.bin argc=%u argv=REDACTED",
           reason, game_wire.argc);
    int loadexec = sceSystemServiceLoadExec("/app0/game.bin", game_wire.argv);
    io_log("b1 maintenance LoadExec returned result=%d; falling back to exit", loadexec);
}

static int pw_maintenance_direct_game(void)
{
    static const char flag[] = "/app0/wine/direct-game";
    struct stat flag_stat;
    struct wow_channel channel;
    char selector[WOW_FOLDER_BYTES + 16];
    struct pw_game_wire replay;
    int recorded = 0, error, present = !stat(flag, &flag_stat);
    if (!present) return 0;
    error = forever_read_file(flag, selector, sizeof(selector));
    int removed = unlink(flag), unlink_errno = errno;
    if (!pw_game_direct_flag_consumed(present, removed))
    {
        io_log("b1 maintenance direct game flag=unlink-failed errno=%d", unlink_errno);
        return 0;
    }
    io_log("b1 maintenance direct game flag=consumed");
    if (error < 0 || pw_game_direct_selector(selector))
    {
        io_log("b1 maintenance direct game selector invalid; continuing maintenance");
        return 0;
    }
    error = wow_channel_select(PW_RUNTIME_PREFIX, selector, &channel);
    if (error)
    {
        io_log("b1 maintenance direct game unavailable channel=%s error=%d; no fallback",
               selector[0] ? selector : "(automatic)", -error);
        forever_say("Direct start unavailable: the requested WoW channel has no executable and loader pair.");
        return 0;
    }
    error = pw_game_direct_launch(&replay, FOREVER_STATE, &channel, &recorded);
    if (!error)
        error = pw_parent_stage_game_launch((char **)replay.argv + 7, replay.argv[5], replay.argv[3], 0);
    if (error)
    {
        io_log("b1 maintenance direct game validation failed error=%d argv=REDACTED cwd=REDACTED; continuing maintenance",
               error);
        return 0;
    }
    io_log("b1 maintenance direct game source=%s argc=%u argv=REDACTED cwd=REDACTED",
           recorded ? "recorded-Play" : "flavor-fallback", replay.argc - 7);
    pw_maintenance_loadexec_game("direct game startup");
    return 1; /* LoadExec returned: use the same exit fallback, never start Wine. */
}


void pw_parent_request_game_launch(void)
{
    io_log("b1 maintenance Play caught image=Wow*.exe argc=%u argv=REDACTED cwd=REDACTED",
           game_wire.argc - 7);
    wine_ps5_title_native_request_exit((int)0xc0000120);
}

static int pw_game_launch(const struct wine_ps5_title_game_launch *launch)
{
    int error;
    if (!launch || launch->size != sizeof(*launch)) return EINVAL;
    if ((error = pw_parent_stage_game_launch(launch->argv, launch->windows_cwd, launch->unix_cwd, 1)))
        return error;
    pw_parent_request_game_launch();
    return 0;
}
#else

static int pw_game_startup_argv(int argc, char **argv, char ***wine_argv, const char **unix_cwd)
{
    unsigned separator;
    int error = pw_game_wire_decode(argc, argv, PW_RUNTIME_PREFIX,
                                    wow_channel_resolve_files, NULL, &separator);
    if (error) return error;
    *wine_argv = argv + separator;
    (*wine_argv)[0] = (char *)PW_RUNTIME_LOADER;
    *unix_cwd = argv[3];
    return argc - separator;
}
#endif
#endif
#ifndef PW_PARENT_ROLE
static struct pw_p1c_bootstrap child_bootstrap;
static pthread_t control_thread;
static int control_started;
static int (*signal_thread)(unsigned int, void *, int);
static int server_socket_transferred;
#ifdef PW_RUNTIME_B1
/* CreateProcess from a child: the parent's supervisor admits the sibling
 * (runtime-parent.c serve_child_spawns). One request at a time per child. */
static int spawn_channel = -1;
static pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pw_p1c_bootstrap spawn_request;
/* Wait for the parent's reply until the transaction deadline. EINTR retries
 * (Wine signals its threads constantly); a timeout is ETIMEDOUT, never a
 * stale errno from an earlier call. */
static int pw_child_wait_reply(int fd, int64_t deadline)
{
    for (;;)
    {
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        int64_t remaining = deadline - io_now_ms();
        int ready;

        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        ready = poll(&wait, 1, remaining > INT32_MAX ? INT32_MAX : (int)remaining);
        if (ready == 1) return 0;
        if (ready == 0) { errno = ETIMEDOUT; return -1; }
        if (errno != EINTR) return -1;
    }
}

/* A failure inside a transaction leaves the parent's reply (or part of it)
 * in flight; the next request would read it. Close the channel instead. */
static void pw_child_poison_channel(void)
{
    if (spawn_channel >= 0) close(spawn_channel);
    spawn_channel = -1;
}

static int pw_child_spawn(const struct wine_ps5_title_spawn *spawn)
{
    const uint32_t kind = PW_CHILD_SPAWN;
    int32_t reply = EPROTO;
    int error;
    if (!spawn || spawn->size != sizeof(*spawn)) return EINVAL;
    pthread_mutex_lock(&spawn_lock);
    error = pw_p1c_bootstrap_init(&spawn_request, child_bootstrap.generation, child_bootstrap.nonce,
                                  spawn->windows_pid, spawn->argv, spawn->unix_cwd);
    if (!error && spawn_channel < 0) error = EPIPE;
    else if (!error)
    {
        int64_t deadline = io_now_ms() + PW_OVERALL_MS;
        /* The parent's admission has its own PW_OVERALL_MS bound. */
        errno = 0;
        if (io_transfer(spawn_channel, (void *)&kind, sizeof(kind), 1, deadline) ||
            io_transfer(spawn_channel, &spawn_request, sizeof(spawn_request), 1, deadline) ||
            pw_send_fd(spawn_channel, spawn->server_socket, deadline) ||
            pw_child_wait_reply(spawn_channel, io_now_ms() + 2 * PW_OVERALL_MS) ||
            io_transfer(spawn_channel, &reply, sizeof(reply), 0, io_now_ms() + PW_OVERALL_MS))
        {
            error = errno ? errno : EPIPE;
            pw_child_poison_channel();
        }
        else error = reply;
    }
    pthread_mutex_unlock(&spawn_lock);
    io_log("b1 child spawn request windows_pid=%u error=%d", spawn->windows_pid, error);
    return error;
}

#if !PW_RUNTIME_GPU
static int pw_child_game_launch(const struct wine_ps5_title_game_launch *launch)
{
    const uint32_t kind = PW_CHILD_GAME_LAUNCH;
    struct pw_game_wire wire;
    struct pw_game_request request;
    int32_t reply = EPROTO;
    int error;
    int64_t deadline;

    if (!launch || launch->size != sizeof(*launch)) return EINVAL;
    if ((error = pw_game_wire_encode(&wire, launch->argv,
                                     launch->windows_cwd, launch->unix_cwd)) ||
        (error = pw_game_request_pack(&request, &wire))) return error;
    pthread_mutex_lock(&spawn_lock);
    deadline = io_now_ms() + 2 * PW_OVERALL_MS;
    errno = 0;
    if (spawn_channel < 0) error = EPIPE;
    else if (io_transfer(spawn_channel, (void *)&kind, sizeof(kind), 1, deadline) ||
             io_transfer(spawn_channel, &request, sizeof(request), 1, deadline) ||
             pw_child_wait_reply(spawn_channel, deadline) ||
             io_transfer(spawn_channel, &reply, sizeof(reply), 0, deadline))
    {
        error = errno ? errno : EPIPE;
        pw_child_poison_channel();
    }
    else if (reply < 0 || reply > 4095)
    {
        error = EPROTO;
        pw_child_poison_channel();
    }
    else error = reply;
    pthread_mutex_unlock(&spawn_lock);
    io_log("b1 child game launch request image=Wow*.exe result=%d argv=REDACTED cwd=REDACTED", error);
    return error;
}
#endif

/* A resolver lookup can take the DNS timeouts; the parent bounds nothing else. */
#define PW_NATIVE_REPLY_MS 120000
static int child_net_errno;
static int pw_child_native(struct pw_native_request *request, struct pw_native_reply *reply)
{
    const uint32_t kind = PW_CHILD_NATIVE;
    int error = 0;
    pthread_mutex_lock(&spawn_lock);
    errno = 0;
    if (spawn_channel < 0) error = EPIPE;
    else if (io_transfer(spawn_channel, (void *)&kind, sizeof(kind), 1, io_now_ms() + PW_OVERALL_MS) ||
             io_transfer(spawn_channel, request, sizeof(*request), 1, io_now_ms() + PW_OVERALL_MS) ||
             pw_child_wait_reply(spawn_channel, io_now_ms() + PW_NATIVE_REPLY_MS) ||
             io_transfer(spawn_channel, reply, sizeof(*reply), 0, io_now_ms() + PW_OVERALL_MS))
    {
        error = errno ? errno : EPIPE;
        pw_child_poison_channel();
    }
    pthread_mutex_unlock(&spawn_lock);
    if (error)
    {
        io_log("b1 child native op=%u failed errno=%d", request->op, error);
        reply->result = -1;
        reply->net_errno = error;
    }
    child_net_errno = reply->net_errno;
    return reply->result;
}
static int pw_child_payload_spawn(const char *cmdline, const char *cwd)
{
    const uint32_t kind = PW_CHILD_PAYLOAD_SPAWN;
    struct pw_payload_request request;
    int32_t reply = EPROTO;
    int error = 0;
    size_t length;
    int64_t deadline;

    if (!cmdline || !(length = strnlen(cmdline, PW_PAYLOAD_CMDLINE_BYTES)) ||
        length == PW_PAYLOAD_CMDLINE_BYTES) return EINVAL;
    request.cmdline_bytes = length + 1;
    length = cwd ? strnlen(cwd, PW_PAYLOAD_CWD_BYTES) : 0;
    if (length == PW_PAYLOAD_CWD_BYTES) return EINVAL;
    request.cwd_bytes = cwd ? length + 1 : 0;
    pthread_mutex_lock(&spawn_lock);
    deadline = io_now_ms() + 2 * PW_OVERALL_MS;
    errno = 0;
    if (spawn_channel < 0) error = EPIPE;
    else if (io_transfer(spawn_channel, (void *)&kind, sizeof(kind), 1, deadline) ||
             io_transfer(spawn_channel, &request, sizeof(request), 1, deadline) ||
             io_transfer(spawn_channel, (void *)cmdline, request.cmdline_bytes, 1, deadline) ||
             (request.cwd_bytes && io_transfer(spawn_channel, (void *)cwd, request.cwd_bytes, 1, deadline)) ||
             pw_child_wait_reply(spawn_channel, deadline) ||
             io_transfer(spawn_channel, &reply, sizeof(reply), 0, deadline))
    {
        error = errno ? errno : EPIPE;
        pw_child_poison_channel();
    }
    else if (reply < 0 || reply > 4095)
    {
        error = EPROTO;
        pw_child_poison_channel();
    }
    else error = reply;
    pthread_mutex_unlock(&spawn_lock);
    return error;
}

static int pw_child_open_url(const char *url)
{
    const uint32_t kind = PW_CHILD_OPEN_URL;
    struct pw_url_request request;
    int32_t reply = EPROTO;
    int error = 0;
    size_t length;
    int64_t deadline;

    if (!url || !(length = strnlen(url, PW_URL_BYTES)) ||
        length == PW_URL_BYTES) return EINVAL;
    request.url_bytes = length + 1;
    pthread_mutex_lock(&spawn_lock);
    deadline = io_now_ms() + 2 * PW_OVERALL_MS;
    errno = 0;
    if (spawn_channel < 0) error = EPIPE;
    else if (io_transfer(spawn_channel, (void *)&kind, sizeof(kind), 1, deadline) ||
             io_transfer(spawn_channel, &request, sizeof(request), 1, deadline) ||
             io_transfer(spawn_channel, (void *)url, request.url_bytes, 1, deadline) ||
             pw_child_wait_reply(spawn_channel, deadline) ||
             io_transfer(spawn_channel, &reply, sizeof(reply), 0, deadline))
    {
        error = errno ? errno : EPIPE;
        pw_child_poison_channel();
    }
    else if (reply < 0 || reply > 4095)
    {
        error = EPROTO;
        pw_child_poison_channel();
    }
    else error = reply;
    pthread_mutex_unlock(&spawn_lock);
    return error;
}

#if !PW_RUNTIME_HEADLESS
static int pw_child_desktop_call(uint32_t kind, const void *request, size_t request_size,
                                 int descriptor_kind)
{
    struct pw_desktop_reply reply = {.result = -1, .error = EPROTO};
    int result = -1, error = 0, framing_error = 0;
    int64_t deadline;

    /* A forwarded resolver call may hold the channel for a long time; the
     * deadline covers this transaction only, not the wait for the lock. */
    pthread_mutex_lock(&spawn_lock);
    deadline = io_now_ms() + 2 * PW_OVERALL_MS;
    errno = 0;
    if (spawn_channel < 0)
        error = EPIPE;  /* poisoned earlier: fail without closing anything */
    else if (io_transfer(spawn_channel, &kind, sizeof(kind), 1, deadline) ||
             (request_size && io_transfer(spawn_channel, (void *)request, request_size, 1, deadline)) ||
             pw_child_wait_reply(spawn_channel, deadline) ||
             io_transfer(spawn_channel, &reply, sizeof(reply), 0, deadline))
    {
        /* Mid-transaction: a late reply would desynchronize the channel. */
        error = errno ? errno : EPIPE;
        framing_error = 1;
    }
    else if ((reply.result != 0 && reply.result != -1) || reply.error < 0 || reply.error > 4095 ||
             (reply.result == 0 && reply.error) || (reply.result == -1 && !reply.error))
    {
        error = EPROTO;
        framing_error = 1;
    }
    else if (reply.result < 0)
        error = reply.error;
    else if (descriptor_kind == 1)
    {
        result = pw_receive_fd(spawn_channel, io_now_ms() + PW_OVERALL_MS);
        if (result < 0)
        {
            error = errno ? errno : EPIPE;
            framing_error = 1;
        }
    }
    else if (descriptor_kind == 2)
    {
        result = pw_receive_descriptor(spawn_channel, io_now_ms() + PW_OVERALL_MS);
        if (result < 0)
        {
            error = errno ? errno : EPIPE;
            framing_error = 1;
        }
    }
    else result = 0;
    if (framing_error && spawn_channel >= 0)
    {
        close(spawn_channel);
        spawn_channel = -1;
    }
    pthread_mutex_unlock(&spawn_lock);
    if (error)
    {
        errno = error;
        return -1;
    }
    errno = 0;
    return result;
}

static int pw_child_desktop_connect(void)
{
    int result = pw_child_desktop_call(PW_CHILD_DESKTOP, NULL, 0, 1);
    if (result < 0) io_log("b1 child desktop connect failed errno=%d", errno);
    return result;
}

static int pw_child_desktop_surface(const char *name, uint64_t size, int op)
{
    struct pw_surface_request request = {.size = size, .op = op};
    size_t length;
    int descriptor_kind;

    if (!name || (length = strnlen(name, sizeof(request.name))) >= sizeof(request.name) ||
        strncmp(name, "/wine-ps5-", 10) || length <= 10 ||
        size > PW_DESKTOP_SURFACE_MAX ||
        (op != PS5_TITLE_SURFACE_CREATE && op != PS5_TITLE_SURFACE_OPEN &&
         op != PS5_TITLE_SURFACE_UNLINK) ||
        (op == PS5_TITLE_SURFACE_CREATE && !size) ||
        (op == PS5_TITLE_SURFACE_UNLINK && size))
    {
        errno = EINVAL;
        return -1;
    }
    memcpy(request.name, name, length + 1);
    descriptor_kind = op == PS5_TITLE_SURFACE_UNLINK ? 0 : 2;
    return pw_child_desktop_call(PW_CHILD_SURFACE, &request, sizeof(request), descriptor_kind);
}
#endif

#endif
#endif /* !PW_PARENT_ROLE: the native call wrappers below serve both roles */

#ifdef PW_RUNTIME_B1
/* FreeBSD <_ctype.h> inlines in GnuTLS's gnulib (c32is*, c32tolower,
 * wcwidth) call these; SceLibcInternal does not export them. The title has
 * only the C locale, so this is FreeBSD's C locale: ASCII classes, digit
 * values in the low byte, width 1 for printable characters, nothing above
 * 0x7f. */
#include <_ctype.h>
#include <runetype.h>
static _RuneLocale c_rune_locale;
static pthread_once_t c_rune_once = PTHREAD_ONCE_INIT;
static void c_rune_init(void)
{
    _RuneLocale *l = &c_rune_locale;
    memcpy(l->__magic, "RuneMagi", sizeof(l->__magic));
    strcpy(l->__encoding, "NONE");
    for (int c = 0; c < _CACHED_RUNES; ++c) {
        unsigned long t = 0;
        l->__maplower[c] = c >= 'A' && c <= 'Z' ? c + 32 : c;
        l->__mapupper[c] = c >= 'a' && c <= 'z' ? c - 32 : c;
        if (c >= 0x80) { l->__runetype[c] = 0; continue; }
        if (c < 0x20 || c == 0x7f) t |= _CTYPE_C;
        if ((c >= '\t' && c <= '\r') || c == ' ') t |= _CTYPE_S;
        if (c == '\t' || c == ' ') t |= _CTYPE_B;
        if (c >= 0x20 && c < 0x7f) t |= _CTYPE_R | _CTYPE_SW1;
        if (c > 0x20 && c < 0x7f) t |= _CTYPE_G;
        if (c >= '0' && c <= '9') t |= _CTYPE_D | _CTYPE_X | _CTYPE_N | (unsigned long)(c - '0');
        else if (c >= 'A' && c <= 'Z')
            t |= _CTYPE_U | _CTYPE_A | (c <= 'F' ? _CTYPE_X | (unsigned long)(c - 'A' + 10) : 0);
        else if (c >= 'a' && c <= 'z')
            t |= _CTYPE_L | _CTYPE_A | (c <= 'f' ? _CTYPE_X | (unsigned long)(c - 'a' + 10) : 0);
        else if (c > 0x20 && c < 0x7f) t |= _CTYPE_P;
        l->__runetype[c] = t;
    }
}
const _RuneLocale *__getCurrentRuneLocale(void)
{
    pthread_once(&c_rune_once, c_rune_init);
    return &c_rune_locale;
}
unsigned long ___runetype(__ct_rune_t c) { (void)c; return 0; }
__ct_rune_t ___tolower(__ct_rune_t c) { return c; }
#endif

#ifdef PW_RUNTIME_B1
/* libSceNet, libSceNetCtl and libSceSystemService calls of the title's Unix
 * modules and of netdb.o (renamed by build-runtime.py). The parent calls the
 * real functions; a PsmKit child, which has none of those modules loaded,
 * forwards each call to the parent (runtime-parent.c serve_native). */
int sceNetPoolCreate(const char *, int, int);
int sceNetPoolDestroy(int);
int sceNetResolverCreate(const char *, int, int);
int sceNetResolverDestroy(int);
int sceNetResolverStartNtoa(int, const char *, uint32_t *, int, int, int);
int sceNetResolverStartAton(int, const uint32_t *, char *, int, int, int, int);
int *sceNetErrnoLoc(void);
int sceNetCtlInit(void);
int sceNetCtlGetInfo(int, void *);
int sceSystemServiceParamGetString(int, char *, size_t);

#ifdef PW_PARENT_ROLE
#ifdef PW_RUNTIME_B1
/* The title's own resolver. Every lookup of the title and its children ends
 * in the parent; first lookups through the console resolver (and the user's
 * nanoDNS) took ~200 ms each, while public resolvers answer in ~20 ms. Ask
 * the profile's upstream servers directly over UDP, cache the answers for
 * their TTL, and fall back to the console resolver on any failure. It only
 * sends queries from an ephemeral port: nothing listens on port 53 and the
 * console's DNS settings (nanoDNS) stay in force for everything else. */
static const uint32_t pw_dns_upstreams[] = PW_RUNTIME_DNS_UPSTREAMS; /* host byte order */
#define PW_DNS_UPSTREAM_TIMEOUT_MS 500
#define PW_DNS_CACHE_ENTRIES 64
static struct { char name[256]; uint32_t address; int64_t expires; } pw_dns_cache[PW_DNS_CACHE_ENTRIES];
static unsigned int pw_dns_cache_next;
static pthread_mutex_t pw_dns_lock = PTHREAD_MUTEX_INITIALIZER;

static int pw_dns_cache_get(const char *name, uint32_t *address)
{
    int64_t now = io_now_ms();
    int found = 0;
    pthread_mutex_lock(&pw_dns_lock);
    for (unsigned int i = 0; i < PW_DNS_CACHE_ENTRIES && !found; ++i)
        if (pw_dns_cache[i].expires > now && !strcmp(pw_dns_cache[i].name, name))
        {
            *address = pw_dns_cache[i].address;
            found = 1;
        }
    pthread_mutex_unlock(&pw_dns_lock);
    return found;
}

static void pw_dns_cache_put(const char *name, uint32_t address, uint32_t ttl)
{
    unsigned int slot = PW_DNS_CACHE_ENTRIES;
    if (ttl < 30) ttl = 30;
    if (ttl > 3600) ttl = 3600;
    pthread_mutex_lock(&pw_dns_lock);
    for (unsigned int i = 0; i < PW_DNS_CACHE_ENTRIES && slot == PW_DNS_CACHE_ENTRIES; ++i)
        if (!strcmp(pw_dns_cache[i].name, name)) slot = i;
    if (slot == PW_DNS_CACHE_ENTRIES) slot = pw_dns_cache_next++ % PW_DNS_CACHE_ENTRIES;
    snprintf(pw_dns_cache[slot].name, sizeof(pw_dns_cache[slot].name), "%s", name);
    pw_dns_cache[slot].address = address;
    pw_dns_cache[slot].expires = io_now_ms() + (int64_t)ttl * 1000;
    pthread_mutex_unlock(&pw_dns_lock);
}

/* A standard recursive A query; 0 if the name cannot be encoded. */
static size_t pw_dns_query(const char *name, uint16_t id, uint8_t *out, size_t size)
{
    size_t pos = 12;
    if (size < 12 + 256 + 4) return 0;
    memset(out, 0, 12);
    out[0] = id >> 8; out[1] = id & 0xff;
    out[2] = 0x01;              /* RD */
    out[5] = 1;                 /* one question */
    while (*name)
    {
        size_t length = 0;
        while (name[length] && name[length] != '.') ++length;
        if (!length || length > 63 || pos + 1 + length > 12 + 255) return 0;
        out[pos++] = (uint8_t)length;
        memcpy(out + pos, name, length);
        pos += length;
        name += length;
        if (*name) ++name;
    }
    out[pos++] = 0;
    out[pos++] = 0; out[pos++] = 1;  /* type A */
    out[pos++] = 0; out[pos++] = 1;  /* class IN */
    return pos;
}

static size_t pw_dns_skip_name(const uint8_t *message, size_t length, size_t pos)
{
    while (pos < length)
    {
        uint8_t label = message[pos];
        if (!label) return pos + 1;
        if ((label & 0xc0) == 0xc0) return pos + 2 <= length ? pos + 2 : 0;
        if (label & 0xc0) return 0;
        pos += 1 + label;
    }
    return 0;
}

/* 1: first IN A record of a successful reply to our query (CNAMEs before it
 * are skipped). -1: our reply, but no usable address (NXDOMAIN, no A record,
 * or 0.0.0.0, a filter's block answer): the console resolver decides.
 * 0: not our reply. */
static int pw_dns_parse(const uint8_t *message, size_t length, uint16_t id, uint32_t *address, uint32_t *ttl)
{
    size_t pos;
    unsigned int answers;
    if (length < 12 || message[0] != id >> 8 || message[1] != (id & 0xff) || !(message[2] & 0x80))
        return 0;
    if ((message[3] & 0x0f) || message[4] || message[5] != 1) return -1;
    answers = (unsigned int)message[6] << 8 | message[7];
    if (!(pos = pw_dns_skip_name(message, length, 12)) || pos + 4 > length) return -1;
    pos += 4;
    while (answers--)
    {
        unsigned int type, class_, data_length;
        if (!(pos = pw_dns_skip_name(message, length, pos)) || pos + 10 > length) return -1;
        type = (unsigned int)message[pos] << 8 | message[pos + 1];
        class_ = (unsigned int)message[pos + 2] << 8 | message[pos + 3];
        data_length = (unsigned int)message[pos + 8] << 8 | message[pos + 9];
        if (pos + 10 + data_length > length) return -1;
        if (type == 1 && class_ == 1 && data_length == 4)
        {
            memcpy(address, message + pos + 10, 4);
            if (!*address) return -1;
            *ttl = (uint32_t)message[pos + 4] << 24 | (uint32_t)message[pos + 5] << 16 |
                   (uint32_t)message[pos + 6] << 8 | message[pos + 7];
            return 1;
        }
        pos += 10 + data_length;
    }
    return -1;
}

static int pw_dns_ask(uint32_t server, const char *name, uint32_t *address, uint32_t *ttl)
{
    static unsigned int sequence;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(server)};
    uint8_t query[12 + 256 + 4], reply[1500];
    uint16_t id = (uint16_t)(io_now_ms() * 2654435761u + __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED));
    size_t size = pw_dns_query(name, id, query, sizeof(query));
    int64_t deadline = io_now_ms() + PW_DNS_UPSTREAM_TIMEOUT_MS;
    int fd, found = 0;

    if (!size || (fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return 0;
    if (!connect(fd, (struct sockaddr *)&to, sizeof(to)) && write(fd, query, size) == (ssize_t)size)
    {
        while (!found)
        {
            struct pollfd wait = {.fd = fd, .events = POLLIN};
            int64_t remaining = deadline - io_now_ms();
            ssize_t count;
            if (remaining <= 0) break;
            int ready = poll(&wait, 1, (int)remaining);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) break;
            if ((count = read(fd, reply, sizeof(reply))) <= 0) break;
            found = pw_dns_parse(reply, (size_t)count, id, address, ttl);
        }
    }
    close(fd);
    return found;
}

/* Names the console must answer: no dot (local names), or numeric. */
static int pw_dns_internal_name(const char *name)
{
    int dot = 0, alpha = 0;
    if (!name || !*name || strlen(name) > 253) return 0;
    for (const char *c = name; *c; ++c)
    {
        if (*c == '.') dot = 1;
        else if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '-') alpha = 1;
        else if (*c < '0' || *c > '9') return 0;
    }
    return dot && alpha;
}

static const char *pw_dns_internal(const char *name, uint32_t *address)
{
    uint32_t ttl;
    if (!sizeof(pw_dns_upstreams) || !pw_dns_upstreams[0] || !pw_dns_internal_name(name)) return NULL;
    if (pw_dns_cache_get(name, address)) return "cache";
    for (unsigned int i = 0; i < sizeof(pw_dns_upstreams) / sizeof(pw_dns_upstreams[0]); ++i)
    {
        int found = pw_dns_upstreams[i] ? pw_dns_ask(pw_dns_upstreams[i], name, address, &ttl) : 0;
        if (found < 0) break;   /* a server answered without an address */
        if (found)
        {
            pw_dns_cache_put(name, *address, ttl);
            return i ? "upstream2" : "upstream1";
        }
    }
    return NULL;
}
#endif

/* Every name lookup of the title and its children ends here. WoW's startup
 * threads sat in name resolution for whole 10-s profile windows (runs
 * 183a-185a); record how long each lookup takes and who answered it. Slow or
 * failed lookups are always logged, fast ones only for the first 32. */
static int pw_resolver_ntoa_timed(int resolver, const char *host, uint32_t *address,
                                  int timeout, int retry, int flags, const char *via)
{
    static int fast_lines, lines;
    int64_t start = io_now_ms();
    const char *source = NULL;
    int result, net_errno = 0;
#ifdef PW_RUNTIME_B1
    if (address) source = pw_dns_internal(host, address);
#endif
    if (source) result = 0;
    else
    {
        source = "console";
        result = sceNetResolverStartNtoa(resolver, host, address, timeout, retry, flags);
        net_errno = *sceNetErrnoLoc();
    }
    int64_t ms = io_now_ms() - start;

    if ((ms >= 50 || result < 0 || __atomic_fetch_add(&fast_lines, 1, __ATOMIC_RELAXED) < 32) &&
        __atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) < 256)
        io_log("b1 dns ntoa host=%.64s ms=%lld result=%#x errno=%d via=%s source=%s timeout=%d retry=%d",
               host ? host : "(null)", (long long)ms, (unsigned int)result, net_errno, via, source,
               timeout, retry);
    return result;
}
#ifdef PW_RUNTIME_B1
/* Resolve the profile's known hosts at start, one thread each, so the
 * console's per-process lookup cache already holds them when the program
 * asks (first lookups cost ~200 ms each and the cache does not survive the
 * eboot->game.bin hop, runs 186a-193a). Results are discarded. */
static char pw_prefetch_list[] = PW_RUNTIME_PREFETCH_HOSTS;
static void *pw_prefetch_host(void *host)
{
    uint32_t address = 0;
    int pool = sceNetPoolCreate("pw_prefetch", 0x4000, 0);
    if (pool < 0) return NULL;
    int resolver = sceNetResolverCreate("pw_prefetch", pool, 0);
    if (resolver >= 0)
    {
        pw_resolver_ntoa_timed(resolver, host, &address, 0, 0, 0, "prefetch");
        sceNetResolverDestroy(resolver);
    }
    sceNetPoolDestroy(pool);
    return NULL;
}
static void pw_prefetch_hosts(void)
{
    char *next = pw_prefetch_list;
    while (*next)
    {
        char *host = next;
        pthread_t thread;
        while (*next && *next != ';') ++next;
        if (*next) *next++ = 0;
        if (!create_thread(&thread, NULL, pw_prefetch_host, host)) pthread_detach(thread);
    }
}
#endif
#endif

#ifdef PW_PARENT_ROLE
int pw_native_execute(const struct pw_native_request *q, struct pw_native_reply *r)
{
    char text[PW_NATIVE_BYTES];
    union { uint8_t bytes[2 * PW_NATIVE_BYTES]; uint32_t address; char name[PW_NATIVE_BYTES]; } out;
    const int32_t *a = q->arg;
    size_t size;
    memcpy(text, q->text, sizeof(text));
    text[sizeof(text) - 1] = 0;
    memset(&out, 0, sizeof(out));
    switch (q->op)
    {
    case PW_NET_POOL_CREATE: r->result = sceNetPoolCreate(text, a[0], a[1]); break;
    case PW_NET_POOL_DESTROY: r->result = sceNetPoolDestroy(a[0]); break;
    case PW_NET_RESOLVER_CREATE: r->result = sceNetResolverCreate(text, a[0], a[1]); break;
    case PW_NET_RESOLVER_DESTROY: r->result = sceNetResolverDestroy(a[0]); break;
    case PW_NET_RESOLVER_NTOA:
        r->result = pw_resolver_ntoa_timed(a[0], text, &out.address, a[1], a[2], a[3], "child");
        break;
    case PW_NET_RESOLVER_ATON:
    {
        uint32_t address = (uint32_t)a[1];
        size = a[2] > 0 && a[2] <= PW_NATIVE_BYTES ? (size_t)a[2] : PW_NATIVE_BYTES;
        r->result = sceNetResolverStartAton(a[0], &address, out.name, (int)size, a[3], a[4], a[5]);
        break;
    }
    case PW_NET_CTL_INIT: r->result = sceNetCtlInit(); break;
    case PW_NET_CTL_GET_INFO: r->result = sceNetCtlGetInfo(a[0], out.bytes); break;
    case PW_SS_PARAM_GET_STRING:
        size = a[1] > 0 && a[1] <= PW_NATIVE_BYTES ? (size_t)a[1] : PW_NATIVE_BYTES;
        r->result = sceSystemServiceParamGetString(a[0], out.name, size);
        break;
    default:
        return -1;
    }
    r->net_errno = *sceNetErrnoLoc();
    memcpy(r->data, out.bytes, sizeof(r->data));
    return 0;
}
#define PW_FORWARD(call, ...) return call
#else
#define PW_FORWARD(call, ...) do { \
        struct pw_native_request q = {__VA_ARGS__}; struct pw_native_reply r; \
        memset(&r, 0, sizeof(r)); \
        return pw_child_native(&q, &r); } while (0)
#endif

#ifndef PW_PARENT_ROLE
static void pw_native_text(char *dst, const char *src)
{
    snprintf(dst, PW_NATIVE_BYTES, "%s", src ? src : "");
}
#endif

int pw_net_pool_create(const char *name, int size, int flags)
{
#ifdef PW_PARENT_ROLE
    return sceNetPoolCreate(name, size, flags);
#else
    struct pw_native_request q = {.op = PW_NET_POOL_CREATE, .arg = {size, flags}};
    struct pw_native_reply r;
    pw_native_text(q.text, name);
    return pw_child_native(&q, &r);
#endif
}
int pw_net_pool_destroy(int pool)
{
    PW_FORWARD(sceNetPoolDestroy(pool), .op = PW_NET_POOL_DESTROY, .arg = {pool});
}
int pw_net_resolver_create(const char *name, int pool, int flags)
{
#ifdef PW_PARENT_ROLE
    return sceNetResolverCreate(name, pool, flags);
#else
    struct pw_native_request q = {.op = PW_NET_RESOLVER_CREATE, .arg = {pool, flags}};
    struct pw_native_reply r;
    pw_native_text(q.text, name);
    return pw_child_native(&q, &r);
#endif
}
int pw_net_resolver_destroy(int resolver)
{
    PW_FORWARD(sceNetResolverDestroy(resolver), .op = PW_NET_RESOLVER_DESTROY, .arg = {resolver});
}
int pw_net_resolver_ntoa(int resolver, const char *host, uint32_t *address, int timeout, int retry, int flags)
{
#ifdef PW_PARENT_ROLE
    return pw_resolver_ntoa_timed(resolver, host, address, timeout, retry, flags, "parent");
#else
    struct pw_native_request q = {.op = PW_NET_RESOLVER_NTOA, .arg = {resolver, timeout, retry, flags}};
    struct pw_native_reply r;
    int result;
    pw_native_text(q.text, host);
    result = pw_child_native(&q, &r);
    if (result >= 0 && address) memcpy(address, r.data, sizeof(*address));
    return result;
#endif
}
int pw_net_resolver_aton(int resolver, const uint32_t *address, char *host, int size,
                         int timeout, int retry, int flags)
{
#ifdef PW_PARENT_ROLE
    return sceNetResolverStartAton(resolver, address, host, size, timeout, retry, flags);
#else
    struct pw_native_request q = {.op = PW_NET_RESOLVER_ATON,
        .arg = {resolver, address ? (int32_t)*address : 0, size, timeout, retry, flags}};
    struct pw_native_reply r;
    int result = pw_child_native(&q, &r);
    if (result >= 0 && host && size > 0)
        snprintf(host, (size_t)size, "%.*s", PW_NATIVE_BYTES, (const char *)r.data);
    return result;
#endif
}
int *pw_net_errno_loc(void)
{
#ifdef PW_PARENT_ROLE
    return sceNetErrnoLoc();
#else
    return &child_net_errno;
#endif
}
int pw_net_ctl_init(void)
{
    PW_FORWARD(sceNetCtlInit(), .op = PW_NET_CTL_INIT);
}
int pw_net_ctl_get_info(int code, void *info)
{
#ifdef PW_PARENT_ROLE
    return sceNetCtlGetInfo(code, info);
#else
    /* Callers' SceNetCtlInfo is a union of at most 256-byte names. */
    struct pw_native_request q = {.op = PW_NET_CTL_GET_INFO, .arg = {code}};
    struct pw_native_reply r;
    int result = pw_child_native(&q, &r);
    if (!result && info) memcpy(info, r.data, PW_NATIVE_BYTES);
    return result;
#endif
}
int pw_ss_param_get_string(int id, char *buffer, size_t size)
{
#ifdef PW_PARENT_ROLE
    return sceSystemServiceParamGetString(id, buffer, size);
#else
    struct pw_native_request q = {.op = PW_SS_PARAM_GET_STRING,
        .arg = {id, size > PW_NATIVE_BYTES ? PW_NATIVE_BYTES : (int32_t)size}};
    struct pw_native_reply r;
    int result = pw_child_native(&q, &r);
    if (!result && buffer && size)
        snprintf(buffer, size, "%.*s", PW_NATIVE_BYTES, (const char *)r.data);
    return result;
#endif
}
#undef PW_FORWARD
#endif
#ifndef PW_PARENT_ROLE
static void *control_agent(void *unused)
{
    (void)unused;
    for (;;)
    {
        struct pw_control request;
        struct pollfd fd = {.fd = 3, .events = POLLIN};
        int ready = poll(&fd, 1, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || io_transfer(3, &request, sizeof(request), 0,
                                    io_now_ms() + IO_OPERATION_MS)) break;
        int error = pw_control_validate(&request, PW_SIGNAL, child_bootstrap.generation,
                                        child_bootstrap.nonce);
        if (error || request.pid != (unsigned int)getpid()) break;
        errno = 0;
        int result = signal_thread(request.thread_id, (void *)(uintptr_t)request.teb, request.sig);
        request.type = PW_SIGNAL_REPLY;
        request.error = result ? (errno ? errno : ESRCH) : 0;
        if (io_transfer(3, &request, sizeof(request), 1, io_now_ms() + IO_OPERATION_MS)) break;
    }
    io_log("p1c child control EOF pid=%d generation=%llx", (int)getpid(),
           (unsigned long long)child_bootstrap.generation);
    return NULL;
}
#endif
/* Only designated value fields vary; fixed lines remain byte-exact. */
#ifndef PW_RUNTIME_B1
static int p1w_output_matches(const char *line, size_t length, unsigned int index)
{
#ifdef PW_PARENT_ROLE
    static const char *const expected[] = {PW_PARENT_FIBER, PW_PARENT_OUTPUT};
#else
    static const char *const expected[] = {
        PW_CHILD_ENTRY, PW_CHILD_FIBER, PW_CHILD_CONTEXT, PW_CHILD_OUTPUT
    };
#endif
    if (index >= sizeof(expected) / sizeof(expected[0])) return 0;
    const char *format = expected[index];
    if (length != strlen(format)) return 0;
    for (size_t i = 0; i < length; ++i) {
        if (format[i] == 'X') {
            if (!((line[i] >= '0' && line[i] <= '9') ||
                  (line[i] >= 'a' && line[i] <= 'f'))) return 0;
        } else if (line[i] != format[i]) return 0;
    }
    return 1;
}
#endif
static void *collect_output(void *unused)
{
    (void)unused;
    char line[256]; size_t used=0;
    for (;;) {
        struct pollfd p={output_read,POLLIN,0};
        int ready=poll(&p,1,250);
        if (ready<0 && errno==EINTR) continue;
        if (!ready) { if (atomic_load(&output_done)) break; else continue; }
        if (ready<0) { output_bad=1; break; }
        char bytes[256]; ssize_t n=read(output_read,bytes,sizeof(bytes));
        if (n<0 && errno==EINTR) continue;
        if (!n) break;
        if (n<0) { output_bad=1; break; }
        for (ssize_t i=0;i<n;++i) {
            if (used==sizeof(line)-1) { output_bad=1; used=0; }
            line[used++]=bytes[i];
            if (bytes[i]=='\n') {
                line[used]=0;
#ifndef PW_RUNTIME_B1
                if (p1w_output_matches(line, used, output_ok)) ++output_ok;
                else output_bad=1;
#endif
                sceKernelDebugOutText(0,"psmkit-wine stdout BEGIN\n");
                sceKernelDebugOutText(0,line);
                sceKernelDebugOutText(0,"psmkit-wine stdout END\n"); used=0;
            }
        }
    }
    if (used) output_bad=1;
    close(output_read); return NULL;
}
/* Wine's threads log from here on: their klog lines (and io_log's) leave on
 * title-log.c's writer thread. Startup failures before it log synchronously. */
static void pw_title_log_begin(void)
{
#ifdef PW_RUNTIME_B1
    pw_title_log_start(PW_RUNTIME_PREFIX "/drive_c/wineps5-tune.txt");
#endif
}
void __attribute__((noreturn)) pw_native_exit(int status)
{
#ifdef PW_RUNTIME_B1
    /* Every queued line first; the exit lines below are written synchronously. */
    pw_title_log_shutdown();
#endif
#ifdef PW_PARENT_ROLE
    pw_supervisor_shutdown();
#if PW_RUNTIME_GPU
    pw_title_audio_close();
#endif
#else
    if (control_started) {
        /* Stop only our read side. SHUT_WR/close would publish EOF before
         * native stdout and ELF teardown have finished. */
        shutdown(3, SHUT_RD);
        join_thread(control_thread, NULL);
    }
    if (!server_socket_transferred && native_runtime.server_socket >= 0)
        close(native_runtime.server_socket);
#endif
    if (output_started) {
        errno=0;
        int closed=close(output_write), saved_errno=errno;
        output_write=-1;
        io_log("psmkit-wine shutdown stage=stdout_writer_close result=%d errno=%d reserved_fd1_unchanged=1",
               closed,saved_errno);
        atomic_store(&output_done,1);
        join_thread(output_thread,NULL);
        if (closed) output_bad=1;
    }
#ifdef PW_RUNTIME_B1
#ifdef PW_PARENT_ROLE
    io_log("b1 parent result=WINE_EXIT wine_exit=%d topology=PARENT_SHARED_INPROCESS_WINESERVER", status);
#else
    io_log("b1 child result=WINE_EXIT wine_exit=%d topology=CHILD_PARENT_WINESERVER", status);
#endif
#elif defined(PW_PARENT_ROLE)
    io_log("p1w parent result=%s wine_exit=%d stdout_exact=%d topology=PARENT_SHARED_INPROCESS_WINESERVER",
           !status && output_ok==2 && !output_bad ? "P1W_PE32_PASS" : "FAIL_OR_INCONCLUSIVE",
           status,output_ok==2 && !output_bad);
    if (!status && (output_ok!=2 || output_bad)) status=1;
#else
    io_log("p1w child result=%s wine_exit=%d stdout_exact=%d topology=CHILD_PARENT_WINESERVER",
           status==5 && output_ok==4 && !output_bad ? "P1W_PE32_CHILD_PASS" : "FAIL_OR_INCONCLUSIVE",
           status,output_ok==4 && !output_bad);
#endif
    pw_duplicate_bridge_close(duplicate_channel);
    /* The OS reclaims these private mappings at exit. Do not run module
     * finalizers or unmap their code here: Wine may still have a native
     * thread completing teardown after the supervised initial thread. */
#ifndef PW_PARENT_ROLE
    close(3); /* Last native action: supervisor EOF now means lifetime ended. */
#endif
#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE) && !PW_RUNTIME_GPU
#if !PW_RUNTIME_HEADLESS
    if (atomic_load(&forever_menu_relaunch_pending))
    {
        io_log("forever: Battle.net reinstall restart; LoadExec path=/app0/eboot.bin");
        fflush(NULL);
        int result = sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
        io_log("forever: Battle.net reinstall LoadExec returned result=%d", result);
        forever_say("Battle.net was removed, but Forever Launcher could not restart itself.\n"
                    "Close Forever Launcher and start it again to reinstall Battle.net.");
        b1_wait();
    }
#endif
    if (game_relaunch_pending)
        pw_maintenance_loadexec_game("shutdown complete");
    if (forever_prefix_init)
    {
        int finished = forever_finish_init(FOREVER_PREFIX);
        int ready, permission_stopped = 0;
        if (forever_permission_error(finished))
        {
            pw_forever_permission_stop(FOREVER_PREFIX, FOREVER_PREFIX);
            permission_stopped = 1;
        }
        ready = forever_prefix_ready(FOREVER_PREFIX);
        io_log("forever: first-start prefix setup finished=%d ready=%d status=%d",
               finished, ready, status);
        if (permission_stopped)
            b1_wait();
        else if (ready)
        {
            fflush(NULL);
            int result = sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
            io_log("forever: restart LoadExec returned result=%d", result);
        }
        /* Native libc exit SIGSYS-traps on fw 5.50 (below): keep the reason
         * on screen until the user closes the app. */
        if (!permission_stopped)
            forever_say(ready ? "Setup finished, but Forever Launcher could not restart itself.\n"
                                "Close Forever Launcher and start it again."
                              : "Setting up the Windows files did not finish.\n"
                                "Close Forever Launcher and start it again.");
        b1_wait();
    }
#endif
#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE) && PW_RUNTIME_GPU
    io_log("b1 game exit status=%d; closing the app", status);
    /* Native title libc exit/_exit triggers SIGSYS in libkernel on fw 5.50.
     * Wine/server persistence, children, audio and stdout are already closed;
     * ask the shell to end the app instead, and never fall back to libc exit. */
    io_log("b1 game shutdown complete; LoadExec path=exit status=%d", status);
    fflush(NULL);
    int result = sceSystemServiceLoadExec("exit", NULL);
    io_log("b1 game exit LoadExec returned result=%d; waiting for app close", result);
    for (;;) usleep(100000);
#endif
    exit(status);
}

#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE)
int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2Term(int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2GetResponseContentLength(int, int *, uint64_t *);
int sceHttp2ReadData(int, void *, size_t);
#if !PW_RUNTIME_HEADLESS
void title_desktop_message(const char *);
#endif

#if !PW_RUNTIME_GPU
static void b1_message(const char *text)
{
#if !PW_RUNTIME_HEADLESS
    title_desktop_message(text);
#else
    if (text && *text) io_log("b1 parent status: %s", text);
#endif
}

static void b1_wait(void)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&mutex);
    for (;;) pthread_cond_wait(&condition, &mutex);
}

#define B1_DOWNLOAD_CALL(label, call) \
    do { *step = label; if ((result = (call)) < 0) goto done; } while (0)

/* One GET into the temporary file. Returns 0 only when it holds the whole
 * announced length and an MZ image; otherwise the failing step's code. */
static int b1_fetch_installer(int tmpl, const char *url, const char *part, const char **step,
                              uint64_t *total, uint64_t *length)
{
    static unsigned char buffer[65536];
    const uint64_t cap = 256u * 1024u * 1024u;
    int req = -1, fd = -1, result = 0, status = 0, length_type = 0;
    uint64_t progress = UINT64_MAX;
    unsigned char first[2] = {0};
    char message[160];

    *total = *length = 0;
    B1_DOWNLOAD_CALL("creating request", req = sceHttp2CreateRequestWithURL(tmpl, "GET", url, 0));
    B1_DOWNLOAD_CALL("sending request", sceHttp2SendRequest(req, NULL, 0));
    B1_DOWNLOAD_CALL("reading HTTP status", sceHttp2GetStatusCode(req, &status));
    *step = "HTTP status is not 200";
    if (status != 200) { result = status; goto done; }
    B1_DOWNLOAD_CALL("reading content length", sceHttp2GetResponseContentLength(req, &length_type, length));
    /* A length the server does not announce (chunked) is checked only by the cap. */
    if (length_type != 0) *length = 0;
    *step = "invalid announced installer length";
    if (*length && (*length < 2 || *length > cap)) { result = EINVAL; goto done; }
    B1_DOWNLOAD_CALL("opening temporary installer", fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666));
    for (;;)
    {
        B1_DOWNLOAD_CALL("reading installer", sceHttp2ReadData(req, buffer, sizeof(buffer)));
        if (!result) break;
        size_t count = (size_t)result, offset = 0;
        *step = "installer exceeds announced length or 256 MiB";
        if (count > cap - *total || (*length && count > *length - *total)) { result = EFBIG; goto done; }
        for (size_t i = 0; *total + i < 2 && i < count; i++) first[*total + i] = buffer[i];
        *step = "writing installer";
        while (offset < count)
        {
            ssize_t written = write(fd, buffer + offset, count - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { result = written < 0 ? errno : EIO; goto done; }
            offset += (size_t)written;
        }
        *total += count;
        if (*total / (1024u * 1024u) != progress || *total == *length)
        {
            progress = *total / (1024u * 1024u);
            if (*length)
                snprintf(message, sizeof(message), "Downloading the Battle.net installer: %.1f of %.1f MB",
                         (double)*total / (1024 * 1024), (double)*length / (1024 * 1024));
            else
                snprintf(message, sizeof(message), "Downloading the Battle.net installer: %.1f MB",
                         (double)*total / (1024 * 1024));
            b1_message(message);
        }
    }
    *step = "installer length or MZ signature mismatch";
    if ((*length && *total != *length) || *total < 2 || first[0] != 'M' || first[1] != 'Z') { result = EINVAL; goto done; }
    *step = "closing installer";
    result = close(fd);
    fd = -1;
    if (result) result = errno;
done:
    if (fd >= 0) close(fd);
    if (req >= 0) sceHttp2DeleteRequest(req);
    return result;
}

/* Native I/O stays off the desktop thread. Publish only a complete installer.
 * The Live URL redirects to the current versioned file; when that download
 * fails (one console received the 200 and length but no body), the direct
 * versioned file is tried once before giving up. */
static int b1_download_installer(void)
{
    static const char *const urls[] = {PW_RUNTIME_INSTALLER_URL, PW_RUNTIME_INSTALLER_FALLBACK_URL};
    const char *part = PW_RUNTIME_INSTALLER_CWD "/.Battle.net-Setup.exe.part";
    int pool = -1, ssl = -1, http = -1, tmpl = -1, result = 0, complete = 0, logged = 0;
    uint64_t total = 0, length = 0;
    const char *what = "creating prefix directory", **step = &what;
    char message[640], directory[] = PW_RUNTIME_INSTALLER_CWD;
    unsigned int i = 0;

    b1_message("Downloading the Battle.net installer...");
    for (char *p = directory + 1; ; p++)
    {
        if (*p && *p != '/') continue;
        char saved = *p;
        *p = 0;
        if (mkdir(directory, 0777) && errno != EEXIST) { result = errno; goto done; }
        *p = saved;
        if (!saved) break;
    }
    /* DT_NEEDED loads Net/SSL/HTTP2, as in the measured HTTPS title probe.
     * SceHttp2's default redirect handling follows the Live URL's 302. */
    B1_DOWNLOAD_CALL("initializing Net", sceNetInit());
    B1_DOWNLOAD_CALL("creating network pool", pool = sceNetPoolCreate("b1-installer", 64 * 1024, 0));
    B1_DOWNLOAD_CALL("initializing SSL", ssl = sceSslInit(256 * 1024));
    B1_DOWNLOAD_CALL("initializing HTTP2", http = sceHttp2Init(pool, ssl, 256 * 1024, 1));
    B1_DOWNLOAD_CALL("creating HTTP2 template", tmpl = sceHttp2CreateTemplate(http, "Mozilla/5.0", 3, 1));
    for (i = 0; i < sizeof(urls) / sizeof(urls[0]); i++)
    {
        if (i) b1_message("Retrying the Battle.net installer download from its direct link...");
        result = b1_fetch_installer(tmpl, urls[i], part, step, &total, &length);
        if (!result) break;
        io_log("b1 parent installer download failed: %s result=%#x bytes=%llu length=%llu url=%u",
               *step, result, (unsigned long long)total, (unsigned long long)length, i);
        logged = 1;
    }
    if (result) goto done;
    logged = 0;
    *step = "publishing installer";
    if (rename(part, PW_RUNTIME_INSTALLER_UNIX)) { result = errno; goto done; }
    complete = 1;
done:
    if (tmpl >= 0) sceHttp2DeleteTemplate(tmpl);
    if (http >= 0) sceHttp2Term(http);
    if (ssl >= 0) sceSslTerm(ssl);
    if (pool >= 0) sceNetPoolDestroy(pool);
    if (!complete)
    {
        unlink(part);
        if (!logged)
            io_log("b1 parent installer download failed: %s result=%#x bytes=%llu length=%llu",
                   *step, result, (unsigned long long)total, (unsigned long long)length);
        snprintf(message, sizeof(message), "Battle.net installer download failed: %s (%#x).\nClose the title and try again.",
                 *step, result);
        b1_message(message);
        return -1;
    }
    io_log("b1 parent installer downloaded bytes=%llu url=%u", (unsigned long long)total, i);
    return 0;
}
#undef B1_DOWNLOAD_CALL
#endif
#endif

#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE)
static void forever_say(const char *text)
{
    io_log("forever: %s", text);
#if !PW_RUNTIME_GPU
    b1_message(text);
#endif
}

static int pw_forever_restart_broker(const char *failing_path)
{
    int result = pw_writebroker_claim_repair_restart(
        &forever_broker_repair_restart_claimed);
    if (!result) result = pw_title_writebroker_restart();
    io_log("forever: permission repair restart path=%s result=%d", failing_path, result);
    return result;
}

static void pw_forever_permission_stop(const char *failing_path, const char *repair_root)
{
    char message[512];
    if (forever_permission_message(
            message, sizeof(message), failing_path, repair_root) < 0)
        snprintf(message, sizeof(message),
                 "Forever Launcher can't write to %s (permission denied).",
                 failing_path);
    forever_say(message);
}

/* Report each attempt before callers restart the broker or show a dialog. */
static int pw_forever_sync_managed(void)
{
    struct forever_managed_report report;
    int result = forever_sync_managed(
        FOREVER_APP_ROOT, FOREVER_APP_SHARE "/prefix-files.txt", FOREVER_PREFIX, &report);
    io_log("forever: prefix sync checked=%u hashed=%u repaired=%u",
           report.checked, report.hashed, report.repaired);
    io_log("forever: prefix sync state read=%d loaded=%u publication=%s result=%d bytes=%zu",
           report.state_read_result, report.state_loaded,
           report.state_publish_attempted ? "attempted" : "unchanged",
           report.state_publish_result, report.state_publish_bytes);
    if (report.mismatch_field)
        io_log("forever: prefix sync mismatch path=%s field=%s recorded=%s current=%s ignored=%d",
               report.mismatch_path, report.mismatch_field, report.mismatch_recorded,
               report.mismatch_current, report.mismatch_ignored);
    for (unsigned i = 0; i < report.repaired && i < FOREVER_MANAGED_LOG_LIMIT; ++i)
        io_log("forever: prefix sync repaired %s", report.paths[i]);
    if (result < 0)
        io_log("forever: prefix sync failed op=%s errno=%d path=%s",
               report.failed_op, -result, report.failed_path);
    return result;
}

/* Every start, before Wine: put the user's data in /data/forever (moving an
 * older in-app prefix there once), write default settings files the user does
 * not have yet, bring the prefix files the app owns up to this version, and
 * install WoW's preset Config.wtf once. 0 to continue, -1 to stop. */
static int pw_forever_start(void)
{
    static const char *const legacy[] = {
        "/data/homebrew/PPSA99030/wine",
        "/data/prosperostore/previous/PPSA99030/wine",
    };
    static const char *const directories[] = { FOREVER_PREFIX, FOREVER_HOME, FOREVER_SHADER_CACHE };
    static const char *const defaults[][2] = {
        { FOREVER_DRIVE_C "/wineps5-tune.txt", FOREVER_TUNE_DEFAULT },
        { FOREVER_DRIVE_C "/wineps5-fg.txt", FOREVER_FG_DEFAULT },
        { FOREVER_DRIVE_C "/wineps5-hud.txt", FOREVER_HUD_DEFAULT },
        { FOREVER_DRIVE_C "/wineps5-hdr-output.txt", FOREVER_HDR_OUTPUT_DEFAULT },
    };
    char from[256] = "", migration_failure[256] = "", message[512];
    int broker_repaired = 0, repair, result = forever_mkdirs(FOREVER_ROOT);

    if (forever_permission_error(result))
    {
        repair = pw_forever_restart_broker(FOREVER_ROOT);
        if (!repair)
        {
            broker_repaired = 1;
            result = forever_mkdirs(FOREVER_ROOT);
        }
        if (repair || forever_permission_error(result))
        {
            pw_forever_permission_stop(FOREVER_ROOT, FOREVER_ROOT);
            return -1;
        }
    }
    if (result)
    {
        if (result == -ENOENT || result == -EROFS)
            snprintf(message, sizeof(message), "Forever Launcher cannot use " FOREVER_ROOT
                     " (error %d).\nUpdate ShadowMountPlus to a version that lets apps use /data,\n"
                     "then start Forever Launcher again.", -result);
        else
            snprintf(message, sizeof(message), "Could not create " FOREVER_ROOT " (error %d).",
                     -result);
        forever_say(message);
        return -1;
    }

    result = forever_migrate(FOREVER_PREFIX, FOREVER_HOME, FOREVER_SHADER_CACHE, legacy,
                             sizeof(legacy) / sizeof(legacy[0]), from, sizeof(from));
    if (forever_permission_error(result))
    {
        snprintf(migration_failure, sizeof(migration_failure), "%s",
                 from[0] ? from : FOREVER_ROOT);
        repair = pw_forever_restart_broker(migration_failure);
        if (!repair)
        {
            broker_repaired = 1;
            result = forever_migrate(FOREVER_PREFIX, FOREVER_HOME, FOREVER_SHADER_CACHE, legacy,
                                     sizeof(legacy) / sizeof(legacy[0]), from, sizeof(from));
        }
        if (repair || forever_permission_error(result))
        {
            pw_forever_permission_stop(migration_failure, migration_failure);
            return -1;
        }
    }
    io_log("forever: migration result=%d from=%s", result, result == 1 ? from : "-");
    if (result < 0)
    {
        snprintf(message, sizeof(message), "Could not move your earlier Wine data to " FOREVER_ROOT
                 " (error %d).", -result);
        forever_say(message);
        return -1;
    }
    if (result == 1 && !broker_repaired)
    {
        repair = pw_forever_restart_broker(FOREVER_ROOT);
        if (repair)
        {
            pw_forever_permission_stop(FOREVER_ROOT, FOREVER_ROOT);
            return -1;
        }
    }

    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
    {
        result = forever_mkdirs(directories[i]);
        if (forever_permission_error(result))
        {
            repair = pw_forever_restart_broker(directories[i]);
            if (!repair) result = forever_mkdirs(directories[i]);
            if (repair || forever_permission_error(result))
            {
                pw_forever_permission_stop(directories[i], FOREVER_ROOT);
                return -1;
            }
        }
        if (result)
        {
            snprintf(message, sizeof(message), "Could not create %s (error %d).",
                     directories[i], -result);
            forever_say(message);
            return -1;
        }
    }
#if PW_RUNTIME_GPU
    if (setenv("MESA_SHADER_CACHE_DIR", FOREVER_SHADER_CACHE, 1)) return -1;
#endif
    if (!forever_prefix_ready(FOREVER_PREFIX))
    {
#if PW_RUNTIME_GPU
        io_log("forever: the game started without an initialized prefix");
        return -1;
#else
        result = forever_begin_init(FOREVER_PREFIX);
        if (forever_permission_error(result))
        {
            repair = pw_forever_restart_broker(FOREVER_PREFIX);
            if (!repair) result = forever_begin_init(FOREVER_PREFIX);
            if (repair || forever_permission_error(result))
            {
                pw_forever_permission_stop(FOREVER_PREFIX, FOREVER_PREFIX);
                return -1;
            }
        }
        if (result)
        {
            snprintf(message, sizeof(message), "Could not prepare " FOREVER_PREFIX " (error %d).",
                     -result);
            forever_say(message);
            return -1;
        }
        forever_prefix_init = 1;
        forever_say("First start: setting up the Windows files.\n"
                    "This takes a few minutes. Forever Launcher restarts by itself when it is done.");
        return 0;
#endif
    }

    result = forever_standard_folders(FOREVER_PREFIX);
    if (forever_permission_error(result))
    {
        repair = pw_forever_restart_broker(FOREVER_DRIVE_C);
        if (!repair) result = forever_standard_folders(FOREVER_PREFIX);
        if (repair || forever_permission_error(result))
        {
            pw_forever_permission_stop(FOREVER_DRIVE_C, FOREVER_PREFIX);
            return -1;
        }
    }
    io_log("forever: standard folders result=%d", result);
    if (result < 0)
    {
        snprintf(message, sizeof(message), "Could not create the Program Files folders in "
                 FOREVER_DRIVE_C " (error %d).", -result);
        forever_say(message);
        return -1;
    }

    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i)
    {
        result = forever_write_if_absent(defaults[i][0], defaults[i][1]);
        if (forever_permission_error(result))
        {
            repair = pw_forever_restart_broker(defaults[i][0]);
            if (!repair) result = forever_write_if_absent(defaults[i][0], defaults[i][1]);
            if (repair || forever_permission_error(result))
            {
                pw_forever_permission_stop(defaults[i][0], FOREVER_PREFIX);
                return -1;
            }
        }
        io_log("forever: default %s result=%d", defaults[i][0], result);
        if (result < 0)
        {
            snprintf(message, sizeof(message), "Could not write %s (error %d).",
                     defaults[i][0], -result);
            forever_say(message);
            return -1;
        }
    }

    /* Maintenance's supervisor already opened/presented the desktop and
     * started its independent frame thread. This scan stays on the startup
     * thread, and all DLLs are checked before normal Wine or game handoff. */
    forever_say("Checking the app's Windows files...");
    result = pw_forever_sync_managed();
    if (forever_permission_error(result))
    {
        repair = pw_forever_restart_broker(FOREVER_PREFIX);
        if (!repair)
            result = pw_forever_sync_managed();
        if (repair || forever_permission_error(result))
        {
            pw_forever_permission_stop(FOREVER_PREFIX, FOREVER_PREFIX);
            return -1;
        }
    }
    io_log("forever: app-owned prefix files result=%d", result);
    if (result < 0)
    {
        snprintf(message, sizeof(message), "Could not update the app's Windows files in "
                 FOREVER_PREFIX " (error %d).", -result);
        forever_say(message);
        return -1;
    }
#if !PW_RUNTIME_GPU
    b1_message("");
#endif

    result = forever_install_wow_config(FOREVER_APP_SHARE, FOREVER_PREFIX);
    if (forever_permission_error(result))
    {
        repair = pw_forever_restart_broker(FOREVER_PREFIX);
        if (!repair)
            result = forever_install_wow_config(FOREVER_APP_SHARE, FOREVER_PREFIX);
        if (repair || forever_permission_error(result))
        {
            pw_forever_permission_stop(FOREVER_PREFIX, FOREVER_PREFIX);
            return -1;
        }
    }
    io_log("forever: WoW preset Config.wtf %s=%d",
           result < 0 ? "error" : "installed", result < 0 ? -result : result);
    if (result < 0)
    {
        snprintf(message, sizeof(message), "Could not install WoW's Config.wtf in "
                 FOREVER_PREFIX " (error %d).", -result);
        forever_say(message);
        return -1;
    }
    return 0;
}

#if !PW_RUNTIME_GPU
/* wineboot -i does not end the title's Wine by itself: services.exe and
 * explorer.exe stay. Its last step writes FOREVER_WINEBOOT_DONE: then end
 * them and Wine, which saves the registries; pw_native_exit restarts the app. */
static void *pw_forever_init_watch(void *unused)
{
    (void)unused;
    for (unsigned seconds = 0; seconds < 900; seconds += 2)
    {
        usleep(2000000);
        if (!forever_wineboot_done(FOREVER_PREFIX)) continue;
        io_log("forever: wineboot finished after %u s; stopping Wine", seconds);
        pw_supervisor_end_children();
        wine_ps5_title_native_request_exit(0);
        return NULL;
    }
    forever_say("Setting up the Windows files did not finish in 15 minutes.\n"
                "Close Forever Launcher and start it again.");
    wine_ps5_title_native_request_exit(1);
    return NULL;
}

#if !PW_RUNTIME_HEADLESS
void title_desktop_menu_status(const char *, int);

int forever_title_menu_load(struct forever_menu_values *values)
{
    return forever_menu_read_settings(
        FOREVER_DRIVE_C "/wineps5-fg.txt",
        FOREVER_DRIVE_C "/wineps5-hdr-output.txt",
        FOREVER_DRIVE_C "/wineps5-hud.txt",
        &values->frame_generation, &values->hdr, &values->hud);
}

void title_desktop_addons_snapshot(const struct forever_addon_snapshot *);
void title_desktop_addons_error(const char *);
void title_desktop_addons_pause(const char *);
static int pw_writebroker_connect_port(unsigned int, int64_t);

struct forever_addons_request
{
    unsigned generation;
    enum forever_menu_command command;
    unsigned channel;
    enum forever_addon_tab tab;
    char key[ADDONFORGE_WAGO_KEY_BYTES];
};

static struct
{
    pthread_mutex_t lock;
    pthread_mutex_t lifecycle_lock;
    struct forever_addons_request request;
    struct forever_addon_snapshot snapshot;
    struct forever_addon_install channels[FOREVER_ADDON_CHANNELS];
    size_t channel_count;
    char response[ADDONFORGE_RESPONSE_BYTES];
    char token[65];
    char control_token[65];
    unsigned generation;
    enum forever_wago_key_status wago_status; /* global key state survives per-view failures */
    int open, busy, stopping, available;
} forever_addons = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .lifecycle_lock = PTHREAD_MUTEX_INITIALIZER,
};

static int forever_addons_urandom(uint8_t *buffer, size_t bytes)
{
    size_t used = 0;
    int fd, result = 0;
    if ((fd = open("/dev/urandom", O_RDONLY)) < 0)
        return errno ? -errno : -EIO;
    while (used < bytes)
    {
        ssize_t count = read(fd, buffer + used, bytes - used);
        if (count > 0) used += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else
        {
            result = count < 0 && errno ? -errno : -EIO;
            break;
        }
    }
    close(fd);
    return result;
}

static int forever_addons_new_secret(char output[65])
{
    return forever_entropy_hex(output, 65, 32, forever_addons_urandom);
}

static int forever_addons_control_secret_init(void)
{
    int error = forever_addons_new_secret(forever_addons.control_token);
    if (error) return error;
    if (setenv("FOREVER_ADDONS_CONTROL_TOKEN", forever_addons.control_token, 1))
    {
        memset(forever_addons.control_token, 0, sizeof(forever_addons.control_token));
        return -errno;
    }
    return 0;
}

static int forever_addons_control(const char *command)
{
    char response[32] = {0}, wire[256];
    size_t used = 0;
    int fd = -1, error = 0, length;
    int64_t deadline;
    unsigned timeout_ms = !strncmp(command, "STOP", 4) ? 750000 : 10000;
    length = snprintf(wire, sizeof(wire), "AUTH %s %s\n",
                      forever_addons.control_token, command);
    if (length < 0 || (size_t)length >= sizeof(wire)) return -EFBIG;
    for (unsigned attempt = 0; attempt < 100; attempt++)
    {
        deadline = io_now_ms() + 1000;
        fd = pw_writebroker_connect_port(ADDONFORGE_CONTROL_PORT, deadline);
        if (fd >= 0) break;
        error = errno ? errno : ECONNREFUSED;
        usleep(100000);
    }
    if (fd < 0) return -error;
    deadline = io_now_ms() + timeout_ms;
    if (io_transfer(fd, wire, (size_t)length, 1, deadline))
    {
        error = errno ? errno : EIO;
        close(fd);
        return -error;
    }
    while (used < sizeof(response) - 1)
    {
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        int64_t remaining = deadline - io_now_ms();
        int ready;
        ssize_t count;
        if (remaining <= 0) { error = ETIMEDOUT; break; }
        ready = poll(&wait, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { error = ready ? errno : ETIMEDOUT; break; }
        count = recv(fd, response + used, sizeof(response) - 1 - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count < 0) error = errno; break; }
        used += (size_t)count;
        if (memchr(response, '\n', used)) break;
    }
    close(fd);
    response[used] = 0;
    if (error) return -error;
    if (strcmp(response, "OK\n")) return -EIO;
    return 0;
}

static int forever_addons_current(unsigned generation)
{
    int current;
    pthread_mutex_lock(&forever_addons.lock);
    current = forever_addons.open && forever_addons.generation == generation;
    pthread_mutex_unlock(&forever_addons.lock);
    return current;
}

static void forever_addons_publish_error(unsigned generation, int error, const char *detail)
{
    char message[FOREVER_MENU_STATUS_BYTES];
    if (!forever_addons_current(generation)) return;
    if (detail && *detail)
        snprintf(message, sizeof(message), "%s", detail);
    else if (error == -ETIMEDOUT)
        snprintf(message, sizeof(message), "The add-on service did not answer in time.");
    else
        snprintf(message, sizeof(message), "Could not reach the add-on service (error %d).",
                 error < 0 ? -error : error);
    io_log("forever addons error generation=%u result=%d detail=%.160s",
           generation, error, detail ? detail : "");
    snprintf(forever_addons.snapshot.message, sizeof(forever_addons.snapshot.message), "%s", message);
    title_desktop_addons_snapshot(&forever_addons.snapshot);
}

static int forever_addons_command(
    struct addonforge_client *client, const char *command, const char *arguments,
    unsigned timeout_ms, char *plain, size_t plain_size)
{
    int error;
    client->timeout_ms = timeout_ms;
    error = addonforge_http_command(client, command, arguments,
                                    forever_addons.response, sizeof(forever_addons.response));
    if (error) return error;
    return addonforge_response_error(forever_addons.response, plain, plain_size);
}

static int forever_addons_select_channel(
    struct addonforge_client *client, unsigned channel,
    char *plain, size_t plain_size)
{
    char arguments[2 * WOW_PATH_BYTES + 32];
    int error;
    if (channel >= forever_addons.channel_count)
    {
        snprintf(plain, plain_size, "No installed WoW channels found.");
        return -ENOENT;
    }
    error = addonforge_install_arguments(forever_addons.channels[channel].path, arguments, sizeof(arguments));
    return error ? error : forever_addons_command(client, "set_install_path", arguments, 15000,
                                                 plain, plain_size);
}

static int forever_addons_refresh(
    struct addonforge_client *client, enum forever_addon_tab tab,
    char *plain, size_t plain_size)
{
    static const struct
    {
        const char *command, *arguments;
        unsigned timeout_ms;
    } requests[FOREVER_ADDON_TAB_COUNT] = {
        {"check_updates", "{}", 180000},
        {"check_updates", "{}", 180000},
        {"catalog_list", "{\"refresh\":false}", 30000},
        {"bundles", "{}", 30000},
    };
    int error = forever_addons_command(
        client, requests[tab].command, requests[tab].arguments,
        requests[tab].timeout_ms, plain, plain_size);
    if (error) return error;
    return addonforge_parse_snapshot(tab, forever_addons.response,
                                     &forever_addons.snapshot, plain, plain_size);
}

static int forever_addons_safe_key(const char *key)
{
    size_t length;
    if (!key || !(length = strnlen(key, FOREVER_ADDON_KEY_BYTES)) ||
        length == FOREVER_ADDON_KEY_BYTES) return 0;
    for (size_t i = 0; i < length; i++)
        if (!((key[i] >= 'a' && key[i] <= 'z') ||
              (key[i] >= 'A' && key[i] <= 'Z') ||
              (key[i] >= '0' && key[i] <= '9') ||
              strchr("._-:/", key[i]))) return 0;
    return 1;
}

static int forever_addons_mutate(
    struct addonforge_client *client, const struct forever_addons_request *request,
    char *plain, size_t plain_size)
{
    char arguments[FOREVER_ADDON_KEY_BYTES + 64];
    const char *command, *key = request->key;
    int length;
    if (request->command == FOREVER_MENU_COMMAND_ADDONS_UPDATE_ALL)
        return forever_addons_command(client, "update_all", "{}", 600000, plain, plain_size);
    if (request->command == FOREVER_MENU_COMMAND_WAGO_SET ||
        request->command == FOREVER_MENU_COMMAND_WAGO_FILE ||
        request->command == FOREVER_MENU_COMMAND_WAGO_REMOVE)
    {
        enum forever_wago_key_status status = FOREVER_WAGO_NOT_SET;
        int error;
        client->timeout_ms = 15000;
        error = request->command == FOREVER_MENU_COMMAND_WAGO_FILE ?
            addonforge_set_wago_key_file(client, ADDONFORGE_WAGO_KEY_FILE, &status) :
            addonforge_set_wago_key(client,
                request->command == FOREVER_MENU_COMMAND_WAGO_REMOVE ? "" : key, &status);
        if (!error || status == FOREVER_WAGO_REJECTED)
            forever_addons.wago_status = status;
        if (error == -EACCES)
            snprintf(plain, plain_size, "Wago rejected this key.");
        else if (error == -ENOENT && request->command == FOREVER_MENU_COMMAND_WAGO_FILE)
            snprintf(plain, plain_size, "No wago-key.txt found in C:\\Forever\\Addons.");
        else if (error == -EFBIG)
            snprintf(plain, plain_size, "Wago key is too long (maximum 256 characters).");
        else if (error == -EINVAL)
            snprintf(plain, plain_size, "The Wago key is empty or contains invalid characters.");
        else if (error)
            snprintf(plain, plain_size, "Could not save the Wago key.");
        return error;
    }
    if (!forever_addons_safe_key(key)) return -EINVAL;
    if (request->command == FOREVER_MENU_COMMAND_ADDONS_INSTALL_GITHUB)
    {
        int error;
        client->timeout_ms = 600000;
        error = addonforge_install_github(client, key, forever_addons.response,
                                          sizeof(forever_addons.response));
        if (error) return error;
        return addonforge_response_error(forever_addons.response, plain, plain_size);
    }
    switch (request->command)
    {
    case FOREVER_MENU_COMMAND_ADDONS_INSTALL:
        if (strncmp(key, "cat:", 4)) return -EINVAL;
        command = "install_catalog";
        length = snprintf(arguments, sizeof(arguments), "{\"id\":\"%s\"}", key + 4);
        break;
    case FOREVER_MENU_COMMAND_ADDONS_UPDATE:
        command = "update_package";
        length = snprintf(arguments, sizeof(arguments), "{\"key\":\"%s\"}", key);
        break;
    case FOREVER_MENU_COMMAND_ADDONS_REMOVE:
        command = "remove_package";
        length = snprintf(arguments, sizeof(arguments), "{\"key\":\"%s\"}", key);
        break;
    case FOREVER_MENU_COMMAND_ADDONS_INSTALL_PACK:
        if (strncmp(key, "starter:", 8)) return -EINVAL;
        command = "install_bundle";
        length = snprintf(arguments, sizeof(arguments), "{\"id\":\"%s\"}", key + 8);
        break;
    default:
        return 0;
    }
    if (length < 0 || (size_t)length >= sizeof(arguments)) return -EFBIG;
    return forever_addons_command(client, command, arguments, 600000, plain, plain_size);
}

static int forever_addons_channel_visit(const struct wow_channel *channel, void *unused)
{
    struct forever_addon_install *install;
    (void)unused;
    if (forever_addons.channel_count == FOREVER_ADDON_CHANNELS) return -E2BIG;
    install = &forever_addons.channels[forever_addons.channel_count++];
    snprintf(install->path, sizeof(install->path), "%s", channel->windows_cwd);
    snprintf(install->label, sizeof(install->label), "%s", wow_channel_label(channel));
    return 0;
}

static void *forever_addons_worker(void *unused)
{
    struct forever_addons_request request;
    enum forever_wago_key_status wago_status;
    struct addonforge_client client = {
        .port = ADDONFORGE_CORE_PORT, .token = forever_addons.token, .timeout_ms = 1000};
    char plain[FOREVER_MENU_STATUS_BYTES] = {0}, control[160];
    int error = 0;
    (void)unused;
    pthread_mutex_lock(&forever_addons.lock);
    request = forever_addons.request;
    memset(forever_addons.request.key, 0, sizeof(forever_addons.request.key));
    pthread_mutex_unlock(&forever_addons.lock);

    if (request.command == FOREVER_MENU_COMMAND_ADDONS_OPEN)
    {
        forever_addons.channel_count = 0;
        error = wow_channels_scan(PW_RUNTIME_PREFIX, forever_addons_channel_visit, NULL);
        if (error) goto done;
        snprintf(control, sizeof(control), "START %s", forever_addons.token);
        pthread_mutex_lock(&forever_addons.lifecycle_lock);
        if (!forever_addons_current(request.generation))
            error = -ECANCELED;
        else
            error = forever_addons_control(control);
        if (!error && !forever_addons_current(request.generation))
        {
            int stop_error = forever_addons_control("STOP");
            error = stop_error ? stop_error : -ECANCELED;
        }
        pthread_mutex_unlock(&forever_addons.lifecycle_lock);
        if (error) goto done;
        for (unsigned attempt = 0; attempt < 60; attempt++)
        {
            if (!forever_addons_current(request.generation))
            {
                pthread_mutex_lock(&forever_addons.lifecycle_lock);
                error = forever_addons_control("STOP");
                pthread_mutex_unlock(&forever_addons.lifecycle_lock);
                if (!error) error = -ECANCELED;
                goto done;
            }
            if (!(error = addonforge_http_health(&client))) break;
            usleep(250000);
        }
        if (error) goto done;
        io_log("forever addons core ready generation=%u port=%u",
               request.generation, ADDONFORGE_CORE_PORT);
    }
    if ((error = forever_addons_select_channel(
             &client, request.channel, plain, sizeof(plain)))) goto done;
    if (request.command != FOREVER_MENU_COMMAND_ADDONS_OPEN &&
        request.command != FOREVER_MENU_COMMAND_ADDONS_REFRESH &&
        (error = forever_addons_mutate(&client, &request, plain, sizeof(plain))))
        goto done;
    if ((error = forever_addons_refresh(&client, request.tab, plain, sizeof(plain)))) goto done;
    forever_addons.snapshot.channel_count = forever_addons.channel_count;
    memcpy(forever_addons.snapshot.channels, forever_addons.channels,
           forever_addons.channel_count * sizeof(forever_addons.channels[0]));
    client.timeout_ms = 15000;
    if ((error = addonforge_wago_key_status(&client, &wago_status))) goto done;
    forever_addons.wago_status = wago_status;
    addonforge_snapshot_wago_status(&forever_addons.snapshot, wago_status);
    forever_addons.snapshot.wago_saved =
        request.command == FOREVER_MENU_COMMAND_WAGO_REMOVE ? 2 :
        request.command == FOREVER_MENU_COMMAND_WAGO_SET ||
        request.command == FOREVER_MENU_COMMAND_WAGO_FILE;
    if (forever_addons_current(request.generation))
    {
        title_desktop_addons_snapshot(&forever_addons.snapshot);
        io_log("forever addons result generation=%u command=%d channel=%u tab=%d rows=%zu",
               request.generation, request.command, request.channel, request.tab,
               forever_addons.snapshot.count);
    }
done:
    if (error && error != -ECANCELED && forever_addons_current(request.generation))
    {
        memset(&forever_addons.snapshot, 0, sizeof(forever_addons.snapshot));
        forever_addons.snapshot.tab = request.tab;
        forever_addons.snapshot.wago_status = forever_addons.wago_status;
        forever_addons.snapshot.channel_count = forever_addons.channel_count;
        memcpy(forever_addons.snapshot.channels, forever_addons.channels,
               forever_addons.channel_count * sizeof(forever_addons.channels[0]));
    }
    if (error && error != -ECANCELED)
        forever_addons_publish_error(request.generation, error, plain);
    memset(request.key, 0, sizeof(request.key));
    pthread_mutex_lock(&forever_addons.lock);
    forever_addons.busy = 0;
    pthread_mutex_unlock(&forever_addons.lock);
    return NULL;
}

static void *forever_addons_stop_worker(void *unused)
{
    int error;
    (void)unused;
    pthread_mutex_lock(&forever_addons.lifecycle_lock);
    error = forever_addons_control("STOP");
    pthread_mutex_unlock(&forever_addons.lifecycle_lock);
    io_log("forever addons core stop result=%d", error);
    pthread_mutex_lock(&forever_addons.lock);
    memset(forever_addons.token, 0, sizeof(forever_addons.token));
    forever_addons.stopping = 0;
    pthread_mutex_unlock(&forever_addons.lock);
    return NULL;
}

int forever_title_addons_action(
    enum forever_menu_command command, unsigned channel,
    enum forever_addon_tab tab, const char *key)
{
    pthread_t thread;
    int result;
    if (!forever_addons.available)
    {
        if (command == FOREVER_MENU_COMMAND_ADDONS_CLOSE) return 0;
        title_desktop_addons_error("Add-ons are unavailable on this console");
        return -ENOTSUP;
    }
    if (command == FOREVER_MENU_COMMAND_ADDONS_CLOSE)
    {
        pthread_mutex_lock(&forever_addons.lock);
        forever_addons.open = 0;
        forever_addons.generation++;
        if (forever_addons.stopping)
        {
            pthread_mutex_unlock(&forever_addons.lock);
            return 0;
        }
        forever_addons.stopping = 1;
        pthread_mutex_unlock(&forever_addons.lock);
        result = pthread_create(&thread, NULL, forever_addons_stop_worker, NULL);
        if (result)
        {
            pthread_mutex_lock(&forever_addons.lock);
            forever_addons.stopping = 0;
            pthread_mutex_unlock(&forever_addons.lock);
            return -result;
        }
        pthread_detach(thread);
        return 0;
    }
    if (tab < FOREVER_ADDON_INSTALLED || tab >= FOREVER_ADDON_TAB_COUNT ||
        channel >= FOREVER_ADDON_CHANNELS) return -EINVAL;
    if (key && strlen(key) >= sizeof(forever_addons.request.key)) return -EFBIG;
    pthread_mutex_lock(&forever_addons.lock);
    if (forever_addons.busy || forever_addons.stopping)
    {
        pthread_mutex_unlock(&forever_addons.lock);
        return -EBUSY;
    }
    if (command == FOREVER_MENU_COMMAND_ADDONS_OPEN)
    {
        result = forever_addons_new_secret(forever_addons.token);
        if (result)
        {
            pthread_mutex_unlock(&forever_addons.lock);
            return result;
        }
        forever_addons.open = 1;
        forever_addons.generation++;
    }
    else if (!forever_addons.open)
    {
        pthread_mutex_unlock(&forever_addons.lock);
        return -ESRCH;
    }
    forever_addons.request = (struct forever_addons_request){
        .generation = forever_addons.generation,
        .command = command, .channel = channel, .tab = tab};
    if (key) snprintf(forever_addons.request.key, sizeof(forever_addons.request.key), "%s", key);
    forever_addons.busy = 1;
    result = pthread_create(&thread, NULL, forever_addons_worker, NULL);
    if (result)
    {
        forever_addons.busy = 0;
        if (command == FOREVER_MENU_COMMAND_ADDONS_OPEN) forever_addons.open = 0;
    }
    pthread_mutex_unlock(&forever_addons.lock);
    if (result) return -result;
    pthread_detach(thread);
    return 0;
}

/* Admission pressure pauses the optional service, not Battle.net's renderer.
 * CLOSE invalidates in-flight snapshots and starts the existing graceful stop
 * worker. The supervisor bounds its wait and can terminate the child if needed. */
void forever_title_addons_yield(void)
{
    (void)forever_title_addons_action(FOREVER_MENU_COMMAND_ADDONS_CLOSE, 0,
                                     FOREVER_ADDON_INSTALLED, NULL);
    title_desktop_addons_pause(
        "Add-ons paused to keep Battle.net running. Close and reopen Add-ons to continue.");
}

static void *pw_forever_menu_remove(void *argument)
{
    enum forever_menu_command command = (enum forever_menu_command)(uintptr_t)argument;
    int reinstall = command == FOREVER_MENU_COMMAND_REINSTALL;
    int error;

    title_desktop_menu_status("Stopping Battle.net and its update agent...", 0);
    error = pw_supervisor_quiesce_children();
#ifdef FOREVER_MENU_DIAG
    io_log("menu diag action quiesce result=%d", error);
#endif
    if (error)
    {
        char message[FOREVER_MENU_STATUS_BYTES];
        snprintf(message, sizeof(message),
                 "Could not stop every Battle.net process (error %d). Nothing was removed.",
                 -error);
        title_desktop_menu_status(message, 1);
        forever_title_menu_finish_lifetime(FOREVER_MENU_TERMINAL_NOTICE);
        return NULL;
    }
    title_desktop_menu_status(reinstall ? "Removing Battle.net and the cached installer..." :
                                         "Removing Battle.net and its update agent...", 0);
    error = 0;
    for (unsigned i = 0; !error && i < 3u + !!reinstall; i++)
    {
        char path[1100];
        error = forever_menu_path(path, sizeof(path), FOREVER_PREFIX, i, reinstall);
        if (!error && !forever_menu_delete_path_allowed(FOREVER_PREFIX, path, reinstall))
            error = -EACCES;
        if (!error)
            for (unsigned attempt = 0; attempt < 50; attempt++)
            {
                error = pw_writebroker_remove_path(path);
                if (error != -EAGAIN && error != -ECONNREFUSED && error != -ENOENT) break;
                if (attempt == 49) break;
                usleep(100000);
            }
#ifdef FOREVER_MENU_DIAG
        io_log("menu diag action delete index=%u path=%.240s result=%d", i, path, error);
#endif
    }
    io_log("forever: Battle.net %s remove result=%d games=kept",
           reinstall ? "reinstall" : "uninstall", error);
    if (error)
    {
        char message[FOREVER_MENU_STATUS_BYTES];
        snprintf(message, sizeof(message),
                 "Could not remove all Battle.net files (error %d). Your games were not touched.",
                 -error);
        title_desktop_menu_status(message, 1);
        forever_title_menu_finish_lifetime(FOREVER_MENU_TERMINAL_NOTICE);
        return NULL;
    }
    if (!reinstall)
    {
        title_desktop_menu_status(
            "Battle.net removed. Close and start Forever Launcher to reinstall it.", 1);
        forever_title_menu_finish_lifetime(FOREVER_MENU_TERMINAL_NOTICE);
        return NULL;
    }
    atomic_store(&forever_menu_relaunch_pending, 1);
    title_desktop_menu_status("Restarting Forever Launcher to download a fresh installer...", 0);
    forever_title_menu_finish_lifetime(FOREVER_MENU_REINSTALL_READY);
    wine_ps5_title_native_request_exit(0);
    return NULL;
}

int forever_title_menu_action(enum forever_menu_command command, int enabled)
{
    const char *permission_message;
    int result;
    pthread_t thread;
    switch (command)
    {
    case FOREVER_MENU_COMMAND_SET_FRAME_GENERATION:
        result = forever_menu_rewrite_switch(
            FOREVER_DRIVE_C "/wineps5-fg.txt", "fg", enabled);
        permission_message = forever_setting_permission_message(result);
        if (permission_message) forever_say(permission_message);
#ifdef FOREVER_MENU_DIAG
        io_log("menu diag action file=" FOREVER_DRIVE_C
               "/wineps5-fg.txt value=fg=%s result=%d",
               enabled ? "on" : "off", result);
#endif
        return result;
    case FOREVER_MENU_COMMAND_SET_HUD:
        result = forever_menu_rewrite_switch(
            FOREVER_DRIVE_C "/wineps5-hud.txt", "hud", enabled);
        permission_message = forever_setting_permission_message(result);
        if (permission_message) forever_say(permission_message);
#ifdef FOREVER_MENU_DIAG
        io_log("menu diag action file=" FOREVER_DRIVE_C
               "/wineps5-hud.txt value=hud=%s result=%d",
               enabled ? "on" : "off", result);
#endif
        return result;
    case FOREVER_MENU_COMMAND_SET_HDR:
        result = forever_menu_set_wow_hdr(FOREVER_APP_SHARE, FOREVER_PREFIX, enabled);
        permission_message = forever_setting_permission_message(result);
        if (permission_message) forever_say(permission_message);
#ifdef FOREVER_MENU_DIAG
        io_log("menu diag action configs=Config.wtf value=HDROutput=%d result=%d",
               !!enabled, result);
#endif
        if (result < 0) return result;
        result = forever_menu_rewrite_switch(
            FOREVER_DRIVE_C "/wineps5-hdr-output.txt", "hdr", enabled);
        permission_message = forever_setting_permission_message(result);
        if (permission_message) forever_say(permission_message);
#ifdef FOREVER_MENU_DIAG
        io_log("menu diag action file=" FOREVER_DRIVE_C
               "/wineps5-hdr-output.txt value=hdr=%s result=%d",
               enabled ? "on" : "off", result);
#endif
        return result;
    case FOREVER_MENU_COMMAND_REINSTALL:
    case FOREVER_MENU_COMMAND_UNINSTALL:
        if ((result = forever_title_menu_begin_lifetime())) return result;
        result = pthread_create(&thread, NULL, pw_forever_menu_remove,
                                (void *)(uintptr_t)command);
        if (result)
        {
            forever_title_menu_finish_lifetime(FOREVER_MENU_IDLE);
            return -result;
        }
        pthread_detach(thread);
        return 0;
    default:
        return -EINVAL;
    }
}
#endif
#endif
#endif

#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE)
static int pw_writebroker_connect_port(unsigned int port, int64_t deadline)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int fd, result, socket_error = 0, saved_errno, one = 1;
    socklen_t option_size;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    /* Title sockets reject F_SETFL/FIONBIO; Sony's public SO_NBIO option is
     * the measured nonblocking connect path on both supported firmwares. */
    if (setsockopt(fd, SOL_SOCKET, SO_NBIO, &one, sizeof(one))) goto failed;
    if (!connect(fd, (struct sockaddr *)&address, sizeof(address))) return fd;
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) goto failed;
    for (;;)
    {
        struct pollfd wait = {.fd = fd, .events = POLLOUT};
        int64_t now = io_now_ms();

        if (now < 0) goto failed;
        if (now >= deadline)
        {
            errno = ETIMEDOUT;
            goto failed;
        }
        result = poll(&wait, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) goto failed;
        if (!result)
        {
            errno = ETIMEDOUT;
            goto failed;
        }
        option_size = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &option_size)) goto failed;
        if (socket_error)
        {
            errno = socket_error;
            goto failed;
        }
        return fd;
    }

failed:
    saved_errno = errno ? errno : EIO;
    close(fd);
    errno = saved_errno;
    return -1;
}

/* Move size bytes on the nonblocking probe socket before deadline: 0, or -1
 * with errno (EPIPE when the broker closed the connection). */
static int pw_writebroker_transfer(int fd, void *data, size_t size, int sending, int64_t deadline)
{
    unsigned char *bytes = data;
    size_t done = 0;

    while (done < size)
    {
        struct pollfd wait = {.fd = fd, .events = sending ? POLLOUT : POLLIN};
        int64_t now = io_now_ms();
        ssize_t count;
        int result;

        if (now < 0) return -1;
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        result = poll(&wait, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (!result) { errno = ETIMEDOUT; return -1; }
        count = sending ? send(fd, bytes + done, size - done, MSG_NOSIGNAL)
                        : recv(fd, bytes + done, size - done, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0 && errno != ECONNRESET && errno != EPIPE) return -1;
        if (count <= 0) { errno = EPIPE; return -1; }
        done += (size_t)count;
    }
    return 0;
}

static int pw_writebroker_request_quit(void)
{
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_write request = {.magic = WB_QUIT_MAGIC};
    struct wb_reply reply = {0};
    int64_t deadline = io_now_ms() + 2000;
    int fd = -1, token_fd, result = -EIO;

    if ((fd = pw_writebroker_connect_port(PW_WRITEBROKER_PORT, deadline)) < 0)
        return -errno;
    token_fd = open(WB_TOKEN_FILE, O_RDONLY);
    if (token_fd < 0) { result = -errno; goto done; }
    if (read(token_fd, hello.token, sizeof(hello.token)) != (ssize_t)sizeof(hello.token))
    {
        result = -(errno ? errno : EIO);
        close(token_fd);
        goto done;
    }
    close(token_fd);
    if (pw_writebroker_transfer(fd, &hello, sizeof(hello), 1, deadline) ||
        pw_writebroker_transfer(fd, &reply, sizeof(reply), 0, deadline) ||
        reply.magic != WB_MAGIC || reply.result)
    {
        result = reply.magic == WB_MAGIC && reply.result < 0 ? reply.result : -EPROTO;
        goto done;
    }
    result = pw_writebroker_transfer(fd, &request, sizeof(request), 1, deadline) ?
        -EIO : 0;
done:
    if (fd >= 0) close(fd);
    return result;
}

static int pw_writebroker_wait_stopped(void)
{
    int64_t deadline = io_now_ms() + 2000;

    for (;;)
    {
        int64_t now = io_now_ms();
        int fd;
        if (now < 0) return -(errno ? errno : EIO);
        if (now >= deadline) return -ETIMEDOUT;
        fd = pw_writebroker_connect_port(
            PW_WRITEBROKER_PORT,
            pw_writebroker_min_deadline(now + PW_WRITEBROKER_PROBE_MS, deadline));
        if (fd < 0)
        {
            if (errno == ECONNREFUSED) return 0;
            if (errno != ETIMEDOUT && errno != ECONNRESET && errno != EPIPE)
                return -(errno ? errno : EIO);
        }
        else close(fd);
        usleep(PW_WRITEBROKER_POLL_MS * 1000);
    }
}

#if !PW_RUNTIME_GPU && !PW_RUNTIME_HEADLESS
static int pw_writebroker_remove_path(const char *path)
{
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_write capability = {.magic = WB_MENU_CAPS_MAGIC};
    struct wb_write request = {.magic = WB_MENU_REMOVE_MAGIC};
    struct wb_reply reply = {0};
    int64_t deadline = io_now_ms() + 2000;
    size_t length = strlen(path);
    int fd = -1, token_fd, result = -EIO;

    if (!length || length > WB_MAX_PATH) return -ENAMETOOLONG;
    if ((fd = pw_writebroker_connect_port(PW_WRITEBROKER_PORT, deadline)) < 0) return -errno;
    token_fd = open(WB_TOKEN_FILE, O_RDONLY);
    if (token_fd < 0) { result = -errno; goto done; }
    if (read(token_fd, hello.token, sizeof(hello.token)) != (ssize_t)sizeof(hello.token))
    {
        result = -(errno ? errno : EIO);
        close(token_fd);
        goto done;
    }
    close(token_fd);
    if (pw_writebroker_transfer(fd, &hello, sizeof(hello), 1, deadline) ||
        pw_writebroker_transfer(fd, &reply, sizeof(reply), 0, deadline) ||
        reply.magic != WB_MAGIC || reply.result)
    {
        result = reply.magic == WB_MAGIC && reply.result < 0 ? reply.result : -EPROTO;
        goto done;
    }
    if (pw_writebroker_transfer(fd, &capability, sizeof(capability), 1, deadline) ||
        pw_writebroker_transfer(fd, &reply, sizeof(reply), 0, deadline) ||
        reply.magic != WB_MAGIC || reply.result != WB_MENU_CAPS_REMOVE)
    {
        result = -EAGAIN;
        goto done;
    }
    request.path_length = (uint32_t)length;
    if (pw_writebroker_transfer(fd, &request, sizeof(request), 1, deadline) ||
        pw_writebroker_transfer(fd, (void *)path, length, 1, deadline))
    {
        result = -EIO;
        goto done;
    }
    deadline = io_now_ms() + 300000;
    if (pw_writebroker_transfer(fd, &reply, sizeof(reply), 0, deadline) ||
        reply.magic != WB_MAGIC)
        result = -EIO;
    else
        result = reply.result;
done:
    if (fd >= 0) close(fd);
    return result;
}
#endif

/* 1: a broker of this app's protocol version answers. 0: none, or one left
 * running by an older app (it refuses or reports another version), which the
 * broker this app sends replaces. -1 with *error otherwise. */
static int pw_writebroker_probe(void *context, int64_t deadline, int *error)
{
    static int reported;
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_reply reply, capabilities;
    struct wb_write menu_request = {.magic = WB_MENU_CAPS_MAGIC};
    int fd, token_fd, have_token;

    (void)context;
    *error = 0;
    fd = pw_writebroker_connect_port(PW_WRITEBROKER_PORT, deadline);
    if (fd < 0)
    {
        *error = errno ? errno : EIO;
        return *error == ECONNREFUSED ? 0 : -1;
    }
    token_fd = open(WB_TOKEN_FILE, O_RDONLY);
    have_token = token_fd >= 0 && read(token_fd, hello.token, sizeof(hello.token)) == (ssize_t)sizeof(hello.token);
    if (token_fd >= 0) close(token_fd);
    if (have_token && !pw_writebroker_transfer(fd, &hello, sizeof(hello), 1, deadline) &&
        !pw_writebroker_transfer(fd, &reply, sizeof(reply), 0, deadline))
    {
        if (reply.magic == WB_MAGIC && !reply.result &&
            !pw_writebroker_transfer(fd, &menu_request, sizeof(menu_request), 1, deadline) &&
            !pw_writebroker_transfer(fd, &capabilities, sizeof(capabilities), 0, deadline) &&
            capabilities.magic == WB_MAGIC &&
            capabilities.result == WB_MENU_CAPS_REMOVE)
        {
            close(fd);
            return 1;
        }
        close(fd);
        if (!reported++)
            io_log("title write broker lacks menu removal capability; replacing it");
        return 0;
    }
    if (have_token && errno != EPIPE)
    {
        *error = errno ? errno : EIO;
        close(fd);
        return -1;
    }
    close(fd);
    if (!reported++) io_log("title write broker refused this app's hello (older version or new token); replacing it");
    return 0;
}

static int pw_writebroker_send_elf(void *context, int64_t deadline, size_t *sent,
                                   int *error)
{
    unsigned char bytes[16384];
    struct stat status;
    int file = -1, socket_fd = -1, result = PW_WRITEBROKER_SEND_ERROR;
    int saved_errno = 0;

    (void)context;
    *sent = 0;
    if ((file = open(PW_RUNTIME_WRITEBROKER_ELF, O_RDONLY)) < 0)
    {
        saved_errno = errno;
        goto done;
    }
    if (fstat(file, &status))
    {
        saved_errno = errno;
        goto done;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 4 ||
        (uint64_t)status.st_size > PW_WRITEBROKER_ELF_MAX_BYTES)
    {
        saved_errno = (uint64_t)status.st_size > PW_WRITEBROKER_ELF_MAX_BYTES ? EFBIG : EINVAL;
        goto done;
    }
    if ((socket_fd = pw_writebroker_connect_port(PW_WRITEBROKER_ELFLDR_PORT,
                                                  deadline)) < 0)
    {
        saved_errno = errno;
        result = PW_WRITEBROKER_SEND_ELFLDR_UNAVAILABLE;
        goto done;
    }
    while (*sent < (size_t)status.st_size)
    {
        size_t remaining = (size_t)status.st_size - *sent;
        size_t wanted = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        ssize_t count;

        do count = read(file, bytes, wanted);
        while (count < 0 && errno == EINTR);
        if (count <= 0)
        {
            saved_errno = count < 0 ? errno : EIO;
            goto done;
        }
        if (!*sent && ((size_t)count < 4 || memcmp(bytes, "\177ELF", 4)))
        {
            saved_errno = ENOEXEC;
            goto done;
        }
        if (io_transfer(socket_fd, bytes, (size_t)count, 1, deadline))
        {
            saved_errno = errno ? errno : EIO;
            goto done;
        }
        *sent += (size_t)count;
    }
    if (shutdown(socket_fd, SHUT_WR))
    {
        saved_errno = errno;
        goto done;
    }
    result = PW_WRITEBROKER_SEND_OK;

done:
    if (file >= 0 && close(file) && !saved_errno) saved_errno = errno;
    if (socket_fd >= 0 && close(socket_fd) && !saved_errno) saved_errno = errno;
    if (saved_errno && result == PW_WRITEBROKER_SEND_OK)
        result = PW_WRITEBROKER_SEND_ERROR;
    *error = saved_errno;
    return result;
}

static int pw_writebroker_wait_until(void *context, int64_t deadline, int *error)
{
    (void)context;
    for (;;)
    {
        int64_t now = io_now_ms();
        int result;

        if (now < 0)
        {
            *error = errno ? errno : EIO;
            return -1;
        }
        if (now >= deadline)
        {
            *error = 0;
            return 0;
        }
        result = poll(NULL, 0, (int)(deadline - now));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0)
        {
            *error = errno ? errno : EIO;
            return -1;
        }
    }
}

static int64_t pw_writebroker_now(void *context)
{
    (void)context;
    return io_now_ms();
}

static struct pw_writebroker_outcome pw_title_writebroker_autostart(void)
{
    static const struct pw_writebroker_ops ops = {
        .now_ms = pw_writebroker_now,
        .probe = pw_writebroker_probe,
        .send_elf = pw_writebroker_send_elf,
        .wait_until = pw_writebroker_wait_until,
    };
    static int identity_reported;
    struct pw_writebroker_outcome outcome = pw_writebroker_autostart(&ops, NULL);
    if (!identity_reported++)
        io_log("title write broker client identity=%u:%u effective=%u:%u",
               (unsigned)getuid(), (unsigned)getgid(),
               (unsigned)geteuid(), (unsigned)getegid());
    if (outcome.status == PW_WRITEBROKER_PRESENT)
        io_log("title write broker present");
    else
        io_log("title write broker autostart sent=%zu result=%s errno=%d",
               outcome.sent, pw_writebroker_status_name(outcome.status), outcome.error);
    return outcome;
}

static int pw_title_writebroker_restart(void)
{
    struct pw_writebroker_outcome outcome;
    int quit = pw_writebroker_request_quit();

    if (!quit)
    {
        int stopped = pw_writebroker_wait_stopped();
        if (stopped)
        {
            io_log("title write broker restart stop result=%d", stopped);
            return stopped;
        }
    }
    else if (quit != -ENOENT && quit != -ECONNREFUSED && quit != -EPIPE &&
             quit != -ECONNRESET && quit != -EPROTONOSUPPORT && quit != -EPROTO)
    {
        io_log("title write broker restart quit result=%d", quit);
        return quit;
    }
    outcome = pw_title_writebroker_autostart();
    io_log("title write broker restart quit=%d sent=%zu result=%s errno=%d",
           quit, outcome.sent, pw_writebroker_status_name(outcome.status), outcome.error);
    if (outcome.status == PW_WRITEBROKER_PRESENT ||
        outcome.status == PW_WRITEBROKER_STARTED)
        return 0;
    return -(outcome.error ? outcome.error : EIO);
}

#if !PW_RUNTIME_GPU
/* Ask the broker to put WB_ICON_FILE on the home screen. 0 with *answer
 * (copies replaced, or -errno), or -1 with *error when no broker of this
 * version answered (retry later). */
static int pw_writebroker_request_icon(int *answer, int *error)
{
    struct wb_hello hello = {.magic = WB_MAGIC, .version = WB_VERSION};
    struct wb_write request = {.magic = WB_ICON_MAGIC};
    struct wb_reply reply;
    int64_t deadline = io_now_ms() + 5000;
    int fd, token_fd, have_token;

    token_fd = open(WB_TOKEN_FILE, O_RDONLY);
    have_token = token_fd >= 0 && read(token_fd, hello.token, sizeof(hello.token)) == (ssize_t)sizeof(hello.token);
    if (token_fd >= 0) close(token_fd);
    if (!have_token)
    {
        *error = ENOENT;
        return -1;
    }
    if ((fd = pw_writebroker_connect_port(PW_WRITEBROKER_PORT, deadline)) < 0)
    {
        *error = errno ? errno : EIO;
        return -1;
    }
    if (pw_writebroker_transfer(fd, &hello, sizeof(hello), 1, deadline) ||
        pw_writebroker_transfer(fd, &reply, sizeof(reply), 0, deadline) || reply.magic != WB_MAGIC ||
        reply.result || pw_writebroker_transfer(fd, &request, sizeof(request), 1, deadline) ||
        pw_writebroker_transfer(fd, &reply, sizeof(reply), 0, deadline) || reply.magic != WB_MAGIC)
    {
        *error = errno ? errno : EPROTO;
        close(fd);
        return -1;
    }
    close(fd);
    *answer = reply.result;
    return 0;
}

/* The home-screen icon: the installed Battle.net launcher's own, made when
 * that program changes (title-icon.h) and put back by the broker whenever
 * the home screen's copies differ, e.g. after an app update. */
static void *pw_title_icon_thread(void *unused)
{
    int result = title_icon_update(PW_RUNTIME_LAUNCHER_UNIX, WB_ICON_FILE, FOREVER_ICON_SOURCE);
    int answer = 0, error = 0;

    (void)unused;
    io_log("forever: icon from the Battle.net launcher result=%d", result);
    if (result < 0 && !forever_exists(WB_ICON_FILE)) return NULL;
    /* the broker may still be starting (the ELF loader took up to 5.7 s) */
    for (int attempt = 0; attempt < 60; ++attempt)
    {
        if (!pw_writebroker_request_icon(&answer, &error))
        {
            io_log("forever: home-screen icon copies replaced result=%d", answer);
            return NULL;
        }
        usleep(1000000);
    }
    io_log("forever: home-screen icon not installed: no broker errno=%d", error);
    return NULL;
}

static void pw_title_icon_start(void)
{
    struct stat launcher;
    pthread_t thread;

    if (stat(PW_RUNTIME_LAUNCHER_UNIX, &launcher)) return; /* Battle.net is not installed yet */
    if (create_thread(&thread, NULL, pw_title_icon_thread, NULL)) return;
    pthread_detach(thread);
}
#endif
#endif

int main(int argc, char **argv)
{
    errno=0;
    int64_t now=io_now_ms();
    int saved_errno=errno, result;
#if defined(PW_PARENT_ROLE) && defined(PW_RUNTIME_B1) && PW_RUNTIME_GPU
    char **game_args = NULL;
    const char *game_cwd = NULL;
    int game_argc = pw_game_startup_argv(argc, argv, &game_args, &game_cwd);
    if (game_argc < 0)
    {
        io_log("b1 game invalid relaunch argv error=%d argv=REDACTED; returning to maintenance", -game_argc);
        result = sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
        io_log("b1 game fallback LoadExec returned result=%d", result);
        return 1;
    }
    io_log("b1 game relaunch argv accepted argc=%d argv=REDACTED cwd=REDACTED", game_argc - 1);
#endif
    io_log("psmkit-wine startup stage=clock result=%lld errno=%d", (long long)now, saved_errno);
    if (now<0) return 1;
    native_runtime.vulkan_proc = pw_title_profile_proc;
#ifdef PW_PARENT_ROLE
#if !(defined(PW_RUNTIME_B1) && PW_RUNTIME_GPU)
    (void)argc; (void)argv;
#endif
    result = pw_supervisor_init();
    if (result) return 1;
    native_runtime.spawn_process = pw_supervisor_spawn;
#if defined(PW_RUNTIME_B1) && !PW_RUNTIME_GPU
    native_runtime.game_launch = pw_game_launch;
#endif
    native_runtime.signal_child_thread = pw_supervisor_signal;
    native_runtime.terminate_child = pw_supervisor_terminate;
#if PW_RUNTIME_GPU
    native_runtime.vulkan_proc = pw_title_vulkan_proc;
    native_runtime.audio_open = pw_title_audio_open;
    native_runtime.audio_output = pw_title_audio_output;
    native_runtime.audio_close = pw_title_audio_close;
    if (!pw_title_hdr_luminance_init())
        native_runtime.get_hdr_luminance = pw_title_get_hdr_luminance;
    io_log("wine: title audio runtime callbacks role=parent open=1 output=1 close=1");
    /* RADV reports VideoOut registration and present failures on C stderr,
     * which no B1 role routes anywhere; send its lines to klog. */
    if (ps5_klog_capture_stderr("[radv] "))
        io_log("psmkit-wine startup stage=radv_stderr errno=%d", errno);
    else
        setvbuf(stderr, NULL, _IOLBF, 0);
    if ((result = pw_title_input_start()))
    {
        io_log("title input thread failed: %d", result);
        return 1;
    }
#endif
#ifdef PW_RUNTIME_B1
    native_runtime.payload_spawn = pw_payload_spawn;
    native_runtime.open_url = pw_open_url;
#endif
#if defined(PW_RUNTIME_B1) && !PW_RUNTIME_HEADLESS
    native_runtime.desktop_connect = pw_desktop_connect;
    native_runtime.desktop_surface = pw_desktop_surface;
#endif
#else
    /* PsmKit added an argv entry in the qualified P1c-1 run. The trailing
     * generation/nonce remain independent of the untrusted bootstrap. */
    uint64_t generation = 0, nonce = 0;
    for (int i = 0; i < argc; ++i)
        io_log("p1c child argv[%d]=%.64s", i, argv && argv[i] ? argv[i] : "(null)");
    int identity_from_argv = !pw_p1c_argv_identity(argc, argv, &generation, &nonce);
    io_log("p1c child start argc=%d identity=%s fd3_valid=%d", argc,
           identity_from_argv ? "argv" : "bootstrap", pw_socket_valid(3));
    if (!pw_socket_valid(3)) return 1;
    if (io_transfer(3, &child_bootstrap, sizeof(child_bootstrap), 0, now + PW_OVERALL_MS))
    {
        io_log("p1c child bootstrap read failed errno=%d", errno);
        return 1;
    }
    if (!identity_from_argv)
    {
        generation = child_bootstrap.generation;
        nonce = child_bootstrap.nonce;
    }
    result = pw_p1c_bootstrap_validate(&child_bootstrap, generation, nonce);
    io_log("p1c child bootstrap version=%d generation=%llx error=%d", PW_P1C_VERSION,
           (unsigned long long)generation, result);
    struct pw_control validation;
    pw_control_init(&validation, PW_BOOTSTRAP_REPLY, generation, nonce);
    validation.error = result;
    if (io_transfer(3, &validation, sizeof(validation), 1, now + PW_OVERALL_MS) || result)
    {
        io_log("p1c child bootstrap reply errno=%d validation=%d", errno, result);
        return 1;
    }
    native_runtime.server_socket = pw_receive_fd(3, now + PW_OVERALL_MS);
    if (native_runtime.server_socket < 0)
    {
        io_log("p1c child server socket receive failed errno=%d", errno);
        return 1;
    }
    struct pw_control hello;
    pw_control_init(&hello, PW_HELLO, generation, nonce);
    hello.pid = (unsigned int)getpid();
    if (io_transfer(3, &hello, sizeof(hello), 1, now + PW_OVERALL_MS)) return 1;
    io_log("p1c child HELLO pid=%u generation=%llx server_socket=%d", hello.pid,
           (unsigned long long)generation, native_runtime.server_socket);
#ifdef PW_RUNTIME_B1
    spawn_channel = pw_receive_fd(3, now + PW_OVERALL_MS);
    if (spawn_channel < 0)
    {
        io_log("b1 child spawn channel receive failed errno=%d", errno);
        return 1;
    }
    native_runtime.spawn_process = pw_child_spawn;
    native_runtime.payload_spawn = pw_child_payload_spawn;
    native_runtime.open_url = pw_child_open_url;
#if !PW_RUNTIME_GPU
    native_runtime.game_launch = pw_child_game_launch;
#endif
#if !PW_RUNTIME_HEADLESS
    native_runtime.desktop_connect = pw_child_desktop_connect;
    native_runtime.desktop_surface = pw_child_desktop_surface;
#endif
#if PW_RUNTIME_GPU
    io_log("wine: title audio runtime callbacks role=child open=0 output=0 close=0 sink=silent");
#endif
#endif
    /* cwd is data only; Wine applies server startup cwd through its title
     * directory emulation, never a native title chdir(). */
#endif
    /* Never read boot.txt or initialize/enter the user's prefix: the original
     * Wine loader's title-only build consumes this entry's trusted argv/env. */
    const char *prefix=getenv("WINEPREFIX");
    int prefix_matches=!prefix || !strcmp(prefix,PW_RUNTIME_PREFIX);
    io_log("psmkit-wine startup stage=prefix_guard present=%d matches=%d inherited=%.160s",
           prefix!=NULL,prefix_matches,prefix ? prefix : "(unset)");
    if (!prefix_matches) return 1;
#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE)
    {
        struct stat user_root;
        /* An existing stale tree may be unwriteable, so its privileged
         * repair broker must be available before setup first touches it. */
        errno = 0;
        if (!stat(FOREVER_ROOT, &user_root) || errno != ENOENT)
            pw_title_writebroker_autostart();
    }
    if (pw_forever_start())
    {
#if !PW_RUNTIME_GPU
        b1_wait(); /* keep the reason on screen until the user closes the app */
#endif
        return 1;
    }
#if !PW_RUNTIME_GPU
    /* Even direct-game launches verify behind the live maintenance desktop
     * first. A fresh/unfinished prefix must finish wineboot before handoff. */
    if (!forever_prefix_init && pw_maintenance_direct_game()) return 1;
#endif
    /* If this was a genuinely new root, setup made it after the early probe. */
    pw_title_writebroker_autostart();
#if !PW_RUNTIME_GPU
    pw_title_icon_start();
#endif
#endif
#if defined(PW_RUNTIME_B1) && defined(PW_PARENT_ROLE) && !PW_RUNTIME_GPU
    int installer = 0;
    struct stat startup_file;
    installer = !forever_prefix_init && stat(PW_RUNTIME_LAUNCHER_UNIX, &startup_file) != 0;
    if (installer && stat(PW_RUNTIME_INSTALLER_UNIX, &startup_file) && b1_download_installer())
        b1_wait();
#if !PW_RUNTIME_HEADLESS
    if (!forever_prefix_init && !installer)
    {
        result = forever_addons_control_secret_init();
        forever_addons.available = !result;
        io_log("forever addons control entropy result=%d source=urandom", result);
    }
#endif
#endif
    errno=0; result=pw_duplicate_bridge_init(duplicate_channel); saved_errno=errno;
    io_log("psmkit-wine startup stage=dup_bridge result=%d errno=%d send_fd=%d receive_fd=%d",
           result,saved_errno,duplicate_channel[0],duplicate_channel[1]);
    if (result) return 1;
    io_log("psmkit-wine startup stage=elf_init BEGIN recursive_type=%d mutex_bytes=%zu attr_bytes=%zu key_bytes=%zu",
           PTHREAD_MUTEX_RECURSIVE,sizeof(pthread_mutex_t),sizeof(pthread_mutexattr_t),sizeof(pthread_key_t));
    errno=0;
    result=pw_elf_init(&runtime,native_imports,sizeof(native_imports)/sizeof(native_imports[0]),
                       system_modules,sizeof(system_modules)/sizeof(system_modules[0]));
    saved_errno=errno;
    io_log("psmkit-wine startup stage=elf_init result=%d errno=%d detail=%s page_bytes=%zu lock_created=%d key_created=%d",
           result,saved_errno,runtime.error,runtime.page_size,runtime.lock_created,runtime.key_created);
    if (result) return 1;
    char marker[32]; snprintf(marker,sizeof(marker),"%llx",(unsigned long long)(uintptr_t)&native_runtime);
    static const char *const names[]={"WINEPS5_TITLE_RUNTIME","WINEPS5_IN_TITLE","WINEPS5_DIRECT_MEMORY",
                                      "WINEPREFIX","HOME","USER","WINEDEBUG"
#ifdef PW_RUNTIME_B1
        ,"WINEDLLOVERRIDES"
#if PW_RUNTIME_HEADLESS
        ,"WINEPS5_TITLE_HEADLESS"
#endif
        ,"QT_OPENGL","QT_QUICK_BACKEND"
#endif
    };
    const char *values[]={marker,"1","1",PW_RUNTIME_PREFIX,PW_RUNTIME_HOME,"ps5",PW_RUNTIME_DEBUG
#ifdef PW_RUNTIME_B1
#ifdef PW_PARENT_ROLE
        ,PW_RUNTIME_DLL_OVERRIDES
#else
        ,PW_RUNTIME_CHILD_DLL_OVERRIDES
#endif
#if PW_RUNTIME_HEADLESS
        ,"1"
#endif
        ,"software","software"
#endif
    };
    for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        errno=0; result=setenv(names[i],values[i],1); saved_errno=errno;
        io_log("psmkit-wine startup stage=setenv name=%s result=%d errno=%d",names[i],result,saved_errno);
        if (result) return 1;
    }
#ifdef PW_RUNTIME_B1
    if (native_runtime.payload_spawn &&
        setenv("WINEPS5_PAYLOAD_PROGRAMS", PW_RUNTIME_PAYLOAD_PROGRAMS, 1)) return 1;
    /* An empty role value overwrites one inherited across LoadExec. */
    if (setenv("WINEPS5_REFUSED_PROGRAMS", PW_RUNTIME_REFUSED_PROGRAMS, 1)) return 1;
    /* ntdll adds these to launches of the named programs (profile browser_switches) */
    if (*PW_RUNTIME_BROWSER_SWITCHES && setenv("WINEPS5_BROWSER_SWITCHES", PW_RUNTIME_BROWSER_SWITCHES, 1)) return 1;
    if (*PW_RUNTIME_MXCSR_TRACE &&
        setenv("WINEPS5_MXCSR_TRACE", PW_RUNTIME_MXCSR_TRACE, 0)) return 1;
    if (*PW_RUNTIME_WRITE_TRACE &&
        setenv("WINEPS5_WRITE_TRACE", PW_RUNTIME_WRITE_TRACE, 0)) return 1;
    if (*PW_RUNTIME_WRITE_STICKY &&
        setenv("WINEPS5_WRITE_STICKY", PW_RUNTIME_WRITE_STICKY, 0)) return 1;
    if (*PW_RUNTIME_PROFILE &&
        setenv("WINEPS5_PROFILE", PW_RUNTIME_PROFILE, 0)) return 1;
#if PW_RUNTIME_GPU
    /* PE XInput only probes Unix modules admitted by the GPU manifest.
     * Unix-side title/callback checks still gate the native implementation. */
    if (setenv("WINEPS5_TITLE_XINPUT", "1", 1)) return 1;
    /* This marker survives the one-shot loader credentials and makes every
     * game process, including PsmKit children, select the silent-capable
     * title driver on the PE side. */
    errno=0; result=setenv("WINEPS5_TITLE_AUDIO", "1", 1); saved_errno=errno;
    io_log("psmkit-wine startup stage=setenv name=WINEPS5_TITLE_AUDIO value=1 result=%d errno=%d",
           result,saved_errno);
    if (result) return 1;
    /* vkd3d-proton logs info below fixme (its default level) and DXVK at
     * info: a line per capability found at every device creation. Keep
     * vkd3d-proton's errors and DXVK's warnings; a value already in the
     * environment wins. */
    if (setenv("VKD3D_DEBUG", "err", 0) || setenv("DXVK_LOG_LEVEL", "warn", 0)) return 1;
    /* WoWPS5 uses single_queue for its visible WoW login screen. Keep an
     * explicitly supplied configuration, but default the GPU parent to it. */
    if (*PW_RUNTIME_VKD3D_CONFIG &&
        setenv("VKD3D_CONFIG", PW_RUNTIME_VKD3D_CONFIG, 0)) return 1;
    if (setenv("VKD3D_CONFIG", "single_queue", 0)) return 1;
    if (*PW_RUNTIME_VKD3D_QUEUE_PROFILE &&
        setenv("VKD3D_QUEUE_PROFILE", PW_RUNTIME_VKD3D_QUEUE_PROFILE, 0)) return 1;
    if (*PW_RUNTIME_VKD3D_PRESENT_MODE &&
        setenv("VKD3D_SWAPCHAIN_PRESENT_MODE", PW_RUNTIME_VKD3D_PRESENT_MODE, 0)) return 1;
    if (*PW_RUNTIME_VIDEOOUT_STATS &&
        setenv("PS5_VIDEOOUT_STATS", PW_RUNTIME_VIDEOOUT_STATS, 0)) return 1;
    if (*PW_RUNTIME_LOG_ASYNC &&
        setenv("WINEPS5_LOG_ASYNC", PW_RUNTIME_LOG_ASYNC, 0)) return 1;
    int hdr_output = forever_menu_hdr_runtime_enabled(
        FOREVER_DRIVE_C "/wineps5-hdr-output.txt");
    if (!hdr_output)
    {
        if (unsetenv("PROTON_ENABLE_HDR") || unsetenv("DXVK_HDR")) return 1;
    }
    else if (*PW_RUNTIME_PROTON_ENABLE_HDR &&
             setenv("PROTON_ENABLE_HDR", PW_RUNTIME_PROTON_ENABLE_HDR, 0)) return 1;
    if (*PW_RUNTIME_DIRECT_REQUESTS &&
        setenv("WINEPS5_DIRECT_REQUESTS", PW_RUNTIME_DIRECT_REQUESTS, 0)) return 1;
    if (*PW_RUNTIME_HDR_TRACE &&
        setenv("WINEPS5_HDR_TRACE", PW_RUNTIME_HDR_TRACE, 0)) return 1;
    if (*PW_RUNTIME_QUERY_TRACE &&
        setenv("WINEPS5_QUERY_TRACE", PW_RUNTIME_QUERY_TRACE, 0)) return 1;
    if (*PW_RUNTIME_HDR_MODE &&
        setenv("WINEPS5_HDR_MODE", PW_RUNTIME_HDR_MODE, 0)) return 1;
    if (*PW_RUNTIME_SDR_WHITE_NITS &&
        setenv("WINEPS5_SDR_WHITE_NITS", PW_RUNTIME_SDR_WHITE_NITS, 0)) return 1;
    const char *enable_hdr = getenv("PROTON_ENABLE_HDR");
    if (hdr_output && enable_hdr && !strcmp(enable_hdr, "1") &&
        setenv("DXVK_HDR", "1", 1)) return 1;
    const char *dxvk_hdr = getenv("DXVK_HDR");
    fprintf(stderr, "title gpu env: hdr_output=%s PROTON_ENABLE_HDR=%s DXVK_HDR=%s\n",
            hdr_output ? "on" : "off",
            enable_hdr ? enable_hdr : "(unset)", dxvk_hdr ? dxvk_hdr : "(unset)");
    fprintf(stderr, "title gpu diagnostics: present_mode=%s videoout_stats=%s queue_profile=%s "
            "cpu_profile=%s vkd3d_config=%s hdr_mode=%s query_trace=%s\n",
            getenv("VKD3D_SWAPCHAIN_PRESENT_MODE") ? getenv("VKD3D_SWAPCHAIN_PRESENT_MODE") : "(game)",
            getenv("PS5_VIDEOOUT_STATS") ? "enabled" : "disabled",
            getenv("VKD3D_QUEUE_PROFILE") ? "enabled" : "disabled",
            getenv("WINEPS5_PROFILE") && !strcmp(getenv("WINEPS5_PROFILE"), "1") ? "enabled" : "disabled",
            getenv("VKD3D_CONFIG") ? getenv("VKD3D_CONFIG") : "(unset)",
            getenv("WINEPS5_HDR_MODE") ? "configured" : "off",
            getenv("WINEPS5_QUERY_TRACE") ? "enabled" : "disabled");
#else
    /* LoadExec/process environments are not an authority boundary. Clear any
     * inherited game marker explicitly so maintenance retains Wine's default
     * audio list in the parent and every descendant. */
    errno=0; result=setenv("WINEPS5_TITLE_AUDIO", "0", 1); saved_errno=errno;
    io_log("psmkit-wine startup stage=setenv name=WINEPS5_TITLE_AUDIO value=0 result=%d errno=%d",
           result,saved_errno);
    if (result) return 1;
#endif
#endif
    int fds[2];
    errno=0; result=pipe(fds); saved_errno=errno;
    io_log("psmkit-wine startup stage=stdout_pipe result=%d errno=%d",result,saved_errno);
    if (result) return 1;
    output_read=fds[0];
    output_write=fds[1];
    native_runtime.stdout_fd=output_write;
    io_log("psmkit-wine startup stage=stdout_binding version=%u fd=%d contract=WINE_INITIAL_HANDLE reserved_fd1_unchanged=1",
           native_runtime.version,output_write);
    errno=0; result=create_thread(&output_thread,NULL,collect_output,NULL); saved_errno=errno;
    io_log("psmkit-wine startup stage=stdout_pthread_create result=%d errno=%d return_contract=POSIX_ERROR_CODE",
           result,saved_errno);
    if (result) { close(output_read); close(output_write); return 1; }
    output_started=1;
#ifdef PW_PARENT_ROLE
    io_log("p1c parent entering ORIGINAL_WINE_LOADER topology=PARENT_SHARED_INPROCESS_WINESERVER");
#ifdef PW_RUNTIME_B1
#if PW_RUNTIME_GPU
    char **args = game_args;
    int parent_argc = game_argc;
    const char *cwd = game_cwd;
    if (setenv("WINEPS5_TITLE_PARENT_CWD", cwd, 1)) return 1;
    io_log("b1 game parent command=Wow*.exe argc=%d cwd=REDACTED runtime_root=%s headless=%d argv=REDACTED",
           parent_argc - 1, PW_RUNTIME_ROOT, PW_RUNTIME_HEADLESS);
    pw_title_log_begin();
    pw_prefetch_hosts();
    result=wine_public_title_main(parent_argc,args);
#else
    char *args[]=PW_RUNTIME_PARENT_ARGV;
    char *installer_args[]={(char *)PW_RUNTIME_LOADER, "C:\\windows\\system32\\start.exe",
                            "/exec", (char *)PW_RUNTIME_INSTALLER_DOS, NULL};
    char *wineboot_args[]={(char *)PW_RUNTIME_LOADER, "C:\\windows\\system32\\wineboot.exe", "-i", NULL};
    const char *cwd = forever_prefix_init ? FOREVER_PREFIX : installer ? PW_RUNTIME_INSTALLER_CWD : PW_RUNTIME_CWD;
    if (setenv("WINEPS5_TITLE_PARENT_CWD", cwd, 1)) return 1;
    if (forever_prefix_init)
    {
        pthread_t watch;
        /* WINEPS5_PREFIX_INIT makes wineboot write FOREVER_WINEBOOT_DONE as
         * its last step. Bootstrap mode is what Wine gives its own wineboot
         * --init: builtins load from wine-title before system32 has copies. */
        if (setenv("WINEPS5_PREFIX_INIT", "1", 1) || setenv("WINEBOOTSTRAPMODE", "1", 1)) return 1;
        if (create_thread(&watch, NULL, pw_forever_init_watch, NULL)) return 1;
        pthread_detach(watch);
        io_log("b1 maintenance first-start prefix setup command=wineboot -i cwd=%s", cwd);
        pw_title_log_begin();
        result=wine_public_title_main(3, wineboot_args);
    }
    else
    {
        /* WINEPS5_IN_TITLE is a one-shot loader credential and is consumed before
         * the PE environment exists. Publish a maintenance-only explorer gate. */
        if (setenv("WINEPS5_BATTLENET_WATCH","1",1)) return 1;
        if (installer)
            io_log("b1 parent installer command=%s cwd=%s", PW_RUNTIME_INSTALLER_DOS, cwd);
        b1_message(NULL);
        io_log("b1 maintenance parent command=%s argc=%d cwd=%s runtime_root=%s headless=%d", args[1],
               installer ? 4 : PW_RUNTIME_PARENT_ARGC, cwd, PW_RUNTIME_ROOT, PW_RUNTIME_HEADLESS);
        pw_title_log_begin();
        if (!installer) pw_prefetch_hosts();
        result=wine_public_title_main(installer ? 4 : PW_RUNTIME_PARENT_ARGC, installer ? installer_args : args);
    }
#endif
#else
    char *args[]={(char *)PW_RUNTIME_LOADER,"C:\\psmkit-wine-parent.exe",NULL};
    pw_title_log_begin();
    result=wine_public_title_main(2,args);
#endif
#else
    void *ntdll = pw_public_dlopen(PW_RUNTIME_NTDLL, RTLD_NOW);
    if (!ntdll || !(signal_thread = pw_public_dlsym(ntdll, "__wine_ps5_signal_thread")))
        pw_native_exit(1);
    result = create_thread(&control_thread, NULL, control_agent, NULL);
    if (result) pw_native_exit(1);
    control_started = 1;
    char *args[PW_ARGC + 1];
    pw_p1c_argv(&child_bootstrap, args);
    args[0] = (char *)PW_RUNTIME_LOADER;
    io_log("p1c child entering ORIGINAL_WINE_LOADER topology=CHILD_PARENT_WINESERVER");
    server_socket_transferred = 1;
    pw_title_log_begin();
    result=wine_public_title_main((int)child_bootstrap.argc,args);
#endif
    pw_native_exit(result ? result : 1);
}
