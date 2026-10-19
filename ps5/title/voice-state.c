#include "voice-state.h"
#include "input.h"
#include "../probes/psmkit-wine/forever-menu-settings.h"
#include <string.h>

struct voice_hold_result voice_hold_update(struct voice_hold *hold, int down, int connected,
    unsigned touches, unsigned touch_id, int x, int y, uint64_t now, int enabled)
{
    struct voice_hold_result out = {0};
    if (!connected) down = 0;
    if (hold->phase == VOICE_IDLE && down)
    {
        hold->phase = enabled && touches < 2 ? VOICE_DEFERRED : VOICE_PASSTHROUGH;
        hold->pressed_us = now;
        hold->x = x; hold->y = y;
        hold->touches = touches;
        hold->touch_id = touch_id;
        hold->max_move_squared = 0;
        hold->enabled = !!enabled;
        if (hold->phase == VOICE_DEFERRED) out.actions |= VOICE_CAPTURE;
    }
    if (down)
    {
        if (touches != hold->touches || touch_id != hold->touch_id)
        {
            /* Contact replacement or a transient count change is not motion.
             * Keep the earlier maximum, but start this contact at its origin. */
            hold->x = x; hold->y = y;
            hold->touches = touches;
            hold->touch_id = touch_id;
        }
        else if (touches)
        {
            int64_t dx = (int64_t)x - hold->x, dy = (int64_t)y - hold->y;
            uint64_t squared = (uint64_t)(dx * dx + dy * dy);
            if (squared > hold->max_move_squared) hold->max_move_squared = squared;
        }
    }
    if (hold->phase == VOICE_DEFERRED)
    {
        if (down && now - hold->pressed_us < VOICE_HOLD_US &&
            hold->max_move_squared > VOICE_MOVE_UNITS * VOICE_MOVE_UNITS)
        {
            hold->phase = VOICE_DRAG;
            out.actions |= VOICE_DISCARD;
        }
        else if (connected && now - hold->pressed_us >= VOICE_HOLD_US)
        {
            hold->phase = VOICE_RECORDING;
            out.actions |= VOICE_START;
        }
    }
    out.suppress_touch = hold->phase == VOICE_DEFERRED || hold->phase == VOICE_RECORDING;
    out.suppress_motion = out.suppress_touch;
    if (!down && hold->phase != VOICE_IDLE)
    {
        out.ended = hold->phase;
        if (hold->phase == VOICE_DEFERRED)
        {
            out.actions |= VOICE_DISCARD;
            if (connected) out.actions |= VOICE_CLICK;
        }
        if (hold->phase == VOICE_RECORDING)
        {
            out.actions |= connected ? VOICE_FINISH : VOICE_DISCARD;
        }
        hold->phase = VOICE_IDLE;
    }
    return out;
}

