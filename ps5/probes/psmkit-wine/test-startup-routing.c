/* Host dependencies for the unmodified Wine startup functions included below.
 * Mapping results are injected at load_main_exe's OS boundary; routing is real.
 * Stop at the selected consumer (rebuild_argv/start.exe), before unrelated PEB work.
 */
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>

typedef int BOOL;
typedef uint16_t WORD;
typedef wchar_t WCHAR;
typedef size_t SIZE_T;
typedef int32_t NTSTATUS;
typedef struct { WCHAR *Buffer; size_t Length, MaximumLength; } UNICODE_STRING;
typedef struct {
    size_t AllocationSize, Size, Flags, wShowWindow, ProcessGroupId;
    struct { UNICODE_STRING DosPath; } CurrentDirectory;
    UNICODE_STRING ImagePathName, CommandLine, WindowTitle;
    void *Environment;
    size_t EnvironmentSize;
} RTL_USER_PROCESS_PARAMETERS;
#define TRUE 1
#define FALSE 0
#define IMAGE_FILE_MACHINE_I386 0x014c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_ARMNT 0x01c4
#define IMAGE_FILE_MACHINE_ARM64 0xaa64
#define IMAGE_FILE_DLL 0x2000
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_IMAGE_NOT_AT_BASE ((NTSTATUS)0x40000003)
#define STATUS_DLL_NOT_FOUND ((NTSTATUS)0xc0000135)
#define STATUS_INVALID_IMAGE_FORMAT ((NTSTATUS)0xc000007b)
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define WOW64_TLS_FILESYSREDIR 0
#define MAX_PATH 260
#define MEM_COMMIT 0x1000
#define PAGE_READWRITE 4
#define PROCESS_PARAMS_FLAG_NORMALIZED 1
#define MESSAGE(...) ((void)0)
#define TRACE(...) ((void)0)
#ifdef __PROSPERO__
#define TITLE_POLICY 1
#else
#define TITLE_POLICY 0
#endif

static BOOL is_win64, title_active, is_prefix_bootstrap;
static WORD current_machine;
static const char *build_dir, *alt_build_dir, *dll_dir;
static struct { WORD Machine, ImageCharacteristics; } main_image_info;
static char *main_argv[] = { "wine", "probe.exe", NULL };
static WCHAR **main_wargv;
static const WCHAR bootstrapW[] = L"WINEBOOTSTRAPMODE";
static struct { int TlsSlots[1]; } teb;
static NTSTATUS mapping_status;
static jmp_buf selected;
static WCHAR *test_env, *test_curdir, *test_name;
static int route, unmaps;
enum { ROUTE_NATIVE = 1, ROUTE_START, ROUTE_TERMINATE };

static BOOL ps5_direct_enabled(void) { return title_active; }
static WORD get_alt_machine(WORD machine)
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_AMD64: return IMAGE_FILE_MACHINE_I386;
    case IMAGE_FILE_MACHINE_I386: return IMAGE_FILE_MACHINE_AMD64;
    case IMAGE_FILE_MACHINE_ARM64: return IMAGE_FILE_MACHINE_ARMNT;
    default: return IMAGE_FILE_MACHINE_ARM64;
    }
}
static const char *get_so_dir(WORD machine)
{
    return machine == IMAGE_FILE_MACHINE_I386 ? "/i386-unix" : "/other-unix";
}
static WCHAR *get_initial_environment(SIZE_T *pos, SIZE_T *size)
{
    *pos = 0;
    *size = 8;
    return test_env = calloc(*size, sizeof(WCHAR));
}
static WCHAR *get_initial_directory(void) { return test_curdir = wcsdup(L"C:\\"); }
/* NtCurrentTeb64 is NULL here, but its unreachable member access must typecheck. */
#define NtCurrentTeb64() ((typeof(teb) *)NULL)
static WCHAR *get_env_var(WCHAR *env, SIZE_T pos, const WCHAR *name, SIZE_T len)
{ (void)env; (void)pos; (void)name; (void)len; return NULL; }
static void add_dynamic_environment(WCHAR **env, SIZE_T *pos, SIZE_T *size)
{ (void)env; (void)pos; (void)size; }
static void add_registry_environment(WCHAR **env, SIZE_T *pos, SIZE_T *size)
{ (void)env; (void)pos; (void)size; }
static void set_env_var(WCHAR **env, SIZE_T *pos, SIZE_T *size, const WCHAR *name, SIZE_T len, const WCHAR *value)
{ (void)env; (void)pos; (void)size; (void)name; (void)len; (void)value; }
static void run_wineboot(WCHAR *env, SIZE_T pos) { (void)env; (void)pos; }
static void get_full_path(const char *name, WCHAR *curdir, UNICODE_STRING *nt_name)
{ (void)name; (void)curdir; nt_name->Buffer = test_name = wcsdup(L"C:\\probe.exe"); }
static NTSTATUS load_main_exe(UNICODE_STRING *name, WORD machine, void **module)
{
    (void)name; (void)machine;
    *module = NT_SUCCESS(mapping_status) ? (void *)(uintptr_t)0x121000 : NULL;
    return mapping_status;
}
static void *GetCurrentProcess(void) { return NULL; }
static void NtTerminateProcess(void *process, NTSTATUS status)
{ (void)process; (void)status; route = ROUTE_TERMINATE; longjmp(selected, 1); }
static NTSTATUS NtUnmapViewOfSection(void *process, void *module)
{ (void)process; (void)module; ++unmaps; return STATUS_SUCCESS; }
static NTSTATUS load_start_exe(UNICODE_STRING *name, void **module)
{
    (void)name; (void)module;
    test_name = NULL; /* production freed the original name before this call */
    route = ROUTE_START;
    longjmp(selected, 1);
}
static void prepend_argv(const char **args, int count) { (void)args; (void)count; }
static void rebuild_argv(void) { route = ROUTE_NATIVE; longjmp(selected, 1); }
static WCHAR *get_dos_path(WCHAR *path) { return path; }
static WCHAR **build_wargv(WCHAR *path) { (void)path; return NULL; }
static WCHAR *build_command_line(WCHAR **argv) { (void)argv; return NULL; }
static NTSTATUS NtAllocateVirtualMemory(void *process, void **ptr, int bits, SIZE_T *size, int flags, int protect)
{ (void)process; (void)bits; (void)flags; (void)protect; *ptr = calloc(1, *size); return STATUS_SUCCESS; }
#define NtCurrentProcess GetCurrentProcess
static int GetCurrentProcessId(void) { return 1; }
static void put_unicode_string(WCHAR *src, WCHAR **dst, UNICODE_STRING *string)
{ (void)src; (void)dst; (void)string; }
static void get_initial_console(RTL_USER_PROCESS_PARAMETERS *params) { (void)params; }

