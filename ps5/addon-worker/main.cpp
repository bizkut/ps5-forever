// Forever Launcher - native add-on archive worker payload server.
// Copyright (C) 2026 bizkut
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon-worker.hpp"
#include <algorithm>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <memory>
#include <new>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef FOREVER_ADDON_WORKER_CONSOLE
#include <ps5/klog.h>
#endif

namespace
{
using namespace forever::addon_worker;
constexpr std::size_t kHelloBytes = AW_HELLO_BYTES;
constexpr std::size_t kRequestBytes = AW_REQUEST_BYTES;
constexpr std::size_t kReplyBytes = AW_REPLY_BYTES;
std::array<char, kTokenBytes> token{};
std::string root_path = kNativeRoot;
std::string wow_root_path = kNativeWowRoot;
std::atomic<bool> stopping{false};
std::atomic<unsigned> active_connections{0};
pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
constexpr unsigned kMaxConnections = 16;
constexpr std::uint64_t kHandshakeMs = 5000;
constexpr std::uint64_t kFrameMs = 30000;
constexpr std::uint64_t kJobHandoffMs = 2000;
constexpr std::uint64_t kOperationMs = 15 * 60 * 1000;

std::uint64_t monotonic_ms()
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1000 +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000000;
}

struct ConnectionLease
{
    int descriptor;
    std::uint64_t handshake_deadline;
    ~ConnectionLease()
    {
        close(descriptor);
        active_connections.fetch_sub(1);
    }
};

void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
#ifdef FOREVER_ADDON_WORKER_CONSOLE
    klog_printf("addon-worker: %s\n", text);
#else
    std::fprintf(stderr, "addon-worker: %s\n", text);
#endif
}

std::uint32_t get_u32(const unsigned char *bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t get_u64(const unsigned char *bytes)
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    return value;
}

void put_u32(unsigned char *bytes, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
        bytes[index] = static_cast<unsigned char>(value >> (index * 8));
}

void put_u64(unsigned char *bytes, std::uint64_t value)
{
    for (unsigned index = 0; index < 8; ++index)
        bytes[index] = static_cast<unsigned char>(value >> (index * 8));
}

