#include <gtest/gtest.h>

#include <ide/text/byte_source.h>
#include <ide/text/piece_table.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ide::text::OwnedByteSource;
using ide::text::Piece;
using ide::text::PieceTable;
using ide::text::Source;

std::string dump(const PieceTable& table) {
  std::string out(table.size(), '\0');
  if (!out.empty()) {
    const size_t copied = table.copyOut(0, std::span<char>(out.data(), out.size()));
    EXPECT_EQ(copied, out.size());
  }
  return out;
}

PieceTable tableFrom(std::string text) {
  return PieceTable(OwnedByteSource::make(std::move(text)));
}

// --- empty ----------------------------------------------------------------

TEST(PieceTableTest, EmptyDocument) {
  PieceTable table;
  EXPECT_EQ(table.size(), 0u);
  EXPECT_EQ(table.newlineCount(), 0);
  EXPECT_EQ(table.pieceCount(), 0u);
  EXPECT_EQ(table.addBufferSize(), 0u);
  EXPECT_EQ(table.treeHeight(), 0);
  EXPECT_EQ(dump(table), "");
  EXPECT_EQ(table.byteAt(0), '\0');
  EXPECT_EQ(table.lineStartOffset(0), 0u);
  EXPECT_EQ(table.lineStartOffset(5), 0u);
  EXPECT_EQ(table.newlinesBefore(0), 0);
  EXPECT_EQ(table.newlinesBefore(100), 0);
  EXPECT_EQ(table.offsetOfNewline(0), 0u);

  const std::optional<std::string_view> zero = table.contiguous(0, 0);
  ASSERT_TRUE(zero.has_value());
  EXPECT_TRUE(zero->empty());
  EXPECT_FALSE(table.contiguous(0, 1).has_value());
}

TEST(PieceTableTest, NullOriginalIsTreatedAsEmpty) {
  PieceTable table(std::shared_ptr<const ide::text::ByteSource>{});
  EXPECT_EQ(table.size(), 0u);
  table.insert(0, "x");
  EXPECT_EQ(dump(table), "x");
}

TEST(PieceTableTest, EmptyEditsAreNoOps) {
  PieceTable table = tableFrom("abc");
  table.insert(1, "");
  table.erase(1, 0);
  table.erase(99, 5);
  EXPECT_EQ(dump(table), "abc");
  EXPECT_EQ(table.pieceCount(), 1u);
}

// --- original buffer ------------------------------------------------------

TEST(PieceTableTest, SingleOriginalPieceIsZeroCopy) {
  PieceTable table = tableFrom("hello world");
  EXPECT_EQ(table.size(), 11u);
  EXPECT_EQ(table.pieceCount(), 1u);
  EXPECT_EQ(table.addBufferSize(), 0u);
  EXPECT_EQ(dump(table), "hello world");

  const std::vector<Piece> pieces = table.pieceList();
  ASSERT_EQ(pieces.size(), 1u);
  EXPECT_EQ(pieces[0].source, Source::Original);
  EXPECT_EQ(pieces[0].offset, 0u);
  EXPECT_EQ(pieces[0].length, 11u);

  const std::optional<std::string_view> whole = table.contiguous(0, 11);
  ASSERT_TRUE(whole.has_value());
  EXPECT_EQ(*whole, "hello world");
}

TEST(PieceTableTest, SingleCharacterDocument) {
  PieceTable table = tableFrom("x");
  EXPECT_EQ(table.size(), 1u);
  EXPECT_EQ(table.byteAt(0), 'x');
  EXPECT_EQ(table.byteAt(1), '\0');
  table.erase(0, 1);
  EXPECT_EQ(table.size(), 0u);
  EXPECT_EQ(dump(table), "");
  EXPECT_EQ(table.pieceCount(), 0u);
}

// --- inserts --------------------------------------------------------------

TEST(PieceTableTest, InsertAtStartMiddleEnd) {
  PieceTable table = tableFrom("BCD");
  table.insert(0, "A");
  EXPECT_EQ(dump(table), "ABCD");
  table.insert(4, "E");
  EXPECT_EQ(dump(table), "ABCDE");
  table.insert(2, "-");
  EXPECT_EQ(dump(table), "AB-CDE");
  table.insert(100, "!");  // clamped to the end
  EXPECT_EQ(dump(table), "AB-CDE!");
  EXPECT_EQ(table.size(), 7u);
}

TEST(PieceTableTest, InsertIntoEmptyThenEverywhere) {
  PieceTable table;
  table.insert(0, "world");
  table.insert(0, "hello ");
  EXPECT_EQ(dump(table), "hello world");
  table.insert(5, ",");
  EXPECT_EQ(dump(table), "hello, world");
  table.insert(table.size(), "!");
  EXPECT_EQ(dump(table), "hello, world!");
}

