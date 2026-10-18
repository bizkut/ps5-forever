#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SCAN_BENCH_WORKERS
#define SCAN_BENCH_WORKERS 4
#endif
#define WORKERS SCAN_BENCH_WORKERS
#define CHUNK (64u * 1024u)
#define FILE_MIB 1024u
#ifndef AGENT_WORKSPACE
#define AGENT_WORKSPACE (32u * 1024u * 1024u)
#endif
#define SCAN_MIB (AGENT_WORKSPACE ? 256u : FILE_MIB)
#define PASSES (AGENT_WORKSPACE ? 1u : 16u)
#define DATA_PATH "C:\\scan-bench\\data.bin"
#define RESULT_PATH "C:\\scan-bench-result.txt"

struct worker
{
    HANDLE file, port, thread;
    unsigned int index;
    uint64_t bytes, reads, pending, completions, hash_ticks, wait_ticks, allocate_ticks, free_ticks;
    unsigned char digest[16];
};
static struct worker workers[WORKERS];
static HANDLE start_event, stop_event;
static BCRYPT_ALG_HANDLE algorithm;
static LARGE_INTEGER frequency;
static volatile LONG next_chunk;
static uint64_t message_waits, message_timeouts, messages;
static unsigned int passes = PASSES;

static void result(const char *line)
{
    DWORD written;
    HANDLE file = CreateFileA(RESULT_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    fputs(line, stdout);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static _Noreturn void fail(const char *operation)
{
    char line[256];
    snprintf(line, sizeof(line), "SCAN_BENCH FAIL operation=%s error=%lu\n", operation, GetLastError());
    result(line);
    ExitProcess(1);
}

static uint64_t clock_ticks(void)
{
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) fail("QueryPerformanceCounter");
    return now.QuadPart;
}


static void create_data(void)
{
    unsigned char *buffer;
    LARGE_INTEGER size;
    DWORD written, i;
    HANDLE file;
    if (!CreateDirectoryA("C:\\scan-bench", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        fail("CreateDirectory");
    file = CreateFileA(DATA_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) fail("CreateFile(data)");
    if (!GetFileSizeEx(file, &size)) fail("GetFileSizeEx");
    if (size.QuadPart != (uint64_t)FILE_MIB << 20)
    {
        if (!(buffer = malloc(1u << 20))) fail("malloc(create)");
        for (i = 0; i < 1u << 20; i++) buffer[i] = (unsigned char)((i * 131u + (i >> 8)) ^ 0x5a);
        size.QuadPart = 0;
        if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN)) fail("SetFilePointerEx");
        for (i = 0; i < FILE_MIB; i++)
            if (!WriteFile(file, buffer, 1u << 20, &written, NULL) || written != 1u << 20)
                fail("WriteFile(create)");
        if (!SetEndOfFile(file) || !FlushFileBuffers(file)) fail("FlushFileBuffers(create)");
        free(buffer);
    }
    CloseHandle(file);
}

static DWORD WINAPI scan_worker(void *argument)
{
    struct worker *worker = argument;
    unsigned char *buffer = AGENT_WORKSPACE ? NULL : malloc(CHUNK), *object;
    BCRYPT_HASH_HANDLE hash;
    DWORD object_size, returned, bytes;
    uint64_t before;
    unsigned int chunk, chunks = SCAN_MIB * (1u << 20) / CHUNK;
    if ((!AGENT_WORKSPACE && !buffer) ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &returned, 0)) fail("BCryptGetProperty");
    object = malloc(object_size);
    if (!object || BCryptCreateHash(algorithm, &hash, object, object_size, NULL, 0, 0))
        fail("BCryptCreateHash");
    WaitForSingleObject(start_event, INFINITE);
    for (;;)
    {
        OVERLAPPED operation = {0};
        LPOVERLAPPED completed;
        ULONG_PTR key;
        chunk = (unsigned int)InterlockedIncrement(&next_chunk) - 1;
        if (chunk >= chunks * passes) break;
        if (AGENT_WORKSPACE)
        {
            before = clock_ticks();
            buffer = VirtualAlloc(NULL, AGENT_WORKSPACE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!buffer) fail("VirtualAlloc(workspace)");
            worker->allocate_ticks += clock_ticks() - before;
            if (buffer[0] || buffer[AGENT_WORKSPACE - 1]) fail("workspace-zero");
            buffer[AGENT_WORKSPACE - 1] = 0xa5;
        }
        operation.Offset = (chunk % chunks) * CHUNK;
        worker->reads++;
        if (!ReadFile(worker->file, buffer, CHUNK, &bytes, &operation))
        {
            if (GetLastError() != ERROR_IO_PENDING) fail("ReadFile");
            worker->pending++;
        }
        before = clock_ticks();
        if (worker->index & 1)
        {
            OVERLAPPED_ENTRY entry;
            ULONG count;
            if (!GetQueuedCompletionStatusEx(worker->port, &entry, 1, &count, INFINITE, FALSE) ||
                count != 1 || entry.Internal || entry.lpOverlapped != &operation ||
                entry.lpCompletionKey != worker->index + 1 || entry.dwNumberOfBytesTransferred != CHUNK)
                fail("GetQueuedCompletionStatusEx");
        }
        else if (!GetQueuedCompletionStatus(worker->port, &bytes, &key, &completed, INFINITE) ||
                 bytes != CHUNK || completed != &operation || key != worker->index + 1)
            fail("GetQueuedCompletionStatus");
        worker->wait_ticks += clock_ticks() - before;
        worker->completions++;
        before = clock_ticks();
        if (BCryptHashData(hash, buffer, CHUNK, 0)) fail("BCryptHashData");
        worker->hash_ticks += clock_ticks() - before;
        worker->bytes += CHUNK;
        if (AGENT_WORKSPACE)
        {
            before = clock_ticks();
            if (!VirtualFree(buffer, 0, MEM_RELEASE)) fail("VirtualFree(workspace)");
            worker->free_ticks += clock_ticks() - before;
        }
    }
    if (BCryptFinishHash(hash, worker->digest, sizeof(worker->digest), 0)) fail("BCryptFinishHash");
    BCryptDestroyHash(hash);
    free(object);
    if (!AGENT_WORKSPACE) free(buffer);
    return 0;
}

