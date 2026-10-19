#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../title/voice-state.h"
#include "../../title/input.h"

static void holds(void)
{
    struct voice_hold state = {0};
    struct voice_hold_result out;
    out = voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 0, 1);
    assert(out.actions == VOICE_CAPTURE && out.suppress_touch && out.suppress_motion);
    /* Slop is radial from the press origin, not accumulated finger travel. */
    out = voice_hold_update(&state, 1, 1, 1, 7, 180, 100, 399000, 1);
    assert(!out.actions && out.suppress_touch && voice_hold_max_move(&state) == 80);
    out = voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 399000, 1);
    assert(out.actions == (VOICE_DISCARD | VOICE_CLICK) && out.ended == VOICE_DEFERRED);
    assert(voice_hold_max_move(&state) == 80);
    voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 500000, 1);
    out = voice_hold_update(&state, 1, 1, 1, 7, 180, 100, 900000, 0);
    assert(out.actions == VOICE_START && state.phase == VOICE_RECORDING && state.enabled);
    /* After RECORDING starts, arbitrary motion/contact changes stay consumed:
     * no transition to drag and no mouse-down action is ever possible. */
    for (unsigned i = 0; i < 4; i++)
    {
        out = voice_hold_update(&state, 1, 1, i % 3, 20 + i, 1800, 1000, 901000 + i * 1000, 0);
        assert(!out.actions && out.suppress_touch && out.suppress_motion && state.phase == VOICE_RECORDING);
    }
    out = voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 906000, 0);
    assert(out.actions == VOICE_FINISH && out.suppress_touch && out.suppress_motion && out.ended == VOICE_RECORDING);
    voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 1000000, 1);
    out = voice_hold_update(&state, 1, 1, 1, 7, 181, 100, 1399000, 1);
    assert(out.actions == VOICE_DISCARD && !out.suppress_touch && !out.suppress_motion);
    assert(voice_hold_max_move(&state) == 81);
    voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 1400000, 1);
    assert(voice_hold_max_move(&state) == 81); /* Returning cannot erase the peak. */
    out = voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 1500000, 1);
    assert(!out.actions && out.ended == VOICE_DRAG);
    out = voice_hold_update(&state, 1, 1, 2, 7, 100, 100, 1600000, 1);
    assert(!out.actions && !out.suppress_touch);
    out = voice_hold_update(&state, 1, 1, 1, 8, 1500, 600, 2000000, 1);
    assert(!out.actions && state.phase == VOICE_PASSTHROUGH);
    voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 2000001, 1);
    for (unsigned i = 0; i < 10; i++)
    {
        out = voice_hold_update(&state, i < 9, 1, i % 3, i, i * 100, i * 20, 3000000 + i * 400000, 0);
        assert(!out.actions && !out.suppress_motion && !out.suppress_touch);
    }
    voice_hold_update(&state, 1, 1, 1, 7, 10, 10, 8000000, 1);
    out = voice_hold_update(&state, 0, 0, 0, 0, 0, 0, 8000100, 1);
    assert(out.actions == VOICE_DISCARD && !(out.actions & VOICE_CLICK));
    voice_hold_update(&state, 1, 1, 1, 7, 10, 10, 9000000, 1);
    out = voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 9400000, 1);
    assert(out.actions == (VOICE_START | VOICE_FINISH));
    voice_hold_update(&state, 1, 1, 1, 7, 10, 10, 10000000, 1);
    out = voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 10400000, 1);
    assert(out.actions == VOICE_START); /* Movement must win only before 400 ms. */
    voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 10401000, 1);
    voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 11000000, 1);
    for (unsigned i = 0; i < 8; i++)
    {
        out = voice_hold_update(&state, 1, 1, 1, 7, i & 1 ? 50 : 150, 100, 11010000 + i * 40000, 1);
        assert(!out.actions && out.suppress_touch);
    }
    out = voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 11400000, 1);
    assert(out.actions == VOICE_START && voice_hold_max_move(&state) == 50);
    voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 11401000, 1);
    /* A contact ID or count change establishes a new origin, not a drag. */
    voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 12000000, 1);
    out = voice_hold_update(&state, 1, 1, 1, 8, 1500, 800, 12100000, 1);
    assert(!out.actions && out.suppress_touch && !voice_hold_max_move(&state));
    out = voice_hold_update(&state, 1, 1, 1, 8, 1580, 800, 12200000, 1);
    assert(!out.actions && voice_hold_max_move(&state) == 80);
    out = voice_hold_update(&state, 1, 1, 2, 8, 100, 100, 12250000, 1);
    assert(!out.actions && out.suppress_touch && state.x == 100 && state.y == 100);
    out = voice_hold_update(&state, 1, 1, 0, 0, 0, 0, 12300000, 1);
    assert(!out.actions && out.suppress_touch);
    out = voice_hold_update(&state, 1, 1, 1, 9, 1800, 900, 12350000, 1);
    assert(!out.actions && out.suppress_touch);
    out = voice_hold_update(&state, 1, 1, 1, 9, 1800, 900, 12400000, 1);
    assert(out.actions == VOICE_START && voice_hold_max_move(&state) == 80);
    voice_hold_update(&state, 0, 1, 0, 0, 0, 0, 12401000, 1);
    /* A diagonal exactly on the circle is accepted; beyond it is movement. */
    voice_hold_update(&state, 1, 1, 1, 7, 100, 100, 13000000, 1);
    out = voice_hold_update(&state, 1, 1, 1, 7, 164, 148, 13100000, 1);
    assert(!out.actions && voice_hold_max_move(&state) == 80);
    out = voice_hold_update(&state, 1, 1, 1, 7, 165, 148, 13200000, 1);
    assert(out.actions == VOICE_DISCARD && state.phase == VOICE_DRAG && voice_hold_max_move(&state) == 81);
}
static void pending(void)
{
    struct voice_pending state = {0};
    const unsigned buttons[] = {PAD_CROSS, PAD_CIRCLE, PAD_SQUARE};
    const enum voice_pending_action expected[] = {VOICE_SEND, VOICE_CANCEL, VOICE_EDIT};
    for (unsigned i = 0; i < 3; i++)
    {
        voice_pending_begin(&state);
        struct voice_pending_result out = voice_pending_update(&state, buttons[i] | PAD_TRIANGLE, 0, 0);
        assert(out.consumed == buttons[i] && out.count == 1 && out.actions[0] == expected[i]);
        assert(!state.active);
        out = voice_pending_update(&state, buttons[i], 0, 0);
        assert(out.consumed == buttons[i] && !out.count);
        voice_pending_update(&state, 0, 0, 0);
    }
    voice_pending_begin(&state);
    for (unsigned i = 1; i <= 7; i++)
    {
        struct voice_pending_result out = voice_pending_update(&state, PAD_R1 | PAD_UP, 0, 0);
        assert(out.consumed == PAD_R1 && out.count == 4 && state.active && state.prefix == i % 7);
        assert(out.actions[0] == VOICE_ESCAPE && out.actions[1] == VOICE_ENTER &&
            out.actions[2] == VOICE_PREFIX && out.actions[3] == VOICE_RETYPE);
        voice_pending_update(&state, 0, 0, 0);
    }
    voice_pending_update(&state, PAD_L1, 0, 0);
    assert(state.prefix == 6 && !strcmp(voice_prefix(state.prefix), "/r"));
    voice_pending_update(&state, 0, 1, 0);
    assert(!state.active);
    voice_pending_begin(&state);
    assert(!voice_pending_update(&state, PAD_CROSS, 0, 1).count && !state.active);
}
static void text(void)
{
    uint16_t old[VOICE_TEXT_UNITS], next[VOICE_TEXT_UNITS];
    struct voice_delta delta;
    unsigned a = voice_utf16("Hi \xf0\x9f\x98\x80 world", old, VOICE_TEXT_UNITS);
    unsigned b = voice_utf16("Hi \xf0\x9f\x98\x83!", next, VOICE_TEXT_UNITS);
    voice_text_delta(old, a, next, b, &delta);
    assert(delta.erase == 7 && delta.length == 3 && delta.text[0] == 0xd83d && delta.text[1] == 0xde03);
    char back[VOICE_TEXT_BYTES];
    voice_utf8(next, b, back, sizeof(back));
    assert(!strcmp(back, "Hi \xf0\x9f\x98\x83!"));
    a = voice_utf16("caf\xc3\xa9\n", old, VOICE_TEXT_UNITS);
    assert(a == 5 && old[3] == 0xe9 && old[4] == ' ');
    assert(!voice_utf16("\xf0\x9f\x98\x80", old, 1));
    assert(voice_utf16("\xc0\x80", old, 2) == 2 && old[0] == 0xfffd && old[1] == 0xfffd);
    voice_text_delta(next, b, next, b, &delta);
    assert(!delta.erase && !delta.length);
    const char *input[] = {" hello hello hello", "go now go now go now!", "one two three one two three one two three",
        "there therefore", "hello world", "a a a a b b b", "\xe4\xbd\xa0 \xe4\xbd\xa0"};
    const char *output[] = {" hello", "go now go now!", "one two three", "there therefore", "hello world", "a b", "\xe4\xbd\xa0"};
    for (unsigned i = 0; i < sizeof(input) / sizeof(input[0]); i++)
    {
        strcpy(back, input[i]); voice_trim_repetition(back); assert(!strcmp(back, output[i]));
    }
}
int main(void)
{
    holds(); pending(); text();
    assert(!voice_setting_enabled(NULL) && !voice_setting_enabled("") && !voice_setting_enabled("voice=off"));
    assert(voice_setting_enabled("other=on\nvoice=on\t") && !voice_setting_enabled("voice=one"));
    assert(!voice_setting_enabled("voice=on voice=off"));
    assert(!voice_setting_enabled("voice=on voice=on"));
    assert(voice_audio_ctx(1) == 64 && voice_audio_ctx(20480) == 64 && voice_audio_ctx(20481) == 128);
    assert(voice_audio_ctx(480000) == 1500 && voice_audio_ctx((size_t)-1) == 1500);
    assert(voice_audio_ctx(1472u * 320) == 1472 && voice_audio_ctx(1472u * 320 + 1) == 1500);
    assert(voice_max_tokens(1) == 16 && voice_max_tokens(16000) == 16 && voice_max_tokens(16001) == 22);
    assert(voice_max_tokens(576000) == 224 && voice_max_tokens((size_t)-1) == 224);
    int16_t samples[256] = {0};
    assert(!voice_pcm_nonzero(samples, 256)); samples[255] = -1; assert(voice_pcm_nonzero(samples, 256));
    puts("title voice policy: ok");
    return 0;
}
