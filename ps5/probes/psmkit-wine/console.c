/* The future Wine gate's real AMD64 console PE. No CRT, UI or process spawn.
 * Not executed by the descriptor prerequisite; stdout must be captured by
 * a future native-safe Wine host, never substituted by a native echo. */
#include <windows.h>

void mainCRTStartup(void)
{
    static const char output[] = "psmkit-wine PE: cpu-only works=1\r\n";
    DWORD written = 0;
    HANDLE stream = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL success = stream != NULL && stream != INVALID_HANDLE_VALUE &&
        WriteFile(stream, output, sizeof(output) - 1, &written, NULL) && written == sizeof(output) - 1;
    ExitProcess(success ? 0 : 1);
}