static DWORD WINAPI message_worker(void *argument)
{
    MSG message;
    (void)argument;
    PeekMessageA(&message, NULL, 0, 0, PM_NOREMOVE);
    WaitForSingleObject(start_event, INFINITE);
    for (;;)
    {
        DWORD status = MsgWaitForMultipleObjects(1, &stop_event, FALSE, 1, QS_ALLINPUT);
        message_waits++;
        if (status == WAIT_OBJECT_0) break;
        if (status == WAIT_TIMEOUT) message_timeouts++;
        else if (status != WAIT_OBJECT_0 + 1) fail("MsgWaitForMultipleObjects");
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) messages++;
    }
    return 0;
}

static void workspace_contract(void)
{
    MEMORY_BASIC_INFORMATION info;
    unsigned char *buffer;
    if (!AGENT_WORKSPACE) return;
    buffer = VirtualAlloc(NULL, AGENT_WORKSPACE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buffer) fail("VirtualAlloc(contract)");
    buffer[0] = 0xa5; buffer[AGENT_WORKSPACE - 1] = 0x5a;
    if (!VirtualFree(buffer, 0, MEM_RELEASE)) fail("VirtualFree(contract)");
    if (!VirtualQuery(buffer, &info, sizeof(info)) || info.State != MEM_FREE) fail("VirtualQuery(freed)");
    buffer = VirtualAlloc(NULL, AGENT_WORKSPACE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buffer || buffer[0] || buffer[AGENT_WORKSPACE - 1]) fail("VirtualAlloc(fresh-zero)");
    if (!VirtualQuery(buffer, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        info.Protect != PAGE_READWRITE) fail("VirtualQuery(reused)");
    if (!VirtualFree(buffer, 0, MEM_RELEASE)) fail("VirtualFree(reused)");
}

int main(int argc, char **argv)
{
    HANDLE threads[WORKERS], message_thread;
    uint64_t before, elapsed, bytes = 0, reads = 0, pending = 0, completions = 0;
    uint64_t hash = 0, waits = 0, allocate = 0, release = 0;
    unsigned int i;
    char line[1024];
    if (argc > 1 && !strcmp(argv[1], "--cleanup"))
    {
        if (!DeleteFileA(DATA_PATH) && GetLastError() != ERROR_FILE_NOT_FOUND) fail("DeleteFile");
        if (!RemoveDirectoryA("C:\\scan-bench") && GetLastError() != ERROR_PATH_NOT_FOUND)
            fail("RemoveDirectory");
        result("SCAN_BENCH CLEANED\n");
        return 0;
    }
    if (argc == 1)
    {
        STARTUPINFOA startup = {0};
        PROCESS_INFORMATION process;
        DWORD code;
        char command[] = "\"C:\\Agent.exe\" --child";
        startup.cb = sizeof(startup);
        if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
            fail("CreateProcess(child)");
        if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(process.hProcess, &code)) fail("Wait(child)");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return code;
    }
    if (!QueryPerformanceFrequency(&frequency)) fail("QueryPerformanceFrequency");
    create_data();
    workspace_contract();
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, NULL, 0))
        fail("BCryptOpenAlgorithmProvider");
    start_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!start_event || !stop_event) fail("CreateEvent");
    for (i = 0; i < WORKERS; i++)
    {
        struct worker *worker = &workers[i];
        worker->index = i;
        worker->file = CreateFileA(DATA_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (worker->file == INVALID_HANDLE_VALUE) fail("CreateFile(worker)");
        worker->port = CreateIoCompletionPort(worker->file, NULL, i + 1, 1);
        if (!worker->port) fail("CreateIoCompletionPort");
        threads[i] = worker->thread = CreateThread(NULL, 0, scan_worker, worker, 0, NULL);
        if (!threads[i]) fail("CreateThread(scan)");
    }
    message_thread = CreateThread(NULL, 0, message_worker, NULL, 0, NULL);
    if (!message_thread) fail("CreateThread(message)");
    /* One baseline sampler window before timed work, including live tune reload. */
    Sleep(11000);
    printf("SCAN_BENCH TIMED_BEGIN workers=%u workspace_mib=%u\n", WORKERS, AGENT_WORKSPACE >> 20);
    fflush(stdout);
    before = clock_ticks();
    SetEvent(start_event);
    if (WaitForMultipleObjects(WORKERS, threads, TRUE, INFINITE) != WAIT_OBJECT_0) fail("Wait(scan)");
    elapsed = clock_ticks() - before;
    SetEvent(stop_event);
    WaitForSingleObject(message_thread, INFINITE);
    for (i = 0; i < WORKERS; i++)
    {
        struct worker *worker = &workers[i];
        bytes += worker->bytes; reads += worker->reads; pending += worker->pending;
        completions += worker->completions; hash += worker->hash_ticks; waits += worker->wait_ticks;
        allocate += worker->allocate_ticks; release += worker->free_ticks;
        CloseHandle(worker->thread); CloseHandle(worker->port); CloseHandle(worker->file);
    }
    CloseHandle(message_thread); CloseHandle(start_event); CloseHandle(stop_event);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (bytes != (uint64_t)SCAN_MIB * passes << 20 || reads != completions) fail("counters");
    snprintf(line, sizeof(line),
             "SCAN_BENCH PASS workers=%u file_mib=%u passes=%u chunk=%u mib=%llu seconds=%.6f MiB_s=%.3f "
             "reads=%llu pending=%llu completions=%llu hash_ms=%.3f iocp_wait_ms=%.3f "
             "workspace_mib=%u allocate_ms=%.3f free_ms=%.3f "
             "message_waits=%llu message_timeouts=%llu messages=%llu scratch=retained\n",
             WORKERS, FILE_MIB, passes, CHUNK, (unsigned long long)(bytes >> 20),
             (double)elapsed / frequency.QuadPart, (bytes / 1048576.0) * frequency.QuadPart / elapsed,
             (unsigned long long)reads, (unsigned long long)pending, (unsigned long long)completions,
             1000.0 * hash / frequency.QuadPart, 1000.0 * waits / frequency.QuadPart,
             AGENT_WORKSPACE >> 20, 1000.0 * allocate / frequency.QuadPart,
             1000.0 * release / frequency.QuadPart,
             (unsigned long long)message_waits, (unsigned long long)message_timeouts,
             (unsigned long long)messages);
    result(line);
    /* Let the native sampler publish complete cumulative operation counters. */
    fflush(stdout);
    Sleep(11000);
    return 0;
}
