/* Real registry/export check shared with Wineboot, without test API mocks.
 * Compile with -I<WINE_SRC>/programs/wineboot.  The normal signature probe's
 * argv shape is retained: REPORT FILE ORGANIZATION accept|reject|inspect.
 * FILE and ORGANIZATION are unused; this executable never mutates a hive. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#include "ps5-crypto.h"

int wmain(int argc, WCHAR **argv)
{
    FILE *report;
    BOOL ready;
    int result = 0;
    if (argc != 5) return 2;
    report = _wfopen(argv[1], L"wb");
    if (!report) return 3;
    ready = wineboot_crypto_registration_ready();
    if (!wcscmp(argv[4], L"accept")) result = !ready;
    else if (!wcscmp(argv[4], L"reject")) result = ready;
    else if (wcscmp(argv[4], L"inspect")) result = 2;
    fprintf(report, "signature-registration pointer_bits=%u ready=%u exit=%d\n",
            (unsigned)(sizeof(void *) * 8), ready, result);
    fclose(report);
    printf("signature-registration: done ready=%u exit=%d\n", ready, result);
    fflush(stdout);
    return result;
}
