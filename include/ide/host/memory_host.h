#pragma once

// Why this file exists
// -------------------
// An in-memory Host: a std::map of paths to entries. Two consumers:
//   * tests - they never touch the real filesystem, so a test that fails says
//     something about the engine, not about the machine;
//   * a future emscripten/web build, where there is no real filesystem at all.
//
// The tree is flat and path-shaped: keys are normalized paths ("/a/b"), a
// directory exists iff it is a key with isDirectory set. std::map is node-based
// so references to values are stable across inserts, which is what makes
// mapFile() safe to hand out pointers into the stored bytes.

#include <map>
#include <string>
#include <string_view>

#include <ide/host/host.h>

namespace ide::host {

class MemoryHost final : public Host {
public:
  /// Seeds a file (and nothing else) at `path`. Parents are NOT created
  /// implicitly; call addDir for each of them if a test wants them to exist.
  void addFile(std::string_view path, std::string_view bytes);
  /// Seeds an empty directory at `path`.
  void addDir(std::string_view path);

  // --- Host ----------------------------------------------------------------
  std::optional<std::string> readFile(std::string_view path) override;
  bool writeFile(std::string_view path, std::string_view data) override;
  std::vector<std::string> readDir(std::string_view path) override;
  std::optional<FileInfo> stat(std::string_view path) override;
  std::unique_ptr<MappedFile> mapFile(std::string_view path) override;

private:
  struct Entry {
    std::string bytes;
    bool isDirectory = false;
  };

  /// Trailing slashes off (except for the root "/"), so "a/b" and "a/b/" are
  /// the same path. Empty paths stay empty and simply never match.
  [[nodiscard]] static std::string normalize(std::string_view path);

  std::map<std::string, Entry> entries_;
};

}  // namespace ide::host
