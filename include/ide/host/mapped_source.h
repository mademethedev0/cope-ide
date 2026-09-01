#pragma once

// Why this file exists
// --------------------
// The bridge between the Host layer and the text engine: it OWNS a host
// MappedFile and presents it as a text::ByteSource, so opening a 500 MB file
// costs one mmap and zero copies - the Document's pieces point straight into
// the mapping.
//
// The existing text::MappedByteSource is a *non-owning adopter* by design and
// must stay that way (it lives in core/ and knows nothing about hosts). This
// adapter is the owning half: keep the MappedSource alive as long as the
// Document that consumes it, which shared_ptr ownership makes natural.

#include <memory>
#include <utility>

#include <ide/host/host.h>
#include <ide/text/byte_source.h>

namespace ide::host {

/// Owns a MappedFile and exposes its bytes as a ByteSource.
///
/// Default-constructed it is a valid empty source, so tests and optional
/// code paths can hold one without a mapping.
class MappedSource final : public ide::text::ByteSource {
public:
  MappedSource() = default;

  /// Takes ownership of `file`. A null unique_ptr yields a valid empty source
  /// rather than a landmine, matching ByteSource's "data() may be null only
  /// when size() == 0" contract.
  explicit MappedSource(std::unique_ptr<MappedFile> file) noexcept : file_(std::move(file)) {}

  const char* data() const noexcept override { return file_ ? file_->data() : nullptr; }
  size_t size() const noexcept override { return file_ ? file_->size() : 0u; }

  /// True once a mapping is owned.
  [[nodiscard]] bool isMapped() const noexcept { return file_ != nullptr; }

  /// Convenience factory for Document's shared_ptr<const ByteSource> parameter.
  static std::shared_ptr<const ide::text::ByteSource> make(std::unique_ptr<MappedFile> file) {
    return std::make_shared<MappedSource>(std::move(file));
  }

private:
  std::unique_ptr<MappedFile> file_;
};

}  // namespace ide::host
