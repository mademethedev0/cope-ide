#include <ide/host/memory_host.h>

namespace ide::host {

namespace {

/// A MappedFile over bytes owned by the MemoryHost's map. The Entry is
/// node-based and stable, and MemoryHost never erases entries, so the pointer
/// stays valid until this object is destroyed. An empty file maps to a valid
/// non-null data() (std::string's data() is always a valid pointer) with
/// size() == 0, satisfying the "no mmap of length 0" contract by construction.
class MemoryMappedFile final : public MappedFile {
public:
  explicit MemoryMappedFile(const std::string& bytes) noexcept : bytes_(&bytes) {}

  const char* data() const noexcept override { return bytes_->data(); }
  size_t size() const noexcept override { return bytes_->size(); }

private:
  const std::string* bytes_;
};

bool isDotEntry(std::string_view name) { return !name.empty() && name.front() == '.'; }

}  // namespace

void MemoryHost::addFile(std::string_view path, std::string_view bytes) {
  entries_[normalize(path)] = Entry{std::string(bytes), false};
}

void MemoryHost::addDir(std::string_view path) {
  Entry entry;
  entry.isDirectory = true;
  entries_[normalize(path)] = std::move(entry);
}

std::string MemoryHost::normalize(std::string_view path) {
  std::string out(path);
  while (out.size() > 1 && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

std::optional<std::string> MemoryHost::readFile(std::string_view path) {
  const auto it = entries_.find(normalize(path));
  if (it == entries_.end() || it->second.isDirectory) {
    return std::nullopt;
  }
  return it->second.bytes;
}

bool MemoryHost::writeFile(std::string_view path, std::string_view data) {
  // In-memory, so the write is trivially atomic: the map node is assigned in
  // one step and the old bytes stay intact until the assignment lands.
  entries_[normalize(path)] = Entry{std::string(data), false};
  return true;
}

std::vector<std::string> MemoryHost::readDir(std::string_view path) {
  std::vector<std::string> names;
  const std::string prefix = normalize(path);
  const std::string base = prefix.empty() ? std::string() : prefix + "/";
  for (const auto& [key, entry] : entries_) {
    if (key.size() <= base.size() || key.compare(0, base.size(), base) != 0) {
      continue;
    }
    const std::string_view rest(key.data() + base.size(), key.size() - base.size());
    if (rest.find('/') != std::string_view::npos) {
      continue;  // not a direct child
    }
    if (isDotEntry(rest)) {
      continue;
    }
    names.push_back(std::string(rest));
  }
  return names;
}

std::optional<FileInfo> MemoryHost::stat(std::string_view path) {
  const auto it = entries_.find(normalize(path));
  if (it == entries_.end()) {
    return std::nullopt;
  }
  FileInfo info;
  info.isDirectory = it->second.isDirectory;
  info.size = it->second.isDirectory ? 0u : it->second.bytes.size();
  return info;
}

std::unique_ptr<MappedFile> MemoryHost::mapFile(std::string_view path) {
  const auto it = entries_.find(normalize(path));
  if (it == entries_.end() || it->second.isDirectory) {
    return nullptr;
  }
  return std::make_unique<MemoryMappedFile>(it->second.bytes);
}

}  // namespace ide::host