TEST(PieceTableTest, SequentialTypingCoalescesIntoOnePiece) {
  // The typing fast path must not allocate one piece per keystroke.
  PieceTable table;
  const std::string typed = "abcdefghijklmnopqrstuvwxyz";
  for (size_t i = 0; i < typed.size(); ++i) {
    table.insert(i, std::string_view(typed).substr(i, 1));
  }
  EXPECT_EQ(dump(table), typed);
  EXPECT_EQ(table.pieceCount(), 1u);
  EXPECT_EQ(table.addBufferSize(), typed.size());
}

TEST(PieceTableTest, TypingAtEndOfOriginalStillOnePiecePerRun) {
  PieceTable table = tableFrom("int main");
  for (int i = 0; i < 5; ++i) {
    table.insert(table.size(), "()");
  }
  EXPECT_EQ(dump(table), "int main()()()()()");
  // One original piece plus one coalesced add piece.
  EXPECT_EQ(table.pieceCount(), 2u);
}

TEST(PieceTableTest, PrependingCreatesOnePiecePerInsert) {
  PieceTable table;
  for (int i = 0; i < 5; ++i) {
    table.insert(0, "a");
  }
  EXPECT_EQ(dump(table), "aaaaa");
  EXPECT_EQ(table.pieceCount(), 5u);
}

TEST(PieceTableTest, InsertingTextThatAliasesTheAddBuffer) {
  PieceTable table;
  table.insert(0, "hello");
  const std::optional<std::string_view> view = table.contiguous(0, 5);
  ASSERT_TRUE(view.has_value());
  // *view points into the add buffer; appending must not read freed memory.
  table.insert(2, *view);
  EXPECT_EQ(dump(table), "hehellollo");
}

TEST(PieceTableTest, InsertingTextThatAliasesTheOriginal) {
  PieceTable table = tableFrom("abcdef");
  const std::optional<std::string_view> view = table.contiguous(1, 3);
  ASSERT_TRUE(view.has_value());
  EXPECT_EQ(*view, "bcd");
  table.insert(0, *view);
  EXPECT_EQ(dump(table), "bcdabcdef");
}

// --- erases ---------------------------------------------------------------

TEST(PieceTableTest, EraseInsidePieceSplitsIt) {
  PieceTable table = tableFrom("0123456789");
  table.erase(3, 4);
  EXPECT_EQ(dump(table), "012789");
  EXPECT_EQ(table.pieceCount(), 2u);
}

TEST(PieceTableTest, EraseAtStartAndEnd) {
  PieceTable table = tableFrom("0123456789");
  table.erase(0, 3);
  EXPECT_EQ(dump(table), "3456789");
  table.erase(4, 3);
  EXPECT_EQ(dump(table), "3456");
  table.erase(0, 100);  // clamped
  EXPECT_EQ(dump(table), "");
  EXPECT_EQ(table.pieceCount(), 0u);
}

TEST(PieceTableTest, EraseAcrossPieceBoundary) {
  PieceTable table = tableFrom("AAAA");
  table.insert(2, "BBBB");  // AA|BBBB|AA -> three pieces
  ASSERT_EQ(dump(table), "AABBBBAA");
  ASSERT_EQ(table.pieceCount(), 3u);
  table.erase(1, 6);  // spans all three pieces partially
  EXPECT_EQ(dump(table), "AA");
  EXPECT_EQ(table.size(), 2u);
}

TEST(PieceTableTest, EraseSpanningManyPieces) {
  PieceTable table;
  // Build 40 separate pieces by always inserting at the front.
  std::string expected;
  for (int i = 0; i < 40; ++i) {
    const std::string chunk(3, static_cast<char>('a' + i % 26));
    table.insert(0, chunk);
    expected = chunk + expected;
  }
  ASSERT_EQ(dump(table), expected);
  ASSERT_EQ(table.pieceCount(), 40u);

  // Remove everything except the first and last byte.
  table.erase(1, table.size() - 2);
  std::string trimmed;
  trimmed += expected.front();
  trimmed += expected.back();
  EXPECT_EQ(dump(table), trimmed);
  EXPECT_EQ(table.pieceCount(), 2u);
}

TEST(PieceTableTest, EraseThenReinsertAtSamePlace) {
  PieceTable table = tableFrom("hello world");
  table.erase(5, 6);
  EXPECT_EQ(dump(table), "hello");
  table.insert(5, " there");
  EXPECT_EQ(dump(table), "hello there");
}

// --- reading --------------------------------------------------------------

