#include <gtest/gtest.h>

#include <ide/text/undo.h>

#include <cstdint>
#include <string>
#include <utility>

namespace {

using ide::text::CursorState;
using ide::text::EditGroup;
using ide::text::EditRecord;
using ide::text::UndoTree;

EditRecord insertRecord(size_t offset, std::string text) {
  EditRecord record;
  record.offset = offset;
  record.inserted = std::move(text);
  return record;
}

EditRecord eraseRecord(size_t offset, std::string text) {
  EditRecord record;
  record.offset = offset;
  record.removed = std::move(text);
  return record;
}

EditGroup makeGroup(EditRecord record, CursorState before, CursorState after, int64_t timestampMs,
                    bool coalescable) {
  EditGroup group;
  group.edits.push_back(std::move(record));
  group.before = before;
  group.after = after;
  group.timestampMs = timestampMs;
  group.coalescable = coalescable;
  return group;
}

TEST(UndoTreeTest, FreshTreeHasNothingToUndoOrRedo) {
  UndoTree tree;
  EXPECT_FALSE(tree.canUndo());
  EXPECT_FALSE(tree.canRedo());
  EXPECT_EQ(tree.undoGroup(), nullptr);
  EXPECT_EQ(tree.redoBranchCount(), 0u);
  EXPECT_EQ(tree.redoGroup(0), nullptr);
  EXPECT_EQ(tree.nodeCount(), 1u);
  EXPECT_EQ(tree.currentNode(), 0u);
  EXPECT_EQ(tree.depth(), 0u);
  // No-ops rather than crashes.
  tree.moveToParent();
  tree.moveToRedo(0);
  EXPECT_EQ(tree.currentNode(), 0u);
}

TEST(UndoTreeTest, PushUndoRedoRoundTrip) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "A"), CursorState{0, 0}, CursorState{1, 1}, 10, false));
  EXPECT_TRUE(tree.canUndo());
  EXPECT_FALSE(tree.canRedo());
  EXPECT_EQ(tree.depth(), 1u);

  const EditGroup* group = tree.undoGroup();
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->edits.at(0).inserted, "A");

  tree.moveToParent();
  EXPECT_FALSE(tree.canUndo());
  EXPECT_TRUE(tree.canRedo());
  EXPECT_EQ(tree.redoBranchCount(), 1u);
  ASSERT_NE(tree.redoGroup(0), nullptr);
  EXPECT_EQ(tree.redoGroup(0)->edits.at(0).inserted, "A");

  tree.moveToRedo(0);
  EXPECT_TRUE(tree.canUndo());
  EXPECT_FALSE(tree.canRedo());
}

TEST(UndoTreeTest, DeepLinearHistory) {
  UndoTree tree;
  for (int i = 0; i < 500; ++i) {
    tree.push(makeGroup(insertRecord(static_cast<size_t>(i), "x"),
                        CursorState{static_cast<size_t>(i), static_cast<size_t>(i)},
                        CursorState{static_cast<size_t>(i + 1), static_cast<size_t>(i + 1)}, i,
                        false));
  }
  EXPECT_EQ(tree.depth(), 500u);
  EXPECT_EQ(tree.nodeCount(), 501u);
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(tree.canUndo()) << "i=" << i;
    tree.moveToParent();
  }
  EXPECT_FALSE(tree.canUndo());
  EXPECT_EQ(tree.depth(), 0u);
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(tree.canRedo()) << "i=" << i;
    tree.moveToRedo(tree.newestRedoBranch());
  }
  EXPECT_FALSE(tree.canRedo());
  EXPECT_EQ(tree.depth(), 500u);
}

TEST(UndoTreeTest, DivergingKeepsBothBranches) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "A"), CursorState{}, CursorState{1, 1}, 1, false));
  tree.push(makeGroup(insertRecord(1, "B"), CursorState{1, 1}, CursorState{2, 2}, 2, false));
  EXPECT_EQ(tree.depth(), 2u);

  tree.moveToParent();  // back at A
  tree.push(makeGroup(insertRecord(1, "C"), CursorState{1, 1}, CursorState{2, 2}, 3, false));
  EXPECT_EQ(tree.nodeCount(), 4u);
  EXPECT_FALSE(tree.canRedo());  // the new node is a childless leaf

  tree.moveToParent();  // back at A again
  ASSERT_EQ(tree.redoBranchCount(), 2u);
  EXPECT_EQ(tree.newestRedoBranch(), 1u);
  ASSERT_NE(tree.redoGroup(0), nullptr);
  ASSERT_NE(tree.redoGroup(1), nullptr);
  EXPECT_EQ(tree.redoGroup(0)->edits.at(0).inserted, "B");  // the abandoned branch
  EXPECT_EQ(tree.redoGroup(1)->edits.at(0).inserted, "C");  // the newest branch
  EXPECT_EQ(tree.redoGroup(2), nullptr);

  tree.moveToRedo(0);
  ASSERT_NE(tree.undoGroup(), nullptr);
  EXPECT_EQ(tree.undoGroup()->edits.at(0).inserted, "B");
}

TEST(UndoTreeTest, ClearResetsToPristine) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "A"), CursorState{}, CursorState{1, 1}, 1, false));
  tree.push(makeGroup(insertRecord(1, "B"), CursorState{1, 1}, CursorState{2, 2}, 2, false));
  tree.clear();
  EXPECT_EQ(tree.nodeCount(), 1u);
  EXPECT_EQ(tree.currentNode(), 0u);
  EXPECT_FALSE(tree.canUndo());
  EXPECT_FALSE(tree.canRedo());
}

