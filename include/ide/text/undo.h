#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ide::text {

/// Cursor + selection anchor as byte offsets. Captured before and after every
/// edit group so undo/redo can put the user back exactly where they were --
/// restoring only the text is what makes undo feel broken.
///
/// Offsets are size_t; line/column are derived on demand (see Document) and
/// deliberately not stored, because they would go stale on every edit.
struct CursorState {
  size_t offset = 0;
  size_t anchor = 0;
  bool operator==(const CursorState&) const = default;
};

/// One primitive, exactly invertible mutation of the byte sequence:
/// `removed` was at [offset, offset + removed.size()) and has been replaced by
/// `inserted`. A pure insert has empty `removed`, a pure erase empty
/// `inserted`. Both non-empty describes a replacement.
///
/// The removed bytes are stored verbatim, which is what makes inversion exact
/// (including CRLF and invalid UTF-8 -- nothing is normalised, ever).
struct EditRecord {
  size_t offset = 0;
  std::string removed;
  std::string inserted;
};

/// The unit of undo: what one Ctrl-Z reverts. Holds its records in application
/// order plus the cursor either side.
struct EditGroup {
  std::vector<EditRecord> edits;
  CursorState before;
  CursorState after;
  int64_t timestampMs = 0;
  /// True when this group was created by single-codepoint typing/deleting and
  /// may therefore absorb further adjacent single-codepoint edits. A paste or a
  /// programmatic edit is never coalescable, so typing after a paste starts a
  /// new group.
  bool coalescable = false;
};

/// Why a tree and not a stack: with a stack, "undo, then type" throws away the
/// redo branch forever. A tree keeps it as a sibling, so a later phase can
/// offer real branch navigation ("undo to before I went down that path") and
/// session history without changing the edit representation.
///
/// Node 0 is the pristine state (no group). Every other node stores the group
/// that transforms its parent's state into its own, so:
///   undo  = invert nodes_[current].group, current = parent
///   redo  = apply   nodes_[child].group,  current = child
/// Branches are kept in creation order; `redo()` in Document follows the newest
/// one, which reproduces familiar flat-stack behaviour (a fresh edit makes redo
/// unavailable because the new node is a childless leaf) while keeping the old
/// branches reachable through the indexed overloads.
///
/// Memory grows with the number of edits; nothing is pruned in this phase.
/// Pruning belongs with session persistence, which needs to decide what to
/// keep on disk anyway.
class UndoTree {
public:
  UndoTree();

  /// Back to a single pristine node.
  void clear();

  bool canUndo() const noexcept;
  bool canRedo() const noexcept;

  /// Adds `group` as a new child of the current node and moves there.
  void push(EditGroup group);

  /// Tries to fold `rec` into the current node's group instead of pushing a new
  /// one. Succeeds only when all of these hold:
  ///   - the current node is a leaf (never rewrite history a redo branch shares)
  ///   - its group is `coalescable` and holds exactly one record
  ///   - `before` equals that group's `after` cursor (the caret has not moved)
  ///   - `timestampMs` is within `windowMs` of the group's timestamp
  ///   - the records are adjacent in the right direction (forward typing,
  ///     backspace run, or forward-delete run)
  /// On success the group's `after` cursor and timestamp slide forward, so a
  /// continuous typing run keeps extending one group.
  bool tryCoalesce(const EditRecord& rec, const CursorState& before,
                   const CursorState& after, int64_t timestampMs, int64_t windowMs);

  /// Group that must be inverted to undo, or nullptr at the pristine node.
  const EditGroup* undoGroup() const noexcept;

  /// Moves to the parent. No-op at the pristine node.
  void moveToParent() noexcept;

  /// Number of redo branches leaving the current node.
  size_t redoBranchCount() const noexcept;

  /// Index of the most recently created branch, or 0 when there is none.
  size_t newestRedoBranch() const noexcept;

  /// Group to re-apply for branch `branchIndex` (creation order), or nullptr.
  const EditGroup* redoGroup(size_t branchIndex) const noexcept;

  /// Moves onto branch `branchIndex`. No-op when out of range.
  void moveToRedo(size_t branchIndex) noexcept;

  /// Diagnostics / tests.
  size_t nodeCount() const noexcept { return nodes_.size(); }
  size_t currentNode() const noexcept { return current_; }
  size_t depth() const noexcept;

private:
  struct Node {
    uint32_t parent = 0;
    std::vector<uint32_t> children;
    EditGroup group;
  };

  std::vector<Node> nodes_;
  uint32_t current_ = 0;
};

}  // namespace ide::text
