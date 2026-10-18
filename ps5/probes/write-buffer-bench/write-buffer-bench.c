#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHUNK (64u * 1024u)
#define DEFAULT_MIB 128u
#define QUERY_INTERVAL (8u * 1024u * 1024u)
#define RESULT_PATH "C:\\write-buffer-bench-result.txt"

static unsigned char bytes[MAX_CHUNK];
static unsigned char verify_bytes[MAX_CHUNK];

static void write_result(const char *line)
{
    DWORD written;
    HANDLE file = CreateFileA(RESULT_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static void fail(const char *operation)
{
    char line[256];
    snprintf(line, sizeof(line), "WRITE_BUFFER_BENCH FAIL operation=%s error=%lu\n",
             operation, GetLastError());
    write_result(line);
    fputs(line, stderr);
    ExitProcess(1);
}

static void make_directory(const char *path)
{
    if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) fail(path);
}

static unsigned char pattern(uint64_t offset)
{
    return (unsigned char)((offset * UINT64_C(1315423911) + (offset >> 7) + 0x5a) & 0xff);
}

static void fill_pattern(unsigned char *buffer, DWORD size, uint64_t offset)
{
    DWORD i;
    for (i = 0; i < size; i++) buffer[i] = pattern(offset + i);
}

static void verify_at(HANDLE file, uint64_t offset, DWORD size)
{
    LARGE_INTEGER position;
    DWORD got, i;

    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN)) fail("SetFilePointerEx(interleaved)");
    if (!ReadFile(file, verify_bytes, size, &got, NULL) || got != size) fail("ReadFile(interleaved)");
    for (i = 0; i < size; i++)
        if (verify_bytes[i] != pattern(offset + i))
        {
            fprintf(stderr, "WRITE_BUFFER_BENCH FAIL verify_offset=%llu expected=%u got=%u\n",
                    (unsigned long long)(offset + i), pattern(offset + i), verify_bytes[i]);
            ExitProcess(2);
        }
}
static int parse_mib(const char *text, DWORD *mib)
{
    char *end;
    unsigned long parsed = strtoul(text, &end, 10);

    while (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t') end++;
    if (!parsed || *end || parsed > 2048) return 0;
    *mib = parsed;
    return 1;
}

static int configured_mib(DWORD *mib)
{
    char text[32];
    DWORD got;
    HANDLE file = CreateFileA("C:\\write-buffer-bench-mib.txt", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE) return 1;
    if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
    {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    text[got] = 0;
    return parse_mib(text, mib);
}


int main(int argc, char **argv)
{
    static const DWORD chunks[] = {2u * 1024u, 4u * 1024u, 8u * 1024u,
                                   16u * 1024u, 32u * 1024u, 64u * 1024u};
    static const char path[] =
        "C:\\write-buffer-bench\\World of Warcraft\\Data\\data\\data.000";
    LARGE_INTEGER frequency, before, after, size, position;
    HANDLE writer, reader;
    uint64_t total, offset = 0, next_query = QUERY_INTERVAL;
    DWORD mib = DEFAULT_MIB, written, got, chunk, i;
    double seconds, rate;
    char result_line[512];

    if ((argc > 1 && !parse_mib(argv[1], &mib)) || (argc == 1 && !configured_mib(&mib)))
    {
        fprintf(stderr, "usage: %s [MiB:1..2048]\\n", argv[0]);
        return 64;
    }
    total = (uint64_t)mib << 20;

    make_directory("C:\\write-buffer-bench");
    make_directory("C:\\write-buffer-bench\\World of Warcraft");
    make_directory("C:\\write-buffer-bench\\World of Warcraft\\Data");
    make_directory("C:\\write-buffer-bench\\World of Warcraft\\Data\\data");
    DeleteFileA("C:\\write-buffer-bench\\World of Warcraft\\Data\\data\\agent-pattern.bin");

    writer = CreateFileA(path, GENERIC_WRITE | GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (writer == INVALID_HANDLE_VALUE) fail("CreateFile(writer)");
    reader = CreateFileA(path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (reader == INVALID_HANDLE_VALUE) fail("CreateFile(reader)");

    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before))
        fail("QueryPerformanceCounter(start)");
    while (offset < total)
    {
        chunk = chunks[(offset >> 11) % (sizeof(chunks) / sizeof(chunks[0]))];
        if (chunk > total - offset) chunk = (DWORD)(total - offset);
        fill_pattern(bytes, chunk, offset);
        if (!WriteFile(writer, bytes, chunk, &written, NULL) || written != chunk) fail("WriteFile");
        offset += chunk;

        if (offset >= next_query)
        {
            if (!GetFileSizeEx(reader, &size) || (uint64_t)size.QuadPart != offset)
            {
                fprintf(stderr, "WRITE_BUFFER_BENCH FAIL size expected=%llu got=%llu error=%lu\n",
                        (unsigned long long)offset, (unsigned long long)size.QuadPart, GetLastError());
                return 3;
            }
            verify_at(reader, offset - min((uint64_t)4096, offset),
                      (DWORD)min((uint64_t)4096, offset));
            next_query += QUERY_INTERVAL;
        }
    }
    if (!FlushFileBuffers(writer)) fail("FlushFileBuffers");
    if (!QueryPerformanceCounter(&after)) fail("QueryPerformanceCounter(end)");
    seconds = (double)(after.QuadPart - before.QuadPart) / frequency.QuadPart;
    rate = mib / seconds;

    position.QuadPart = 0;
    if (!SetFilePointerEx(reader, position, NULL, FILE_BEGIN)) fail("SetFilePointerEx(verify)");
    offset = 0;
    while (offset < total)
    {
        chunk = (DWORD)min((uint64_t)MAX_CHUNK, total - offset);
        if (!ReadFile(reader, verify_bytes, chunk, &got, NULL) || got != chunk) fail("ReadFile(verify)");
        for (i = 0; i < chunk; i++)
            if (verify_bytes[i] != pattern(offset + i))
            {
                fprintf(stderr, "WRITE_BUFFER_BENCH FAIL final_offset=%llu expected=%u got=%u\n",
                        (unsigned long long)(offset + i), pattern(offset + i), verify_bytes[i]);
                return 4;
            }
        offset += chunk;
    }
    CloseHandle(reader);
    CloseHandle(writer);
    if (!DeleteFileA(path)) fail("DeleteFile(data.000)");
    if (!RemoveDirectoryA("C:\\write-buffer-bench\\World of Warcraft\\Data\\data"))
        fail("RemoveDirectory(data)");
    if (!RemoveDirectoryA("C:\\write-buffer-bench\\World of Warcraft\\Data"))
        fail("RemoveDirectory(Data)");
    if (!RemoveDirectoryA("C:\\write-buffer-bench\\World of Warcraft"))
        fail("RemoveDirectory(World of Warcraft)");
    if (!RemoveDirectoryA("C:\\write-buffer-bench"))
        fail("RemoveDirectory(write-buffer-bench)");
    snprintf(result_line, sizeof(result_line),
             "WRITE_BUFFER_BENCH PASS mib=%lu seconds=%.3f MiB_s=%.2f path=\"%s\" "
             "chunks=2K,4K,8K,16K,32K,64K query_interval_mib=8\n",
             mib, seconds, rate, path);
    write_result(result_line);
    fputs(result_line, stdout);
    return 0;
}
