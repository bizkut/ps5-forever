/* Device-independent Voice-to-Text policy; no audio, platform or Wine calls. */
#ifndef FOREVER_VOICE_STATE_H
#define FOREVER_VOICE_STATE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_HOLD_US 400000u
#define VOICE_MOVE_UNITS 80
#define VOICE_TEXT_UNITS 512
#define VOICE_TEXT_BYTES (VOICE_TEXT_UNITS * 4 + 1)

enum voice_action
{
    VOICE_CAPTURE = 1u, VOICE_DISCARD = 2u, VOICE_START = 4u,
    VOICE_FINISH = 8u, VOICE_CLICK = 16u,
};
enum voice_hold_phase { VOICE_IDLE, VOICE_PASSTHROUGH, VOICE_DEFERRED, VOICE_DRAG, VOICE_RECORDING };
struct voice_hold
{
    enum voice_hold_phase phase;
    uint64_t pressed_us;
    int x, y;
    unsigned touches, touch_id;
    uint64_t max_move_squared;
    int enabled;
};
struct voice_hold_result
{
    unsigned actions;
    int suppress_touch, suppress_motion;
    enum voice_hold_phase ended;
};
/* enabled is sampled only at the physical down edge. Initial two-finger
 * presses are never deferred; contact changes during a press rebase motion. */
struct voice_hold_result voice_hold_update(struct voice_hold *, int down, int connected,
    unsigned touches, unsigned touch_id, int x, int y, uint64_t now_us, int enabled);
/* Maximum radial excursion, rounded up to an integer touch unit for logging. */
unsigned voice_hold_max_move(const struct voice_hold *);
int voice_setting_enabled(const char *text);
unsigned voice_audio_ctx(size_t samples);
unsigned voice_max_tokens(size_t samples);
int voice_pcm_nonzero(const int16_t *samples, size_t count);

/* Pending controls emit an ordered action list. Buttons are ScePad bits. */
enum voice_pending_action { VOICE_SEND, VOICE_CANCEL, VOICE_EDIT, VOICE_ESCAPE,
    VOICE_ENTER, VOICE_PREFIX, VOICE_RETYPE };
struct voice_pending { int active; unsigned prefix, previous, swallowed; };
struct voice_pending_result
{
    unsigned consumed, count;
    enum voice_pending_action actions[4];
};
void voice_pending_begin(struct voice_pending *);
void voice_pending_end(struct voice_pending *);
struct voice_pending_result voice_pending_update(struct voice_pending *, unsigned buttons,
    int keyboard, int new_hold);
const char *voice_prefix(unsigned index);

/* UTF-16 append units and scalar Backspaces; common prefixes never split a
 * surrogate pair. Invalid UTF-8 becomes U+FFFD. CR/LF become spaces. */
struct voice_delta { unsigned erase, length; uint16_t text[VOICE_TEXT_UNITS]; };
unsigned voice_utf16(const char *, uint16_t *, unsigned capacity);
size_t voice_utf8(const uint16_t *, unsigned length, char *, size_t capacity);
void voice_text_delta(const uint16_t *old, unsigned old_length,
    const uint16_t *text, unsigned length, struct voice_delta *);
/* In-place, consecutive repeated word n-grams only; preserves other prose. */
void voice_trim_repetition(char *text);
#ifdef __cplusplus
}
#endif
#endif
