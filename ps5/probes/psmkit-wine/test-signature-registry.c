/* Destructive qualification helper for the DISPOSABLE issue6 QA prefix only.
 * Never linked into the title/runtime.  It accepts the signature-file probe's
 * arguments, using argv[1] as its report path.  The marker is an explicit
 * second guard, in addition to the exact native WINEPREFIX path check.
 * Marker bytes (LF, no NUL):
 *   FOREVER ISSUE6 DISPOSABLE PREFIX\nACTION=DELETE\n
 * or ACTION=REGISTER to call this architecture's real DllRegisterServer.
 * REGISTER must be run as both 32-bit and 64-bit helpers to repair both views.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

static FILE *report;
static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fflush(stdout);
    if (!report) return;
    va_start(args, format);
    vfprintf(report, format, args);
    va_end(args);
    fflush(report);
}

static int guarded_action(void)
{
    static const WCHAR prefix[] = L"/data/forever/issue6-fresh/prefix";
    static const WCHAR marker[] = L"C:\\SignatureQA-issue6\\qa-disposable-prefix.flag";
    static const char remove[] = "FOREVER ISSUE6 DISPOSABLE PREFIX\nACTION=DELETE\n";
    static const char restore[] = "FOREVER ISSUE6 DISPOSABLE PREFIX\nACTION=REGISTER\n";
    WCHAR environment[512];
    char contents[128];
    DWORD length, got = 0;
    HANDLE file;
    BOOL ret;
    int action = 0;

    length = GetEnvironmentVariableW(L"WINEPREFIX", environment, sizeof(environment) / sizeof(environment[0]));
    if (!length || length >= sizeof(environment) / sizeof(environment[0]) || wcscmp(environment, prefix))
    {
        say("guard prefix_match=0 length=%lu refusal=NO_MUTATION\n", length);
        return 0;
    }
    file = CreateFileW(marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        say("guard marker_open=0 error=%08lx refusal=NO_MUTATION\n", GetLastError());
        return 0;
    }
    ret = ReadFile(file, contents, sizeof(contents), &got, NULL);
    if (ret && got == sizeof(remove) - 1 && !memcmp(contents, remove, got)) action = 1;
    if (ret && got == sizeof(restore) - 1 && !memcmp(contents, restore, got)) action = 2;
    CloseHandle(file);
    say("guard prefix_match=1 marker_match=%u action=%s\n", !!action,
        action == 1 ? "DELETE" : action == 2 ? "REGISTER" : "REFUSE");
    return action;
}

static int flush_registry(void)
{
    static const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    unsigned int view;
    int failed = 0;

    /* Predefined HKLM is a pseudo-handle. Flush real opened keys in both
     * views so Wine can identify their backing registry branches. */
    for (view = 0; view < sizeof(views) / sizeof(views[0]); ++view)
    {
        HKEY key;
        LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_READ | views[view], &key);
        say("flush_open view=%u status=%08lx\n", view ? 64 : 32, (DWORD)error);
        if (error)
        {
            failed = 1;
            continue;
        }
        error = RegFlushKey(key);
        say("flush view=%u status=%08lx\n", view ? 64 : 32, (DWORD)error);
        RegCloseKey(key);
        if (error) failed = 1;
    }
    return failed;
}

static int remove_providers(void)
{
    static const WCHAR *const trust[] = {L"Initialization", L"Message", L"Signature", L"Certificate", L"FinalPolicy"};
    static const WCHAR *const sip[] = {L"GetSignedDataMsg", L"PutSignedDataMsg", L"CreateIndirectData", L"VerifyIndirectData", L"RemoveSignedDataMsg"};
    static const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    WCHAR key[256];
    unsigned int view, i;
    int failed = 0, flush_error;
    LONG error;

    for (view = 0; view < sizeof(views) / sizeof(views[0]); ++view)
        for (i = 0; i < sizeof(trust) / sizeof(trust[0]); ++i)
        {
            swprintf(key, sizeof(key) / sizeof(key[0]),
                     L"Software\\Microsoft\\Cryptography\\Providers\\Trust\\%ls\\{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}", trust[i]);
            error = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, views[view], 0);
            say("delete family=trust index=%u view=%u status=%08lx\n", i, view ? 64 : 32, (DWORD)error);
            if (error && error != ERROR_FILE_NOT_FOUND) failed = 1;
            swprintf(key, sizeof(key) / sizeof(key[0]),
                     L"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDll%ls\\{C689AAB8-8E78-11D0-8C47-00C04FC295EE}", sip[i]);
            error = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key, views[view], 0);
            say("delete family=sip index=%u view=%u status=%08lx\n", i, view ? 64 : 32, (DWORD)error);
            if (error && error != ERROR_FILE_NOT_FOUND) failed = 1;
        }
    flush_error = flush_registry();
    return failed || flush_error;
}

static int register_provider(void)
{
    HMODULE module = LoadLibraryW(L"wintrust.dll");
    HRESULT (WINAPI *registration)(void);
    HRESULT result;
    int flush_error;

    if (!module)
    {
        say("register load=0 error=%08lx\n", GetLastError());
        return 1;
    }
    registration = (void *)GetProcAddress(module, "DllRegisterServer");
    if (!registration)
    {
        say("register entrypoint=0 error=%08lx\n", GetLastError());
        FreeLibrary(module);
        return 1;
    }
    result = registration();
    say("register pointer_bits=%u hresult=%08lx\n", (unsigned)(sizeof(void *) * 8), (DWORD)result);
    FreeLibrary(module);
    flush_error = flush_registry();
    return result != S_OK || flush_error;
}

int wmain(int argc, WCHAR **argv)
{
    int action, result;
    if (argc != 5) return 2;
    action = guarded_action();
    if (!action)
    {
        say("signature-registry-qa: done exit=4 refusal=NO_MUTATION\n");
        return 4;
    }
    report = _wfopen(argv[1], L"wb");
    if (!report) return 3;
    say("signature-registry-qa pointer_bits=%u guarded_action=%s\n",
        (unsigned)(sizeof(void *) * 8), action == 1 ? "DELETE" : "REGISTER");
    result = action == 1 ? remove_providers() : register_provider();
    say("signature-registry-qa: done exit=%d\n", result);
    fclose(report);
    return result;
}
