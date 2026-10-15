/* P1c-3 child: a named readiness handshake and bounded release wait. */
#include <windows.h>

static const WCHAR stale_token[] = L"--pw-p1c-stale-bootstrap";

static BOOL token(const WCHAR **cursor, const WCHAR *expected)
{
    const WCHAR *text = *cursor;
    while (*text == L' ' || *text == L'\t') ++text;
    while (*expected && *text == *expected) { ++text; ++expected; }
    if (*expected || (*text && *text != L' ' && *text != L'\t')) return FALSE;
    *cursor = text;
    return TRUE;
}

static BOOL number(const WCHAR **cursor, unsigned int *value, unsigned int maximum)
{
    const WCHAR *text = *cursor;
    while (*text == L' ' || *text == L'\t') ++text;
    if (*text < L'0' || *text > L'0' + maximum) return FALSE;
    *value = *text++ - L'0';
    if (*text && *text != L' ' && *text != L'\t') return FALSE;
    *cursor = text;
    return TRUE;
}

static unsigned int append(WCHAR *text, unsigned int used, const WCHAR *value)
{
    while (*value) text[used++] = *value++;
    text[used] = 0;
    return used;
}

static void diagnostic(const char *step, unsigned int repeat, unsigned int index, DWORD error)
{
    char output[160], digits[10];
    const char *text = "p1c child: step=";
    unsigned int used = 0, count = 0;
    while (*text) output[used++] = *text++;
    while (*step) output[used++] = *step++;
    text = " repeat="; while (*text) output[used++] = *text++;
    output[used++] = '0' + repeat;
    text = " index="; while (*text) output[used++] = *text++;
    output[used++] = '0' + index;
    text = " error="; while (*text) output[used++] = *text++;
    do { digits[count++] = '0' + error % 10; error /= 10; } while (error);
    while (count) output[used++] = digits[--count];
    output[used++] = '\r'; output[used++] = '\n';
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, used, &written, NULL);
}

void mainCRTStartup(void)
{
    const WCHAR *cursor = GetCommandLineW(), *mode = NULL;
    unsigned int repeat = 0, index = 1;
    DWORD error = ERROR_INVALID_PARAMETER;
    const char *failure = "arguments";
    HANDLE ready = NULL, release = NULL;
    WCHAR name[64];
    BOOL wait = FALSE;
    while (*cursor == L' ' || *cursor == L'\t') ++cursor;
    if (*cursor == L'"')
    {
        ++cursor;
        while (*cursor && *cursor != L'"') ++cursor;
        if (*cursor != L'"') goto cleanup;
        ++cursor;
    }
    else while (*cursor && *cursor != L' ' && *cursor != L'\t') ++cursor;
    token(&cursor, stale_token);
    if (token(&cursor, L"stale")) ExitProcess(11);
    if (token(&cursor, L"wait")) { mode = L"wait"; wait = TRUE; }
    else if (token(&cursor, L"block")) mode = L"block";
    else goto cleanup;
    if (!number(&cursor, &repeat, 2) || !repeat) goto cleanup;
    while (*cursor == L' ' || *cursor == L'\t') ++cursor;
    if (*cursor && (!number(&cursor, &index, 8) || !index)) goto cleanup;
    while (*cursor == L' ' || *cursor == L'\t') ++cursor;
    if (*cursor) goto cleanup;

    unsigned int used = append(name, 0, L"Local\\p1c3-ready-");
    used = append(name, used, mode);
    name[used++] = L'-'; name[used++] = L'0' + repeat;
    name[used++] = L'-'; name[used++] = L'0' + index; name[used] = 0;
    failure = "ready-open";
    ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!ready) { error = GetLastError(); goto cleanup; }
    if (wait)
    {
        used = append(name, 0, L"Local\\p1c3-release-");
        name[used++] = L'0' + repeat; name[used] = 0;
        failure = "release-open";
        release = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (!release) { error = GetLastError(); goto cleanup; }
    }
    failure = "ready-signal";
    if (!SetEvent(ready)) { error = GetLastError(); goto cleanup; }
    failure = "ready-close";
    if (!CloseHandle(ready)) { error = GetLastError(); ready = NULL; goto cleanup; }
    ready = NULL;
    if (!wait) Sleep(INFINITE);
    failure = "release-wait";
    DWORD result = WaitForSingleObject(release, 20000);
    if (result != WAIT_OBJECT_0)
    { error = result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT; goto cleanup; }
    failure = "release-close";
    if (!CloseHandle(release)) { error = GetLastError(); release = NULL; goto cleanup; }
    release = NULL;
    char output[] = "p1c child: lifecycle wait ok repeat=1\r\n";
    output[sizeof("p1c child: lifecycle wait ok repeat=") - 1] = '0' + repeat;
    DWORD written = 0;
    failure = "stdout";
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, sizeof(output) - 1, &written, NULL))
    { error = GetLastError(); goto cleanup; }
    if (written != sizeof(output) - 1) { error = ERROR_WRITE_FAULT; goto cleanup; }
    ExitProcess(5);

cleanup:
    if (ready) CloseHandle(ready);
    if (release) CloseHandle(release);
    diagnostic(failure, repeat, index, error);
    ExitProcess(1);
}
