#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <ide/text/byte_source.h>
#include <ide/text/piece_table.h>
#include <ide/text/undo.h>

namespace ide::text {

/// Byte range of one line. `start`..`end` bound the line *content*; the line
/// terminator occupies [end, end + terminatorLength) and is 0 bytes (last line
/// of a file with no trailing newline), 1 byte (LF) or 2 bytes (CRLF).
///
/// Splitting the terminator out is what lets CRLF behave as a single line break
/// for navigation while the exact bytes stay in the buffer for saving: nothing
/// in this module ever rewrites a '\r'.
struct LineRange {
  size_t start = 0;
  size_t end = 0;
  size_t terminatorLength = 0;

  /// Content length in bytes, terminator excluded.
  size_t length() const noexcept { return end - start; }
  /// One past the terminator, i.e. the next line's start (or size()).
  size_t rawEnd() const noexcept { return end + terminatorLength; }
  bool operator==(const LineRange&) const = default;
};

/// 0-based line/column pair. `column` is a **byte** offset within the line, not
/// a codepoint index and not a display column -- see Document::displayColumnOf
/// for the tab-aware visual column. Both fields are int64_t per the convention
/// that line/column indices are int64_t while byte offsets are size_t.
struct Position {
  int64_t line = 0;
  int64_t column = 0;
  bool operator==(const Position&) const = default;
};

/// Per-edit knobs. `timestampMs` exists so tests are deterministic: pass an
/// explicit monotonic millisecond value and coalescing becomes reproducible.
/// -1 means "read the steady clock".
struct EditOptions {
  int64_t timestampMs = -1;
  bool coalesce = true;
};

/// Why: the single façade the rest of the IDE talks to. It owns the piece
/// table (bytes), the newline aggregation (lines) and the undo tree (history),
/// and it is the only place where those three are kept consistent.
///
/// Index conventions, repeated at every boundary below:
///   * byte offsets are size_t and are clamped, never asserted;
///   * line and column indices are 0-based int64_t;
///   * lineCount() is newlineCount + 1, so an empty document has 1 line and
///     "a\n" has 2 (the second one empty) -- the usual editor convention.
///
/// Costs: insert/erase are O(log pieces) plus the length of the edit. Nothing
/// here is proportional to document size, so a 500 MB file edits exactly as
/// fast as a 5 KB one. The one linear pass over the file is a memchr newline
/// count at construction (see PieceTable).
class Document {
public:
  /// Sentinel for redo(): follow the most recently created branch.
  static constexpr size_t kNewestBranch = static_cast<size_t>(-1);

  /// Empty document.
  Document();

  /// Copies `bytes` into an OwnedByteSource. Convenience for tests and the CLI.
  explicit Document(std::string bytes);

  /// Zero-copy: the original bytes are referenced, never duplicated. This is
  /// the path a Host-provided mmap takes.
  explicit Document(std::shared_ptr<const ByteSource> original);

  // --- state -------------------------------------------------------------

  /// Document length in bytes.
  size_t size() const noexcept { return pieces_.size(); }

  /// Monotonically increasing mutation counter. Bumped by every insert, erase,
  /// undo and redo that actually changes bytes; a no-op edit does not bump it.
  ///
  /// PURPOSE: later phases run tokenisation, diagnostics and indexing
  /// asynchronously. They capture version() before starting and compare it on
  /// completion; if it moved, the result describes a document that no longer
  /// exists and must be discarded. Never reused, never decremented, never
  /// wrapped in practice (int64_t).
  int64_t version() const noexcept { return version_; }

  /// 0-based line count; always >= 1.
  int64_t lineCount() const noexcept { return pieces_.newlineCount() + 1; }

  const PieceTable& pieces() const noexcept { return pieces_; }
  const UndoTree& history() const noexcept { return undo_; }

  // --- reading -----------------------------------------------------------

  /// Copies min(dest.size(), size() - offset) bytes from `offset`; returns the
  /// count copied. This is the allocation-free extraction path.
  size_t copyOut(size_t offset, std::span<char> dest) const;

  /// Zero-copy view when [offset, offset + length) lies inside a single piece.
  /// LIFETIME: valid only until the next mutation (see PieceTable::contiguous).
  std::optional<std::string_view> contiguousText(size_t offset, size_t length) const;

  /// Allocating helpers for tests, the CLI and small ranges.
  std::string text() const;
  std::string textRange(size_t offset, size_t length) const;

  /// Single byte, '\0' when out of range.
  char byteAt(size_t offset) const noexcept { return pieces_.byteAt(offset); }

  // --- lines -------------------------------------------------------------

