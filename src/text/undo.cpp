#include <ide/text/undo.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ide::text {

UndoTree::UndoTree() { clear(); }

void UndoTree::clear() {
  nodes_.clear();
  nodes_.emplace_back();  // node 0: the pristine state, its group unused
  current_ = 0;
}

bool UndoTree::canUndo() const noexcept { return current_ != 0; }

bool UndoTree::canRedo() const noexcept { return !nodes_[current_].children.empty(); }

void UndoTree::push(EditGroup group) {
  const uint32_t id = static_cast<uint32_t>(nodes_.size());
  nodes_.emplace_back();  // may reallocate, so index through nodes_ afterwards
  nodes_[id].parent = current_;
  nodes_[id].group = std::move(group);
  nodes_[current_].children.push_back(id);
  current_ = id;
}

bool UndoTree::tryCoalesce(const EditRecord& rec, const CursorState& before,
                           const CursorState& after, int64_t timestampMs, int64_t windowMs) {
  if (current_ == 0) {
    return false;
  }
  Node& node = nodes_[current_];
  if (!node.children.empty()) {
    return false;  // a redo branch shares this state; never rewrite it
  }
  EditGroup& group = node.group;
  if (!group.coalescable || group.edits.size() != 1) {
    return false;
  }
  if (timestampMs < group.timestampMs || timestampMs - group.timestampMs > windowMs) {
    return false;
  }
  if (!(group.after == before)) {
    return false;  // the caret moved between the two edits
  }

  EditRecord& prev = group.edits.front();
  const bool prevInsert = prev.removed.empty() && !prev.inserted.empty();
  const bool prevErase = prev.inserted.empty() && !prev.removed.empty();
  const bool recInsert = rec.removed.empty() && !rec.inserted.empty();
  const bool recErase = rec.inserted.empty() && !rec.removed.empty();

  if (prevInsert && recInsert) {
    if (rec.offset != prev.offset + prev.inserted.size()) {
      return false;  // not typed straight after the previous character
    }
    prev.inserted += rec.inserted;
  } else if (prevErase && recErase) {
    if (rec.offset + rec.removed.size() == prev.offset) {
      // Backspace run: the new deletion sits immediately before the old one.
      prev.offset = rec.offset;
      prev.removed.insert(0, rec.removed);
    } else if (rec.offset == prev.offset) {
      // Forward-delete run: the new deletion sits immediately after.
      prev.removed += rec.removed;
    } else {
      return false;
    }
  } else {
    return false;
  }

  group.after = after;
  group.timestampMs = timestampMs;  // sliding window: a typing run keeps growing
  return true;
}

const EditGroup* UndoTree::undoGroup() const noexcept {
  if (current_ == 0) {
    return nullptr;
  }
  return &nodes_[current_].group;
}

void UndoTree::moveToParent() noexcept {
  if (current_ != 0) {
    current_ = nodes_[current_].parent;
  }
}

size_t UndoTree::redoBranchCount() const noexcept { return nodes_[current_].children.size(); }

size_t UndoTree::newestRedoBranch() const noexcept {
  const size_t count = nodes_[current_].children.size();
  return count == 0 ? 0 : count - 1;
}

const EditGroup* UndoTree::redoGroup(size_t branchIndex) const noexcept {
  const std::vector<uint32_t>& children = nodes_[current_].children;
  if (branchIndex >= children.size()) {
    return nullptr;
  }
  return &nodes_[children[branchIndex]].group;
}

void UndoTree::moveToRedo(size_t branchIndex) noexcept {
  const std::vector<uint32_t>& children = nodes_[current_].children;
  if (branchIndex >= children.size()) {
    return;
  }
  current_ = children[branchIndex];
}

size_t UndoTree::depth() const noexcept {
  size_t levels = 0;
  uint32_t walk = current_;
  while (walk != 0) {
    walk = nodes_[walk].parent;
    ++levels;
  }
  return levels;
}

}  // namespace ide::text
