/* P1c-3: concurrent children, termination, stale bootstrap and slot capacity. */
#include <windows.h>

#define CHILDREN 7
#define WAIT_MS 20000
static const WCHAR child_path[] = L"C:\\psmkit-wine-child.exe";
static const char success[] =
    "p1c parent: lifecycle=ok repeats=2 concurrent=2 terminated=7 stale=87 admitted=7 refused=1816 child exit=5\r\n";
struct child
{
    PROCESS_INFORMATION process;
    HANDLE ready;
};
static const char *failure;
static DWORD failure_error;
static unsigned int repeat, index;

static void fail(const char *step, DWORD error)
{
    if (!failure) { failure = step; failure_error = error; }
}

static unsigned int append_text(WCHAR *text, unsigned int used, const WCHAR *value)
{
    while (*value) text[used++] = *value++;
    text[used] = 0;
    return used;
}

static void event_name(WCHAR *name, const WCHAR *mode, unsigned int r, unsigned int i)
{
    unsigned int used = append_text(name, 0, L"Local\\p1c3-ready-");
    used = append_text(name, used, mode);
    name[used++] = L'-'; name[used++] = L'0' + r;
    name[used++] = L'-'; name[used++] = L'0' + i; name[used] = 0;
}

static void release_name(WCHAR *name, unsigned int r)
{
    unsigned int used = append_text(name, 0, L"Local\\p1c3-release-");
    name[used++] = L'0' + r; name[used] = 0;
}

static BOOL close_handle(HANDLE *handle)
{
    BOOL result = TRUE;
    if (*handle)
    {
        result = CloseHandle(*handle);
        if (!result) fail("close", GetLastError());
        *handle = NULL;
    }
    return result;
}

static BOOL wait_handle(HANDLE handle, const char *step)
{
    DWORD result = WaitForSingleObject(handle, WAIT_MS);
    if (result == WAIT_OBJECT_0) return TRUE;
    fail(step, result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    return FALSE;
}

static BOOL finish_child(struct child *child, BOOL terminate, DWORD expected)
{
    DWORD code;
    if (terminate && !TerminateProcess(child->process.hProcess, 7))
    { fail("terminate", GetLastError()); return FALSE; }
    if (!wait_handle(child->process.hProcess, "exit-wait")) return FALSE;
    if (!GetExitCodeProcess(child->process.hProcess, &code))
    { fail("exit-code", GetLastError()); return FALSE; }
    if (code != expected) { fail("exit-code", ERROR_INVALID_DATA); return FALSE; }
    return close_handle(&child->process.hThread) &&
           close_handle(&child->process.hProcess) && close_handle(&child->ready);
}

static BOOL spawn_child(struct child *child, const WCHAR *mode, BOOL stale)
{
    STARTUPINFOW startup = {0};
    WCHAR command[160];
    unsigned int used = append_text(command, 0, child_path);
    used = append_text(command, used, stale ? L" --pw-p1c-stale-bootstrap " : L" ");
    used = append_text(command, used, mode);
    command[used++] = L' '; command[used++] = L'0' + repeat;
    if (!stale) { command[used++] = L' '; command[used++] = L'0' + index; }
    command[used] = 0;
    startup.cb = sizeof(startup);
    return CreateProcessW(child_path, command, NULL, NULL, FALSE, 0, NULL, NULL,
                          &startup, &child->process);
}

static BOOL create_ready(struct child *child, const WCHAR *mode)
{
    WCHAR name[64];
    event_name(name, mode, repeat, index);
    child->ready = CreateEventW(NULL, TRUE, FALSE, name);
    if (!child->ready) { fail("ready-create", GetLastError()); return FALSE; }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    { fail("ready-create", ERROR_ALREADY_EXISTS); return FALSE; }
    return TRUE;
}

static unsigned int ascii_text(char *text, unsigned int used, const char *value)
{
    while (*value) text[used++] = *value++;
    return used;
}

static unsigned int ascii_decimal(char *text, unsigned int used, DWORD value)
{
    char digits[10];
    unsigned int count = 0;
    do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
    while (count) text[used++] = digits[--count];
    return used;
}

static BOOL report(void)
{
    char output[192];
    DWORD written = 0;
    unsigned int used;
    if (!failure)
        return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), success, sizeof(success) - 1,
                         &written, NULL) && written == sizeof(success) - 1;
    used = ascii_text(output, 0, "p1c parent: step=");
    used = ascii_text(output, used, failure);
    used = ascii_text(output, used, " repeat=");
    used = ascii_decimal(output, used, repeat);
    used = ascii_text(output, used, " index=");
    used = ascii_decimal(output, used, index);
    used = ascii_text(output, used, " error=");
    used = ascii_decimal(output, used, failure_error);
    output[used++] = '\r'; output[used++] = '\n';
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, used, &written, NULL) &&
           written == used;
}

