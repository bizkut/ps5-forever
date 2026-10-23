// Forever Launcher - bounded native add-on archive worker.
// Copyright (C) 2026 bizkut
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "protocol.h"
#include "../probes/psmkit-wine/sha256.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace forever::addon_worker
{
inline constexpr std::uint16_t kPort = AW_PORT;
inline constexpr std::uint32_t kMagic = AW_MAGIC;
inline constexpr std::uint32_t kVersion = AW_VERSION;
inline constexpr std::size_t kTokenBytes = AW_TOKEN_BYTES;
inline constexpr std::size_t kOperationIdBytes = AW_OPERATION_ID_BYTES;
inline constexpr std::size_t kChunkBytes = AW_CHUNK_BYTES;
inline constexpr std::size_t kExtractWorkers = 8;
inline constexpr std::uint64_t kMaxArchiveBytes = 4ull << 30;
inline constexpr std::uint64_t kMaxUnpackedBytes = 16ull << 30;
inline constexpr const char *kNativeWowRoot =
    "/data/forever/prefix/drive_c/Program Files (x86)/World of Warcraft";
inline constexpr std::size_t kMaxActivationFolders = 2048;
inline constexpr std::size_t kMaxArchiveEntries = 32768;
inline constexpr std::size_t kMaxArchivePath = 1024;
inline constexpr const char *kNativeRoot = AW_NATIVE_ROOT;
inline constexpr const char *kWindowsRoot = AW_WINDOWS_ROOT;

enum class Command : std::uint32_t
{
    probe = AW_COMMAND_PROBE,
    store = AW_COMMAND_STORE,
    extract = AW_COMMAND_EXTRACT,
    clean = AW_COMMAND_CLEAN,
    clean_all = AW_COMMAND_CLEAN_ALL,
    quit = AW_COMMAND_QUIT,
    activate = AW_COMMAND_ACTIVATE,
};

enum class Phase : std::uint32_t
{
    ready = AW_PHASE_READY,
    progress = AW_PHASE_PROGRESS,
    complete = AW_PHASE_COMPLETE,
};

using Digest = std::array<std::uint8_t, 32>;

struct ExtractTimes
{
    std::uint64_t write_ms = 0;
    std::uint64_t sync_ms = 0;
    std::uint64_t total_ms = 0;
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

struct ActivationEntry
{
    std::string source;
    std::string name;
};

bool valid_operation_id(const std::string &id);
std::string archive_path(const std::string &root, const std::string &id);
std::string extracted_path(const std::string &root, const std::string &id);

class ArchiveStore
{
  public:
    ArchiveStore(const std::string &root, const std::string &id, std::uint64_t limit,
                 std::string &error);
    ArchiveStore(const ArchiveStore &) = delete;
    ArchiveStore &operator=(const ArchiveStore &) = delete;
    ~ArchiveStore();

    bool ready() const;
    bool write(const void *data, std::size_t size, std::string &error);
    bool finish(std::uint64_t expected_size, const Digest &expected_digest,
                Digest &actual_digest, std::string &error);

  private:
    std::string id_;
    int root_fd_ = -1;
    int session_fd_ = -1;
    int archive_fd_ = -1;
    std::uint64_t limit_ = 0;
    std::uint64_t written_ = 0;
    sha256_context hash_{};
    std::unique_ptr<std::array<char, kChunkBytes>> buffer_;
    std::size_t held_ = 0;
    bool complete_ = false;
    bool owns_session_ = false;

    bool flush(std::string &error);
    void abandon();
};

// Hashing, validation and all parallel readers share one opened archive object.
bool extract_session(const std::string &root, const std::string &id,
                     std::uint64_t expected_size, const Digest &expected_digest,
                     const std::atomic<bool> &cancelled,
                     const std::function<void(std::uint64_t, std::uint64_t)> &progress,
                     ExtractTimes &times, std::string &error);
// Cancellation rolls back until the durable activation marker; committed work stays committed.
bool activate_session(const std::string &root, const std::string &wow_root,
                      const std::string &id, const std::string &channel,
                      const std::vector<ActivationEntry> &entries,
                      const std::function<bool()> &cancelled,
                      std::uint64_t &move_ms, std::string &error);
// Unfinished activation is recovered before deletion; failed recovery retains backups.
bool clean_session(const std::string &root, const std::string &wow_root,
                   const std::string &id, std::string &error);
bool clean_all_sessions(const std::string &root, const std::string &wow_root,
                        std::string &error);
std::string digest_hex(const Digest &digest);
bool parse_digest_hex(const std::string &text, Digest &digest);
}
