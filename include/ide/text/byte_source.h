#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ide::text {

/// Why: the piece table must be able to reference bytes it does not own, so
/// that opening a 500 MB file costs nothing beyond creating a single piece.
/// A ByteSource is an *immutable*, *contiguous*, *stable-address* byte range.
///
/// "Stable" is a hard requirement: pieces store (source, offset, length) and
/// resolve `data() + offset` lazily on every read, so the object may not move
/// its bytes for its whole lifetime. This is what lets a later phase swap an
/// OwnedByteSource for a real mmap without touching PieceTable at all.
///
/// core/ never opens, reads or maps a file itself: an implementation of this
/// interface is handed in from the Host layer (later phase).
class ByteSource {
public:
  ByteSource() = default;
  virtual ~ByteSource() = default;

  ByteSource(const ByteSource&) = delete;
  ByteSource& operator=(const ByteSource&) = delete;
  ByteSource(ByteSource&&) = delete;
  ByteSource& operator=(ByteSource&&) = delete;

  /// First byte of the range. May be null only when size() == 0.
  virtual const char* data() const noexcept = 0;

  /// Length in bytes. Byte lengths and offsets are always size_t.
  virtual size_t size() const noexcept = 0;

  /// Never produces a view over a null pointer, so callers may use it freely
  /// (std::string_view over (nullptr, 0) is not portable).
  std::string_view view() const noexcept {
    const size_t n = size();
    return n == 0 ? std::string_view{} : std::string_view{data(), n};
  }
};

/// Why: the only ByteSource that exists in phase 1. Holds the bytes in a
/// std::vector<char> so that unit tests and the dev CLI can build a Document
/// from an in-memory blob. Address stability is guaranteed because the vector
/// is never resized after construction.
class OwnedByteSource final : public ByteSource {
public:
  OwnedByteSource() = default;
  explicit OwnedByteSource(std::string bytes);
  explicit OwnedByteSource(std::vector<char> bytes);

  const char* data() const noexcept override;
  size_t size() const noexcept override;

  /// Convenience factory, because PieceTable/Document take a shared_ptr.
  static std::shared_ptr<const ByteSource> make(std::string bytes);
  static std::shared_ptr<const ByteSource> make(std::vector<char> bytes);

private:
  std::vector<char> bytes_;
};

/// Why: reserves the seam for memory-mapped files without pulling any OS API
/// into core/.
///
/// LANDS IN A LATER PHASE, together with the Host interface. This class does
/// **not** and must never call mmap/open/CreateFileMapping: mapping is Host
/// territory. The Host will create the mapping, keep it alive, and hand the
/// (pointer, length) pair here; MappedByteSource is a non-owning adopter, so
/// the Host outlives the Document.
///
/// Default-constructed it is a valid, empty source, which keeps the type
/// usable in tests today without any platform code.
class MappedByteSource final : public ByteSource {
public:
  MappedByteSource() = default;

  /// Adopts a mapping owned by the Host. `data` must stay valid and unchanged
  /// for the lifetime of this object. `data` may be null only if size == 0.
  MappedByteSource(const char* data, size_t size) noexcept;

  const char* data() const noexcept override;
  size_t size() const noexcept override;

  /// True once a Host-provided mapping has been adopted.
  bool isMapped() const noexcept { return data_ != nullptr; }

private:
  const char* data_ = nullptr;
  size_t size_ = 0;
};

}  // namespace ide::text
