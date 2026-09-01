#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ide/text/byte_source.h>
#include <ide/text/line_index.h>

namespace ide::text {

/// Which byte source a piece points into. Exactly two exist: the immutable
/// original (whatever was opened) and the append-only add buffer (everything
/// ever typed). Bytes are never copied out of the original and never moved
/// inside the add buffer, so a piece's (source, offset) stays valid forever.
enum class Source : uint8_t { Original = 0, Add = 1 };

/// A contiguous run of bytes in one source. The document is the concatenation
/// of the pieces in tree order. Byte offsets and lengths are size_t.
struct Piece {
  Source source = Source::Original;
  size_t offset = 0;
  size_t length = 0;
};

/// Why: opening a file must cost O(1), and an edit must cost O(log pieces)
/// regardless of file size or edit history.
///
/// DATA STRUCTURE: a **treap** (randomised balanced binary search tree) keyed
/// implicitly by byte rank, held in a flat arena (std::vector<Node>) with
/// uint32_t handles and a free list. Every node caches
///   - its piece's newline count,
///   - subtree total bytes,
///   - subtree total newlines,
/// which is what makes both "which piece holds byte N" and "where is the k-th
/// newline" logarithmic instead of linear.
///
/// Complexity, P = number of pieces:
///   construct            O(N) once, one memchr pass to index the original's
///                        newlines (see line_index.h). No byte is copied.
///   insert / erase       O(log P) expected, plus O(len) to copy the inserted
///                        text into the add buffer. No memmove of document
///                        bytes ever happens, at any size.
///   locate(offset)       O(log P) expected
///   newlinesBefore()     O(log P + 4 KiB scan)
///   offsetOfNewline()    O(log P + 4 KiB scan)
///   copyOut(len)         O(log P + len)
///
/// Why a treap and not a red-black tree: split/join are ~15 lines each and are
/// the *only* two structural primitives needed (insert = split+join+join,
/// erase = split+split+join), so there is no delete-rebalancing code to get
/// wrong. Expected height is ~1.39 log2 P; priorities come from a fixed-seed
/// splitmix64 so behaviour is fully deterministic and reproducible in tests.
/// The alternative "vector of pieces + binary search" was rejected: an insert
/// in the middle of a 5 M-piece list is a 60 MB memmove, i.e. milliseconds per
/// keystroke, which violates the phase goal.
///
/// Typing fast path: inserting at the exact end of a piece that is also the
/// tail of the add buffer extends that piece in place instead of allocating a
/// node, so a typing run of k characters produces one piece, not k.
///
/// Thread safety: none. const methods are pure reads (all lazy work happens on
/// the mutating path) so concurrent readers are safe, but a writer excludes
/// everything.
class PieceTable {
public:
  /// Empty document.
  PieceTable();

  /// Takes the original bytes by shared_ptr; they are never copied. A null
  /// pointer is treated as an empty source.
  explicit PieceTable(std::shared_ptr<const ByteSource> original);

  /// Total document length in bytes.
  size_t size() const noexcept;

  /// Number of '\n' bytes in the whole document. lineCount() == this + 1.
  int64_t newlineCount() const noexcept;

  /// Number of pieces currently in the tree (diagnostics / tests).
  size_t pieceCount() const noexcept;

  /// Bytes accumulated in the add buffer, including bytes no longer referenced
  /// by any piece (the add buffer is never compacted). Diagnostics / tests.
  size_t addBufferSize() const noexcept;

  /// Inserts `text` at byte `offset` (clamped to size()). No-op when empty.
  /// `text` may safely alias this table's own add buffer.
  void insert(size_t offset, std::string_view text);

  /// Removes up to `length` bytes starting at `offset`. Out-of-range offsets
  /// and over-long lengths are clamped; nothing is removed past the end.
  void erase(size_t offset, size_t length);

  /// Copies min(dest.size(), size() - offset) bytes into `dest` and returns
  /// the number copied. 0 when `offset` is at/after the end.
  size_t copyOut(size_t offset, std::span<char> dest) const;

