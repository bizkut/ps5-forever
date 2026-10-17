/* Small maintenance-role Windows parent: starts Battle.net, and starts/stops
 * addonforge-core only while the native Add-ons page is open. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "addonforge-control.h"

#define CONTROL_PORT 31980
#define CORE_PORT 31981
#define CORE_STOP_TIMEOUT_MS 720000

static HANDLE core_process;
static char control_secret[ADDONFORGE_CONTROL_SECRET_HEX + 1];
static char core_token[ADDONFORGE_CORE_TOKEN_MAX + 1];

static int send_all(SOCKET socket, const char *data, size_t size)
{
    while (size)
    {
        int count = send(socket, data, (int)size, 0);
        if (count <= 0) return 0;
        data += count;
        size -= (size_t)count;
    }
    return 1;
}

static enum addonforge_shutdown_delivery request_core_shutdown(void)
{
    struct sockaddr_in address;
    char request[768], response[1024];
    static const char body[] = "{\"command\":\"shutdown\",\"args\":{}}";
    const ULONGLONG delivery_deadline = GetTickCount64() + 15000;
    int length;
    if (!core_token[0]) return ADDONFORGE_SHUTDOWN_UNDELIVERED;
    length = snprintf(request, sizeof(request),
        "POST /command HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        CORE_PORT, core_token, sizeof(body) - 1, body);
    if (length < 0 || (size_t)length >= sizeof(request))
        return ADDONFORGE_SHUTDOWN_UNDELIVERED;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(CORE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    while (GetTickCount64() < delivery_deadline)
    {
        DWORD timeout = 2000;
        SOCKET socket_fd;
        size_t received = 0;
        if (WaitForSingleObject(core_process, 0) == WAIT_OBJECT_0)
            return ADDONFORGE_SHUTDOWN_ACKNOWLEDGED;
        socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_fd == INVALID_SOCKET) { Sleep(100); continue; }
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
        if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) ||
            !send_all(socket_fd, request, (size_t)length))
        {
            closesocket(socket_fd);
            Sleep(100);
            continue;
        }

        timeout = CORE_STOP_TIMEOUT_MS;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        for (;;)
        {
            int count = recv(socket_fd, response + received,
                             (int)sizeof(response) - 1 - (int)received, 0);
            if (count == 0) break;
            if (count < 0 || received + (size_t)count >= sizeof(response) - 1)
            {
                closesocket(socket_fd);
                return ADDONFORGE_SHUTDOWN_DELIVERED;
            }
            received += (size_t)count;
        }
        response[received] = 0;
        closesocket(socket_fd);
        if (strstr(response, "HTTP/1.1 200 ") && strstr(response, "\"ok\":true"))
            return ADDONFORGE_SHUTDOWN_ACKNOWLEDGED;
        return ADDONFORGE_SHUTDOWN_DELIVERED;
    }
    return ADDONFORGE_SHUTDOWN_UNDELIVERED;
}

static DWORD stop_core(void)
{
    enum addonforge_shutdown_delivery delivery;
    DWORD wait;
    if (!core_process) return ERROR_SUCCESS;
    delivery = request_core_shutdown();
    if (!addonforge_shutdown_should_wait(delivery))
    {
        TerminateProcess(core_process, 1);
        WaitForSingleObject(core_process, 5000);
        wait = WAIT_TIMEOUT;
    }
    else
    {
        wait = WaitForSingleObject(core_process, CORE_STOP_TIMEOUT_MS);
        if (wait != WAIT_OBJECT_0)
        {
            TerminateProcess(core_process, 1);
            WaitForSingleObject(core_process, 5000);
        }
    }
    CloseHandle(core_process);
    core_process = NULL;
    SecureZeroMemory(core_token, sizeof(core_token));
    return wait == WAIT_OBJECT_0 ? ERROR_SUCCESS : WAIT_TIMEOUT;
}

static int valid_token(const char *token)
{
    return addonforge_hex_token(token, 32, ADDONFORGE_CORE_TOKEN_MAX);
}

static DWORD start_core(const char *token)
{
    static const char executable[] = "C:\\Forever\\Addons\\addonforge-core.exe";
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    char command[512];

    if (!valid_token(token)) return ERROR_INVALID_PARAMETER;
    if (core_process)
    {
        if (WaitForSingleObject(core_process, 0) != WAIT_OBJECT_0) return ERROR_BUSY;
        CloseHandle(core_process);
        core_process = NULL;
        SecureZeroMemory(core_token, sizeof(core_token));
    }
    if (snprintf(command, sizeof(command),
                 "\"%s\" serve --port %u --token %s", executable, CORE_PORT, token) < 0)
        return ERROR_INVALID_PARAMETER;
    memset(&process, 0, sizeof(process));
    if (!CreateProcessA(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process))
        return GetLastError();
    CloseHandle(process.hThread);
    core_process = process.hProcess;
    snprintf(core_token, sizeof(core_token), "%s", token);
    return ERROR_SUCCESS;
}

static void answer(SOCKET socket, const char *text)
{
    send_all(socket, text, strlen(text));
}

static void serve(SOCKET client)
{
    struct addonforge_control_request request;
    char line[256], response[48];
    const ULONGLONG deadline = GetTickCount64() + 2000;
    int used = 0, count, error;
    while (used < (int)sizeof(line) - 1)
    {
        DWORD timeout = addonforge_deadline_remaining(GetTickCount64(), deadline);
        if (!timeout) return;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        count = recv(client, line + used, (int)sizeof(line) - 1 - used, 0);
        if (count <= 0) return;
        used += count;
        if (memchr(line, '\n', (size_t)used)) break;
    }
    if (!memchr(line, '\n', (size_t)used)) return;
    line[used] = 0;
    char *end = strpbrk(line, "\r\n");
    if (end) *end = 0;
    error = addonforge_control_parse(line, control_secret, &request);
    if (error)
    {
        snprintf(response, sizeof(response), "ERR %d\n", -error);
        answer(client, response);
        return;
    }
    if (request.action == ADDONFORGE_CONTROL_STOP)
        error = (int)stop_core();
    else
        error = (int)start_core(request.core_token);
    if (!error) answer(client, "OK\n");
    else
    {
        snprintf(response, sizeof(response), "ERR %d\n", error);
        answer(client, response);
    }
}

static HANDLE start_battlenet(void)
{
    static const char explorer[] = "C:\\windows\\system32\\explorer.exe";
    STARTUPINFOA startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    char command[] =
        "explorer.exe /desktop \"C:\\Program Files (x86)\\Battle.net\\Battle.net Launcher.exe\" "
        "\"--exec=launch_uid wow_classic_beta\"";
    memset(&process, 0, sizeof(process));
    if (!CreateProcessA(explorer, command, NULL, NULL, FALSE, 0, NULL,
                        "C:\\Program Files (x86)\\Battle.net", &startup, &process))
        return NULL;
    CloseHandle(process.hThread);
    return process.hProcess;
}

int main(void)
{
    WSADATA winsock;
    HANDLE battlenet;
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    int reuse = 1;
    DWORD secret_length;
    int control_available;

    secret_length = GetEnvironmentVariableA(
        "FOREVER_ADDONS_CONTROL_TOKEN", control_secret, sizeof(control_secret));
    SetEnvironmentVariableA("FOREVER_ADDONS_CONTROL_TOKEN", NULL);
    control_available = addonforge_control_secret_available(control_secret, secret_length);
    battlenet = start_battlenet();
    if (!battlenet) return 1;
    if (!control_available)
    {
        while (WaitForSingleObject(battlenet, 250) == WAIT_TIMEOUT) {}
        goto done;
    }
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) goto done;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) goto done_winsock;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(CONTROL_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 2))
        goto done_winsock;

    for (;;)
    {
        fd_set reads;
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
        if (WaitForSingleObject(battlenet, 0) == WAIT_OBJECT_0) break;
        FD_ZERO(&reads);
        FD_SET(listener, &reads);
        if (select(0, &reads, NULL, NULL, &timeout) > 0 && FD_ISSET(listener, &reads))
        {
            SOCKET client = accept(listener, NULL, NULL);
            if (client != INVALID_SOCKET)
            {
                serve(client);
                closesocket(client);
            }
        }
    }

done_winsock:
    if (listener != INVALID_SOCKET) closesocket(listener);
    stop_core();
    WSACleanup();
done:
    SecureZeroMemory(control_secret, sizeof(control_secret));
    CloseHandle(battlenet);
    return 0;
}
