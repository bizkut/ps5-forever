#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../title/installer-continue.h"
_Static_assert(FOREVER_INSTALLER_CONTINUE_WINDOW_DELAY_US == 8000000u,
               "installer window settle delay");
_Static_assert(FOREVER_INSTALLER_CONTINUE_AGENT_DELAY_US == 12000000u,
               "installer Agent settle delay");

static void select_installer(struct forever_installer_continue *state)
{
    forever_installer_continue_mode(state, 1);
    forever_installer_continue_process(state, 164);
}
static void observe_spawn(struct forever_installer_continue *state, char *const *argv,
                          uint32_t pid, uint64_t now_us)
{
    if (forever_installer_setup_program(argv))
        forever_installer_continue_process(state, pid);
    else if (forever_installer_agent_program(argv))
        forever_installer_continue_agent(state, now_us);
}


int main(void)
{
    struct forever_installer_continue state;
    char *setup[] = {"wine", "C:\\ProgramData\\Battle.net\\Setup\\bna_2\\Battle.net-Setup.exe", NULL};
    char *setup_case[] = {"wine", "c:/programdata/battle.net/setup/BNA_2/BATTLE.NET-SETUP.EXE", NULL};
    char *bootstrap[] = {"wine", "C:\\Battle.net-Setup.exe", NULL};
    char *later[] = {"wine", "C:\\Program Files (x86)\\Battle.net\\Battle.net.exe", NULL};
    char *agent[] = {"wine", "C:\\ProgramData\\Battle.net\\Agent\\Agent.exe", NULL};
    char *versioned_agent[] = {"wine", "C:/ProgramData/Battle.net/Agent/Agent.9824/AGENT.EXE", NULL};
    char *bad_agent[] = {"wine", "C:\\ProgramData\\Battle.net\\Agent\\Agent.beta\\Agent.exe", NULL};

    assert(forever_installer_setup_program(setup));
    assert(forever_installer_setup_program(setup_case));
    assert(!forever_installer_setup_program(bootstrap));
    assert(!forever_installer_setup_program(later));
    assert(!forever_installer_setup_program(NULL));
    assert(!forever_installer_agent_program(agent));
    assert(forever_installer_agent_program(versioned_agent));
    assert(!forever_installer_agent_program(bad_agent));
    assert(!forever_installer_agent_program(NULL));

    select_installer(&state);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_foreground(&state, 40, 0x10020, 1, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 0, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_tick(
               &state, 100 + FOREVER_INSTALLER_CONTINUE_WINDOW_DELAY_US) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_agent(&state, 200));
    assert(!forever_installer_continue_agent(&state, 300)); /* A later Agent cannot move the one-shot. */
    assert(forever_installer_continue_tick(
               &state, 200 + FOREVER_INSTALLER_CONTINUE_AGENT_DELAY_US - 1) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_tick(
               &state, 200 + FOREVER_INSTALLER_CONTINUE_AGENT_DELAY_US) ==
           FOREVER_INSTALLER_CONTINUE_SEND);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, UINT64_MAX) ==
           FOREVER_INSTALLER_CONTINUE_NONE);

    select_installer(&state);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_input(&state) == FOREVER_INSTALLER_CONTINUE_SKIP_USER_INPUT);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);

    select_installer(&state);
    assert(forever_installer_continue_input(&state) == FOREVER_INSTALLER_CONTINUE_SKIP_USER_INPUT);
    forever_installer_continue_process(&state, 164);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);

    select_installer(&state);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_foreground(&state, 40, 0x10020, 1, 200) ==
           FOREVER_INSTALLER_CONTINUE_SKIP_FOCUS_LOST);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);

    select_installer(&state);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_destroyed(&state, 164, 0x20040) ==
           FOREVER_INSTALLER_CONTINUE_SKIP_WINDOW_GONE);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);
    forever_installer_continue_mode(&state, 1);
    observe_spawn(&state, setup, 164, 0);
    observe_spawn(&state, agent, 292, 50); /* The unversioned Agent is not the ready signal. */
    observe_spawn(&state, versioned_agent, 300, 100);
    assert(forever_installer_continue_tick(
               &state, 100 + FOREVER_INSTALLER_CONTINUE_AGENT_DELAY_US) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, 5000000) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_tick(
               &state, 5000000 + FOREVER_INSTALLER_CONTINUE_WINDOW_DELAY_US - 1) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_tick(
               &state, 5000000 + FOREVER_INSTALLER_CONTINUE_WINDOW_DELAY_US) ==
           FOREVER_INSTALLER_CONTINUE_SEND);

    forever_installer_continue_mode(&state, 0);
    forever_installer_continue_process(&state, 164);
    assert(forever_installer_continue_foreground(&state, 164, 0x20040, 1, 100) ==
           FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_input(&state) == FOREVER_INSTALLER_CONTINUE_NONE);
    assert(forever_installer_continue_tick(&state, UINT64_MAX) == FOREVER_INSTALLER_CONTINUE_NONE);

    puts("installer continue: bna window + versioned Agent deadlines, once and input guards PASS");
    return 0;
}