TEST(PieceTableTest, CopyOutClampsAndReportsCount) {
  PieceTable table = tableFrom("abcdef");
  char buffer[16] = {};
  EXPECT_EQ(table.copyOut(0, std::span<char>(buffer, 3)), 3u);
  EXPECT_EQ(std::string_view(buffer, 3), "abc");
  EXPECT_EQ(table.copyOut(4, std::span<char>(buffer, 16)), 2u);
  EXPECT_EQ(std::string_view(buffer, 2), "ef");
  EXPECT_EQ(table.copyOut(6, std::span<char>(buffer, 16)), 0u);
  EXPECT_EQ(table.copyOut(99, std::span<char>(buffer, 16)), 0u);
  EXPECT_EQ(table.copyOut(0, std::span<char>(buffer, 0)), 0u);
}

TEST(PieceTableTest, CopyOutAcrossManyPieces) {
  PieceTable table = tableFrom("XXXX");
  table.insert(2, "y");
  table.insert(1, "z");
  ASSERT_EQ(dump(table), "XzXyXX");
  for (size_t from = 0; from <= 6; ++from) {
    for (size_t length = 0; from + length <= 6; ++length) {
      std::string out(length, '\0');
      const size_t copied =
          length == 0 ? 0u : table.copyOut(from, std::span<char>(out.data(), out.size()));
      EXPECT_EQ(copied, length) << "from=" << from << " len=" << length;
      EXPECT_EQ(out, std::string("XzXyXX").substr(from, length))
          << "from=" << from << " len=" << length;
    }
  }
}

TEST(PieceTableTest, ContiguousOnlyInsideOnePiece) {
  PieceTable table = tableFrom("AAAA");
  table.insert(2, "BB");
  ASSERT_EQ(dump(table), "AABBAA");
  EXPECT_TRUE(table.contiguous(0, 2).has_value());
  EXPECT_TRUE(table.contiguous(2, 2).has_value());
  EXPECT_TRUE(table.contiguous(4, 2).has_value());
  EXPECT_FALSE(table.contiguous(1, 2).has_value());  // crosses into the add piece
  EXPECT_FALSE(table.contiguous(0, 6).has_value());
  EXPECT_FALSE(table.contiguous(5, 2).has_value());  // past the end
  EXPECT_TRUE(table.contiguous(6, 0).has_value());   // empty range at the end
}

TEST(PieceTableTest, ByteAtEveryOffset) {
  PieceTable table = tableFrom("abc");
  table.insert(1, "12");
  const std::string expected = "a12bc";
  ASSERT_EQ(dump(table), expected);
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(table.byteAt(i), expected[i]) << "i=" << i;
  }
  EXPECT_EQ(table.byteAt(expected.size()), '\0');
}

// --- newline queries ------------------------------------------------------

TEST(PieceTableTest, NewlineQueriesOnOriginal) {
  PieceTable table = tableFrom("a\nbb\n\nccc");
  EXPECT_EQ(table.newlineCount(), 3);
  EXPECT_EQ(table.newlinesBefore(0), 0);
  EXPECT_EQ(table.newlinesBefore(1), 0);
  EXPECT_EQ(table.newlinesBefore(2), 1);
  // "a\nbb\n\nccc" -> newlines live at bytes 1, 4 and 5, so the counts below
  // include the blank line's second newline.
  EXPECT_EQ(table.newlinesBefore(5), 2);
  EXPECT_EQ(table.newlinesBefore(6), 3);
  EXPECT_EQ(table.newlinesBefore(table.size()), 3);

  EXPECT_EQ(table.offsetOfNewline(0), 1u);
  EXPECT_EQ(table.offsetOfNewline(1), 4u);
  EXPECT_EQ(table.offsetOfNewline(2), 5u);
  EXPECT_EQ(table.offsetOfNewline(3), table.size());  // out of range
  EXPECT_EQ(table.offsetOfNewline(-1), table.size());

  EXPECT_EQ(table.lineStartOffset(0), 0u);
  EXPECT_EQ(table.lineStartOffset(1), 2u);
  EXPECT_EQ(table.lineStartOffset(2), 5u);
  EXPECT_EQ(table.lineStartOffset(3), 6u);
  EXPECT_EQ(table.lineStartOffset(4), table.size());  // past the last line
  EXPECT_EQ(table.lineStartOffset(-3), 0u);
}

