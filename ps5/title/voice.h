/* Game-parent microphone and Whisper worker. Called by the input owner;
 * shutdown is called before the parent tears down its runtime. */
#ifndef FOREVER_TITLE_VOICE_H
#define FOREVER_TITLE_VOICE_H
#include "voice-state.h"
#ifdef __cplusplus
extern "C" {
#endif
struct pw_voice_result
{
    unsigned generation;
    int final, failed, muted;
    char text[VOICE_TEXT_BYTES];
};
void pw_voice_capture(int user);
void pw_voice_start(void);
void pw_voice_finish(void);
void pw_voice_discard(void);
int pw_voice_poll(struct pw_voice_result *);
void pw_voice_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
