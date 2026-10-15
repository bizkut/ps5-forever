#include <windows.h>

static BOOL write_exact(const char *text, DWORD size)
{
    DWORD written = 0;
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, size, &written, NULL) && written == size;
}

static void hex32(char *text, DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    unsigned int i;
    for (i = 0; i < 8; ++i) text[i] = digits[(value >> (28 - 4 * i)) & 15];
}

static void fail(unsigned int step)
{
    char line[] = "p1w parent: fail step=00\r\n";
    line[22] = '0' + step / 10;
    line[22 + 1] = '0' + step % 10;
    write_exact(line, sizeof(line) - 1);
    ExitProcess(20 + step);
}

static EXCEPTION_DISPOSITION NTAPI seh_handler(EXCEPTION_RECORD *record, void *frame,
                                                CONTEXT *context, void *dispatch)
{
    (void)dispatch;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->ExceptionAddress != (void *)(ULONG_PTR)context->Eip)
        return ExceptionContinueSearch;
    context->Eip += 5; /* mov eax, dword ptr [0] */
    ++((DWORD *)frame)[2]; /* Count belongs to this thread's stack registration. */
    return ExceptionContinueExecution;
}

__attribute__((naked)) static DWORD run_seh(PEXCEPTION_ROUTINE handler __attribute__((unused)))
{
    __asm__ volatile(
        "pushl $0\n\t"
        "pushl 8(%esp)\n\t"
        "pushl %fs:0\n\t"
        "movl %esp,%fs:0\n\t"
        ".byte 0xa1,0,0,0,0\n\t"
        "movl (%esp),%ecx\n\t"
        "movl %ecx,%fs:0\n\t"
        "movl 8(%esp),%eax\n\t"
        "addl $12,%esp\n\t"
        "ret");
}

static DWORD WINAPI loop_worker(void *unused)
{
    volatile DWORD sum = 0;
    DWORD i;
    (void)unused;
    for (i = 0; i < 65536; ++i) sum += i;
    return sum == 0x7fff8000 && run_seh(seh_handler) == 1 ? 7 : 8;
}

static void check_threads(void)
{
    HANDLE threads[2];
    DWORD i, code;
    for (i = 0; i < 2; ++i) {
        threads[i] = CreateThread(NULL, 0, loop_worker, NULL, 0, NULL);
        if (!threads[i]) fail(4);
    }
    if (WaitForMultipleObjects(2, threads, TRUE, 5000) != WAIT_OBJECT_0) fail(5);
    for (i = 0; i < 2; ++i) {
        if (!GetExitCodeThread(threads[i], &code) || code != 7) fail(6);
        if (!CloseHandle(threads[i])) fail(7);
    }
}

static void *main_fiber;
static void *volatile fiber_param;
static void WINAPI fiber_proc(void *param)
{
    fiber_param = param;
    SwitchToFiber(main_fiber);
}

static void check_fiber(void)
{
    char line[] = "p1w parent: fiber data=XXXXXXXX current=XXXXXXXX fs10=XXXXXXXX converted=XXXXXXXX error=XXXXXXXX\r\n";
    void *worker;
    DWORD data, current, raw, error;
    SetLastError(0);
    main_fiber = ConvertThreadToFiber((void *)0x13572468);
    error = main_fiber ? 0 : GetLastError();
    /* On the PS5 title, fs:[0x10] holds libkernel's thread pointer while 32-bit
     * code runs; Wine keeps the fiber elsewhere. Never dereference fs:[0x10]
     * (GetFiberData); read the parameter from the returned fiber instead. */
    data = main_fiber ? *(DWORD *)main_fiber : 0;
    current = (DWORD)(ULONG_PTR)GetCurrentFiber();
    __asm__ volatile("movl %%fs:0x10,%0" : "=r"(raw));
    hex32(line + 23, data);
    hex32(line + 40, current);
    hex32(line + 54, raw);
    hex32(line + 73, (DWORD)(ULONG_PTR)main_fiber);
    hex32(line + 88, error);
    if (!write_exact(line, sizeof(line) - 1)) fail(9);
    if (!main_fiber || data != 0x13572468) fail(8);
    if (!IsThreadAFiber()) fail(30);
    if (!(worker = CreateFiber(0x10000, fiber_proc, (void *)0x2468ace0))) fail(28);
    SwitchToFiber(worker);
    if (fiber_param != (void *)0x2468ace0) fail(29);
    DeleteFiber(worker);
    if (!ConvertFiberToThread() || IsThreadAFiber()) fail(10);
}

