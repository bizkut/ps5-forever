/* The system's inline on-screen keyboard (libSceIme), as the console's web
 * browser uses it: each character typed or deleted on the panel reaches the
 * program at once, instead of a dialog's box committed at the end. Measured on
 * 5.50: ps5/probes/imeinline (evidence/2026-10-08-imeinline.txt). */
#ifndef PS5_TITLE_IME_H
#define PS5_TITLE_IME_H

#include <stdint.h>

#define IME_MAX_TEXT 512

/* What changed since the last ime_poll: delete `erase` characters before the
 * caret, then type the `length` characters of text; then Enter if enter. */
struct ime_edit
{
    unsigned erase;
    unsigned length;
    uint16_t text[IME_MAX_TEXT];
    int enter;
};

/* Opens the keyboard for the user (show), or closes it; 0, or the failing
 * call's result. Opening an open keyboard keeps its text. */
int ime_show(int user, int show);
/* Opens the keyboard, or closes it when it is open (the pad's L3+R3). */
int ime_start(int user);
/* GPU shortcut: open again with the last inline text for corrections. */
int ime_resume(int user);
int ime_is_open(void);
/* Each frame, on the thread that called ime_start: runs the keyboard and
 * fills edit; nonzero when there is something to send. */
int ime_poll(struct ime_edit *edit);

#endif