TEST(PieceTableTest, NewlineQueriesSurviveEdits) {
  PieceTable table = tableFrom("one\ntwo\nthree");
  ASSERT_EQ(table.newlineCount(), 2);

  // Split a line in the middle.
  table.insert(5, "\n");
  ASSERT_EQ(dump(table), "one\nt\nwo\nthree");
  EXPECT_EQ(table.newlineCount(), 3);
  EXPECT_EQ(table.lineStartOffset(1), 4u);
  EXPECT_EQ(table.lineStartOffset(2), 6u);
  EXPECT_EQ(table.lineStartOffset(3), 9u);

  // Join two lines by erasing a newline.
  table.erase(5, 1);
  ASSERT_EQ(dump(table), "one\ntwo\nthree");
  EXPECT_EQ(table.newlineCount(), 2);
  EXPECT_EQ(table.lineStartOffset(1), 4u);
  EXPECT_EQ(table.lineStartOffset(2), 8u);

  // Erase a whole line including its terminator.
  table.erase(0, 4);
  ASSERT_EQ(dump(table), "two\nthree");
  EXPECT_EQ(table.newlineCount(), 1);
  EXPECT_EQ(table.lineStartOffset(1), 4u);
}

TEST(PieceTableTest, NewlinesInsideAddBufferPieces) {
  PieceTable table;
  table.insert(0, "a\nb");
  table.insert(1, "\n\n");
  ASSERT_EQ(dump(table), "a\n\n\nb");
  EXPECT_EQ(table.newlineCount(), 3);
  EXPECT_EQ(table.offsetOfNewline(0), 1u);
  EXPECT_EQ(table.offsetOfNewline(1), 2u);
  EXPECT_EQ(table.offsetOfNewline(2), 3u);
  EXPECT_EQ(table.lineStartOffset(3), 4u);
}

TEST(PieceTableTest, NewlineCountAcrossBlockBoundaries) {
  // Force the block-prefix path in SourceLineTable: many blocks, then split a
  // piece in the middle so both halves must be re-counted from the table.
  std::string text;
  for (int i = 0; i < 5000; ++i) {
    text += "0123456789\n";  // 11 bytes, 1 newline
  }
  PieceTable table = tableFrom(text);
  ASSERT_EQ(table.size(), 55000u);
  ASSERT_EQ(table.newlineCount(), 5000);

  table.insert(27500, "X");
  EXPECT_EQ(table.newlineCount(), 5000);
  EXPECT_EQ(table.size(), 55001u);
  EXPECT_EQ(table.newlinesBefore(27500), 2500);
  EXPECT_EQ(table.offsetOfNewline(0), 10u);
  EXPECT_EQ(table.offsetOfNewline(2499), 27499u);
  EXPECT_EQ(table.offsetOfNewline(2500), 27511u);  // shifted by the inserted X
  EXPECT_EQ(table.offsetOfNewline(4999), 55000u);
  EXPECT_EQ(table.lineStartOffset(5000), 55001u);
}

// --- scale / balance ------------------------------------------------------

TEST(PieceTableTest, ManyRandomEditsStayBalancedAndCorrect) {
  std::mt19937_64 rng(0xBEEF);
  PieceTable table;
  std::string reference;
  for (int i = 0; i < 20000; ++i) {
    const size_t offset = static_cast<size_t>(rng() % (reference.size() + 1));
    const char c = static_cast<char>('a' + static_cast<int>(rng() % 26));
    table.insert(offset, std::string_view(&c, 1));
    reference.insert(offset, 1, c);
  }
  ASSERT_EQ(table.size(), reference.size());
  ASSERT_EQ(dump(table), reference);

  // A treap over P pieces has expected height ~1.39*log2(P) (about 25 here);
  // this bound is deliberately loose but still catches a degenerate list.
  EXPECT_LT(table.treeHeight(), 100) << "pieces=" << table.pieceCount();

  for (int i = 0; i < 2000; ++i) {
    if (table.size() == 0) {
      break;
    }
    const size_t offset = static_cast<size_t>(rng() % table.size());
    const size_t length = 1 + static_cast<size_t>(rng() % 7);
    table.erase(offset, length);
    reference.erase(offset, length);
  }
  EXPECT_EQ(dump(table), reference);
  EXPECT_LT(table.treeHeight(), 100) << "pieces=" << table.pieceCount();
}

TEST(PieceTableTest, FreedNodesAreRecycled) {
  PieceTable table;
  for (int i = 0; i < 100; ++i) {
    table.insert(0, "abc");
  }
  ASSERT_EQ(table.pieceCount(), 100u);
  table.erase(0, table.size());
  ASSERT_EQ(table.pieceCount(), 0u);
  // Re-inserting must reuse the free list rather than growing the arena
  // forever; the observable contract is simply that it still works.
  std::string expected;
  for (int i = 0; i < 100; ++i) {
    table.insert(0, "xy");
    expected += "xy";
  }
  EXPECT_EQ(table.pieceCount(), 100u);
  EXPECT_EQ(table.size(), 200u);
  EXPECT_EQ(dump(table), expected);
}

}  // namespace