// --- coalescing -----------------------------------------------------------

TEST(UndoTreeTest, CoalescesAdjacentTyping) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true));
  ASSERT_TRUE(tree.tryCoalesce(insertRecord(1, "b"), CursorState{1, 1}, CursorState{2, 2}, 1100,
                               500));
  ASSERT_TRUE(tree.tryCoalesce(insertRecord(2, "c"), CursorState{2, 2}, CursorState{3, 3}, 1200,
                               500));
  EXPECT_EQ(tree.nodeCount(), 2u);  // still one group
  const EditGroup* group = tree.undoGroup();
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(group->edits.size(), 1u);
  EXPECT_EQ(group->edits.at(0).offset, 0u);
  EXPECT_EQ(group->edits.at(0).inserted, "abc");
  EXPECT_TRUE(group->after == (CursorState{3, 3}));
  EXPECT_EQ(group->timestampMs, 1200);  // window slides with each keystroke
}

TEST(UndoTreeTest, RefusesCoalesceWhenTooSlow) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true));
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(1, "b"), CursorState{1, 1}, CursorState{2, 2}, 1600,
                                500));
}

TEST(UndoTreeTest, RefusesCoalesceWhenClockGoesBackwards) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true));
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(1, "b"), CursorState{1, 1}, CursorState{2, 2}, 900,
                                500));
}

TEST(UndoTreeTest, RefusesCoalesceWhenNotAdjacent) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true));
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(9, "b"), CursorState{1, 1}, CursorState{10, 10}, 1010,
                                500));
}

TEST(UndoTreeTest, RefusesCoalesceWhenCursorMoved) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true));
  // The caret was somewhere else immediately before this edit.
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(1, "b"), CursorState{7, 7}, CursorState{2, 2}, 1010,
                                500));
}

TEST(UndoTreeTest, RefusesCoalesceIntoNonCoalescableGroup) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "pasted"), CursorState{0, 0}, CursorState{6, 6}, 1000,
                      false));
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(6, "x"), CursorState{6, 6}, CursorState{7, 7}, 1010,
                                500));
}

TEST(UndoTreeTest, RefusesCoalesceWhenNotAtALeaf) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true));
  tree.push(makeGroup(insertRecord(1, "b"), CursorState{1, 1}, CursorState{2, 2}, 1010, true));
  tree.moveToParent();  // node "a" now has a child: never rewrite it
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(1, "z"), CursorState{1, 1}, CursorState{2, 2}, 1020,
                                500));
}

TEST(UndoTreeTest, RefusesCoalesceAtPristineNode) {
  UndoTree tree;
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(0, "a"), CursorState{}, CursorState{1, 1}, 1, 500));
}

TEST(UndoTreeTest, CoalescesBackspaceRun) {
  UndoTree tree;
  // Deleted the byte at offset 5, caret ended at 5.
  tree.push(makeGroup(eraseRecord(5, "z"), CursorState{6, 6}, CursorState{5, 5}, 1000, true));
  ASSERT_TRUE(
      tree.tryCoalesce(eraseRecord(4, "y"), CursorState{5, 5}, CursorState{4, 4}, 1050, 500));
  ASSERT_TRUE(
      tree.tryCoalesce(eraseRecord(3, "x"), CursorState{4, 4}, CursorState{3, 3}, 1100, 500));
  const EditGroup* group = tree.undoGroup();
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(group->edits.size(), 1u);
  EXPECT_EQ(group->edits.at(0).offset, 3u);
  EXPECT_EQ(group->edits.at(0).removed, "xyz");
  EXPECT_TRUE(group->after == (CursorState{3, 3}));
}

TEST(UndoTreeTest, CoalescesForwardDeleteRun) {
  UndoTree tree;
  tree.push(makeGroup(eraseRecord(5, "a"), CursorState{5, 5}, CursorState{5, 5}, 1000, true));
  ASSERT_TRUE(
      tree.tryCoalesce(eraseRecord(5, "b"), CursorState{5, 5}, CursorState{5, 5}, 1050, 500));
  const EditGroup* group = tree.undoGroup();
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->edits.at(0).offset, 5u);
  EXPECT_EQ(group->edits.at(0).removed, "ab");
}

TEST(UndoTreeTest, RefusesCoalesceAcrossEditKinds) {
  UndoTree tree;
  tree.push(makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true));
  EXPECT_FALSE(
      tree.tryCoalesce(eraseRecord(0, "a"), CursorState{1, 1}, CursorState{0, 0}, 1010, 500));

  UndoTree other;
  other.push(makeGroup(eraseRecord(3, "q"), CursorState{4, 4}, CursorState{3, 3}, 1000, true));
  EXPECT_FALSE(
      other.tryCoalesce(insertRecord(3, "q"), CursorState{3, 3}, CursorState{4, 4}, 1010, 500));
}

TEST(UndoTreeTest, RefusesCoalesceIntoMultiRecordGroup) {
  UndoTree tree;
  EditGroup group =
      makeGroup(insertRecord(0, "a"), CursorState{0, 0}, CursorState{1, 1}, 1000, true);
  group.edits.push_back(insertRecord(1, "b"));
  tree.push(std::move(group));
  EXPECT_FALSE(tree.tryCoalesce(insertRecord(2, "c"), CursorState{1, 1}, CursorState{3, 3}, 1010,
                                500));
}

}  // namespace