  /// A zero-copy view of [offset, offset + length) when that range lies inside
  /// one piece, otherwise nullopt. length == 0 always yields an empty view.
  ///
  /// LIFETIME: a view into the add buffer is invalidated by the next mutation
  /// (the add buffer reallocates as it grows). A view into the original stays
  /// valid as long as the ByteSource lives.
  std::optional<std::string_view> contiguous(size_t offset, size_t length) const;

  /// Single byte read; '\0' when `offset` is out of range.
  char byteAt(size_t offset) const noexcept;

  /// Number of '\n' in [0, offset), i.e. the 0-based line index of `offset`.
  /// `offset` is clamped to size(). Returns int64_t per the line-index rule.
  int64_t newlinesBefore(size_t offset) const noexcept;

  /// Document offset of the `rank`-th (0-based) '\n'.
  /// Returns size() if rank is out of [0, newlineCount()).
  size_t offsetOfNewline(int64_t rank) const noexcept;

  /// First byte offset of 0-based `line`. Clamped: negative lines give 0 and
  /// lines past the last give size().
  size_t lineStartOffset(int64_t line) const noexcept;

  /// In-order piece list. Tests only -- O(P).
  std::vector<Piece> pieceList() const;

  /// Height of the treap. Tests only -- O(P). Used to assert balance.
  int64_t treeHeight() const;

private:
  static constexpr uint32_t kNil = 0;

  struct Node {
    Piece piece{};
    int64_t pieceNewlines = 0;
    size_t subtreeBytes = 0;
    int64_t subtreeNewlines = 0;
    uint32_t left = kNil;
    uint32_t right = kNil;
    uint32_t priority = 0;
  };

  struct Located {
    uint32_t node = kNil;
    size_t nodeStart = 0;  ///< document offset of the piece's first byte
    size_t within = 0;     ///< offset inside the piece, < piece.length
  };

  // --- arena -------------------------------------------------------------
  // INVARIANT: nodes_[kNil] is a zeroed sentinel so aggregate reads need no
  // null checks. It is never handed out by allocNode and never mutated.
  uint32_t allocNode(const Piece& piece, int64_t newlines);
  void freeSubtree(uint32_t node);
  void update(uint32_t node) noexcept;
  uint32_t nextPriority() noexcept;

  size_t bytesOf(uint32_t node) const noexcept { return nodes_[node].subtreeBytes; }
  int64_t newlinesOf(uint32_t node) const noexcept { return nodes_[node].subtreeNewlines; }

  // --- treap primitives --------------------------------------------------
  // NOTE: outLeft/outRight must be locals, never fields of nodes_, because
  // split may reallocate the arena.
  void split(uint32_t node, size_t at, uint32_t& outLeft, uint32_t& outRight);
  uint32_t join(uint32_t left, uint32_t right);

  std::optional<Located> locate(size_t offset) const noexcept;
  bool extendAt(uint32_t node, size_t offset, std::string_view text);
  void copyRange(uint32_t node, size_t from, size_t to, char*& out) const;
  void collect(uint32_t node, std::vector<Piece>& out) const;
  int64_t heightOf(uint32_t node) const;

  // --- sources -----------------------------------------------------------
  std::string_view sourceView(Source source) const noexcept;
  const SourceLineTable& tableFor(Source source) const noexcept;
  int64_t newlinesInSource(Source source, size_t from, size_t to) const noexcept;
  size_t kthNewlineInSource(Source source, size_t from, size_t to, int64_t k) const noexcept;
  void appendAdd(std::string_view text);
  bool aliasesAdd(std::string_view text) const noexcept;

  std::shared_ptr<const ByteSource> original_;
  std::vector<char> add_;
  SourceLineTable originalLines_;
  SourceLineTable addLines_;
  std::vector<Node> nodes_;
  std::vector<uint32_t> free_;
  uint32_t root_ = kNil;
  uint64_t rng_ = 0x243F6A8885A308D3ull;  ///< fixed seed: deterministic shape
};

}  // namespace ide::text
