#ifndef PSMKIT_WINE_RUNTIME_PARENT_H
#define PSMKIT_WINE_RUNTIME_PARENT_H
#include <wine/ps5_title.h>
int pw_supervisor_init(void);
int pw_desktop_connect(void);
int pw_desktop_surface(const char *, uint64_t, int);
int pw_payload_spawn(const char *, const char *);
int pw_open_url(const char *);
void pw_supervisor_shutdown(void);
int pw_supervisor_spawn(const struct wine_ps5_title_spawn *);
int pw_supervisor_signal(int, unsigned int, void *, int);
int pw_supervisor_terminate(int);
void pw_supervisor_end_children(void);
/* Refuse new children, end all supervised Battle.net/Agent processes and wait
 * until their control channels close. The desktop parent remains alive. */
int pw_supervisor_quiesce_children(void);
int pw_parent_stage_game_launch(char *const *, const char *, const char *);
void pw_parent_request_game_launch(void);
#endif
