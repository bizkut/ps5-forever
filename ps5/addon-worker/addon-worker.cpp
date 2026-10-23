// Forever Launcher - bounded native add-on archive worker core.
// Copyright (C) 2026 bizkut
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon-worker.hpp"
#include "third_party/miniz/miniz.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <mutex>
#include <pthread.h>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace forever::addon_worker
{
namespace
{
constexpr std::size_t kReaderMemoryBudget = 16u << 20;
constexpr std::size_t kAllocationHeader = 16;
constexpr unsigned kMaxDepth = 32;

struct ActivationCancellation
{
    const std::function<bool()> &callback;
    bool stopped = false;

    bool operator()()
    {
        if (!stopped)
            stopped = !callback || callback();
        return stopped;
    }
};

void set_error(std::string &error, const char *message)
{
    error = message;
}

std::uint64_t now_ms()
{
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<std::uint64_t>(value.tv_sec) * 1000u +
           static_cast<std::uint64_t>(value.tv_nsec) / 1000000u;
}

class Descriptor
{
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    ~Descriptor() { if (value_ >= 0) close(value_); }
    int get() const { return value_; }
    int release() { const int value = value_; value_ = -1; return value; }
  private:
    int value_;
};

int open_directory_child(int parent, const char *name)
{
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

// O_NOFOLLOW on the final pathname alone does not protect mutable ancestors.
int open_directory_path(const std::string &path)
{
    if (path.empty() || path.front() != '/')
        return -1;
    int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    std::size_t start = 1;
    while (current >= 0 && start < path.size())
    {
        const auto slash = path.find('/', start);
        const std::string component = path.substr(start, slash - start);
        if (component.empty() || component == "." || component == "..")
        {
            close(current);
            return -1;
        }
        const int next = open_directory_child(current, component.c_str());
        close(current);
        current = next;
        if (slash == path.npos)
            break;
        start = slash + 1;
    }
    return current;
}

bool protected_directory(int descriptor)
{
    struct stat info{};
    return descriptor >= 0 && fstat(descriptor, &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == geteuid() && (info.st_mode & 0022) == 0;
}

int open_root(const std::string &root)
{
    Descriptor descriptor(open_directory_path(root));
    if (descriptor.get() < 0)
        return -1;
    if (!protected_directory(descriptor.get()))
    {
        errno = EPERM;
        return -1;
    }
    return descriptor.release();
}

int open_session(int root, const std::string &id)
{
    Descriptor descriptor(open_directory_child(root, id.c_str()));
    if (descriptor.get() < 0)
        return -1;
    if (!protected_directory(descriptor.get()))
    {
        errno = EPERM;
        return -1;
    }
    return descriptor.release();
}

bool write_state(int session, const char *state, std::string &error)
{
    const int descriptor = openat(session, ".state-new",
                                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0)
    {
        set_error(error, "could not create worker state");
        return false;
    }
    const std::size_t length = std::strlen(state);
    std::size_t done = 0;
    while (done < length)
    {
        const auto count = write(descriptor, state + done, length - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        done += static_cast<std::size_t>(count);
    }
    bool ok = done == length;
    if (ok && fsync(descriptor) != 0)
        ok = false;
    if (close(descriptor) != 0)
        ok = false;
    if (ok)
        ok = renameat(session, ".state-new", session, ".state") == 0 && fsync(session) == 0;
    if (!ok)
    {
        unlinkat(session, ".state-new", 0);
        set_error(error, "could not persist worker state");
    }
    return ok;
}
struct CloseDirectory
{
    void operator()(DIR *directory) const
    {
        if (directory)
            closedir(directory);
    }
};

bool directory_names(int descriptor, std::vector<std::string> &names,
                      ActivationCancellation *cancelled = nullptr)
{
    Descriptor scan(open_directory_child(descriptor, "."));
    if (scan.get() < 0)
        return false;
    std::unique_ptr<DIR, CloseDirectory> directory(fdopendir(scan.get()));
    if (!directory)
        return false;
    scan.release(); // fdopendir owns it only on success.
    for (;;)
    {
        if (cancelled && (*cancelled)())
            return false;
        errno = 0;
        const auto *entry = readdir(directory.get());
        if (!entry)
            return errno == 0;
        const std::string name = entry->d_name;
        if (name != "." && name != "..")
        {
            if (names.size() >= kMaxArchiveEntries)
                return false;
            names.push_back(name);
        }
    }
}

// Never follow a link, including when an entry changes between inspection and use.
bool remove_tree_at(int parent, const std::string &name, unsigned depth = 0)
{
    struct stat info{};
    if (fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(info.st_mode))
        return unlinkat(parent, name.c_str(), 0) == 0;
    if (depth > kMaxDepth + 4)
        return false;
    Descriptor directory(open_directory_child(parent, name.c_str()));
    struct stat opened{};
    if (directory.get() < 0 || fstat(directory.get(), &opened) != 0 ||
        opened.st_dev != info.st_dev || opened.st_ino != info.st_ino)
        return false;
    std::vector<std::string> names;
    if (!directory_names(directory.get(), names))
        return false;
    for (const auto &child : names)
        if (!remove_tree_at(directory.get(), child, depth + 1))
            return false;
    return fsync(directory.get()) == 0 && unlinkat(parent, name.c_str(), AT_REMOVEDIR) == 0 &&
           fsync(parent) == 0;
}

struct AllocationBudget
{
    std::size_t used = 0;
};

void *budget_realloc(void *opaque, void *address, std::size_t items, std::size_t size)
{
    auto &budget = *static_cast<AllocationBudget *>(opaque);
    if (size != 0 && items > (kReaderMemoryBudget - kAllocationHeader) / size)
        return nullptr;
    const std::size_t wanted = items * size;
    auto *block = address ? static_cast<unsigned char *>(address) - kAllocationHeader : nullptr;
    std::size_t previous = 0;
    if (block)
        std::memcpy(&previous, block, sizeof(previous));
    if (previous > budget.used || wanted > kReaderMemoryBudget - (budget.used - previous))
        return nullptr;
    auto *next = static_cast<unsigned char *>(std::realloc(block, wanted + kAllocationHeader));
    if (!next)
        return nullptr;
    std::memcpy(next, &wanted, sizeof(wanted));
    budget.used = budget.used - previous + wanted;
    return next + kAllocationHeader;
}

void *budget_alloc(void *opaque, std::size_t items, std::size_t size)
{
    return budget_realloc(opaque, nullptr, items, size);
}

void budget_free(void *opaque, void *address)
{
    if (!address)
        return;
    auto *block = static_cast<unsigned char *>(address) - kAllocationHeader;
    std::size_t previous = 0;
    std::memcpy(&previous, block, sizeof(previous));
    auto &budget = *static_cast<AllocationBudget *>(opaque);
    if (previous <= budget.used)
        budget.used -= previous;
    std::free(block);
}

class Reader
{
  public:
    Reader() = default;
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    ~Reader()
    {
        if (open_)
            mz_zip_reader_end(&zip_);
    }

    bool open(int descriptor)
    {
        descriptor_ = descriptor; // Borrowed for the lifetime of the complete operation.
        struct stat info{};
        if (descriptor_ < 0 || fstat(descriptor_, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_nlink != 1 || info.st_size <= 0 ||
            static_cast<std::uint64_t>(info.st_size) > kMaxArchiveBytes)
            return false;
        mz_zip_zero_struct(&zip_);
        zip_.m_pRead = +[](void *opaque, mz_uint64 offset, void *buffer, std::size_t count)
        {
            const int descriptor = *static_cast<int *>(opaque);
            std::size_t done = 0;
            while (done < count)
            {
                const auto result = pread(descriptor, static_cast<char *>(buffer) + done,
                                          count - done, static_cast<off_t>(offset + done));
                if (result < 0 && errno == EINTR)
                    continue;
                if (result <= 0)
                    break;
                done += static_cast<std::size_t>(result);
            }
            return done;
        };
        zip_.m_pIO_opaque = &descriptor_;
        zip_.m_pAlloc = budget_alloc;
        zip_.m_pFree = budget_free;
        zip_.m_pRealloc = budget_realloc;
        zip_.m_pAlloc_opaque = &budget_;
        open_ = mz_zip_reader_init(&zip_, static_cast<mz_uint64>(info.st_size), 0) != 0;
        return open_;
    }

    mz_zip_archive *zip()
    {
        return &zip_;
    }

  private:
    mz_zip_archive zip_{};
    AllocationBudget budget_{};
    int descriptor_ = -1;
    bool open_ = false;
};

bool valid_component(std::string_view part)
{
    if (part.empty() || part == "." || part == ".." || part.size() > 255 ||
        part.back() == '.' || part.back() == ' ')
        return false;
    std::string device;
    for (const unsigned char character : part)
    {
        // ASCII only: do not claim Windows-equivalent Unicode case folding.
        if (character < 32 || character >= 127 ||
            std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos)
            return false;
        device.push_back(character >= 'a' && character <= 'z' ? character - 32 : character);
    }
    device.resize(device.find('.') == device.npos ? device.size() : device.find('.'));
    return device != "CON" && device != "PRN" && device != "AUX" && device != "NUL" &&
           !(device.size() == 4 && (device.starts_with("COM") || device.starts_with("LPT")) &&
             device[3] >= '1' && device[3] <= '9');
}

bool clean_relative(std::string_view path)
{
    if (path.empty() || path.size() > kMaxArchivePath || path.back() == '/')
        return false;
    unsigned depth = 0;
    while (!path.empty())
    {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (!valid_component(part))
            return false;
        if (++depth > kMaxDepth)
            return false;
        if (slash == path.npos)
            break;
        path.remove_prefix(slash + 1);
    }
    return true;
}

std::string ascii_lower(std::string value)
{
    for (char &character : value)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    return value;
}

struct Entry
{
    mz_uint index = 0;
    std::string relative;
    bool directory = false;
    std::uint64_t size = 0;
};

bool list_archive(Reader &reader, std::vector<Entry> &entries, std::uint64_t &total,
                  std::string &error)
{
    auto *zip = reader.zip();
    const mz_uint count = mz_zip_reader_get_num_files(zip);
    if (count == 0 || count > kMaxArchiveEntries)
    {
        set_error(error, "archive is empty or has too many entries");
        return false;
    }
    std::vector<char> name_buffer(kMaxArchivePath + 2);
    std::set<std::string> seen;
    std::map<std::string, std::pair<std::string, bool>> namespace_nodes;
    for (mz_uint index = 0; index < count; ++index)
    {
        const mz_uint length = mz_zip_reader_get_filename(zip, index, nullptr, 0);
        if (length < 2 || length - 1 > kMaxArchivePath)
        {
            set_error(error, "archive path is invalid");
            return false;
        }
        if (!mz_zip_reader_get_filename(zip, index, name_buffer.data(),
                                        static_cast<mz_uint>(name_buffer.size())))
            return false;
        std::string name(name_buffer.data(), length - 1);
        for (const unsigned char character : name)
            if (character < 32 || character == 127 || character == '\\')
            {
                set_error(error, "archive path is unsafe");
                return false;
            }
        mz_zip_archive_file_stat status{};
        if (!mz_zip_reader_file_stat(zip, index, &status) || !status.m_is_supported ||
            status.m_is_encrypted)
        {
            set_error(error, "archive entry is unsupported or encrypted");
            return false;
        }
        Entry entry;
        entry.index = index;
        entry.directory = name.ends_with('/');
        if (entry.directory != (status.m_is_directory != 0))
        {
            set_error(error, "archive directory metadata disagrees");
            return false;
        }
        if (entry.directory)
            name.pop_back();
        if (!clean_relative(name) || !seen.insert(ascii_lower(name)).second)
        {
            set_error(error, "archive contains an unsafe or duplicate path");
            return false;
        }
        std::size_t component_start = 0;
        for (;;)
        {
            const auto slash = name.find('/', component_start);
            const bool leaf = slash == name.npos;
            const std::string component_path = name.substr(0, slash);
            const bool component_is_directory = !leaf || entry.directory;
            const auto key = ascii_lower(component_path);
            const auto [node, inserted] =
                namespace_nodes.emplace(key, std::make_pair(component_path,
                                                             component_is_directory));
            if (!inserted &&
                (node->second.first != component_path ||
                 node->second.second != component_is_directory))
            {
                set_error(error, "archive paths collide in the Windows namespace");
                return false;
            }
            if (namespace_nodes.size() > kMaxArchiveEntries)
            {
                set_error(error, "archive has too many explicit or implicit paths");
                return false;
            }
            if (leaf)
                break;
            component_start = slash + 1;
        }
        if ((status.m_version_made_by >> 8) == 3)
        {
            const auto type = (status.m_external_attr >> 16) & 0170000u;
            if (type != 0 && type != (entry.directory ? 0040000u : 0100000u))
            {
                set_error(error, "archive contains a link or special file");
                return false;
            }
        }
        if (entry.directory)
        {
            if (status.m_uncomp_size != 0)
                return false;
        }
        else
        {
            entry.size = status.m_uncomp_size;
            if (entry.size > kMaxArchiveBytes || entry.size > kMaxUnpackedBytes - total)
            {
                set_error(error, "archive expands beyond its limit");
                return false;
            }
            total += entry.size;
        }
        entry.relative = std::move(name);
        entries.push_back(std::move(entry));
    }
    return true;
}

int open_relative_parent(int base, const std::string &relative, std::string &leaf,
                         bool create = false)
{
    if (!clean_relative(relative))
        return -1;
    int current = open_directory_child(base, ".");
    std::size_t start = 0;
    while (current >= 0)
    {
        const auto slash = relative.find('/', start);
        const std::string component = relative.substr(start, slash - start);
        if (slash == relative.npos)
        {
            leaf = component;
            return current;
        }
        if (create && mkdirat(current, component.c_str(), 0755) != 0 && errno != EEXIST)
        {
            close(current);
            return -1;
        }
        const int next = open_directory_child(current, component.c_str());
        close(current);
        current = next;
        if (current >= 0 && (!protected_directory(current) ||
                             (create && fchmod(current, 0755) != 0)))
        {
            close(current);
            return -1;
        }
        start = slash + 1;
    }
    return -1;
}

struct Output
{
    int descriptor;
    std::uint64_t expected;
    std::uint64_t written = 0;
    const std::atomic<bool> &cancelled;
    std::array<char, kChunkBytes> &buffer;
    std::size_t held = 0;
    std::uint64_t write_ms = 0;

    bool flush()
    {
        const auto started = now_ms();
        std::size_t done = 0;
        while (done < held)
        {
            const auto result = ::write(descriptor, buffer.data() + done, held - done);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                return false;
            done += static_cast<std::size_t>(result);
        }
        held = 0;
        write_ms += now_ms() - started;
        return true;
    }
};

std::size_t write_output(void *opaque, mz_uint64 offset, const void *data, std::size_t count)
{
    auto &output = *static_cast<Output *>(opaque);
    if (output.cancelled.load() || offset != output.written ||
        count > output.expected - output.written)
        return 0;
    const char *bytes = static_cast<const char *>(data);
    std::size_t taken = 0;
    while (taken < count)
    {
        const auto room = output.buffer.size() - output.held;
        const auto piece = std::min(room, count - taken);
        std::memcpy(output.buffer.data() + output.held, bytes + taken, piece);
        output.held += piece;
        taken += piece;
        if (output.held == output.buffer.size() && !output.flush())
            return 0;
    }
    output.written += count;
    return count;
}

struct ExtractWork
{
    int archive;
    int destination;
    const std::vector<Entry> &entries;
    const std::atomic<bool> &cancelled;
    const std::function<void(std::uint64_t, std::uint64_t)> &progress;
    const std::uint64_t total_files;
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> write_ms{0};
    std::atomic<bool> failed{false};
    std::mutex progress_guard;

    ExtractWork(int archive_descriptor, int extract_descriptor,
                const std::vector<Entry> &archive_entries,
                const std::atomic<bool> &stop,
                const std::function<void(std::uint64_t, std::uint64_t)> &report)
        : archive(archive_descriptor), destination(extract_descriptor), entries(archive_entries),
          cancelled(stop), progress(report),
          total_files(static_cast<std::uint64_t>(std::count_if(
              archive_entries.begin(), archive_entries.end(),
              [](const Entry &entry) { return !entry.directory; })))
    {
    }
};

void *extract_worker(void *opaque)
{
    auto &work = *static_cast<ExtractWork *>(opaque);
    Reader reader;
    auto buffer = std::unique_ptr<std::array<char, kChunkBytes>>(
        new (std::nothrow) std::array<char, kChunkBytes>());
    if (!buffer || !reader.open(work.archive))
    {
        work.failed = true;
        return nullptr;
    }
    for (;;)
    {
        const auto index = work.next.fetch_add(1);
        if (index >= work.entries.size() || work.failed.load() || work.cancelled.load())
            return nullptr;
        const auto &entry = work.entries[index];
        std::string leaf;
        Descriptor parent(open_relative_parent(work.destination, entry.relative, leaf, true));
        if (parent.get() < 0)
        {
            work.failed = true;
            return nullptr;
        }
        if (entry.directory)
        {
            if (mkdirat(parent.get(), leaf.c_str(), 0755) != 0 && errno != EEXIST)
                work.failed = true;
            Descriptor directory(open_directory_child(parent.get(), leaf.c_str()));
            if (!protected_directory(directory.get()) || fchmod(directory.get(), 0755) != 0)
                work.failed = true;
            continue;
        }
        Output output{openat(parent.get(), leaf.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644),
                      entry.size,
                      0,
                      work.cancelled,
                      *buffer};
        bool ok = output.descriptor >= 0 && fchmod(output.descriptor, 0644) == 0 &&
                  mz_zip_reader_extract_to_callback(reader.zip(), entry.index, write_output,
                                                    &output, 0) != 0 &&
                  output.written == output.expected && output.flush() &&
                  fsync(output.descriptor) == 0;
        if (output.descriptor >= 0)
            ok = close(output.descriptor) == 0 && ok;
        work.write_ms.fetch_add(output.write_ms);
        if (!ok)
        {
            work.failed = true;
            return nullptr;
        }
        work.bytes.fetch_add(entry.size);
        work.completed.fetch_add(1);
        if (work.progress)
        {
            std::lock_guard lock(work.progress_guard);
            work.progress(work.completed.load(), work.total_files);
        }
    }
}

bool sync_tree(int directory, const std::atomic<bool> &cancelled, unsigned depth = 0)
{
    if (cancelled.load() || depth > kMaxDepth)
        return false;
    std::vector<std::string> names;
    if (!directory_names(directory, names))
        return false;
    for (const auto &name : names)
    {
        struct stat info{};
        if (fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
            return false;
        if (S_ISDIR(info.st_mode))
        {
            Descriptor child(open_directory_child(directory, name.c_str()));
            if (child.get() < 0 || !sync_tree(child.get(), cancelled, depth + 1))
                return false;
        }
        else if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
            return false;
    }
    return !cancelled.load() && fsync(directory) == 0;
}
} // namespace

bool valid_operation_id(const std::string &id)
{
    if (id.size() != kOperationIdBytes)
        return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char value)
    { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

std::string archive_path(const std::string &root, const std::string &id)
{
    return root + "/" + id + "/archive.zip";
}

std::string extracted_path(const std::string &root, const std::string &id)
{
    return root + "/" + id + "/x";
}

std::string digest_hex(const Digest &digest)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        text[index * 2] = digits[digest[index] >> 4];
        text[index * 2 + 1] = digits[digest[index] & 15];
    }
    return text;
}

bool parse_digest_hex(const std::string &text, Digest &digest)
{
    if (text.size() != digest.size() * 2)
        return false;
    const auto nibble = [](char value) -> int
    {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

ArchiveStore::ArchiveStore(const std::string &root, const std::string &id, std::uint64_t limit,
                           std::string &error)
    : id_(id), limit_(limit),
      buffer_(new (std::nothrow) std::array<char, kChunkBytes>())
{
    if (!buffer_ || !valid_operation_id(id) || limit == 0 || limit > kMaxArchiveBytes)
    {
        set_error(error, "invalid archive operation");
        return;
    }
    root_fd_ = open_root(root);
    if (root_fd_ < 0 || mkdirat(root_fd_, id.c_str(), 0700) != 0)
    {
        set_error(error, "could not create archive staging");
        return;
    }
    owns_session_ = true;
    session_fd_ = open_session(root_fd_, id);
    if (session_fd_ < 0 || fchmod(session_fd_, 0700) != 0)
    {
        set_error(error, "archive staging is not a real directory");
        return;
    }
    archive_fd_ = openat(session_fd_, "archive.zip",
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (archive_fd_ < 0)
    {
        set_error(error, "could not create staged archive");
        return;
    }
    sha256_init(&hash_);
}

ArchiveStore::~ArchiveStore()
{
    if (!complete_)
        abandon();
    if (session_fd_ >= 0)
        close(session_fd_);
    if (root_fd_ >= 0)
        close(root_fd_);
}

bool ArchiveStore::ready() const
{
    return archive_fd_ >= 0 && buffer_ != nullptr;
}

bool ArchiveStore::flush(std::string &error)
{
    std::size_t done = 0;
    while (done < held_)
    {
        const auto count = ::write(archive_fd_, buffer_->data() + done, held_ - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            set_error(error, "could not write staged archive");
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    held_ = 0;
    return true;
}

bool ArchiveStore::write(const void *data, std::size_t size, std::string &error)
{
    if (!ready() || complete_ || size > limit_ - written_)
    {
        set_error(error, "archive exceeds its declared limit");
        return false;
    }
    const auto *bytes = static_cast<const char *>(data);
    std::size_t taken = 0;
    while (taken < size)
    {
        const auto room = buffer_->size() - held_;
        const auto piece = std::min(room, size - taken);
        std::memcpy(buffer_->data() + held_, bytes + taken, piece);
        held_ += piece;
        taken += piece;
        if (held_ == buffer_->size() && !flush(error))
            return false;
    }
    sha256_update(&hash_, data, size);
    written_ += size;
    return true;
}

bool ArchiveStore::finish(std::uint64_t expected_size, const Digest &expected_digest,
                          Digest &actual_digest, std::string &error)
{
    if (!ready() || complete_ || !flush(error))
        return false;
    sha256_final(&hash_, actual_digest.data());
    if (written_ != expected_size || actual_digest != expected_digest)
    {
        set_error(error, "archive size or SHA-256 does not match the stream");
        abandon();
        return false;
    }
    bool ok = fchmod(archive_fd_, 0400) == 0 && fsync(archive_fd_) == 0;
    if (close(archive_fd_) != 0)
        ok = false;
    archive_fd_ = -1;
    const std::string state = "stored " + std::to_string(written_) + " " +
                              digest_hex(actual_digest) + "\n";
    ok = ok && write_state(session_fd_, state.c_str(), error) && fsync(root_fd_) == 0;
    if (!ok)
    {
        if (error.empty())
            set_error(error, "could not make staged archive durable");
        abandon();
        return false;
    }
    complete_ = true;
    return true;
}

void ArchiveStore::abandon()
{
    if (archive_fd_ >= 0)
    {
        close(archive_fd_);
        archive_fd_ = -1;
    }
    if (owns_session_ && root_fd_ >= 0)
    {
        remove_tree_at(root_fd_, id_);
        owns_session_ = false;
    }
}

namespace
{
bool verify_archive(int descriptor, std::uint64_t expected_size, const Digest &expected_digest,
                    const std::atomic<bool> &cancelled, std::string &error)
{
    struct stat info{};
    if (descriptor < 0 || expected_size == 0 || expected_size > kMaxArchiveBytes ||
        fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        info.st_uid != geteuid() || (info.st_mode & 0222) != 0 ||
        static_cast<std::uint64_t>(info.st_size) != expected_size)
    {
        set_error(error, "staged archive identity changed");
        return false;
    }
    sha256_context hash{};
    Digest actual{};
    auto buffer = std::unique_ptr<std::array<char, kChunkBytes>>(
        new (std::nothrow) std::array<char, kChunkBytes>());
    if (!buffer)
    {
        set_error(error, "could not allocate archive verification buffer");
        return false;
    }
    sha256_init(&hash);
    std::uint64_t offset = 0;
    while (offset < expected_size && !cancelled.load())
    {
        const auto count = pread(descriptor, buffer->data(),
                                 std::min<std::uint64_t>(buffer->size(), expected_size - offset),
                                 static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        sha256_update(&hash, buffer->data(), static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    sha256_final(&hash, actual.data());
    if (cancelled.load() || offset != expected_size || actual != expected_digest)
    {
        set_error(error, cancelled.load() ? "cancelled" : "staged archive SHA-256 changed");
        return false;
    }
    return true;
}
} // namespace

bool extract_session(const std::string &root, const std::string &id,
                     std::uint64_t expected_size, const Digest &expected_digest,
                     const std::atomic<bool> &cancelled,
                     const std::function<void(std::uint64_t, std::uint64_t)> &progress,
                     ExtractTimes &times, std::string &error)
{
    if (!valid_operation_id(id))
        return false;
    const auto started = now_ms();
    Descriptor root_descriptor(open_root(root));
    Descriptor session(open_session(root_descriptor.get(), id));
    Descriptor archive(openat(session.get(), "archive.zip",
                               O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (session.get() < 0 || fchmod(session.get(), 0700) != 0)
    {
        set_error(error, "could not open protected archive staging");
        return false;
    }
    if (!verify_archive(archive.get(), expected_size, expected_digest, cancelled, error))
        return false;
    struct stat journal{};
    if (fstatat(session.get(), ".activation", &journal, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT)
    {
        set_error(error, "activation recovery is required before extraction");
        return false;
    }
    if (!remove_tree_at(session.get(), "x") || mkdirat(session.get(), "x", 0755) != 0)
    {
        set_error(error, "could not create extraction staging");
        return false;
    }
    Descriptor destination(open_directory_child(session.get(), "x"));
    Reader reader;
    std::vector<Entry> entries;
    std::uint64_t unpacked = 0;
    if (destination.get() < 0 || fchmod(destination.get(), 0755) != 0 ||
        !reader.open(archive.get()) ||
        !list_archive(reader, entries, unpacked, error))
    {
        remove_tree_at(session.get(), "x");
        if (error.empty())
            set_error(error, "archive is invalid");
        return false;
    }
    ExtractWork work{archive.get(), destination.get(), entries, cancelled, progress};
    pthread_t threads[kExtractWorkers];
    std::size_t running = 0;
    for (auto &thread : threads)
        if (pthread_create(&thread, nullptr, extract_worker, &work) == 0)
            threads[running++] = thread;
    if (running == 0)
        extract_worker(&work);
    for (std::size_t index = 0; index < running; ++index)
        pthread_join(threads[index], nullptr);
    const auto syncing = now_ms();
    if (cancelled.load() || work.failed.load() || !sync_tree(destination.get(), cancelled))
    {
        remove_tree_at(session.get(), "x");
        set_error(error, cancelled.load() ? "cancelled" : "archive extraction or sync failed");
        return false;
    }
    times.write_ms = work.write_ms.load();
    times.sync_ms = now_ms() - syncing;
    times.total_ms = now_ms() - started;
    times.files = work.completed.load();
    times.bytes = work.bytes.load();
    const std::string state = "extracted " + std::to_string(times.files) + " " +
                              std::to_string(times.bytes) + "\n";
    // Publish read-only traversal only after the complete tree and state are durable.
    if (!write_state(session.get(), state.c_str(), error) ||
        fchmod(session.get(), 0711) != 0 || fsync(session.get()) != 0)
    {
        fchmod(session.get(), 0700);
        remove_tree_at(session.get(), "x");
        return false;
    }
    error.clear();
    return true;
}

namespace
{
struct ObjectIdentity
{
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
};

ObjectIdentity identity(const struct stat &info)
{
    return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
}

bool same_identity(const struct stat &info, const ObjectIdentity &object)
{
    return S_ISDIR(info.st_mode) && static_cast<std::uint64_t>(info.st_dev) == object.device &&
           static_cast<std::uint64_t>(info.st_ino) == object.inode;
}

struct ActivationMove
{
    ActivationEntry entry;
    ObjectIdentity staged;
    ObjectIdentity previous;
};

bool valid_channel(const std::string &channel)
{
    if (channel.size() < 3 || channel.size() > 64 ||
        channel.front() != '_' || channel.back() != '_')
        return false;
    return std::all_of(channel.begin(), channel.end(), [](unsigned char value)
    {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '_' || value == '-';
    });
}

bool valid_mappings(const std::vector<ActivationEntry> &entries,
                     ActivationCancellation *cancelled = nullptr)
{
    if (entries.empty() || entries.size() > kMaxActivationFolders)
        return false;
    std::set<std::string> sources, names;
    std::map<std::string, std::string> components;
    for (const auto &entry : entries)
    {
        if ((cancelled && (*cancelled)()) ||
            !clean_relative(entry.source) || !valid_component(entry.name) ||
            entry.name.front() == '.' || entry.name.find("..") != entry.name.npos ||
            !sources.insert(ascii_lower(entry.source)).second ||
            !names.insert(ascii_lower(entry.name)).second)
            return false;
        std::size_t start = 0;
        for (;;)
        {
            const auto slash = entry.source.find('/', start);
            const auto part = entry.source.substr(0, slash);
            const auto [found, inserted] = components.emplace(ascii_lower(part), part);
            if (!inserted && found->second != part)
                return false;
            if (slash == entry.source.npos)
                break;
            start = slash + 1;
        }
    }
    for (const auto &source : sources)
    {
        if (cancelled && (*cancelled)())
            return false;
        for (auto slash = source.find('/'); slash != source.npos;
             slash = source.find('/', slash + 1))
            if (sources.contains(source.substr(0, slash)))
                return false;
    }
    return true;
}

bool unique_name(int directory, const std::string &wanted,
                  ActivationCancellation *cancelled = nullptr)
{
    std::vector<std::string> names;
    if (!directory_names(directory, names, cancelled))
        return false;
    const auto folded = ascii_lower(wanted);
    return std::none_of(names.begin(), names.end(), [&](const std::string &name)
    { return ascii_lower(name) == folded && name != wanted; });
}

int open_destination_child(int parent, const char *name, bool create,
                            ActivationCancellation *cancelled = nullptr)
{
    if (cancelled && (*cancelled)())
        return -1;
    bool created = false;
    if (create)
    {
        created = mkdirat(parent, name, 0777) == 0;
        if (!created && errno != EEXIST)
            return -1;
    }
    Descriptor child(open_directory_child(parent, name));
    if (child.get() < 0 || (cancelled && (*cancelled)()) ||
        (created && fchmod(child.get(), 0777) != 0) ||
        fsync(child.get()) != 0 || fsync(parent) != 0)
        return -1;
    return child.release();
}

int open_addons_directory(const std::string &wow_root, const std::string &channel, bool create,
                           ActivationCancellation *cancelled = nullptr)
{
#ifdef FOREVER_ADDON_WORKER_CONSOLE
    if (wow_root != kNativeWowRoot)
        return -1;
#endif
    Descriptor wow(open_directory_path(wow_root));
    if (wow.get() < 0 || !unique_name(wow.get(), channel, cancelled))
        return -1;
    Descriptor product(open_directory_child(wow.get(), channel.c_str()));
    if (product.get() < 0 || !unique_name(product.get(), "Interface", cancelled))
        return -1;
    Descriptor interface_directory(
        open_destination_child(product.get(), "Interface", create, cancelled));
    if (interface_directory.get() < 0 ||
        !unique_name(interface_directory.get(), "AddOns", cancelled))
        return -1;
    Descriptor addons(
        open_destination_child(interface_directory.get(), "AddOns", create, cancelled));
    if (addons.get() < 0)
        return -1;
    return addons.release();
}

// Validate every staged descendant before changing any installed add-on.
bool staged_tree(int directory, std::size_t &count, unsigned depth = 0,
                  bool make_editable = false, ActivationCancellation *cancelled = nullptr)
{
    if ((cancelled && (*cancelled)()) || depth > kMaxDepth || !protected_directory(directory))
        return false;
    std::vector<std::string> names;
    std::set<std::string> folded;
    if (!directory_names(directory, names, cancelled))
        return false;
    for (const auto &name : names)
    {
        if ((cancelled && (*cancelled)()) ||
            ++count > kMaxArchiveEntries || !valid_component(name) ||
            !folded.insert(ascii_lower(name)).second)
            return false;
        struct stat info{};
        if (fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
            info.st_uid != geteuid() || (info.st_mode & 0022) != 0)
            return false;
        if (S_ISDIR(info.st_mode))
        {
            Descriptor child(open_directory_child(directory, name.c_str()));
            if (child.get() < 0 ||
                !staged_tree(child.get(), count, depth + 1, make_editable, cancelled) ||
                (cancelled && (*cancelled)()) ||
                (make_editable && (fchmod(child.get(), 0777) != 0 || fsync(child.get()) != 0)))
                return false;
        }
        else
        {
            if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
                return false;
            Descriptor file(openat(directory, name.c_str(),
                                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
            struct stat opened{};
            if (file.get() < 0 ||
                fstat(file.get(), &opened) != 0 || opened.st_dev != info.st_dev ||
                opened.st_ino != info.st_ino ||
                (cancelled && (*cancelled)()) ||
                (make_editable && (fchmod(file.get(), 0666) != 0 || fsync(file.get()) != 0)))
                return false;
        }
    }
    return !(cancelled && (*cancelled)()) && fsync(directory) == 0;
}

std::string identity_text(const ObjectIdentity &object)
{
    return std::to_string(object.device) + " " + std::to_string(object.inode);
}

bool persist_file(int session, const char *name, const std::string &bytes,
                    ActivationCancellation *cancelled = nullptr)
{
    if (cancelled && (*cancelled)())
        return false;
    Descriptor file(openat(session, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                            0600));
    if (file.get() < 0)
        return false;
    std::size_t done = 0;
    while (done < bytes.size())
    {
        const auto count = write(file.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        done += static_cast<std::size_t>(count);
    }
    if (cancelled && (*cancelled)())
        return false;
    const bool synced = fsync(file.get()) == 0;
    const bool closed = close(file.release()) == 0;
    if (!synced || !closed || (cancelled && (*cancelled)()))
        return false;
    // This is the durable commit boundary for .activated. Do not poll after success.
    return fsync(session) == 0;
}

bool write_activation_journal(int session, const std::string &channel,
                              const ObjectIdentity &addons,
                              const std::vector<ActivationMove> &moves)
{
    std::string journal = "AWACT1\n" + channel + "\n" + identity_text(addons) + "\n";
    for (const auto &move : moves)
        journal += move.entry.source + "\t" + move.entry.name + "\t" +
                   identity_text(move.staged) + " " + identity_text(move.previous) + "\n";
    if (!persist_file(session, ".activation-new", journal))
        return false;
    return renameat(session, ".activation-new", session, ".activation") == 0 &&
           fsync(session) == 0;
}

bool read_private_file(int session, const char *name, std::string &bytes)
{
    Descriptor file(openat(session, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    constexpr std::size_t maximum = (kMaxArchivePath + 512) * kMaxActivationFolders + 128;
    if (file.get() < 0 || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1 || info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
        info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > maximum)
        return false;
    bytes.resize(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (done < bytes.size())
    {
        const auto count = read(file.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

bool take_field(std::string_view &input, char separator, std::string_view &field)
{
    const auto end = input.find(separator);
    if (end == input.npos)
        return false;
    field = input.substr(0, end);
    input.remove_prefix(end + 1);
    return true;
}

bool parse_numbers(std::string_view text, std::uint64_t *values, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto end = text.find(' ');
        const auto field = text.substr(0, end);
        const auto parsed = std::from_chars(field.data(), field.data() + field.size(),
                                             values[index]);
        if (field.empty() || parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size())
            return false;
        if (index + 1 == count)
            return end == text.npos;
        if (end == text.npos)
            return false;
        text.remove_prefix(end + 1);
    }
    return false;
}

bool read_activation_journal(int session, std::string &channel, ObjectIdentity &addons,
                             std::vector<ActivationMove> &moves)
{
    std::string bytes;
    if (!read_private_file(session, ".activation", bytes))
        return false;
    std::string_view remaining(bytes), line;
    if (!take_field(remaining, '\n', line) || line != "AWACT1" ||
        !take_field(remaining, '\n', line))
        return false;
    channel = line;
    if (!valid_channel(channel) || !take_field(remaining, '\n', line))
        return false;
    std::uint64_t numbers[4]{};
    if (!parse_numbers(line, numbers, 2) || numbers[1] == 0)
        return false;
    addons = {numbers[0], numbers[1]};
    std::vector<ActivationEntry> entries;
    while (!remaining.empty())
    {
        std::string_view source, name;
        if (moves.size() >= kMaxActivationFolders || !take_field(remaining, '\n', line) ||
            !take_field(line, '\t', source) || !take_field(line, '\t', name) ||
            !parse_numbers(line, numbers, 4) || numbers[1] == 0 ||
            (numbers[3] == 0 && numbers[2] != 0))
            return false;
        ActivationMove move{{std::string(source), std::string(name)},
                            {numbers[0], numbers[1]}, {numbers[2], numbers[3]}};
        entries.push_back(move.entry);
        moves.push_back(std::move(move));
    }
    return valid_mappings(entries);
}

// 0 means absent, 1 means exactly this directory, -1 means foreign/error.
int object_at(int parent, const std::string &name, const ObjectIdentity &object)
{
    struct stat info{};
    if (fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : -1;
    return object.inode != 0 && same_identity(info, object) ? 1 : -1;
}

bool rollback_activation(int session, int extraction, int addons, int old,
                          const std::vector<ActivationMove> &moves)
{
    bool ok = true;
    for (auto move = moves.rbegin(); move != moves.rend(); ++move)
    {
        std::string leaf;
        Descriptor parent(open_relative_parent(extraction, move->entry.source, leaf));
        if (parent.get() < 0)
        {
            ok = false;
            continue;
        }
        int source = object_at(parent.get(), leaf, move->staged);
        const int target_new = object_at(addons, move->entry.name, move->staged);
        if (source < 0 || (source == 1 && target_new == 1))
        {
            ok = false;
            continue;
        }
        if (target_new == 1)
        {
            if (source != 0 || renameat(addons, move->entry.name.c_str(), parent.get(),
                                        leaf.c_str()) != 0 ||
                fsync(parent.get()) != 0 || fsync(addons) != 0)
            {
                ok = false;
                continue;
            }
            source = 1;
        }
        const int backup = object_at(old, move->entry.name, move->previous);
        if (backup == 1)
        {
            if (object_at(addons, move->entry.name, {}) != 0 ||
                renameat(old, move->entry.name.c_str(), addons, move->entry.name.c_str()) != 0 ||
                fsync(old) != 0 || fsync(addons) != 0)
                ok = false;
        }
        else if (backup < 0 ||
                 object_at(addons, move->entry.name, move->previous) !=
                     (move->previous.inode == 0 ? 0 : 1))
            ok = false;
        if (source != 1)
            ok = false;
    }
    if (!ok || fsync(extraction) != 0 || fsync(session) != 0)
        return false;
    return unlinkat(session, ".activation", 0) == 0 && fsync(session) == 0;
}

bool recover_activation(int session, const std::string &wow_root, std::string &error)
{
    struct stat info{};
    if (fstatat(session, ".activation", &info, AT_SYMLINK_NOFOLLOW) != 0)
    {
        if (errno != ENOENT)
            return false;
        Descriptor old(open_directory_child(session, "old"));
        std::vector<std::string> names;
        return old.get() < 0 ? errno == ENOENT
                             : directory_names(old.get(), names) && names.empty();
    }
    std::string channel;
    ObjectIdentity destination;
    std::vector<ActivationMove> moves;
    if (!read_activation_journal(session, channel, destination, moves))
    {
        set_error(error, "activation journal is invalid; backups retained");
        return false;
    }
    if (fstatat(session, ".activated", &info, AT_SYMLINK_NOFOLLOW) == 0)
    {
        std::string marker;
        if (!read_private_file(session, ".activated", marker) || !marker.empty())
            return false;
        return true; // Durable commit: never restore backups over user edits.
    }
    if (errno != ENOENT)
        return false;
    Descriptor extraction(open_directory_child(session, "x"));
    Descriptor old(open_directory_child(session, "old"));
    Descriptor addons(open_addons_directory(wow_root, channel, false));
    if (extraction.get() < 0 || old.get() < 0 || addons.get() < 0 ||
        fstat(addons.get(), &info) != 0 || !same_identity(info, destination) ||
        !rollback_activation(session, extraction.get(), addons.get(), old.get(), moves))
    {
        set_error(error, "activation recovery is incomplete; backups retained");
        return false;
    }
    return true;
}

bool clean_session_at(int root, const std::string &wow_root, const std::string &id,
                       std::string &error)
{
    Descriptor session(open_session(root, id));
    if (session.get() < 0)
    {
        if (errno == ENOENT)
            return true;
        set_error(error, "archive session is not a protected directory");
        return false;
    }
    if (fchmod(session.get(), 0700) != 0 || !recover_activation(session.get(), wow_root, error))
        return false;
    if (!remove_tree_at(root, id))
    {
        set_error(error, "could not clean archive staging");
        return false;
    }
    return true;
}
} // namespace

bool activate_session(const std::string &root, const std::string &wow_root,
                      const std::string &id, const std::string &channel,
                      const std::vector<ActivationEntry> &entries,
                      const std::function<bool()> &cancelled,
                      std::uint64_t &move_ms, std::string &error)
{
    ActivationCancellation stop{cancelled};
    if (stop())
    {
        set_error(error, "cancelled");
        return false;
    }
    if (!valid_operation_id(id) || !valid_channel(channel) || !valid_mappings(entries, &stop))
    {
        set_error(error, stop.stopped ? "cancelled" :
                                       "activation paths are unsafe, overlapping or ambiguous");
        return false;
    }
    Descriptor root_descriptor(open_root(root));
    Descriptor session(open_session(root_descriptor.get(), id));
    if (session.get() < 0 || stop() || fchmod(session.get(), 0700) != 0 ||
        !recover_activation(session.get(), wow_root, error))
        return false;
    struct stat info{};
    if (fstatat(session.get(), ".activated", &info, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT)
    {
        set_error(error, "session has already been activated");
        return false;
    }
    Descriptor extraction(open_directory_child(session.get(), "x"));
    Descriptor addons(open_addons_directory(wow_root, channel, true, &stop));
    if (!protected_directory(extraction.get()) || addons.get() < 0 ||
        fstat(addons.get(), &info) != 0)
    {
        set_error(error, stop.stopped ? "cancelled" : "could not open activation directories");
        return false;
    }
    const auto destination = identity(info);
    std::vector<std::string> installed;
    std::map<std::string, std::string> installed_names;
    if (!directory_names(addons.get(), installed, &stop))
        return false;
    for (const auto &name : installed)
    {
        if (stop())
        {
            set_error(error, "cancelled");
            return false;
        }
        const auto [found, inserted] = installed_names.emplace(ascii_lower(name), name);
        if (!inserted)
            found->second.clear(); // An ambiguous existing spelling is never a destination.
    }
    std::vector<ActivationMove> moves;
    std::size_t checked = 0;
    for (const auto &entry : entries)
    {
        std::string leaf;
        Descriptor parent(open_relative_parent(extraction.get(), entry.source, leaf));
        Descriptor source(open_directory_child(parent.get(), leaf.c_str()));
        struct stat staged{}, previous{};
        const auto existing = installed_names.find(ascii_lower(entry.name));
        if (stop() || source.get() < 0 || fstat(source.get(), &staged) != 0 ||
            !staged_tree(source.get(), checked, 0, false, &stop) ||
            (existing != installed_names.end() && existing->second != entry.name))
        {
            set_error(error, stop.stopped ? "cancelled" : "activation source or destination is unsafe");
            return false;
        }
        ActivationMove move{entry, identity(staged), {}};
        if (fstatat(addons.get(), entry.name.c_str(), &previous, AT_SYMLINK_NOFOLLOW) == 0)
        {
            if (!S_ISDIR(previous.st_mode))
            {
                set_error(error, "installed add-on is not a real directory");
                return false;
            }
            move.previous = identity(previous);
        }
        else if (errno != ENOENT)
            return false;
        moves.push_back(std::move(move));
    }
    if (stop() || (mkdirat(session.get(), "old", 0700) != 0 && errno != EEXIST))
        return false;
    Descriptor old(open_directory_child(session.get(), "old"));
    std::vector<std::string> old_names;
    if (!protected_directory(old.get()) || !directory_names(old.get(), old_names) ||
        !old_names.empty() || stop() ||
        !write_activation_journal(session.get(), channel, destination, moves))
    {
        set_error(error, stop.stopped ? "cancelled" : "could not persist activation journal");
        return false;
    }
    const auto started = now_ms();
    bool ok = true;
    for (const auto &move : moves)
    {
        std::string leaf;
        Descriptor parent(open_relative_parent(extraction.get(), move.entry.source, leaf));
        Descriptor source(open_directory_child(parent.get(), leaf.c_str()));
        std::size_t prepared = 0;
        if (stop() || parent.get() < 0 || source.get() < 0 ||
            object_at(parent.get(), leaf, move.staged) != 1 ||
            object_at(addons.get(), move.entry.name, move.previous) !=
                (move.previous.inode == 0 ? 0 : 1) ||
            fchmod(source.get(), 0700) != 0 ||
            !staged_tree(source.get(), prepared, 0, true, &stop))
        {
            ok = false;
            break;
        }
        // Descendants are editable, but their top-level directory remains private.
        if (move.previous.inode != 0 &&
            (stop() || renameat(addons.get(), move.entry.name.c_str(), old.get(),
                       move.entry.name.c_str()) != 0 ||
             object_at(old.get(), move.entry.name, move.previous) != 1 ||
             fsync(addons.get()) != 0 || fsync(old.get()) != 0))
        {
            ok = false;
            break;
        }
        if (stop() || object_at(addons.get(), move.entry.name, {}) != 0 ||
            renameat(parent.get(), leaf.c_str(), addons.get(), move.entry.name.c_str()) != 0 ||
            fsync(parent.get()) != 0 || fsync(addons.get()) != 0)
        {
            ok = false;
            break;
        }
    }
    if (ok)
        for (const auto &move : moves)
        {
            Descriptor installed(open_directory_child(addons.get(), move.entry.name.c_str()));
            if (installed.get() < 0 || fstat(installed.get(), &info) != 0 ||
                !same_identity(info, move.staged) || stop() || fchmod(installed.get(), 0777) != 0 ||
                fsync(installed.get()) != 0)
            {
                ok = false;
                break;
            }
        }
    if (ok)
        ok = fsync(addons.get()) == 0 && !stop() &&
             persist_file(session.get(), ".activated", "", &stop);
    if (!ok)
    {
        const bool marker_removed =
            (unlinkat(session.get(), ".activated", 0) == 0 || errno == ENOENT) &&
            fsync(session.get()) == 0;
        const bool restored = marker_removed &&
            rollback_activation(session.get(), extraction.get(), addons.get(), old.get(), moves);
        set_error(error, restored ? (stop.stopped ? "cancelled and rolled back" :
                                                   "activation failed and was rolled back")
                                 : "activation failed; recovery journal and backups retained");
    }
    else
        error.clear();
    move_ms = now_ms() - started;
    return ok;
}

bool clean_session(const std::string &root, const std::string &wow_root,
                   const std::string &id, std::string &error)
{
    if (!valid_operation_id(id))
    {
        set_error(error, "invalid archive operation");
        return false;
    }
    Descriptor descriptor(open_root(root));
    if (descriptor.get() < 0 || !clean_session_at(descriptor.get(), wow_root, id, error))
        return false;
    error.clear();
    return true;
}

bool clean_all_sessions(const std::string &root, const std::string &wow_root, std::string &error)
{
    Descriptor descriptor(open_root(root));
    std::vector<std::string> names;
    if (descriptor.get() < 0 || !directory_names(descriptor.get(), names))
    {
        set_error(error, "could not open protected archive staging");
        return false;
    }
    bool ok = true;
    for (const auto &id : names)
        if (valid_operation_id(id) && !clean_session_at(descriptor.get(), wow_root, id, error))
            ok = false;
    if (ok)
        error.clear();
    return ok;
}
} // namespace forever::addon_worker