  /// Byte range of 0-based `line`, clamped into [0, lineCount() - 1].
  LineRange lineAt(int64_t line) const;

  /// First byte offset of 0-based `line`; clamped.
  size_t lineStartOffset(int64_t line) const noexcept { return pieces_.lineStartOffset(line); }

  /// 0-based line + byte column of a byte offset (clamped to size()).
  Position lineColumnOf(size_t offset) const;

  /// Byte offset of 0-based (line, byteColumn). The column is clamped to the
  /// line's content end (a caret can never sit inside a CRLF) and snapped back
  /// to a codepoint boundary, so the result is always safe to edit at.
  size_t offsetOf(int64_t line, int64_t byteColumn) const;

  // --- UTF-8 aware movement ---------------------------------------------

  /// Next / previous codepoint boundary. These never return a mid-sequence
  /// offset for well-formed UTF-8, and always make progress (so caret movement
  /// loops terminate) even on invalid bytes.
  size_t nextCodepoint(size_t offset) const noexcept;
  size_t prevCodepoint(size_t offset) const noexcept;

  /// Moves `offset` back onto a codepoint boundary; a no-op if it already is.
  size_t snapToCodepointStart(size_t offset) const noexcept;

  /// Visual column of `offset`, counting from its line's start. Tabs advance to
  /// the next multiple of `tabWidth`; combining marks add 0; wide CJK/emoji add
  /// 2. `tabWidth` < 1 is treated as 1.
  /// Cost: O(bytes before `offset` on its line) -- lines are short in practice
  /// and this is a presentation-layer query, not an edit-path one.
  int64_t displayColumnOf(size_t offset, int tabWidth) const;

  /// Inverse of displayColumnOf: the offset on `line` whose display column is
  /// `displayColumn`, rounded *down* to a codepoint boundary so a caret can
  /// never land inside a tab or a multibyte sequence.
  size_t offsetOfDisplayColumn(int64_t line, int64_t displayColumn, int tabWidth) const;

  // --- mutation ----------------------------------------------------------

  /// Inserts `text` at `offset` (clamped to size()). Empty text is a no-op and
  /// does not bump version(). Leaves the cursor after the inserted text.
  void insert(size_t offset, std::string_view text, const EditOptions& options = {});

  /// Removes up to `length` bytes at `offset`; both are clamped. A range that
  /// removes nothing is a no-op and does not bump version(). Leaves the cursor
  /// at `offset`.
  void erase(size_t offset, size_t length, const EditOptions& options = {});

  /// Replaces [offset, offset + length) with `text` as **one** undo group.
  void replace(size_t offset, size_t length, std::string_view text,
               const EditOptions& options = {});

  // --- history -----------------------------------------------------------

  bool canUndo() const noexcept { return undo_.canUndo(); }
  bool canRedo() const noexcept { return undo_.canRedo(); }

  /// Reverts the current edit group and restores its "before" cursor.
  /// Returns false when there is nothing to undo.
  bool undo();

  /// Re-applies a redo branch (default: the newest) and restores its "after"
  /// cursor. Returns false when the branch does not exist.
  bool redo(size_t branchIndex = kNewestBranch);

  /// Number of redo branches at the current history node (> 1 means the user
  /// has diverged more than once from here).
  size_t redoBranchCount() const noexcept { return undo_.redoBranchCount(); }

  // --- cursor / config ---------------------------------------------------

  CursorState cursor() const noexcept { return cursor_; }

  /// Moving the caret explicitly breaks typing coalescing, which is exactly
  /// what a user expects after clicking somewhere else.
  void setCursor(CursorState cursor) noexcept { cursor_ = cursor; }

  int64_t coalesceWindowMs() const noexcept { return coalesceWindowMs_; }
  void setCoalesceWindowMs(int64_t windowMs) noexcept { coalesceWindowMs_ = windowMs; }

  /// Milliseconds from a steady clock. Used when EditOptions::timestampMs < 0.
  static int64_t nowMs() noexcept;

private:
  void recordEdit(EditRecord record, const CursorState& before, const CursorState& after,
                  const EditOptions& options, bool coalescable);
  /// A single non-newline codepoint: the only shape allowed to coalesce.
  static bool isTypingUnit(std::string_view bytes) noexcept;
  /// Line content of `line` as a contiguous string, for column walking.
  std::string lineContent(const LineRange& range) const;

  PieceTable pieces_;
  UndoTree undo_;
  CursorState cursor_{};
  int64_t version_ = 0;
  int64_t coalesceWindowMs_ = 500;
};

}  // namespace ide::text
