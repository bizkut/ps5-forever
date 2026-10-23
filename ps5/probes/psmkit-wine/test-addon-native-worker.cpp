// Forever Launcher - native add-on archive worker behavioral regressions.
// Copyright (C) 2026 bizkut
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../addon-worker/addon-worker.hpp"
#include "../../addon-worker/third_party/miniz/miniz.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <vector>

namespace fs = std::filesystem;
using forever::addon_worker::ArchiveStore;
using forever::addon_worker::Digest;
using forever::addon_worker::ExtractTimes;

#ifdef __linux__
namespace
{
struct SyncFailure
{
    std::atomic<bool> armed{false};
    dev_t destination_device = 0;
    ino_t destination_inode = 0;
    dev_t staged_device = 0;
    ino_t staged_inode = 0;
} sync_failure;

struct CommitCancellation
{
    std::atomic<bool> armed{false};
    std::atomic<bool> requested{false};
    dev_t session_device = 0;
    ino_t session_inode = 0;
} commit_cancellation;
}

// Inject one durability failure, or revoke the operation immediately after its
// final real commit fsync. Every uninjected fsync uses the host syscall unchanged.
extern "C" int fsync(int descriptor)
{
    if (sync_failure.armed.load())
    {
        struct stat directory{}, installed{};
        if (fstat(descriptor, &directory) == 0 &&
            directory.st_dev == sync_failure.destination_device &&
            directory.st_ino == sync_failure.destination_inode &&
            fstatat(descriptor, "Good", &installed, AT_SYMLINK_NOFOLLOW) == 0 &&
            installed.st_dev == sync_failure.staged_device &&
            installed.st_ino == sync_failure.staged_inode &&
            sync_failure.armed.exchange(false))
        {
            errno = EIO;
            return -1;
        }
    }
    const int result = static_cast<int>(syscall(SYS_fsync, descriptor));
    if (result == 0 && commit_cancellation.armed.load())
    {
        struct stat directory{}, marker{};
        if (fstat(descriptor, &directory) == 0 &&
            directory.st_dev == commit_cancellation.session_device &&
            directory.st_ino == commit_cancellation.session_inode &&
            fstatat(descriptor, ".activated", &marker, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(marker.st_mode) && commit_cancellation.armed.exchange(false))
            commit_cancellation.requested = true;
    }
    return result;
}
#endif

namespace
{
std::vector<std::uint8_t> make_zip(const std::vector<std::pair<std::string, std::string>> &files)
{
    mz_zip_archive archive{};
    assert(mz_zip_writer_init_heap(&archive, 0, 0));
    for (const auto &[name, body] : files)
        assert(mz_zip_writer_add_mem(&archive, name.c_str(), body.data(), body.size(),
                                     MZ_BEST_COMPRESSION));
    void *bytes = nullptr;
    std::size_t size = 0;
    assert(mz_zip_writer_finalize_heap_archive(&archive, &bytes, &size));
    std::vector<std::uint8_t> result(static_cast<std::uint8_t *>(bytes),
                                     static_cast<std::uint8_t *>(bytes) + size);
    mz_free(bytes);
    assert(mz_zip_writer_end(&archive));
    return result;
}

Digest sha256(const void *data, std::size_t size)
{
    sha256_context context{};
    Digest digest{};
    sha256_init(&context);
    sha256_update(&context, data, size);
    sha256_final(&context, digest.data());
    return digest;
}

bool store(const std::string &root, const std::string &id,
           const std::vector<std::uint8_t> &bytes, std::string &error)
{
    ArchiveStore writer(root, id, bytes.size(), error);
    if (!writer.ready())
        return false;
    const std::size_t split = bytes.size() / 3;
    if (!writer.write(bytes.data(), split, error) ||
        !writer.write(bytes.data() + split, bytes.size() - split, error))
        return false;
    Digest actual{};
    return writer.finish(bytes.size(), sha256(bytes.data(), bytes.size()), actual, error) &&
           actual == sha256(bytes.data(), bytes.size());
}

void patch_first_entry_as_symlink(std::vector<std::uint8_t> &bytes)
{
    const std::array<std::uint8_t, 4> central{{'P', 'K', 1, 2}};
    const auto found = std::search(bytes.begin(), bytes.end(), central.begin(), central.end());
    assert(found != bytes.end());
    const std::size_t offset = static_cast<std::size_t>(found - bytes.begin());
    assert(offset + 42 <= bytes.size());
    bytes[offset + 5] = 3; // Unix creator.
    const std::uint32_t attributes = (0120000u | 0777u) << 16;
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + 38 + i] = static_cast<std::uint8_t>(attributes >> (i * 8));
}

void corrupt_first_file_data(std::vector<std::uint8_t> &bytes)
{
    const std::array<std::uint8_t, 4> central{{'P', 'K', 1, 2}};
    const auto found = std::search(bytes.begin(), bytes.end(), central.begin(), central.end());
    assert(found != bytes.end());
    const std::size_t offset = static_cast<std::size_t>(found - bytes.begin());
    const auto u16 = [&](std::size_t at) {
        return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
    };
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(bytes[at]) |
            (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
    };
    const std::size_t compressed = u32(offset + 20);
    const std::size_t local = u32(offset + 42);
    const std::size_t data = local + 30 + u16(local + 26) + u16(local + 28);
    assert(compressed > 0 && data + compressed <= bytes.size());
    bytes[data + compressed / 2] ^= 0x5a;
}

void put(const std::string &path, const std::string &bytes, mode_t mode = 0644)
{
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    assert(output && chmod(path.c_str(), mode) == 0);
}

std::string get(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

mode_t mode(const std::string &path)
{
    struct stat info{};
    assert(lstat(path.c_str(), &info) == 0);
    return info.st_mode & 0777;
}

std::string object(const std::string &path)
{
    struct stat info{};
    assert(lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode));
    return std::to_string(static_cast<std::uint64_t>(info.st_dev)) + " " +
           std::to_string(static_cast<std::uint64_t>(info.st_ino));
}

bool extract(const std::string &root, const std::string &id,
             const std::vector<std::uint8_t> &bytes, const std::atomic<bool> &cancelled,
             const std::function<void(std::uint64_t, std::uint64_t)> &progress,
             ExtractTimes &times, std::string &error)
{
    return forever::addon_worker::extract_session(root, id, bytes.size(),
        sha256(bytes.data(), bytes.size()), cancelled, progress, times, error);
}
}