void mainCRTStartup(void)
{
    struct child children[CHILDREN + 1] = {0};
    HANDLE release = NULL;
    WCHAR name[64];
    for (repeat = 1; repeat <= 2; ++repeat)
    {
        index = 0;
        release_name(name, repeat);
        release = CreateEventW(NULL, TRUE, FALSE, name);
        if (!release) { fail("release-create", GetLastError()); goto cleanup; }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        { fail("release-create", ERROR_ALREADY_EXISTS); goto cleanup; }
        for (index = 1; index <= 2; ++index)
        {
            const WCHAR *mode = index == 1 ? L"wait" : L"block";
            if (!create_ready(&children[index - 1], mode)) goto cleanup;
            if (!spawn_child(&children[index - 1], mode, FALSE))
            { fail("concurrent-spawn", GetLastError()); goto cleanup; }
        }
        HANDLE ready[] = {children[0].ready, children[1].ready};
        index = 0;
        DWORD result = WaitForMultipleObjects(2, ready, TRUE, WAIT_MS);
        if (result != WAIT_OBJECT_0)
        { fail("concurrent-ready", result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT); goto cleanup; }
        index = 2;
        if (!finish_child(&children[1], TRUE, 7)) goto cleanup;
        index = 1;
        if (!SetEvent(release)) { fail("release-signal", GetLastError()); goto cleanup; }
        if (!finish_child(&children[0], FALSE, 5) || !close_handle(&release)) goto cleanup;

        index = 0;
        if (spawn_child(&children[0], L"stale", TRUE))
        { fail("stale-admitted", ERROR_INVALID_DATA); goto cleanup; }
        DWORD error = GetLastError();
        if (error != ERROR_INVALID_PARAMETER) { fail("stale-error", error); goto cleanup; }
        Sleep(2000);

        unsigned int admitted = 0;
        DWORD refused = ERROR_SUCCESS;
        for (index = 1; index <= CHILDREN + 1; ++index)
        {
            struct child *child = &children[index - 1];
            if (!create_ready(child, L"block")) goto cleanup;
            if (!spawn_child(child, L"block", FALSE))
            {
                refused = GetLastError();
                if (!close_handle(&child->ready)) goto cleanup;
                break;
            }
            ++admitted;
            if (!wait_handle(child->ready, "capacity-ready")) goto cleanup;
        }
        if (admitted != CHILDREN || refused != ERROR_NOT_ENOUGH_QUOTA)
        {
            index = admitted;
            fail("capacity-admitted", refused);
            goto cleanup;
        }
        for (index = 1; index <= admitted; ++index)
            if (!finish_child(&children[index - 1], TRUE, 7)) goto cleanup;
        Sleep(2000);
    }

cleanup:
    /* A failed witness still bounds cleanup and closes every acquired handle. */
    for (unsigned int i = 0; i <= CHILDREN; ++i)
    {
        struct child *child = &children[i];
        if (child->process.hProcess)
        {
            DWORD code;
            if (GetExitCodeProcess(child->process.hProcess, &code) && code == STILL_ACTIVE)
            {
                TerminateProcess(child->process.hProcess, 7);
                WaitForSingleObject(child->process.hProcess, WAIT_MS);
            }
        }
        close_handle(&child->process.hThread);
        close_handle(&child->process.hProcess);
        close_handle(&child->ready);
    }
    close_handle(&release);
    BOOL printed = report();
    ExitProcess(printed && !failure ? 0 : 1);
}