static HANDLE context_ready, context_gate, context_target;
static DWORD suspended, got_context, resumed, context_error;
static DWORD WINAPI context_worker(void *unused)
{
    (void)unused;
    if (!SetEvent(context_ready)) return 8;
    return WaitForSingleObject(context_gate, 5000) == WAIT_OBJECT_0 ? 7 : 8;
}

static DWORD WINAPI context_controller(void *unused)
{
    CONTEXT context; /* GetThreadContext writes the selected output fields. */
    (void)unused;
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    SetLastError(0);
    suspended = SuspendThread(context_target);
    if (suspended == (DWORD)-1) context_error = GetLastError();
    SetLastError(0);
    got_context = GetThreadContext(context_target, &context);
    if (!got_context && !context_error) context_error = GetLastError();
    resumed = (DWORD)-1;
    if (suspended != (DWORD)-1) {
        SetLastError(0);
        resumed = ResumeThread(context_target);
        if (resumed == (DWORD)-1 && !context_error) context_error = GetLastError();
    }
    return 0;
}

static void check_context(void)
{
    HANDLE controller;
    DWORD code;
    context_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    context_gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!context_ready || !context_gate) fail(11);
    context_target = CreateThread(NULL, 0, context_worker, NULL, 0, NULL);
    if (!context_target || WaitForSingleObject(context_ready, 1000) != WAIT_OBJECT_0) fail(12);
    /* Bound even synchronous server suspension/context requests. */
    controller = CreateThread(NULL, 0, context_controller, NULL, 0, NULL);
    if (!controller || WaitForSingleObject(controller, 3000) != WAIT_OBJECT_0) fail(13);
    if (suspended != 0) fail(14);
    if (!got_context) fail(15);
    if (resumed != 1) fail(16);
    if (!SetEvent(context_gate) || WaitForSingleObject(context_target, 1000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(context_target, &code) || code != 7) fail(17);
    if (!CloseHandle(controller) || !CloseHandle(context_target) ||
        !CloseHandle(context_ready) || !CloseHandle(context_gate)) fail(18);
}

static void check_win32k(void)
{
    typedef int (WINAPI *get_metrics_fn)(int);
    HMODULE user = LoadLibraryW(L"user32.dll");
    get_metrics_fn get_metrics;
    /* Do not import user32 at PE startup: missing win32u Unix admission must
     * be visible as this step's failure, not an exit before mainCRTStartup. */
    if (!user) fail(20);
    get_metrics = (get_metrics_fn)(ULONG_PTR)GetProcAddress(user, "GetSystemMetrics");
    if (!get_metrics || get_metrics(SM_CXBORDER) != 1) fail(21);
    if (!FreeLibrary(user)) fail(22);
}

void mainCRTStartup(void)
{
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    WCHAR command[] = L"C:\\psmkit-wine-child.exe";
    static const char success[] = "p1w parent: pe32 seh=ok threads=2 fiber=ok context=ok win32k=ok child exit=5\r\n";
    DWORD code;
    if (sizeof(void *) != 4) fail(1);
    if (run_seh(seh_handler) != 1) fail(3);
    check_threads();
    check_fiber();
    check_context();
    check_win32k();
    startup.cb = sizeof(startup);
    if (!CreateProcessW(command, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) fail(23);
    if (WaitForSingleObject(process.hProcess, 20000) != WAIT_OBJECT_0) fail(24);
    if (!GetExitCodeProcess(process.hProcess, &code) || code != 5) fail(25);
    if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess)) fail(26);
    if (!write_exact(success, sizeof(success) - 1)) fail(27);
    ExitProcess(0);
}
