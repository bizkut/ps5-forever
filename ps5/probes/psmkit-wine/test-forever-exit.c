/* Exercise production exit admission and shutdown ordering at native boundaries. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../title/forever-menu.h"

#define FOREVER_DRIVE_C "/test/drive_c"
#define FOREVER_APP_SHARE "/test/share"
#define FOREVER_PREFIX "/test/prefix"
#include "forever-exit-types.inc"

static int create_error, creates, detaches, joins, setting_writes;
static int quiesce_error, quiesces, ends, exit_requests;
static void *(*created_worker)(void *);
static void *created_argument;
static char status[FOREVER_MENU_STATUS_BYTES];

static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*worker)(void *), void *argument)
{
    assert(thread && !attr);
    creates++;
    if (create_error) return create_error;
    *thread = pthread_self();
    created_worker = worker;
    created_argument = argument;
    return 0;
}
static int test_pthread_detach(pthread_t thread)
{
    assert(pthread_equal(thread, pthread_self()));
    detaches++;
    return 0;
}
static int test_pthread_join(pthread_t thread, void **result)
{
    (void)thread;
    assert(!result);
    joins++;
    return 0;
}
static int forever_menu_rewrite_switch(const char *path, const char *key, int enabled)
{
    assert(!strcmp(path, FOREVER_DRIVE_C "/wineps5-voice.txt"));
    assert(!strcmp(key, "voice") && !enabled);
    setting_writes++;
    return 0;
}
static const char *forever_setting_permission_message(int result)
{
    assert(!result);
    return NULL;
}
static int forever_menu_set_wow_hdr(const char *share, const char *prefix, int enabled)
{
    (void)share;
    (void)prefix;
    (void)enabled;
    assert(0);
    return -EINVAL;
}
static void forever_say(const char *text) { (void)text; assert(0); }
static void *forever_voice_download_worker(void *argument)
{
    (void)argument;
    assert(0); /* Only admission is exercised, never model I/O. */
    return NULL;
}
static void *pw_forever_menu_remove(void *argument)
{
    (void)argument;
    assert(0); /* A busy removal must never start. */
    return NULL;
}
static void title_desktop_menu_status(const char *text, int terminal)
{
    assert(!terminal && !strcmp(text, "Closing Battle.net..."));
    snprintf(status, sizeof(status), "%s", text);
}
static int pw_supervisor_quiesce_children(void)
{
    assert(forever_menu_operation == FOREVER_MENU_ACTIVE);
    assert(!atomic_load(&forever_menu_exit_pending) && !exit_requests && !ends);
    quiesces++;
    return quiesce_error;
}
static void pw_supervisor_end_children(void)
{
    assert(quiesce_error && quiesces == 1 && !exit_requests);
    assert(!atomic_load(&forever_menu_exit_pending));
    ends++;
}
static void wine_ps5_title_native_request_exit(int result)
{
    assert(!result && quiesces == 1 && ends == !!quiesce_error);
    assert(atomic_load(&forever_menu_exit_pending));
    assert(forever_menu_operation == FOREVER_MENU_EXIT_READY);
    exit_requests++;
}
static void io_log(const char *format, ...) { (void)format; }
#define pthread_create test_pthread_create
#define pthread_detach test_pthread_detach
#define pthread_join test_pthread_join
#include "forever-exit-functions.inc"
#undef pthread_join
#undef pthread_detach
#undef pthread_create

