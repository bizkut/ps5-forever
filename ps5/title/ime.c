/* The inline on-screen keyboard (ime.h).
 *
 * libSceIme has no SDK stub: ps5/title/stubs/libSceIme_stub.c names its
 * imports, and the module is loaded by path before the first call, from the
 * title's sandbox (/system/... is ENOENT in a title). Its parameter block is
 * PS4's SceImeParam; sceImeUpdate calls the handler with events whose id is
 * at +0: 0 open, 1 text changed (the text buffer already updated), 4 Done,
 * 5 the panel's confirm/close button (firmware-5.50 probe and B1 run 60),
 * 6 Enter, and 7 aborted. While the panel is up it has the controller, so
 * confirm is the way to close it. The keyboard edits its own copy of the
 * text; the program gets the difference: characters removed from the end as
 * Backspaces, new ones as text, and Return after a confirmed close. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ime.h"

int sceKernelLoadStartModule(const char *path, size_t args, const void *argp,
                             uint32_t flags, const void *option, int *result);
const char *sceKernelGetFsSandboxRandomWord(void);

struct ime_param
{
    int32_t user;
    uint32_t type;
    uint64_t languages;
    uint32_t enter_label;
    uint32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_length;
    uint16_t *text;
    float x, y;
    uint32_t halign, valign;
    void *work;
    void *arg;
    void (*handler)(void *, const void *);
    uint8_t reserved[8];
};
_Static_assert(sizeof(struct ime_param) == 0x60, "ime param");

int sceImeParamInit(struct ime_param *param);
int sceImeOpen(const struct ime_param *param, const void *extended);
int sceImeUpdate(void (*handler)(void *, const void *));
int sceImeClose(void);

enum { EVENT_OPEN = 0, EVENT_TEXT = 1, EVENT_DONE = 4, EVENT_CLOSE = 5, EVENT_ENTER = 6, EVENT_ABORT = 7 };

static uint16_t buffer[IME_MAX_TEXT + 1], shown[IME_MAX_TEXT + 1];
static uint8_t work[0x40000] __attribute__((aligned(0x4000)));
/* Not "open": the B1 import audit rejects a native definition of libc's name. */
static int loaded, showing, changed, entered, ended;

int sceKernelDebugOutText(int channel, const char *text);

/* The B1 desktop's stderr does not reach the console log: write directly. */
static void note(const char *what, int value)
{
    char line[96];
    snprintf(line, sizeof(line), "ime: %s %#x\n", what, (unsigned)value);
    sceKernelDebugOutText(0, line);
}

static void on_event(void *arg, const void *event)
{
    (void)arg;
    note("event", (int)*(const uint32_t *)event);
    switch (*(const uint32_t *)event)
    {
    case EVENT_TEXT: changed = 1; break;
    case EVENT_DONE:
    case EVENT_CLOSE:
    case EVENT_ENTER: entered = 1; ended = 1; break;
    case EVENT_ABORT: ended = 1; break;
    }
}

static int load(void)
{
    char path[128];
    const char *word;
    int status = 0, module;

    if (loaded) return 0;
    word = sceKernelGetFsSandboxRandomWord();
    snprintf(path, sizeof(path), "/%s/common/lib/libSceIme.sprx", word ? word : "system");
    module = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &status);
    if (module < 0) return module;
    loaded = 1;
    return 0;
}

static int show_keyboard(int user, int show, int preserve)
{
    struct ime_param param;
    int r;

    if (!show)
    {
        if (showing) sceImeClose();
        showing = 0;
        return 0;
    }
    if (showing) return 0;
    if ((r = load())) return r;
    if ((r = sceImeParamInit(&param))) return r;
    if (!preserve)
    {
        memset(buffer, 0, sizeof(buffer));
        memset(shown, 0, sizeof(shown));
    }
    param.user = user;
    param.max_length = IME_MAX_TEXT;
    param.text = buffer;
    /* Right of the centre, beside Battle.net's centred login window; centre
     * alignment is where the panel was seen (probe run imeinline-4). */
    param.x = 1480.0f;
    param.y = 540.0f;
    param.halign = 1;
    param.valign = 1;
    param.work = work;
    param.handler = on_event;
    r = sceImeOpen(&param, NULL);
    note("open", r);
    if (r) return r;
    showing = 1;
    changed = entered = ended = 0;
    return 0;
}

int ime_show(int user, int show)
{
    return show_keyboard(user, show, 0);
}

int ime_resume(int user)
{
    return show_keyboard(user, 1, 1);
}

int ime_is_open(void)
{
    return showing;
}

int ime_start(int user)
{
    return ime_show(user, !showing);
}

int ime_poll(struct ime_edit *edit)
{
    size_t old = 0, now = 0, same = 0;

    if (!showing) return 0;
    {
        static int last;
        int r = sceImeUpdate(on_event);
        if (r != last) note("update", r);
        last = r;
    }
    memset(edit, 0, sizeof(*edit));
    if (changed)
    {
        changed = 0;
        while (old < IME_MAX_TEXT && shown[old]) old++;
        while (now < IME_MAX_TEXT && buffer[now]) now++;
        while (same < old && same < now && shown[same] == buffer[same]) same++;
        edit->erase = (unsigned)(old - same);
        edit->length = (unsigned)(now - same);
        memcpy(edit->text, buffer + same, edit->length * sizeof(uint16_t));
        memcpy(shown, buffer, sizeof(shown));
    }
    edit->enter = entered;
    entered = 0;
    if (ended)
    {
        sceImeClose();
        showing = ended = 0;
    }
    return edit->erase || edit->length || edit->enter;
}
