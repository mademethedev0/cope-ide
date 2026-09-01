// Why this file exists
// --------------------
// The POSIX implementation of ide::host::Host. Pure Linux/POSIX syscalls so it
// builds on the CI runners as-is; Windows is out of scope.
//
// Discipline enforced here, in order of past-pain:
//   * ZERO-LENGTH FILES: Linux mmap of length 0 fails with EINVAL, so mapFile
//     special-cases size 0 *after* fstat and returns a valid empty MappedFile
//     (data() == nullptr is explicitly allowed then by host.h).
//   * FD HYGIENE: every descriptor is closed on every path - success, failure,
//     short read, EINTR. The only fd whose lifetime outlives a function return
//     is the one inside a successful mmap, and that is closed immediately after
//     mmap because the mapping keeps the file open by itself.
//   * SHORT READS: read(2) may return fewer bytes than asked for (pipes,
//     signals, whatever); readFile loops until EOF, it never trusts one read.

#include <posix_host.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ide::host {

namespace {

/// syscalls need NUL-terminated paths; string_view is not. One copy per call.
std::string cPath(std::string_view path) { return std::string(path); }

/// MappedFile over one mmap region (or a size-0 placeholder). Defined here so
/// the mapping type never leaks into a header.
class PosixMappedFile final : public MappedFile {
public:
    /// `map` is null only for the empty-file case, where there is nothing to
    /// unmap; size 0 with a non-null map cannot happen by construction.
    PosixMappedFile(void* map, size_t size) noexcept : map_(map), size_(size) {}

    ~PosixMappedFile() override {
        if (map_ != nullptr && size_ > 0) {
            ::munmap(map_, size_);
        }
    }

    const char* data() const noexcept override {
        return map_ != nullptr ? static_cast<const char*>(map_) : nullptr;
    }
    size_t size() const noexcept override { return size_; }

private:
    void* map_;
    size_t size_;
};

/// open(2) with O_CLOEXEC, or -1.
int openReadonly(const std::string& path) { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); }

/// fstat on an open fd; false on failure. `st` is zero-filled first so a failed
/// call can never leave stale mode bits behind.
bool fstatFd(int fd, struct stat& st) {
    std::memset(&st, 0, sizeof(st));
    return ::fstat(fd, &st) == 0;
}

/// write(2) in a loop with EINTR retry. False on a real failure or a partial
/// write that cannot be completed (the kernel said 0, which must not spin).
bool writeAll(int fd, std::string_view data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

std::optional<std::string> PosixHost::readFile(std::string_view path) {
    const std::string p = cPath(path);
    const int fd = openReadonly(p);
    if (fd < 0) {
        return std::nullopt;
    }

    struct stat st {};
    if (!fstatFd(fd, st) || S_ISDIR(st.st_mode)) {
        ::close(fd);
        return std::nullopt;
    }
    std::string out;
    if (st.st_size > 0) {
        out.reserve(static_cast<size_t>(st.st_size));
    }

    char buffer[65536];
    for (;;) {
        ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0) {
            break;  // EOF
        }
        out.append(buffer, static_cast<size_t>(n));
    }

    ::close(fd);
    return out;
}

bool PosixHost::writeFile(std::string_view path, std::string_view data) {
    const std::string p = cPath(path);
    const std::string tmp = p + ".tmp-cope";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }

    // Every failure path unlinks the temp so no debris is left behind; only a
    // successful rename publishes the bytes, which is what makes the write
    // atomic from the reader's point of view.
    bool ok = writeAll(fd, data);
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), p.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> PosixHost::readDir(std::string_view path) {
    std::vector<std::string> names;
    const std::string p = cPath(path);

    DIR* dir = ::opendir(p.c_str());
    if (dir == nullptr) {
        return names;
    }

    // A leading '.' covers ".", ".." and every hidden file in one test, which
    // is exactly the "names only, no dot entries" contract in host.h.
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        names.emplace_back(entry->d_name);
    }
    ::closedir(dir);
    return names;
}

std::optional<FileInfo> PosixHost::stat(std::string_view path) {
    const std::string p = cPath(path);
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) {
        return std::nullopt;
    }

    FileInfo info;
    info.isDirectory = S_ISDIR(st.st_mode) != 0;
    info.size = (!info.isDirectory && st.st_size > 0) ? static_cast<size_t>(st.st_size) : 0u;
    return info;
}

std::unique_ptr<MappedFile> PosixHost::mapFile(std::string_view path) {
    const std::string p = cPath(path);
    const int fd = openReadonly(p);
    if (fd < 0) {
        return nullptr;
    }

    struct stat st {};
    if (!fstatFd(fd, st) || S_ISDIR(st.st_mode)) {
        ::close(fd);
        return nullptr;
    }

    // THE empty-file special case: mmap(0) fails with EINVAL on Linux, so an
    // empty file gets a valid placeholder mapping with data() == nullptr,
    // which host.h explicitly permits when size() == 0.
    if (st.st_size <= 0) {
        ::close(fd);
        return std::make_unique<PosixMappedFile>(nullptr, 0u);
    }
    const size_t size = static_cast<size_t>(st.st_size);

    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    // The mapping holds its own reference to the file, so the fd is closed on
    // every path from here - success included.
    ::close(fd);
    if (map == MAP_FAILED) {
        return nullptr;
    }
    return std::make_unique<PosixMappedFile>(map, size);
}

}  // namespace ide::host
