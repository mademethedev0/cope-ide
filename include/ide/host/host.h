#pragma once

// Why this file exists
// -------------------
// core/ has zero filesystem, Android, JNI and network dependencies - that is a
// binding rule in docs/CONVENTIONS.md. Every byte that enters the engine enters
// through this interface. Today the CLI and the tests inject a PosixHost or a
// MemoryHost; phase 4 injects an Android host backed by SAF/JNI, and a later
// web build reuses MemoryHost. The engine never knows which one it got.
//
// Error model: NO method throws. Failures surface as std::nullopt, false, an
// empty vector or a null unique_ptr. Callers that need a reason can look at
// errno/their own state; the engine only ever needs "did it work".

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ide::host {

/// A read-only mapping of a whole file, owned by whoever created it.
///
/// data()/size() are the whole file; data() may be null only when size() == 0
/// (Linux refuses mmap of length 0, so implementations must special-case empty
/// files and hand back a valid empty mapping instead).
class MappedFile {
public:
  virtual ~MappedFile() = default;

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&&) = delete;
  MappedFile& operator=(MappedFile&&) = delete;

  /// First byte of the mapping. Null only when size() == 0.
  virtual const char* data() const noexcept = 0;

  /// File length in bytes.
  virtual size_t size() const noexcept = 0;

protected:
  MappedFile() = default;
};

/// The minimal stat the engine needs. Deliberately not struct stat: no
/// timestamps, no permissions, nothing the engine has no use for.
struct FileInfo {
  size_t size = 0;         ///< bytes; 0 for directories and empty files
  bool isDirectory = false;
};

/// All IO the engine is allowed to perform, injected at the boundary.
class Host {
public:
  virtual ~Host() = default;

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  Host(Host&&) = delete;
  Host& operator=(Host&&) = delete;

  /// Whole file as a string. nullopt for missing / unreadable / directory.
  virtual std::optional<std::string> readFile(std::string_view path) = 0;

  /// Whole-file write, atomic where the platform allows (temp file + rename),
  /// so a crash cannot truncate the user's source. False only on real failure.
  virtual bool writeFile(std::string_view path, std::string_view data) = 0;

  /// Entry names directly inside `path`, no ordering guaranteed. Dot entries
  /// ("." / ".." and hidden files) are never included. Empty for a missing or
  /// non-directory `path` - indistinguishable from an empty directory, which is
  /// fine: callers who care call stat() first.
  virtual std::vector<std::string> readDir(std::string_view path) = 0;

  /// nullopt when the path does not exist.
  virtual std::optional<FileInfo> stat(std::string_view path) = 0;

  /// Zero-copy view of a whole file, or nullptr for missing / unreadable /
  /// directory. The mapping lives until the returned MappedFile is destroyed;
  /// bytes must remain stable and unchanged for its lifetime.
  virtual std::unique_ptr<MappedFile> mapFile(std::string_view path) = 0;

protected:
  Host() = default;
};

}  // namespace ide::host
