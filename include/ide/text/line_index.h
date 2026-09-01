#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ide::text {

/// Counts '\n' bytes in `bytes` using memchr. This is the only newline scanner
/// in the module; everything else is built on it.
int64_t countNewlines(std::string_view bytes) noexcept;

/// Why: the "line index" of this editor is deliberately *not* a flat vector of
/// document line-start offsets. A flat vector must be shifted on every edit,
/// which is an O(lines) memmove -- 80 MB of memmove per keystroke on a 500 MB
/// file. Instead the line index is split in two:
///
///   1. Per *byte source* (immutable original, append-only add buffer) we keep
///      a block-prefix table of newline counts. Source offsets never move, so
///      these tables are never shifted -- the add-buffer table is only ever
///      *appended to*, exactly like the buffer itself.
///   2. The document-order aggregation lives in the piece tree, which carries
///      a newline count per subtree (see piece_table.h). An edit therefore
///      updates O(log pieces) counters and rescans nothing.
///
/// SourceLineTable is layer 1: for a source of N bytes it stores one int64_t
/// per 4 KiB block, i.e. N/512 bytes of overhead (1 MiB for a 500 MB file) --
/// versus 8 bytes per line for an exact newline-offset vector (80 MB for the
/// same file at 10 M lines). Queries pay at most one 4 KiB memchr scan, which
/// is sub-microsecond, so both query kinds stay effectively O(log N).
///
/// Cost model:
///   extendTo()       O(new bytes)      one memchr pass, ~GB/s
///   newlinesIn()     O(log N + 4 KiB)
///   kthNewline()     O(log N + 4 KiB)
///
/// The byte view is passed in on *every* call rather than stored, because the
/// add buffer reallocates as it grows and a cached pointer would dangle.
class SourceLineTable {
public:
  /// Block size. Smaller = more memory, shorter scans. 4 KiB keeps the worst
  /// case scan under a microsecond while costing 0.2% of the source size.
  static constexpr size_t kBlockBytes = 4096;

  /// Establishes the empty invariant (prefix_ == {0}, covered_ == 0).
  SourceLineTable() { clear(); }

  /// Back to "covers zero bytes".
  void clear();

  /// Grows coverage to `bytes.size()`. Append-only fast path: block entries
  /// that lie wholly inside the previous coverage are kept, so extending by k
  /// bytes costs O(k) and not O(bytes.size()).
  ///
  /// If `bytes` is *shorter* than the current coverage the table is rebuilt
  /// from scratch (defensive; sources are append-only in practice).
  void extendTo(std::string_view bytes);

  /// Number of bytes currently covered. Callers must keep this equal to the
  /// source size before querying.
  size_t coveredBytes() const noexcept { return covered_; }

  /// Newlines in [0, coveredBytes()).
  int64_t totalNewlines() const noexcept { return prefix_.empty() ? 0 : prefix_.back(); }

  /// Newlines in [0, pos). `pos` is clamped to the covered range.
  int64_t newlinesBefore(std::string_view bytes, size_t pos) const noexcept;

  /// Newlines in [from, to). Returns 0 when the range is empty or inverted.
  int64_t newlinesIn(std::string_view bytes, size_t from, size_t to) const noexcept;

  /// Byte offset of the `k`-th (0-based) '\n' inside [from, to).
  /// Precondition: 0 <= k < newlinesIn(bytes, from, to). If the precondition
  /// is violated the function returns `to` rather than misbehaving.
  size_t kthNewline(std::string_view bytes, size_t from, size_t to, int64_t k) const noexcept;

  /// Number of block entries; exposed for tests only.
  size_t blockEntryCount() const noexcept { return prefix_.size(); }

private:
  /// prefix_[i] == number of '\n' in bytes[0, min(i * kBlockBytes, covered_)).
  /// Invariants: prefix_ is never empty, prefix_[0] == 0 and prefix_.size() ==
  /// ceil(covered_ / B) + 1, so prefix_.back() is always the total for the
  /// covered range.
  std::vector<int64_t> prefix_;
  size_t covered_ = 0;
};

}  // namespace ide::text