unsigned voice_hold_max_move(const struct voice_hold *hold)
{
    uint64_t remainder = hold->max_move_squared, root = 0, bit = UINT64_C(1) << 62;
    while (bit > remainder) bit >>= 2;
    while (bit)
    {
        if (remainder >= root + bit) { remainder -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return (unsigned)(root + !!remainder);
}

int voice_setting_enabled(const char *text)
{
    size_t begin = 0, end = 0;
    if (!text || forever_menu_switch_span(text, strlen(text), "voice", &begin, &end) != 1) return 0;
    return end - begin == 2 && !memcmp(text + begin, "on", 2);
}

unsigned voice_audio_ctx(size_t samples)
{
    size_t frames = samples / 320 + !!(samples % 320);
    return frames > 1472 ? 1500 : (unsigned)((frames + 63) / 64 * 64);
}
unsigned voice_max_tokens(size_t samples)
{
    size_t seconds = samples / 16000 + !!(samples % 16000);
    return seconds >= 36 ? 224 : (unsigned)(10 + 6 * seconds);
}
int voice_pcm_nonzero(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) if (samples[i]) return 1;
    return 0;
}

void voice_pending_begin(struct voice_pending *state) { state->active = 1; state->prefix = 0; }
void voice_pending_end(struct voice_pending *state) { state->active = 0; }
const char *voice_prefix(unsigned index)
{
    static const char *const prefixes[] = {"", "/s", "/p", "/i", "/g", "/raid", "/r"};
    return prefixes[index % 7];
}
struct voice_pending_result voice_pending_update(struct voice_pending *state, unsigned buttons,
    int keyboard, int new_hold)
{
    struct voice_pending_result out = {0};
    unsigned pressed = buttons & ~state->previous;
    state->previous = buttons;
    state->swallowed &= buttons;
    out.consumed = state->swallowed;
    if (keyboard || new_hold) state->active = 0;
    if (!state->active) return out;
    unsigned bit = pressed & (PAD_CROSS | PAD_CIRCLE | PAD_SQUARE | PAD_L1 | PAD_R1);
    if (!bit) return out;
    /* Swallow every simultaneous control until release; one ordered action. */
    out.consumed |= bit;
    state->swallowed |= bit;
    if (bit & PAD_CROSS) { out.actions[out.count++] = VOICE_SEND; state->active = 0; }
    else if (bit & PAD_CIRCLE) { out.actions[out.count++] = VOICE_CANCEL; state->active = 0; }
    else if (bit & PAD_SQUARE) { out.actions[out.count++] = VOICE_EDIT; state->active = 0; }
    else
    {
        state->prefix = (state->prefix + ((bit & PAD_R1) ? 1 : 6)) % 7;
        out.actions[out.count++] = VOICE_ESCAPE;
        out.actions[out.count++] = VOICE_ENTER;
        out.actions[out.count++] = VOICE_PREFIX;
        out.actions[out.count++] = VOICE_RETYPE;
    }
    return out;
}

unsigned voice_utf16(const char *input, uint16_t *out, unsigned capacity)
{
    const unsigned char *s = (const unsigned char *)input;
    unsigned count = 0;
    size_t remaining = strlen(input);
    while (remaining && count < capacity)
    {
        uint32_t cp = 0xfffd;
        unsigned n = 1;
        if (*s < 0x80) cp = *s;
        else
        {
            unsigned want = *s >= 0xc2 && *s <= 0xdf ? 2 :
                *s >= 0xe0 && *s <= 0xef ? 3 : *s >= 0xf0 && *s <= 0xf4 ? 4 : 0;
            if (want && remaining >= want)
            {
                uint32_t value = *s & (0x7f >> want);
                unsigned i;
                for (i = 1; i < want && (s[i] & 0xc0) == 0x80; i++) value = value * 64 + (s[i] & 63);
                if (i == want && value >= (want == 2 ? 0x80u : want == 3 ? 0x800u : 0x10000u) &&
                    value <= 0x10ffff && (value < 0xd800 || value > 0xdfff)) { cp = value; n = want; }
            }
        }
        if (cp == '\r' || cp == '\n') cp = ' ';
        if (cp >= 0x10000)
        {
            if (count + 2 > capacity) break;
            cp -= 0x10000;
            out[count++] = (uint16_t)(0xd800 + (cp >> 10));
            out[count++] = (uint16_t)(0xdc00 + (cp & 1023));
        }
        else out[count++] = (uint16_t)cp;
        s += n; remaining -= n;
    }
    return count;
}
size_t voice_utf8(const uint16_t *text, unsigned length, char *out, size_t capacity)
{
    size_t used = 0;
    if (!capacity) return 0;
    for (unsigned i = 0; i < length; i++)
    {
        uint32_t cp = text[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < length && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
            cp = 0x10000 + ((cp - 0xd800) << 10) + text[++i] - 0xdc00;
        else if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
        unsigned bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (used + bytes >= capacity) break;
        for (unsigned n = bytes; n > 1; n--) { out[used + n - 1] = (char)(0x80 | (cp & 63)); cp >>= 6; }
        out[used] = (char)(bytes == 1 ? cp : (0xf0 << (4 - bytes)) | cp);
        used += bytes;
    }
    out[used] = 0;
    return used;
}
void voice_text_delta(const uint16_t *old, unsigned old_length, const uint16_t *text,
    unsigned length, struct voice_delta *delta)
{
    unsigned same = 0;
    while (same < old_length && same < length && old[same] == text[same]) same++;
    if (same && old[same - 1] >= 0xd800 && old[same - 1] <= 0xdbff) same--;
    delta->erase = 0;
    for (unsigned i = same; i < old_length; i++)
    {
        delta->erase++;
        if (old[i] >= 0xd800 && old[i] <= 0xdbff && i + 1 < old_length &&
            old[i + 1] >= 0xdc00 && old[i + 1] <= 0xdfff) i++;
    }
    delta->length = length - same;
    memcpy(delta->text, text + same, delta->length * sizeof(*text));
}

static int space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
void voice_trim_repetition(char *text)
{
    /* Compare word spans, not substrings: "there" must not eat "therefore".
     * Longest repeat first avoids leaving half of a repeated phrase behind. */
    size_t starts[VOICE_TEXT_BYTES / 2], ends[VOICE_TEXT_BYTES / 2], words, pos;
rescan:
    words = pos = 0;
    while (text[pos] && words < sizeof(starts) / sizeof(starts[0]))
    {
        while (space((unsigned char)text[pos])) pos++;
        if (!text[pos]) break;
        starts[words] = pos;
        while (text[pos] && !space((unsigned char)text[pos])) pos++;
        ends[words++] = pos;
    }
    for (size_t i = 0; i < words; i++)
    {
        size_t max = (words - i) / 2;
        for (size_t n = max; n; n--)
        {
            int same = 1;
            for (size_t j = 0; j < n; j++)
                if (ends[i + j] - starts[i + j] != ends[i + n + j] - starts[i + n + j] ||
                    memcmp(text + starts[i + j], text + starts[i + n + j], ends[i + j] - starts[i + j])) { same = 0; break; }
            if (!same) continue;
            size_t from = i + 2 * n < words ? starts[i + 2 * n] : strlen(text);
            size_t to = starts[i + n];
            memmove(text + to, text + from, strlen(text + from) + 1);
            goto rescan;
        }
    }
    size_t length = strlen(text);
    while (length && space((unsigned char)text[length - 1])) text[--length] = 0;
}