int main()
{
    using namespace forever::addon_worker;
    assert(valid_operation_id("0123456789abcdef0123456789abcdef"));
    assert(!valid_operation_id("0123456789abcdef"));
    assert(!valid_operation_id("0123456789abcdef0123456789abcdeg"));
    assert(!valid_operation_id("../456789abcdef0123456789abcdef"));

    char temporary[] = "/tmp/forever-addon-worker-XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = std::string(temporary) + "/staging";
    assert(mkdir(root.c_str(), 0711) == 0);
    const std::string wow_root = std::string(temporary) + "/wow";
    const std::string addons = wow_root + "/_retail_/Interface/AddOns";
    fs::create_directories(addons);
    std::string error;

    {
        const std::string id = "00000000000000000000000000000001";
        const std::array<std::uint8_t, 3> body{{'a', 'b', 'c'}};
        ArchiveStore writer(root, id, body.size(), error);
        assert(writer.ready());
        assert(writer.write(body.data(), 1, error));
        assert(writer.write(body.data() + 1, 2, error));
        Digest expected{};
        assert(parse_digest_hex(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected));
        Digest actual{};
        assert(writer.finish(body.size(), expected, actual, error));
        assert(actual == expected);
        assert(fs::file_size(archive_path(root, id)) == body.size());
        assert(clean_session(root, wow_root, id, error));
    }

    {
        const std::string id = "00000000000000000000000000000002";
        const std::array<std::uint8_t, 3> body{{'a', 'b', 'c'}};
        ArchiveStore writer(root, id, body.size(), error);
        assert(writer.ready() && writer.write(body.data(), body.size(), error));
        Digest wrong{};
        Digest actual{};
        assert(!writer.finish(body.size(), wrong, actual, error));
        assert(!fs::exists(root + "/" + id));
    }

    {
        const std::string id = "00000000000000000000000000000003";
        const auto bytes = make_zip({{"Good/Good.toc", "## Interface: 16001\n"},
                                     {"Good/Good.lua", "print('ok')\n"},
                                     {"README.md", "read me\n"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        std::uint64_t last = 0, total = 0;
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled,
                               [&](std::uint64_t done, std::uint64_t count)
                               { last = done; total = count; }, times, error));
        assert(times.files == 3 && last == total && total == 3);
        assert(fs::is_directory(extracted_path(root, id)));
        assert(fs::is_regular_file(extracted_path(root, id) + "/Good/Good.toc"));
        assert(mode(root + "/" + id) == 0711);
        assert(mode(archive_path(root, id)) == 0400);
        assert(mode(extracted_path(root, id) + "/Good") == 0755);
        assert(mode(extracted_path(root, id) + "/Good/Good.toc") == 0644);
        assert(clean_session(root, wow_root, id, error));
    }

    {
        const std::string id = "00000000000000000000000000000004";
        const auto bytes = make_zip({{"../escape/Bad.toc", "## Interface: 16001\n"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(!fs::exists(std::string(temporary) + "/escape"));
        assert(clean_session(root, wow_root, id, error));
    }

    {
        const std::string id = "00000000000000000000000000000005";
        auto bytes = make_zip({{"Link", "target"}});
        patch_first_entry_as_symlink(bytes);
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(clean_session(root, wow_root, id, error));
    }

    {
        const std::string id = "00000000000000000000000000000006";
        auto bytes = make_zip({{"Good/Good.toc", "## Interface: 16001\n"}});
        bytes.resize(bytes.size() - 8);
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(clean_session(root, wow_root, id, error));
    }

    {
        const std::string id = "00000000000000000000000000000007";
        auto bytes = make_zip({{"Good/Good.toc", "## Interface: 16001\n"},
                               {"Good/Good.lua", std::string(4096, 'x')}});
        corrupt_first_file_data(bytes);
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(clean_session(root, wow_root, id, error));
    }

    {
        const std::string id = "00000000000000000000000000000008";
        const auto bytes = make_zip({{"Good/Good.toc", "## Interface: 16001\n"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{true};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(!fs::exists(extracted_path(root, id)));
        assert(clean_session(root, wow_root, id, error));
    }

    // Implicit directories participate in the same Windows namespace as leaves.
    for (const auto &files : std::vector<std::vector<std::pair<std::string, std::string>>>{
             {{"Good/Good.toc", "toc"}, {"good/extra.lua", "lua"}},
             {{"Good/Libs/a.lua", "a"}, {"Good/libs/b.lua", "b"}},
             {{"Good", "file"}, {"Good/Good.toc", "toc"}},
             {{"Good/NUL.lua", "device"}},
             {{"Good/file. ", "trailing"}},
             {{"Good/\xc3\x84.lua", "unicode"}}})
    {
        const std::string id = "00000000000000000000000000000009";
        const auto bytes = make_zip(files);
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(clean_session(root, wow_root, id, error));
    }

    // All parallel ZIP readers retain the hashed inode, even after pathname replacement.
    {
        const std::string id = "0000000000000000000000000000000a";
        std::vector<std::pair<std::string, std::string>> files;
        for (unsigned index = 0; index < 64; ++index)
            files.emplace_back("Good/file" + std::to_string(index) + ".lua", "original");
        const auto bytes = make_zip(files);
        assert(store(root, id, bytes, error));
        const auto replacement = make_zip({{"Bad/Bad.lua", "replacement"}});
        bool swapped = false;
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, [&](std::uint64_t, std::uint64_t)
        {
            if (!swapped)
            {
                swapped = true;
                fs::rename(archive_path(root, id), root + "/" + id + "/original.zip");
                put(archive_path(root, id),
                    std::string(reinterpret_cast<const char *>(replacement.data()),
                                replacement.size()), 0400);
            }
        }, times, error));
        assert(swapped && times.files == files.size());
        for (const auto &[name, body] : files)
            assert(get(extracted_path(root, id) + "/" + name) == body);
        assert(!fs::exists(extracted_path(root, id) + "/Bad"));
        assert(clean_session(root, wow_root, id, error));
    }

    // A substituted archive before the opened-descriptor hash never reaches extraction.
    {
        const std::string id = "0000000000000000000000000000000b";
        const auto bytes = make_zip({{"Good/Good.lua", "original"}});
        assert(store(root, id, bytes, error));
        auto changed = bytes;
        changed[0] ^= 1;
        assert(chmod(archive_path(root, id).c_str(), 0600) == 0);
        put(archive_path(root, id),
            std::string(reinterpret_cast<const char *>(changed.data()), changed.size()), 0400);
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(!fs::exists(extracted_path(root, id)));
        assert(clean_session(root, wow_root, id, error));
    }

    // Ancestor/root/session links are rejected; cleanup only unlinks links within a session.
    {
        const std::string id = "0000000000000000000000000000000c";
        const std::string outside = std::string(temporary) + "/outside";
        fs::create_directories(outside);
        put(outside + "/keep", "untouched");
        const std::string alias = std::string(temporary) + "/alias";
        assert(symlink(temporary, alias.c_str()) == 0);
        ArchiveStore through_ancestor(alias + "/staging", id, 3, error);
        assert(!through_ancestor.ready());
        assert(symlink(outside.c_str(), (root + "/" + id).c_str()) == 0);
        ArchiveStore through_session(root, id, 3, error);
        assert(!through_session.ready());
        assert(!clean_session(root, wow_root, id, error));
        assert(get(outside + "/keep") == "untouched");
        assert(unlink((root + "/" + id).c_str()) == 0);
        const auto bytes = make_zip({{"Good/Good.lua", "original"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        assert(symlink(outside.c_str(), (extracted_path(root, id) + "/link").c_str()) == 0);
        assert(clean_session(root, wow_root, id, error));
        assert(get(outside + "/keep") == "untouched");
        assert(unlink(alias.c_str()) == 0);
    }

    // A mutable ancestor may detach a root, but cannot redirect already opened descriptors.
    {
        const std::string id = "0000000000000000000000000000000d";
        const auto bytes = make_zip({{"Good/one.lua", "one"}, {"Good/two.lua", "two"}});
        assert(store(root, id, bytes, error));
        const std::string held = root + "-held";
        const std::string outside = std::string(temporary) + "/outside";
        bool detached = false;
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, [&](std::uint64_t, std::uint64_t)
        {
            if (!detached)
            {
                detached = true;
                fs::rename(root, held);
                assert(symlink(outside.c_str(), root.c_str()) == 0);
            }
        }, times, error));
        assert(get(extracted_path(held, id) + "/Good/one.lua") == "one");
        assert(!fs::exists(outside + "/" + id));
        std::uint64_t elapsed = 0;
        assert(!activate_session(root, wow_root, id, "_retail_", {{"Good", "Good"}},
                                 [] { return false; }, elapsed, error));
        assert(!clean_session(root, wow_root, id, error));
        assert(unlink(root.c_str()) == 0);
        fs::rename(held, root);
        assert(clean_session(root, wow_root, id, error));
    }

    // Stage remains read-only to Wine; final installed files and directories are editable.
    {
        const std::string id = "0000000000000000000000000000000e";
        const auto bytes = make_zip({{"Good/Good.toc", "new"},
                                     {"Good/Libs/deep.lua", "nested"},
                                     {"Fresh/Fresh.toc", "fresh"}});
        fs::create_directories(addons + "/Good");
        put(addons + "/Good/Good.toc", "old");
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        std::uint64_t elapsed = 0;
        assert(activate_session(root, wow_root, id, "_retail_",
                                {{"Good", "Good"}, {"Fresh", "Fresh"}},
                                [] { return false; }, elapsed, error));
        assert(get(addons + "/Good/Good.toc") == "new");
        assert(get(root + "/" + id + "/old/Good/Good.toc") == "old");
        assert(mode(root + "/" + id) == 0700);
        assert(mode(addons + "/Good") == 0777);
        assert(mode(addons + "/Good/Libs") == 0777);
        assert(mode(addons + "/Good/Libs/deep.lua") == 0666);
        put(addons + "/Good/Good.toc", "user edit", 0666);
        assert(clean_session(root, wow_root, id, error));
        assert(get(addons + "/Good/Good.toc") == "user edit");
    }

    // All mappings are admitted before the first rename, including late invalid names.
    {
        const std::string id = "0000000000000000000000000000000f";
        const auto bytes = make_zip({{"Good/Good.toc", "rejected"},
                                     {"Good/Nested/Nested.toc", "nested"},
                                     {"Other/Other.toc", "other"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        std::uint64_t elapsed = 0;
        assert(!activate_session(root, wow_root, id, "_retail_",
                                 {{"Good", "Good"}, {"Other", "Z..bad"}},
                                 [] { return false; }, elapsed, error));
        assert(!activate_session(root, wow_root, id, "_retail_",
                                 {{"Good", "Good"}, {"Good/Nested", "Nested"}},
                                 [] { return false; }, elapsed, error));
        assert(!activate_session(root, wow_root, id, "_retail_",
                                 {{"Good", "Good"}, {"Other", "good"}},
                                 [] { return false; }, elapsed, error));
        assert(!activate_session(root, wow_root, id, "../_retail_",
                                 {{"Good", "Good"}}, [] { return false; }, elapsed, error));
        assert(get(addons + "/Good/Good.toc") == "user edit");
        assert(fs::is_directory(extracted_path(root, id) + "/Good"));
        assert(!fs::exists(root + "/" + id + "/.activation"));
        assert(clean_session(root, wow_root, id, error));
    }

    // Restart recovery covers every rename boundary and a future, untouched mapping.
    for (unsigned boundary = 0; boundary < 5; ++boundary)
    {
        const std::string id = "00000000000000000000000000000010";
        const auto bytes = make_zip({{"Good/Good.toc", "new one"},
                                     {"Second/Second.toc", "new two"}});
        fs::remove_all(addons + "/Good");
        fs::remove_all(addons + "/Second");
        fs::create_directories(addons + "/Good");
        fs::create_directories(addons + "/Second");
        put(addons + "/Good/Good.toc", "original one");
        put(addons + "/Second/Second.toc", "original two");
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        const std::string session = root + "/" + id;
        assert(chmod(session.c_str(), 0700) == 0);
        assert(mkdir((session + "/old").c_str(), 0700) == 0);
        const std::string journal = "AWACT1\n_retail_\n" + object(addons) + "\n" +
            "Good\tGood\t" + object(session + "/x/Good") + " " + object(addons + "/Good") + "\n" +
            "Second\tSecond\t" + object(session + "/x/Second") + " " +
            object(addons + "/Second") + "\n";
        put(session + "/.activation", journal, 0600);
        if (boundary >= 1)
            fs::rename(addons + "/Good", session + "/old/Good");
        if (boundary >= 2)
            fs::rename(session + "/x/Good", addons + "/Good");
        if (boundary >= 3)
            fs::rename(addons + "/Second", session + "/old/Second");
        if (boundary >= 4)
            fs::rename(session + "/x/Second", addons + "/Second");
        // CLEAN_ALL is the helper-open path; it must recover before deleting its backups.
        assert(clean_all_sessions(root, wow_root, error));
        assert(get(addons + "/Good/Good.toc") == "original one");
        assert(get(addons + "/Second/Second.toc") == "original two");
        assert(!fs::exists(session));
        assert(clean_session(root, wow_root, id, error));
    }

    // Recovery never deletes an unrestored backup when destination identity changed.
    {
        const std::string id = "00000000000000000000000000000011";
        const auto bytes = make_zip({{"Good/Good.toc", "new"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        const std::string session = root + "/" + id;
        assert(chmod(session.c_str(), 0700) == 0);
        assert(mkdir((session + "/old").c_str(), 0700) == 0);
        put(session + "/.activation", "AWACT1\n_retail_\n" + object(addons) + "\n" +
            "Good\tGood\t" + object(session + "/x/Good") + " " + object(addons + "/Good") + "\n",
            0600);
        fs::rename(addons + "/Good", session + "/old/Good");
        fs::create_directories(addons + "/Good");
        put(addons + "/Good/foreign", "do not overwrite");
        assert(!clean_session(root, wow_root, id, error));
        assert(fs::exists(session + "/.activation"));
        assert(get(session + "/old/Good/Good.toc") == "original one");
        assert(get(addons + "/Good/foreign") == "do not overwrite");
        fs::remove_all(addons + "/Good");
        assert(clean_session(root, wow_root, id, error));
        assert(get(addons + "/Good/Good.toc") == "original one");
    }

    // Destination ancestors and staged links cannot become rename authorities.
    {
        const std::string id = "00000000000000000000000000000012";
        const auto bytes = make_zip({{"Good/Good.toc", "new"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        const std::string outside = std::string(temporary) + "/outside";
        assert(symlink(outside.c_str(), (extracted_path(root, id) + "/Good/link").c_str()) == 0);
        std::uint64_t elapsed = 0;
        assert(!activate_session(root, wow_root, id, "_retail_", {{"Good", "Good"}},
                                 [] { return false; }, elapsed, error));
        assert(unlink((extracted_path(root, id) + "/Good/link").c_str()) == 0);
        fs::rename(wow_root + "/_retail_/Interface", wow_root + "/_retail_/Interface-held");
        assert(symlink(outside.c_str(), (wow_root + "/_retail_/Interface").c_str()) == 0);
        assert(!activate_session(root, wow_root, id, "_retail_", {{"Good", "Good"}},
                                 [] { return false; }, elapsed, error));
        assert(!fs::exists(outside + "/AddOns"));
        assert(unlink((wow_root + "/_retail_/Interface").c_str()) == 0);
        fs::rename(wow_root + "/_retail_/Interface-held", wow_root + "/_retail_/Interface");
        assert(clean_session(root, wow_root, id, error));
    }

    // When privileged, exercise the actual native-helper/Wine uid separation.
    if (geteuid() == 0)
    {
        const std::string id = "00000000000000000000000000000013";
        const auto bytes = make_zip({{"Good/Good.toc", "readable, not writable"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        assert(chmod(temporary, 0777) == 0);
        const std::string foreign = std::string(temporary) + "/foreign-root";
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0)
        {
            if (seteuid(1) != 0 || mkdir(foreign.c_str(), 0711) != 0)
                _exit(1);
            const int readable = open((extracted_path(root, id) + "/Good/Good.toc").c_str(),
                                       O_RDONLY);
            if (readable < 0)
                _exit(2);
            close(readable);
            const int writable = open((extracted_path(root, id) + "/Good/Good.toc").c_str(),
                                       O_WRONLY);
            if (writable >= 0)
                _exit(3);
            if (open(archive_path(root, id).c_str(), O_RDONLY) >= 0 ||
                rename((root + "/" + id).c_str(), (root + "/stolen").c_str()) == 0 ||
                mkdir((extracted_path(root, id) + "/injected").c_str(), 0777) == 0)
                _exit(4);
            _exit(0);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        ArchiveStore cannot_adopt(foreign, id, 3, error);
        assert(!cannot_adopt.ready());
        assert(mode(foreign) == 0711);
        assert(chmod(temporary, 0700) == 0);
        assert(clean_session(root, wow_root, id, error));
    }

    // A newly installed folder (no previous backup) also rolls back after restart.
    {
        const std::string id = "00000000000000000000000000000014";
        const auto bytes = make_zip({{"BrandNew/BrandNew.toc", "new"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        const std::string session = root + "/" + id;
        assert(chmod(session.c_str(), 0700) == 0);
        assert(mkdir((session + "/old").c_str(), 0700) == 0);
        put(session + "/.activation", "AWACT1\n_retail_\n" + object(addons) + "\n" +
            "BrandNew\tBrandNew\t" + object(session + "/x/BrandNew") + " 0 0\n", 0600);
        fs::rename(session + "/x/BrandNew", addons + "/BrandNew");
        assert(clean_session(root, wow_root, id, error));
        assert(!fs::exists(addons + "/BrandNew"));
    }

    // Neither corrupt journals nor unjournaled backups are disposable staging.
    {
        const std::string id = "00000000000000000000000000000015";
        const auto bytes = make_zip({{"Good/Good.toc", "new"}});
        assert(store(root, id, bytes, error));
        const std::string session = root + "/" + id;
        fs::create_directories(session + "/old/Good");
        put(session + "/old/Good/keep", "backup");
        put(session + "/.activation", "truncated", 0600);
        assert(!clean_session(root, wow_root, id, error));
        assert(get(session + "/old/Good/keep") == "backup");
        assert(unlink((session + "/.activation").c_str()) == 0);
        assert(!clean_session(root, wow_root, id, error));
        assert(get(session + "/old/Good/keep") == "backup");
        // Test teardown only after explicitly restoring the saved data.
        fs::rename(session + "/old/Good", addons + "/Recovered");
        assert(clean_session(root, wow_root, id, error));
        assert(get(addons + "/Recovered/keep") == "backup");
    }

#ifdef __linux__
    // An error after a completed rename restores only moves that actually happened.
    {
        const std::string id = "00000000000000000000000000000016";
        const auto bytes = make_zip({{"Good/Good.toc", "rejected new one"},
                                     {"Second/Second.toc", "rejected new two"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        struct stat destination{}, staged{};
        assert(stat(addons.c_str(), &destination) == 0);
        assert(stat((extracted_path(root, id) + "/Good").c_str(), &staged) == 0);
        sync_failure.destination_device = destination.st_dev;
        sync_failure.destination_inode = destination.st_ino;
        sync_failure.staged_device = staged.st_dev;
        sync_failure.staged_inode = staged.st_ino;
        sync_failure.armed = true;
        std::uint64_t elapsed = 0;
        assert(!activate_session(root, wow_root, id, "_retail_",
                                 {{"Good", "Good"}, {"Second", "Second"}},
                                 [] { return false; }, elapsed, error));
        assert(!sync_failure.armed.load());
        assert(get(addons + "/Good/Good.toc") == "original one");
        assert(get(addons + "/Second/Second.toc") == "original two");
        assert(get(extracted_path(root, id) + "/Good/Good.toc") == "rejected new one");
        assert(get(extracted_path(root, id) + "/Second/Second.toc") == "rejected new two");
        assert(!fs::exists(root + "/" + id + "/.activation"));
        assert(clean_session(root, wow_root, id, error));
    }
#endif

    // Extra native links do not turn an apparently ordinary archive/tree into trusted input.
    {
        const std::string id = "00000000000000000000000000000017";
        const auto bytes = make_zip({{"Good/Good.toc", "new"}});
        assert(store(root, id, bytes, error));
        const std::string extra = std::string(temporary) + "/archive-hardlink";
        assert(link(archive_path(root, id).c_str(), extra.c_str()) == 0);
        std::atomic<bool> cancelled{false};
        ExtractTimes times;
        assert(!extract(root, id, bytes, cancelled, {}, times, error));
        assert(unlink(extra.c_str()) == 0);
        assert(extract(root, id, bytes, cancelled, {}, times, error));
        assert(link((extracted_path(root, id) + "/Good/Good.toc").c_str(),
                    (extracted_path(root, id) + "/Good/alias.lua").c_str()) == 0);
        std::uint64_t elapsed = 0;
        assert(!activate_session(root, wow_root, id, "_retail_", {{"Good", "Good"}},
                                 [] { return false; }, elapsed, error));
        assert(get(addons + "/Good/Good.toc") == "original one");
        assert(clean_session(root, wow_root, id, error));
    }

    // Cancellation is sampled before work, after a rename, and at the final commit gate.
    for (unsigned boundary = 0; boundary < 4; ++boundary)
    {
        const std::string id = "00000000000000000000000000000018";
        const auto bytes = make_zip({{"Good/Good.toc", "cancelled new one"},
                                     {"Second/Second.toc", "cancelled new two"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> extract_cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, extract_cancelled, {}, times, error));
        bool observed = false;
        const auto cancelled = [&]
        {
            const bool at_boundary = boundary == 0 ||
                (boundary == 3 ? fs::exists(root + "/" + id + "/.activated") :
                 (get(addons + "/Good/Good.toc") == "cancelled new one" &&
                  (boundary == 1 ||
                   (get(addons + "/Second/Second.toc") == "cancelled new two" &&
                    mode(addons + "/Good") == 0777 && mode(addons + "/Second") == 0777))));
            observed = observed || at_boundary;
            return observed;
        };
        std::uint64_t elapsed = 0;
        assert(!activate_session(root, wow_root, id, "_retail_",
                                 {{"Good", "Good"}, {"Second", "Second"}},
                                 cancelled, elapsed, error));
        assert(observed);
        assert(get(addons + "/Good/Good.toc") == "original one");
        assert(get(addons + "/Second/Second.toc") == "original two");
        assert(!fs::exists(root + "/" + id + "/.activation"));
        assert(!fs::exists(root + "/" + id + "/.activated"));
        if (boundary == 0)
            assert(mode(root + "/" + id) == 0711);
        assert(clean_session(root, wow_root, id, error));
    }

#ifdef __linux__
    // Revocation after the marker's successful durable fsync cannot undo a commit.
    {
        const std::string id = "00000000000000000000000000000019";
        const auto bytes = make_zip({{"Good/Good.toc", "committed despite revocation"}});
        assert(store(root, id, bytes, error));
        std::atomic<bool> extract_cancelled{false};
        ExtractTimes times;
        assert(extract(root, id, bytes, extract_cancelled, {}, times, error));
        struct stat session{};
        assert(stat((root + "/" + id).c_str(), &session) == 0);
        commit_cancellation.session_device = session.st_dev;
        commit_cancellation.session_inode = session.st_ino;
        commit_cancellation.requested = false;
        commit_cancellation.armed = true;
        std::uint64_t elapsed = 0;
        assert(activate_session(root, wow_root, id, "_retail_", {{"Good", "Good"}},
                                [] { return commit_cancellation.requested.load(); }, elapsed, error));
        assert(commit_cancellation.requested.load() && !commit_cancellation.armed.load());
        assert(fs::exists(root + "/" + id + "/.activated"));
        assert(clean_session(root, wow_root, id, error));
        assert(get(addons + "/Good/Good.toc") == "committed despite revocation");
    }
#endif

    assert(clean_all_sessions(root, wow_root, error));
    fs::remove_all(temporary);
    return 0;
}
