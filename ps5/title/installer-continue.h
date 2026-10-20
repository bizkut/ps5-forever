#ifndef FOREVER_INSTALLER_CONTINUE_H
#define FOREVER_INSTALLER_CONTINUE_H

#include <stdint.h>

#define FOREVER_INSTALLER_CONTINUE_WINDOW_DELAY_US 8000000u
#define FOREVER_INSTALLER_CONTINUE_AGENT_DELAY_US 12000000u

enum forever_installer_continue_stage
{
    FOREVER_INSTALLER_CONTINUE_OFF,
    FOREVER_INSTALLER_CONTINUE_PROCESS,
    FOREVER_INSTALLER_CONTINUE_WINDOW,
    FOREVER_INSTALLER_CONTINUE_SETTLE,
    FOREVER_INSTALLER_CONTINUE_DONE,
};

enum forever_installer_continue_result
{
    FOREVER_INSTALLER_CONTINUE_NONE,
    FOREVER_INSTALLER_CONTINUE_SEND,
    FOREVER_INSTALLER_CONTINUE_SKIP_USER_INPUT,
    FOREVER_INSTALLER_CONTINUE_SKIP_FOCUS_LOST,
    FOREVER_INSTALLER_CONTINUE_SKIP_WINDOW_GONE,
};

struct forever_installer_continue
{
    enum forever_installer_continue_stage stage;
    uint32_t pid;
    uint64_t window;
    uint64_t window_deadline_us;
    uint64_t agent_deadline_us;
    uint64_t deadline_us;
    int agent_ready;
};

static inline unsigned char forever_installer_path_char(unsigned char c)
{
    if (c == '/') return '\\';
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static inline int forever_installer_setup_program(char *const *argv)
{
    static const char path[] = "C:\\ProgramData\\Battle.net\\Setup\\bna_2\\Battle.net-Setup.exe";
    const unsigned char *left, *right;

    if (!argv || !argv[0] || !argv[1]) return 0;
    left = (const unsigned char *)argv[1];
    right = (const unsigned char *)path;
    while (*left && *right && forever_installer_path_char(*left) ==
                              forever_installer_path_char(*right))
    {
        left++;
        right++;
    }
    return !*left && !*right;
}
static inline int forever_installer_agent_program(char *const *argv)
{
    static const char prefix[] = "C:\\ProgramData\\Battle.net\\Agent\\Agent.";
    static const char name[] = "\\Agent.exe";
    const unsigned char *left, *right;
    unsigned digits = 0;

    if (!argv || !argv[0] || !argv[1]) return 0;
    left = (const unsigned char *)argv[1];
    right = (const unsigned char *)prefix;
    while (*left && *right && forever_installer_path_char(*left) ==
                              forever_installer_path_char(*right))
    {
        left++;
        right++;
    }
    if (*right) return 0;
    while (*left >= '0' && *left <= '9')
    {
        left++;
        digits++;
    }
    if (!digits) return 0;
    right = (const unsigned char *)name;
    while (*left && *right && forever_installer_path_char(*left) ==
                              forever_installer_path_char(*right))
    {
        left++;
        right++;
    }
    return !*left && !*right;
}



static inline void forever_installer_continue_mode(struct forever_installer_continue *state,
                                                    int enabled)
{
    *state = (struct forever_installer_continue){
        .stage = enabled ? FOREVER_INSTALLER_CONTINUE_PROCESS : FOREVER_INSTALLER_CONTINUE_OFF,
    };
}

static inline void forever_installer_continue_process(struct forever_installer_continue *state,
                                                       uint32_t pid)
{
    if (state->stage != FOREVER_INSTALLER_CONTINUE_PROCESS || !pid) return;
    state->pid = pid;
    state->stage = FOREVER_INSTALLER_CONTINUE_WINDOW;
}
static inline int forever_installer_continue_agent(struct forever_installer_continue *state,
                                                    uint64_t now_us)
{
    if ((state->stage != FOREVER_INSTALLER_CONTINUE_WINDOW &&
         state->stage != FOREVER_INSTALLER_CONTINUE_SETTLE) ||
        state->agent_ready)
        return 0;
    state->agent_ready = 1;
    state->agent_deadline_us = now_us + FOREVER_INSTALLER_CONTINUE_AGENT_DELAY_US;
    if (state->agent_deadline_us > state->deadline_us)
        state->deadline_us = state->agent_deadline_us;
    return 1;
}


static inline enum forever_installer_continue_result forever_installer_continue_foreground(
    struct forever_installer_continue *state, uint32_t pid, uint64_t window, int ready,
    uint64_t now_us)
{
    if (state->stage == FOREVER_INSTALLER_CONTINUE_WINDOW)
    {
        if (pid != state->pid || !window || !ready) return FOREVER_INSTALLER_CONTINUE_NONE;
        state->window = window;
        state->window_deadline_us = now_us + FOREVER_INSTALLER_CONTINUE_WINDOW_DELAY_US;
        if (state->window_deadline_us > state->deadline_us)
            state->deadline_us = state->window_deadline_us;
        state->stage = FOREVER_INSTALLER_CONTINUE_SETTLE;
        return FOREVER_INSTALLER_CONTINUE_NONE;
    }
    if (state->stage == FOREVER_INSTALLER_CONTINUE_SETTLE &&
        (pid != state->pid || window != state->window))
    {
        state->stage = FOREVER_INSTALLER_CONTINUE_DONE;
        return FOREVER_INSTALLER_CONTINUE_SKIP_FOCUS_LOST;
    }
    return FOREVER_INSTALLER_CONTINUE_NONE;
}

static inline enum forever_installer_continue_result forever_installer_continue_destroyed(
    struct forever_installer_continue *state, uint32_t pid, uint64_t window)
{
    if (state->stage != FOREVER_INSTALLER_CONTINUE_SETTLE ||
        pid != state->pid || window != state->window)
        return FOREVER_INSTALLER_CONTINUE_NONE;
    state->stage = FOREVER_INSTALLER_CONTINUE_DONE;
    return FOREVER_INSTALLER_CONTINUE_SKIP_WINDOW_GONE;
}

static inline enum forever_installer_continue_result forever_installer_continue_input(
    struct forever_installer_continue *state)
{
    if (state->stage != FOREVER_INSTALLER_CONTINUE_PROCESS &&
        state->stage != FOREVER_INSTALLER_CONTINUE_WINDOW &&
        state->stage != FOREVER_INSTALLER_CONTINUE_SETTLE)
        return FOREVER_INSTALLER_CONTINUE_NONE;
    state->stage = FOREVER_INSTALLER_CONTINUE_DONE;
    return FOREVER_INSTALLER_CONTINUE_SKIP_USER_INPUT;
}


static inline enum forever_installer_continue_result forever_installer_continue_tick(
    struct forever_installer_continue *state, uint64_t now_us)
{
    if (state->stage != FOREVER_INSTALLER_CONTINUE_SETTLE || !state->agent_ready ||
        now_us < state->deadline_us)
        return FOREVER_INSTALLER_CONTINUE_NONE;
    state->stage = FOREVER_INSTALLER_CONTINUE_DONE;
    return FOREVER_INSTALLER_CONTINUE_SEND;
}

#endif