// Every transfer has one absolute budget: a trickle never renews it.
bool transfer(int descriptor, void *data, std::size_t size, bool sending,
              std::uint64_t deadline = 0)
{
    if (!deadline)
        deadline = monotonic_ms() + kFrameMs;
    auto *bytes = static_cast<unsigned char *>(data);
    std::size_t done = 0;
    while (done < size)
    {
        const auto now = monotonic_ms();
        if (!now || now >= deadline || (stopping.load() && !sending))
            return false;
        pollfd event{descriptor, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
        const int ready = poll(&event, 1, static_cast<int>(
            std::min<std::uint64_t>(deadline - now, 250)));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0 && (event.revents & (POLLERR | POLLNVAL))))
            return false;
        if (ready == 0)
            continue;
        const auto count = sending
                               ? send(descriptor, bytes + done, size - done, MSG_NOSIGNAL)
                               : recv(descriptor, bytes + done, size - done, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

struct Request
{
    Command command = Command::probe;
    std::string id;
    std::uint64_t limit = 0;
    std::uint64_t expected_size = 0;
    Digest digest{};
};

bool read_request(int client, Request &request)
{
    std::array<unsigned char, kRequestBytes> bytes{};
    if (!transfer(client, bytes.data(), bytes.size(), false) || get_u32(bytes.data()) != kMagic)
        return false;
    request.command = static_cast<Command>(get_u32(bytes.data() + 4));
    const char *id = reinterpret_cast<const char *>(bytes.data() + 8);
    std::size_t length = 0;
    while (length < kOperationIdBytes && id[length])
        ++length;
    request.id.assign(id, length);
    request.limit = get_u64(bytes.data() + 40);
    request.expected_size = get_u64(bytes.data() + 48);
    std::memcpy(request.digest.data(), bytes.data() + 56, request.digest.size());
    return true;
}

bool send_reply(int client, Phase phase, int result, std::uint64_t done = 0,
                std::uint64_t total = 0, const ExtractTimes *times = nullptr,
                const Digest *digest = nullptr, const std::string &path = {})
{
    std::array<unsigned char, kReplyBytes> bytes{};
    put_u32(bytes.data(), kMagic);
    put_u32(bytes.data() + 4, kVersion);
    put_u32(bytes.data() + 8, static_cast<std::uint32_t>(phase));
    put_u32(bytes.data() + 12, static_cast<std::uint32_t>(result));
    put_u64(bytes.data() + 16, done);
    put_u64(bytes.data() + 24, total);
    if (times)
    {
        put_u64(bytes.data() + 32, times->files);
        put_u64(bytes.data() + 40, times->write_ms);
        put_u64(bytes.data() + 48, times->sync_ms);
        put_u64(bytes.data() + 56, times->total_ms);
    }
    if (digest)
        std::memcpy(bytes.data() + 64, digest->data(), digest->size());
    if (path.size() > 4096)
        return false;
    put_u32(bytes.data() + 96, static_cast<std::uint32_t>(path.size()));
    const auto deadline = monotonic_ms() + kFrameMs;
    return transfer(client, bytes.data(), bytes.size(), true, deadline) &&
           (path.empty() || transfer(client, const_cast<char *>(path.data()),
                                     path.size(), true, deadline));
}

bool read_exact(int descriptor, void *data, std::size_t size)
{
    std::size_t done = 0;
    while (done < size)
    {
        const auto count = read(descriptor, static_cast<char *>(data) + done, size - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

bool valid_token()
{
    return std::all_of(token.begin(), token.end(), [](unsigned char value)
    { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

bool read_startup_token()
{
#ifdef FOREVER_ADDON_WORKER_CONSOLE
    // elfldr duplicates its accepted socket onto stdin after consuming the ELF.
    // The sender places this frame at the loader's exact section-derived extent.
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0)
        return false;
    std::array<unsigned char, AW_STARTUP_BYTES> startup{};
    if (!transfer(STDIN_FILENO, startup.data(), startup.size(), false,
                  monotonic_ms() + kHandshakeMs) ||
        get_u32(startup.data()) != kMagic || get_u32(startup.data() + 4) != kVersion)
        return false;
    std::memcpy(token.data(), startup.data() + 8, token.size());
#else
    const char *configured = std::getenv("FOREVER_ADDON_WORKER_TOKEN");
    if (!configured || std::strlen(configured) != token.size())
        return false;
    std::memcpy(token.data(), configured, token.size());
#endif
    return valid_token();
}

bool make_root()
{
    if (root_path.empty() || root_path.front() != '/')
        return false;
    int parent = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0)
        return false;
    std::size_t start = 1;
    for (;;)
    {
        const auto slash = root_path.find('/', start);
        const auto component = root_path.substr(start, slash - start);
        if (component.empty() || component == "." || component == "..")
        {
            close(parent);
            return false;
        }
        if (slash == root_path.npos)
        {
            if (mkdirat(parent, component.c_str(), 0700) != 0 && errno != EEXIST)
            {
                close(parent);
                return false;
            }
            break;
        }
        const int next = openat(parent, component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(parent);
        if (next < 0)
            return false;
        parent = next;
        start = slash + 1;
    }
    const auto leaf = root_path.substr(start);
    const int root = openat(parent, leaf.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    struct stat info{};
    // Never adopt a title-owned or writable replacement. Parent-directory
    // mutation may detach this inode, but cannot redirect descriptor operations.
    const bool ok = root >= 0 && fstat(root, &info) == 0 && S_ISDIR(info.st_mode) &&
                    info.st_uid == geteuid() && (info.st_mode & 0022) == 0 &&
                    fchmod(root, 0711) == 0;
    if (root >= 0)
        close(root);
    return ok;
}

bool random_bytes(void *data, std::size_t size)
{
    const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    const bool ok = read_exact(descriptor, data, size);
    close(descriptor);
    return ok;
}

Digest authentication_tag(const unsigned char challenge[AW_CHALLENGE_BYTES],
                          unsigned char domain)
{
    std::array<unsigned char, 64> inner_pad{}, outer_pad{};
    for (std::size_t index = 0; index < inner_pad.size(); ++index)
    {
        const auto key = static_cast<unsigned char>(token[index]);
        inner_pad[index] = key ^ 0x36u;
        outer_pad[index] = key ^ 0x5cu;
    }
    sha256_context hash{};
    Digest inner{}, result{};
    sha256_init(&hash);
    sha256_update(&hash, inner_pad.data(), inner_pad.size());
    sha256_update(&hash, challenge, AW_CHALLENGE_BYTES);
    sha256_update(&hash, &domain, 1);
    sha256_final(&hash, inner.data());
    sha256_init(&hash);
    sha256_update(&hash, outer_pad.data(), outer_pad.size());
    sha256_update(&hash, inner.data(), inner.size());
    sha256_final(&hash, result.data());
    return result;
}

bool secure_equal(const unsigned char *left, const unsigned char *right, std::size_t size)
{
    unsigned difference = 0;
    for (std::size_t index = 0; index < size; ++index)
        difference |= left[index] ^ right[index];
    return difference == 0;
}

std::string windows_path(const std::string &id, const char *leaf)
{
    return std::string(kWindowsRoot) + "\\" + id + "\\" + leaf;
}
bool cancellation_pending(int client)
{
    unsigned char byte;
    const auto count = recv(client, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return count >= 0 ||
           (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK);
}

bool lease_revoked_now()
{
    if (stopping.load())
        return true;
#ifdef FOREVER_ADDON_WORKER_CONSOLE
    pollfd lease{STDIN_FILENO, POLLIN | POLLHUP | POLLERR, 0};
    int ready;
    do
        ready = poll(&lease, 1, 0);
    while (ready < 0 && errno == EINTR);
    if (ready < 0 || (lease.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
    {
        stopping = true;
        return true;
    }
#endif
    return false;
}

int acquire_job(int client)
{
    // COMPLETE is delivered before the previous handler finishes unwinding.
    // Permit a bounded internal handoff, without exposing retry obligations to
    // the single-core client or letting a genuinely stalled owner queue forever.
    const auto deadline = monotonic_ms() + kJobHandoffMs;
    for (;;)
    {
        const auto now = monotonic_ms();
        if (!now || lease_revoked_now())
            return -ECANCELED;
        unsigned char byte;
        const auto count = recv(client, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count == 0 ||
            (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return -ECANCELED;
        // Positive reads are a queued STORE/ACTIVATE payload, not cancellation.
        // Leave every byte untouched for the handler after ownership transfers.
        if (now >= deadline)
            return -EBUSY;
        const int result = pthread_mutex_trylock(&job_lock);
        if (result == 0)
            return 0;
        if (result != EBUSY)
            return -result;
        timespec pause{0, 10000000};
        nanosleep(&pause, nullptr);
    }
}

// The native service owns abandoned sessions, not a destructor in the core.
// Recovery refuses deletion if an activation backup cannot be restored safely.
struct SessionCleanup
{
    const std::string &id;
    bool keep = false;
    ~SessionCleanup()
    {
        if (!keep)
        {
            std::string error;
            if (!clean_session(root_path, wow_root_path, id, error))
                log_line("cleanup failed id=%s error=%s", id.c_str(), error.c_str());
        }
    }
};

bool handle_store(int client, const Request &request)
{
    std::string error;
    ArchiveStore store(root_path, request.id, request.limit, error);
    if (!store.ready())
        return send_reply(client, Phase::complete, -EINVAL);
    if (!send_reply(client, Phase::ready, 0))
        return false;
    auto buffer = std::unique_ptr<std::array<unsigned char, kChunkBytes>>(
        new (std::nothrow) std::array<unsigned char, kChunkBytes>);
    if (!buffer)
        return send_reply(client, Phase::complete, -ENOMEM);
    bool ok = true;
    const auto overall_deadline = monotonic_ms() + kOperationMs;
    for (;;)
    {
        const auto frame_deadline = std::min(overall_deadline, monotonic_ms() + kFrameMs);
        unsigned char length_bytes[4];
        if (!transfer(client, length_bytes, sizeof(length_bytes), false, frame_deadline))
            return false;
        const auto length = get_u32(length_bytes);
        if (length == 0)
            break;
        if (length > buffer->size() ||
            !transfer(client, buffer->data(), length, false, frame_deadline) ||
            !store.write(buffer->data(), length, error))
        {
            ok = false;
            break;
        }
    }
    std::array<unsigned char, 40> trailer{};
    if (!ok || !transfer(client, trailer.data(), trailer.size(), false,
                         std::min(overall_deadline, monotonic_ms() + kFrameMs)))
        return false;
    const std::uint64_t expected_size = get_u64(trailer.data());
    Digest expected{}, actual{};
    std::memcpy(expected.data(), trailer.data() + 8, expected.size());
    if (!store.finish(expected_size, expected, actual, error))
    {
        log_line("store failed id=%s error=%s", request.id.c_str(), error.c_str());
        return send_reply(client, Phase::complete, -EIO);
    }
    SessionCleanup cleanup{request.id};
    if (cancellation_pending(client))
        return send_reply(client, Phase::complete, -ECANCELED);
    log_line("stored id=%s bytes=%llu sha256=%s", request.id.c_str(),
             static_cast<unsigned long long>(expected_size), digest_hex(actual).c_str());
    cleanup.keep = send_reply(client, Phase::complete, 0, expected_size, expected_size,
                              nullptr, &actual, windows_path(request.id, "archive.zip"));
    return cleanup.keep;
}

struct CancelWatch
{
    int client;
    std::uint64_t deadline = monotonic_ms() + kOperationMs;
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};
};


void *watch_cancel(void *opaque)
{
    auto &watch = *static_cast<CancelWatch *>(opaque);
    pollfd descriptor{watch.client, POLLIN | POLLHUP | POLLERR, 0};
    while (!watch.done.load())
    {
        if (lease_revoked_now() || monotonic_ms() >= watch.deadline)
        {
            watch.cancelled = true;
            return nullptr;
        }
        const int ready = poll(&descriptor, 1, 200);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 ||
            (ready > 0 && (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))))
        {
            watch.cancelled = true;
            return nullptr;
        }
        if (ready > 0 && (descriptor.revents & POLLIN))
        {
            unsigned char command = 0;
            const auto count = recv(watch.client, &command, 1, 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            // Any input during extraction is cancellation; no other frames are valid.
            watch.cancelled = true;
            return nullptr;
        }
    }
    return nullptr;
}

bool handle_extract(int client, const Request &request)
{
    SessionCleanup cleanup{request.id};
    std::string error;
    if (!send_reply(client, Phase::ready, 0))
        return false;
    CancelWatch watch{client};
    pthread_t watcher{};
    const bool watching = pthread_create(&watcher, nullptr, watch_cancel, &watch) == 0;
    if (!watching)
        return send_reply(client, Phase::complete, -EAGAIN);
    ExtractTimes times;
    const bool extracted = extract_session(
        root_path, request.id, request.expected_size, request.digest, watch.cancelled,
        [&](std::uint64_t done, std::uint64_t total)
        {
            if (!send_reply(client, Phase::progress, 0, done, total))
                watch.cancelled = true;
        },
        times, error);
    watch.done = true;
    pthread_join(watcher, nullptr);
    if (cancellation_pending(client))
        watch.cancelled = true;
    if (!extracted || watch.cancelled.load())
    {
        log_line("extract failed id=%s error=%s", request.id.c_str(), error.c_str());
        return send_reply(client, Phase::complete, watch.cancelled ? -ECANCELED : -EIO);
    }
    log_line("extracted id=%s files=%llu bytes=%llu write_ms=%llu sync_ms=%llu total_ms=%llu",
             request.id.c_str(), static_cast<unsigned long long>(times.files),
             static_cast<unsigned long long>(times.bytes),
             static_cast<unsigned long long>(times.write_ms),
             static_cast<unsigned long long>(times.sync_ms),
             static_cast<unsigned long long>(times.total_ms));
    cleanup.keep = send_reply(client, Phase::complete, 0, times.files, times.files, &times,
                              nullptr, windows_path(request.id, "x"));
    return cleanup.keep;
}

bool read_activation_payload(int client, std::string &channel,
                             std::vector<ActivationEntry> &entries)
{
    const auto deadline = monotonic_ms() + kFrameMs;
    std::array<unsigned char, 8> header{};
    if (!transfer(client, header.data(), header.size(), false, deadline))
        return false;
    const auto channel_length = get_u32(header.data());
    const auto count = get_u32(header.data() + 4);
    if (channel_length < 3 || channel_length > 64 || count == 0 ||
        count > kMaxActivationFolders)
        return false;
    channel.resize(channel_length);
    if (!transfer(client, channel.data(), channel.size(), false, deadline))
        return false;
    entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        std::array<unsigned char, 8> lengths{};
        if (!transfer(client, lengths.data(), lengths.size(), false, deadline))
            return false;
        const auto source_length = get_u32(lengths.data());
        const auto name_length = get_u32(lengths.data() + 4);
        if (source_length == 0 || source_length > kMaxArchivePath ||
            name_length == 0 || name_length > 255)
            return false;
        ActivationEntry entry;
        entry.source.resize(source_length);
        entry.name.resize(name_length);
        if (!transfer(client, entry.source.data(), entry.source.size(), false, deadline) ||
            !transfer(client, entry.name.data(), entry.name.size(), false, deadline))
            return false;
        entries.push_back(std::move(entry));
    }
    return true;
}

bool handle_activate(int client, const Request &request)
{
    SessionCleanup cleanup{request.id};
    const auto deadline = monotonic_ms() + kOperationMs;
    const auto cancelled = [client, deadline]
    {
        const auto now = monotonic_ms();
        return cancellation_pending(client) || lease_revoked_now() || !now || now >= deadline;
    };
    std::string channel, error;
    std::vector<ActivationEntry> entries;
    if (!read_activation_payload(client, channel, entries))
        return send_reply(client, Phase::complete, -EINVAL);
    if (cancelled())
        return send_reply(client, Phase::complete, -ECANCELED);
    std::uint64_t move_ms = 0;
    if (!activate_session(root_path, wow_root_path, request.id, channel, entries,
                          cancelled, move_ms, error))
    {
        log_line("activate failed id=%s error=%s", request.id.c_str(), error.c_str());
        return send_reply(client, Phase::complete, cancelled() ? -ECANCELED : -EIO);
    }
    ExtractTimes times{};
    times.files = entries.size();
    times.write_ms = move_ms;
    log_line("activated id=%s channel=%s folders=%zu move_ms=%llu", request.id.c_str(),
             channel.c_str(), entries.size(), static_cast<unsigned long long>(move_ms));
    cleanup.keep = send_reply(client, Phase::complete, 0, entries.size(), entries.size(), &times);
    return cleanup.keep;
}

void *serve_client(void *opaque)
{
    std::unique_ptr<ConnectionLease> lease(static_cast<ConnectionLease *>(opaque));
    const int client = lease->descriptor;
    const auto deadline = lease->handshake_deadline;
    std::array<unsigned char, kHelloBytes> hello{};
    std::array<unsigned char, AW_CHALLENGE_REPLY_BYTES> challenge_reply{};
    std::array<unsigned char, AW_AUTH_BYTES> authentication{};
    std::array<unsigned char, AW_AUTH_REPLY_BYTES> authentication_reply{};
    if (!transfer(client, hello.data(), hello.size(), false, deadline) ||
        get_u32(hello.data()) != kMagic || get_u32(hello.data() + 4) != kVersion ||
        !random_bytes(challenge_reply.data() + 8, AW_CHALLENGE_BYTES))
        return nullptr;
    put_u32(challenge_reply.data(), kMagic);
    put_u32(challenge_reply.data() + 4, kVersion);
    if (!transfer(client, challenge_reply.data(), challenge_reply.size(), true, deadline) ||
        !transfer(client, authentication.data(), authentication.size(), false, deadline))
        return nullptr;
    const auto expected_client = authentication_tag(challenge_reply.data() + 8, 'C');
    const int result = secure_equal(authentication.data(), expected_client.data(),
                                    expected_client.size()) ? 0 : -EACCES;
    put_u32(authentication_reply.data(), kMagic);
    put_u32(authentication_reply.data() + 4, kVersion);
    put_u32(authentication_reply.data() + 8, static_cast<std::uint32_t>(result));
    // Do not grant unauthenticated callers even a server proof.
    if (!result)
    {
        const auto server_proof = authentication_tag(challenge_reply.data() + 8, 'S');
        std::memcpy(authentication_reply.data() + 12, server_proof.data(), server_proof.size());
    }
    if (!transfer(client, authentication_reply.data(), authentication_reply.size(), true,
                  deadline) || result)
        return nullptr;
    Request request;
    if (!read_request(client, request))
        return nullptr;
    bool ok = false;
    if (request.command == Command::probe)
        ok = send_reply(client, Phase::complete, 0);
    else if (request.command == Command::quit)
    {
        ok = send_reply(client, Phase::complete, 0);
        stopping = true;
    }
    else
    {
        const int acquired = acquire_job(client);
        if (acquired != 0)
            ok = send_reply(client, Phase::complete, acquired);
        else
        {
            if (request.command == Command::store && valid_operation_id(request.id))
                ok = handle_store(client, request);
            else if (request.command == Command::extract && valid_operation_id(request.id))
                ok = handle_extract(client, request);
            else if (request.command == Command::activate && valid_operation_id(request.id))
                ok = handle_activate(client, request);
            else if (request.command == Command::clean && valid_operation_id(request.id))
            {
                std::string error;
                ok = send_reply(client, Phase::complete,
                                clean_session(root_path, wow_root_path, request.id, error) ? 0 : -EIO);
            }
            else if (request.command == Command::clean_all)
            {
                std::string error;
                ok = send_reply(client, Phase::complete,
                                clean_all_sessions(root_path, wow_root_path, error) ? 0 : -EIO);
            }
            else
                ok = send_reply(client, Phase::complete, -EINVAL);
            pthread_mutex_unlock(&job_lock);
        }
    }
    if (!ok)
        log_line("request failed command=%u id=%s", static_cast<unsigned>(request.command),
                 request.id.c_str());
    return nullptr;
}
}

int main()
{
    using namespace forever::addon_worker;
    std::signal(SIGPIPE, SIG_IGN);
#ifdef FOREVER_ADDON_WORKER_CONSOLE
    log_line("identity uid=%u euid=%u", static_cast<unsigned>(getuid()),
             static_cast<unsigned>(geteuid()));
    if (geteuid() != 0)
    {
        log_line("startup refused: privileged helper identity required");
        return 1;
    }
#endif
#ifndef FOREVER_ADDON_WORKER_CONSOLE
    if (const char *configured = std::getenv("FOREVER_ADDON_WORKER_ROOT");
        configured && configured[0] == '/')
    {
        root_path = configured;
    }
    if (const char *configured = std::getenv("FOREVER_ADDON_WORKER_WOW_ROOT");
        configured && configured[0] == '/')
        wow_root_path = configured;
#endif
    if (!read_startup_token() || !make_root())
    {
        log_line("startup refused: authenticated configuration or staging root unavailable");
        return 1;
    }
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        return 1;
    int enabled = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(server, 8) != 0)
    {
        log_line("listen failed errno=%d", errno);
        close(server);
        return 1;
    }
    std::string clean_error;
    if (!clean_all_sessions(root_path, wow_root_path, clean_error))
    {
        log_line("startup cleanup failed: %s", clean_error.c_str());
        close(server);
        return 1;
    }
    log_line("listening on 127.0.0.1:%u version=%u", kPort, kVersion);
    while (!stopping.load())
    {
        pollfd descriptors[] = {
            {server, POLLIN, 0},
#ifdef FOREVER_ADDON_WORKER_CONSOLE
            {STDIN_FILENO, POLLIN | POLLHUP | POLLERR, 0},
#endif
        };
        const int ready = poll(descriptors, sizeof(descriptors) / sizeof(descriptors[0]), 250);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
        {
            stopping = true;
            break;
        }
#ifdef FOREVER_ADDON_WORKER_CONSOLE
        // The title retains this loader connection as the worker lifetime lease.
        // EOF, errors, or any unexpected extra bytes revoke the worker key.
        if (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
        {
            log_line("startup lease ended");
            stopping = true;
            break;
        }
#endif
        if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL))
        {
            stopping = true;
            break;
        }
        if (!(descriptors[0].revents & POLLIN))
            continue;
        const int client = accept(server, nullptr, nullptr);
        if (client < 0)
            continue;
        if (active_connections.fetch_add(1) >= kMaxConnections)
        {
            active_connections.fetch_sub(1);
            close(client);
            continue;
        }
        const int flags = fcntl(client, F_GETFL, 0);
        auto *lease = new (std::nothrow) ConnectionLease{
            client, monotonic_ms() + kHandshakeMs};
        if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) != 0 || !lease)
        {
            if (lease)
                delete lease;
            else
            {
                close(client);
                active_connections.fetch_sub(1);
            }
            continue;
        }
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        pthread_t thread{};
        if (pthread_create(&thread, nullptr, serve_client, lease) == 0)
            pthread_detach(thread);
        else
            delete lease;
    }
    // Let owned STORE/EXTRACT handlers observe stop and clean their sessions.
    // Other partial frames also stop reading; sends retain their bounded budget.
    while (active_connections.load() != 0)
    {
        timespec pause{0, 10000000};
        nanosleep(&pause, nullptr);
    }
    // Retain exclusive ownership of the port until every session cleanup has
    // completed: connection refusal is the title's proof that restart is safe.
    close(server);
    log_line("stopped");
    return 0;
}