#ifdef __PROSPERO__
/* Conversion boundary only: parser cases below use ASCII URLs. */
static char *ps5_payload_utf8(const UNICODE_STRING *str)
{
    size_t count = str->Length / sizeof(WCHAR);
    char *result = malloc(count + 1);
    assert(result);
    for (size_t i = 0; i < count; i++)
    {
        assert(str->Buffer[i] < 128);
        result[i] = str->Buffer[i];
    }
    result[count] = 0;
    return result;
}
#endif

#include "startup-routing-functions.inc"

static int failures;
static void check(int condition, const char *name)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", name); ++failures; }
}
static void selection(const char *name, BOOL title, BOOL win64, WORD host, WORD image,
                      const char *arch, BOOL alternate)
{
    char *loader;
    title_active = title;
    is_win64 = win64;
    current_machine = host;
    if (arch) setenv("WINEARCH", arch, 1); else unsetenv("WINEARCH");
    loader = get_alternate_wineloader(image);
    check(!!loader == alternate, name);
    if (loader && image == IMAGE_FILE_MACHINE_I386 && !build_dir)
        check(!strcmp(loader, "/wine/i386-unix/wine"), "payload alternate loader path");
    free(loader);
}
static void startup(const char *name, BOOL title, WORD host, WORD image, WORD characteristics,
                    NTSTATUS status, int expected)
{
    void *module = NULL;
    int expected_unmaps = expected == ROUTE_START && NT_SUCCESS(status);
    title_active = title;
    is_win64 = TRUE;
    current_machine = host;
    main_image_info.Machine = image;
    main_image_info.ImageCharacteristics = characteristics;
    mapping_status = status;
    route = unmaps = 0;
    if (!setjmp(selected)) build_initial_params(&module);
    check(route == expected, name);
    if (unmaps != expected_unmaps)
    {
        fprintf(stderr, "FAIL %s: unmaps actual=%d expected=%d\n", name, unmaps, expected_unmaps);
        ++failures;
    }
    free(test_env); free(test_curdir); free(test_name);
    test_env = test_curdir = test_name = NULL;
}
#ifdef __PROSPERO__
static void url_case(const WCHAR *command, const char *expected)
{
    UNICODE_STRING line = {.Buffer = (WCHAR *)command,
                          .Length = wcslen(command) * sizeof(WCHAR)};
    char *url = ps5_winebrowser_url(&line);
    check(expected ? url && !strcmp(url, expected) : !url, "winebrowser URL extraction");
    free(url);
}
#endif
int main(void)
{
    const char *arches[] = { NULL, "win32", "win64", "wow64" };
    size_t i;
#ifdef __PROSPERO__
    url_case(L"\"C:\\windows\\system32\\winebrowser.exe\" \"https://example.test/login?a=1&b=2\"",
             "https://example.test/login?a=1&b=2");
    url_case(L"winebrowser.exe -nohome \"HTTP://127.0.0.1:22886/login-callback/1\"",
             "HTTP://127.0.0.1:22886/login-callback/1");
    url_case(L"winebrowser.exe https://old.test https://last.test -nohome",
             "https://last.test");
    url_case(L"winebrowser.exe\t-nohome\t\"https://example.test/a b\"\t", "https://example.test/a b");
    url_case(L"winebrowser.exe -nohome", NULL);
    url_case(L"\"winebrowser.exe\"", NULL);
#endif
    dll_dir = "/wine";
    for (i = 0; i < ARRAY_SIZE(arches); ++i)
    {
        BOOL keep_title = TITLE_POLICY || i == 3;
        selection("native title AMD64/i386 keeps loader independent of WINEARCH", TRUE, TRUE,
                  IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_I386, arches[i], !keep_title);
        startup("native title preferred image remains parent", TRUE, IMAGE_FILE_MACHINE_AMD64,
                IMAGE_FILE_MACHINE_I386, 0, STATUS_SUCCESS, keep_title ? ROUTE_NATIVE : ROUTE_START);
        startup("native title rebased image remains parent", TRUE, IMAGE_FILE_MACHINE_AMD64,
                IMAGE_FILE_MACHINE_I386, 0, STATUS_IMAGE_NOT_AT_BASE, TITLE_POLICY ? ROUTE_NATIVE : ROUTE_START);
        startup("native title DLL remains rejected", TRUE, IMAGE_FILE_MACHINE_AMD64,
                IMAGE_FILE_MACHINE_I386, IMAGE_FILE_DLL, STATUS_IMAGE_NOT_AT_BASE, ROUTE_START);
        startup("native title negative load remains rejected", TRUE, IMAGE_FILE_MACHINE_AMD64,
                IMAGE_FILE_MACHINE_I386, 0, STATUS_INVALID_IMAGE_FORMAT, ROUTE_START);
        startup("other informational status is not normalized", TRUE, IMAGE_FILE_MACHINE_AMD64,
                IMAGE_FILE_MACHINE_I386, 0, (NTSTATUS)0x40000001, ROUTE_START);
    }
    selection("payload default alternate unchanged", FALSE, TRUE, IMAGE_FILE_MACHINE_AMD64,
              IMAGE_FILE_MACHINE_I386, NULL, TRUE);
    startup("payload preferred i386 uses proxy", FALSE, IMAGE_FILE_MACHINE_AMD64,
            IMAGE_FILE_MACHINE_I386, 0, STATUS_SUCCESS, ROUTE_START);
    selection("payload forced wow64 unchanged", FALSE, TRUE, IMAGE_FILE_MACHINE_AMD64,
              IMAGE_FILE_MACHINE_I386, "wow64", FALSE);
    startup("payload rebased status unchanged", FALSE, IMAGE_FILE_MACHINE_AMD64,
            IMAGE_FILE_MACHINE_I386, 0, STATUS_IMAGE_NOT_AT_BASE, ROUTE_START);
    selection("native AMD64 selection unchanged", TRUE, TRUE, IMAGE_FILE_MACHINE_AMD64,
              IMAGE_FILE_MACHINE_AMD64, NULL, FALSE);
    startup("native AMD64 informational status unchanged", TRUE, IMAGE_FILE_MACHINE_AMD64,
            IMAGE_FILE_MACHINE_AMD64, 0, STATUS_IMAGE_NOT_AT_BASE, ROUTE_START);
    startup("foreign image informational status unchanged", TRUE, IMAGE_FILE_MACHINE_AMD64,
            IMAGE_FILE_MACHINE_ARMNT, 0, STATUS_IMAGE_NOT_AT_BASE, ROUTE_START);
    selection("ARM64/ARMNT alternate unchanged", TRUE, TRUE, IMAGE_FILE_MACHINE_ARM64,
              IMAGE_FILE_MACHINE_ARMNT, NULL, TRUE);
    startup("ARM64/i386 informational status unchanged", TRUE, IMAGE_FILE_MACHINE_ARM64,
            IMAGE_FILE_MACHINE_I386, 0, STATUS_IMAGE_NOT_AT_BASE, ROUTE_START);
    selection("32bit host alternate unchanged", TRUE, FALSE, IMAGE_FILE_MACHINE_I386,
              IMAGE_FILE_MACHINE_AMD64, NULL, TRUE);
    selection("32bit host same-machine unchanged", TRUE, FALSE, IMAGE_FILE_MACHINE_I386,
              IMAGE_FILE_MACHINE_I386, NULL, FALSE);
    build_dir = "/build"; alt_build_dir = "/build-wow64";
    selection("build-tree native title remains loader", TRUE, TRUE, IMAGE_FILE_MACHINE_AMD64,
              IMAGE_FILE_MACHINE_I386, NULL, !TITLE_POLICY);
    selection("build-tree payload uses alternate", FALSE, TRUE, IMAGE_FILE_MACHINE_AMD64,
              IMAGE_FILE_MACHINE_I386, NULL, TRUE);
    if (failures) return 1;
    puts("STARTUP_ROUTING actual Wine functions: title/payload/machine/status/DLL boundaries PASS");
    return 0;
}
