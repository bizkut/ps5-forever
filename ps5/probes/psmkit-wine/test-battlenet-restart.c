#include "battlenet_restart.h"
#include <assert.h>
#include <stdio.h>

static enum battlenet_restart_action decide( struct battlenet_restart_budget *budget,
                                             unsigned int status, unsigned long long now,
                                             unsigned int expected_restart )
{
    unsigned int restart = 99;
    enum battlenet_restart_action action = battlenet_restart_decide( budget, status, now, &restart );

    assert( restart == expected_restart );
    return action;
}

int main(void)
{
    struct battlenet_restart_budget budget = {0};
    static const wchar_t launcher[] =
        L"C:\\Program Files (x86)\\Battle.net\\Battle.net Launcher.exe";

    assert( battlenet_watch_enabled( L"1", launcher, launcher ) );
    assert( battlenet_watch_enabled(
        L"1", L" \t\"c:\\program files (X86)\\battle.NET\\battle.net launcher.EXE\" \r\n",
        launcher ) );
    assert( !battlenet_watch_enabled( NULL, launcher, launcher ) );
    assert( !battlenet_watch_enabled( L"", launcher, launcher ) );
    assert( !battlenet_watch_enabled( L"0", launcher, launcher ) );
    assert( !battlenet_watch_enabled( L"10", launcher, launcher ) );
    assert( !battlenet_watch_enabled(
        L"1", L"C:\\Program Files (x86)\\Battle.net\\Battle.net Launcher.exe --update",
        launcher ) );
    assert( !battlenet_watch_enabled(
        L"1", L"C:\\Program Files (x86)\\Battle.net\\Battle.net.exe", launcher ) );

    assert( !battlenet_exit_is_crash( 0 ) );
    assert( !battlenet_exit_is_crash( 1 ) );
    assert( !battlenet_exit_is_crash( 0x0000ffff ) );
    assert( !battlenet_exit_is_crash( 0xffffffff ) ); /* title TerminateProcess */
    assert( !battlenet_exit_is_crash( 0xc0000120 ) ); /* maintenance Play hand-off */
    assert( battlenet_exit_is_crash( 0x80000003 ) );  /* breakpoint */
    assert( battlenet_exit_is_crash( 0xc0000005 ) );  /* access violation */
    assert( battlenet_exit_is_crash( 0xc0000409 ) );  /* stack buffer overrun */
    assert( battlenet_exit_is_crash( 0xe06d7363 ) );  /* unhandled C++ exception */
    assert( battlenet_exit_is_crash( 0x40000015 ) );  /* fatal app exit */

    /* Normal quit, a small self-update hand-off, and Play termination neither
     * restart nor consume the crash budget. */
    assert( decide( &budget, 0, 1, 0 ) == BATTLENET_RESTART_NONE );
    assert( decide( &budget, 42, 2, 0 ) == BATTLENET_RESTART_NONE );
    assert( decide( &budget, 0xffffffff, 3, 0 ) == BATTLENET_RESTART_NONE );
    assert( decide( &budget, 0xc0000120, 4, 0 ) == BATTLENET_RESTART_NONE );
    assert( budget.count == 0 );

    assert( decide( &budget, 0x80000003, 0, 1 ) == BATTLENET_RESTART_LAUNCH );
    assert( decide( &budget, 0xc0000005, 100, 2 ) == BATTLENET_RESTART_LAUNCH );
    assert( decide( &budget, 0xc0000409, 200, 3 ) == BATTLENET_RESTART_LAUNCH );
    assert( decide( &budget, 0xe06d7363, 300, 0 ) == BATTLENET_RESTART_CLOSE );
    assert( budget.count == BATTLENET_RESTART_LIMIT );

    /* The oldest restart expires exactly at the ten-minute boundary. */
    assert( decide( &budget, 0xc0000005, BATTLENET_RESTART_WINDOW_MS, 3 ) ==
            BATTLENET_RESTART_LAUNCH );
    assert( decide( &budget, 0xc0000005, 1200001, 1 ) == BATTLENET_RESTART_LAUNCH );

    puts("Battle.net watcher gate, crash codes, normal/update/Play exits and 3-per-10-minute budget PASS");
    return 0;
}
