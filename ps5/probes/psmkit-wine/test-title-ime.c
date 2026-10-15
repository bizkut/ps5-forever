/* Exercise firmware-5.50 inline-IME confirm and cancel behavior on the host. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../title/ime.c"

static uint32_t pending_events[4];
static unsigned pending_count;
static int opens, closes, loads;

int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                             uint32_t flags, const void *option, int *result)
{
    assert(strstr(path, "/sandbox/common/lib/libSceIme.sprx"));
    assert(!args && !argp && !flags && !option && result);
    *result = 0;
    loads++;
    return 9;
}

const char *sceKernelGetFsSandboxRandomWord(void) { return "sandbox"; }
int sceKernelDebugOutText(int channel, const char *text)
{
    assert(!channel && text);
    return 0;
}

int sceImeParamInit(struct ime_param *param)
{
    memset(param, 0, sizeof(*param));
    return 0;
}

int sceImeOpen(const struct ime_param *param, const void *extended)
{
    assert(param && !extended);
    assert(param->user == 77 && param->text == buffer);
    assert(param->max_length == IME_MAX_TEXT && param->handler == on_event);
    opens++;
    return 0;
}

int sceImeUpdate(void (*handler)(void *, const void *))
{
    assert(handler == on_event);
    for (unsigned i = 0; i < pending_count; i++) handler(NULL, &pending_events[i]);
    pending_count = 0;
    return 0;
}

int sceImeClose(void)
{
    closes++;
    return 0;
}

static void schedule(uint32_t first, uint32_t second)
{
    pending_count = 0;
    if (first != UINT32_MAX) pending_events[pending_count++] = first;
    if (second != UINT32_MAX) pending_events[pending_count++] = second;
}

int main(void)
{
    struct ime_edit edit;

    assert(!ime_show(77, 1) && ime_is_open());
    assert(loads == 1 && opens == 1);
    buffer[0] = 'h';
    buffer[1] = 'i';
    schedule(EVENT_TEXT, EVENT_CLOSE);
    assert(ime_poll(&edit));
    assert(!edit.erase && edit.length == 2 && edit.text[0] == 'h' && edit.text[1] == 'i');
    assert(edit.enter && closes == 1 && !ime_is_open());

    assert(!ime_show(77, 1) && ime_is_open());
    buffer[0] = 'x';
    schedule(EVENT_TEXT, EVENT_ABORT);
    assert(ime_poll(&edit));
    assert(edit.length == 1 && edit.text[0] == 'x');
    assert(!edit.enter && closes == 2 && !ime_is_open());

    assert(!ime_show(77, 1) && ime_is_open());
    schedule(EVENT_DONE, UINT32_MAX);
    assert(ime_poll(&edit) && edit.enter);
    assert(closes == 3 && !ime_is_open());

    assert(!ime_show(77, 1) && ime_is_open());
    schedule(EVENT_ENTER, UINT32_MAX);
    assert(ime_poll(&edit) && edit.enter);
    assert(closes == 4 && !ime_is_open());
    assert(loads == 1 && opens == 4);

    /* Native lengths count UTF-16 units, not UTF-8 bytes or code points.
     * Preserve non-ASCII text and the two units of a supplementary character. */
    assert(!ime_show(77, 1) && ime_is_open());
    buffer[0] = 0x00e9;
    buffer[1] = 0xd83d;
    buffer[2] = 0xde00;
    schedule(EVENT_TEXT, UINT32_MAX);
    assert(ime_poll(&edit));
    assert(!edit.erase && edit.length == 3 && !edit.enter);
    assert(edit.text[0] == 0x00e9 && edit.text[1] == 0xd83d && edit.text[2] == 0xde00);
    buffer[1] = buffer[2] = 0;
    schedule(EVENT_TEXT, UINT32_MAX);
    assert(ime_poll(&edit) && edit.erase == 2 && !edit.length && !edit.enter);
    schedule(EVENT_CLOSE, UINT32_MAX);
    assert(ime_poll(&edit) && edit.enter && closes == 5 && !ime_is_open());

    puts("title IME confirm/cancel mapping: PASS");
    return 0;
}