static void fixture(void)
{
    forever_menu_operation = FOREVER_MENU_IDLE;
    atomic_store(&forever_menu_exit_pending, 0);
    assert(!atomic_load(&forever_menu_relaunch_pending));
    forever_voice_download.busy = forever_voice_download.stopping = 0;
    forever_voice_download.joinable = forever_voice_download.canceled = 0;
    create_error = creates = detaches = joins = setting_writes = 0;
    quiesce_error = quiesces = ends = exit_requests = 0;
    created_worker = NULL;
    created_argument = NULL;
    status[0] = 0;
}
static void test_busy(void)
{
    static const enum forever_menu_command commands[] = {
        FOREVER_MENU_COMMAND_EXIT, FOREVER_MENU_COMMAND_REINSTALL,
        FOREVER_MENU_COMMAND_UNINSTALL};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    {
        fixture();
        forever_voice_download.busy = 1;
        assert(forever_title_menu_action(commands[i], 0) == -EBUSY);
        assert(!creates && forever_menu_operation == FOREVER_MENU_IDLE);
        forever_voice_download.busy = 0;
        forever_voice_download.stopping = 1;
        assert(forever_title_menu_action(commands[i], 0) == -EBUSY && !creates);
        forever_voice_download.stopping = 0;
        for (int state = FOREVER_MENU_ACTIVE; state <= FOREVER_MENU_EXIT_READY; state++)
        {
            forever_menu_operation = state;
            assert(forever_title_menu_action(commands[i], 0) == -EBUSY);
            assert(forever_title_voice_enable() == -EBUSY);
            assert(!creates && !setting_writes && forever_menu_operation == state);
        }
    }
}
static void test_exit(int error)
{
    fixture();
    quiesce_error = error;
    assert(!forever_title_menu_action(FOREVER_MENU_COMMAND_EXIT, 0));
    assert(creates == 1 && detaches == 1 && created_worker == pw_forever_menu_exit);
    assert(forever_menu_operation == FOREVER_MENU_ACTIVE && !exit_requests);
    assert(forever_title_menu_action(FOREVER_MENU_COMMAND_EXIT, 0) == -EBUSY);
    assert(forever_title_voice_enable() == -EBUSY && !setting_writes);
    assert(!created_worker(created_argument));
    assert(!strcmp(status, "Closing Battle.net..."));
    assert(exit_requests == 1 && quiesces == 1 && ends == !!error);
    assert(forever_menu_operation == FOREVER_MENU_EXIT_READY);
}
static pthread_barrier_t admission_barrier;
static int exit_result, voice_result;
static void *admit_exit(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&admission_barrier);
    exit_result = forever_title_menu_action(FOREVER_MENU_COMMAND_EXIT, 0);
    return NULL;
}
static void *admit_voice(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&admission_barrier);
    voice_result = forever_title_voice_enable();
    return NULL;
}
static void test_racing_admission(void)
{
    for (unsigned i = 0; i < 100; i++)
    {
        pthread_t exit_thread, voice_thread;
        fixture();
        assert(!pthread_barrier_init(&admission_barrier, NULL, 2));
        assert(!pthread_create(&exit_thread, NULL, admit_exit, NULL));
        assert(!pthread_create(&voice_thread, NULL, admit_voice, NULL));
        assert(!pthread_join(exit_thread, NULL));
        assert(!pthread_join(voice_thread, NULL));
        assert(!pthread_barrier_destroy(&admission_barrier));
        assert((exit_result == 0 && voice_result == -EBUSY &&
                forever_menu_operation == FOREVER_MENU_ACTIVE && !forever_voice_download.busy) ||
               (exit_result == -EBUSY && voice_result == 1 &&
                forever_menu_operation == FOREVER_MENU_IDLE && forever_voice_download.busy));
        assert(creates == 1 && !exit_requests);
    }
}
int main(void)
{
    test_busy();
    test_exit(0);
    test_exit(-ETIMEDOUT);
    fixture();
    create_error = EAGAIN;
    assert(forever_title_menu_action(FOREVER_MENU_COMMAND_EXIT, 0) == -EAGAIN);
    assert(forever_menu_operation == FOREVER_MENU_IDLE && !detaches && !exit_requests);
    create_error = 0;
    assert(forever_title_voice_enable() == 1 && forever_voice_download.busy);
    assert(forever_title_menu_action(FOREVER_MENU_COMMAND_EXIT, 0) == -EBUSY);
    test_racing_admission();
    puts("forever exit: graceful/forced child shutdown, lifetime/voice exclusion and admission races PASS");
    return 0;
}
