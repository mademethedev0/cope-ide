#include <gtest/gtest.h>

#include <ide/text/byte_source.h>
#include <ide/text/document.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ide::text::CursorState;
using ide::text::Document;
using ide::text::EditOptions;
using ide::text::LineRange;
using ide::text::OwnedByteSource;
using ide::text::Position;

// Byte sequences as escapes so the test never depends on this file's encoding.
constexpr const char* kEuro = "\xe2\x82\xac";          // U+20AC, 3 bytes, width 1
constexpr const char* kCjkDay = "\xe6\x97\xa5";        // U+65E5, 3 bytes, width 2
constexpr const char* kClef = "\xf0\x9d\x84\x9e";      // U+1D11E, 4 bytes, width 1
constexpr const char* kEAcute = "\xc3\xa9";            // U+00E9, 2 bytes, width 1
constexpr const char* kCombiningAcute = "\xcc\x81";    // U+0301, 2 bytes, width 0

EditOptions noCoalesce(int64_t timestampMs = 1000) {
  EditOptions options;
  options.timestampMs = timestampMs;
  options.coalesce = false;
  return options;
}

EditOptions typedAt(int64_t timestampMs) {
  EditOptions options;
  options.timestampMs = timestampMs;
  options.coalesce = true;
  return options;
}

// --- basics ---------------------------------------------------------------

TEST(DocumentTest, EmptyDocument) {
  const Document document;
  EXPECT_EQ(document.size(), 0u);
  EXPECT_EQ(document.version(), 0);
  EXPECT_EQ(document.lineCount(), 1);
  EXPECT_EQ(document.text(), "");
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 0, 0}));
  EXPECT_EQ(document.lineAt(5), (LineRange{0, 0, 0}));   // clamped
  EXPECT_EQ(document.lineAt(-5), (LineRange{0, 0, 0}));  // clamped
  EXPECT_EQ(document.lineColumnOf(0), (Position{0, 0}));
  EXPECT_EQ(document.lineColumnOf(77), (Position{0, 0}));
  EXPECT_EQ(document.offsetOf(0, 0), 0u);
  EXPECT_EQ(document.offsetOf(9, 9), 0u);
  EXPECT_EQ(document.nextCodepoint(0), 0u);
  EXPECT_EQ(document.prevCodepoint(0), 0u);
  EXPECT_FALSE(document.canUndo());
  EXPECT_FALSE(document.canRedo());
}

TEST(DocumentTest, SingleCharacter) {
  Document document("a");
  EXPECT_EQ(document.size(), 1u);
  EXPECT_EQ(document.lineCount(), 1);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 1, 0}));
  EXPECT_EQ(document.byteAt(0), 'a');
  EXPECT_EQ(document.textRange(0, 1), "a");
  document.erase(0, 1);
  EXPECT_EQ(document.text(), "");
  EXPECT_EQ(document.lineCount(), 1);
}

TEST(DocumentTest, ConstructionFromByteSourceIsZeroCopy) {
  const std::shared_ptr<const ide::text::ByteSource> source =
      OwnedByteSource::make(std::string("shared bytes"));
  const Document document(source);
  EXPECT_EQ(document.size(), 12u);
  EXPECT_EQ(document.text(), "shared bytes");
  EXPECT_EQ(document.pieces().pieceCount(), 1u);
  EXPECT_EQ(document.pieces().addBufferSize(), 0u);
  EXPECT_EQ(source.use_count(), 2);  // the Document holds on to it
}

TEST(DocumentTest, VersionBumpsOnlyOnRealMutations) {
  Document document("ab");
  EXPECT_EQ(document.version(), 0);
  document.insert(0, "", noCoalesce());
  EXPECT_EQ(document.version(), 0);
  document.erase(0, 0, noCoalesce());
  EXPECT_EQ(document.version(), 0);
  document.erase(99, 5, noCoalesce());
  EXPECT_EQ(document.version(), 0);
  EXPECT_FALSE(document.undo());
  EXPECT_EQ(document.version(), 0);
  EXPECT_FALSE(document.redo());
  EXPECT_EQ(document.version(), 0);

  document.insert(0, "x", noCoalesce());
  EXPECT_EQ(document.version(), 1);
  document.erase(0, 1, noCoalesce());
  EXPECT_EQ(document.version(), 2);
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.version(), 3);
  EXPECT_TRUE(document.redo());
  EXPECT_EQ(document.version(), 4);
  EXPECT_FALSE(document.redo());
  EXPECT_EQ(document.version(), 4);
}

TEST(DocumentTest, InsertAtStartMiddleEndAndClamped) {
  Document document("BCD");
  document.insert(0, "A", noCoalesce());
  EXPECT_EQ(document.text(), "ABCD");
  document.insert(2, "-", noCoalesce());
  EXPECT_EQ(document.text(), "AB-CD");
  document.insert(document.size(), "E", noCoalesce());
  EXPECT_EQ(document.text(), "AB-CDE");
  document.insert(1000, "!", noCoalesce());
  EXPECT_EQ(document.text(), "AB-CDE!");
}

TEST(DocumentTest, TextExtraction) {
  Document document("hello");
  document.insert(2, "XY", noCoalesce());
  ASSERT_EQ(document.text(), "heXYllo");

  EXPECT_EQ(document.textRange(0, 7), "heXYllo");
  EXPECT_EQ(document.textRange(2, 2), "XY");
  EXPECT_EQ(document.textRange(5, 100), "lo");  // clamped
  EXPECT_EQ(document.textRange(7, 1), "");
  EXPECT_EQ(document.textRange(0, 0), "");

  char buffer[16] = {};
  EXPECT_EQ(document.copyOut(1, std::span<char>(buffer, 4)), 4u);
  EXPECT_EQ(std::string_view(buffer, 4), "eXYl");
  EXPECT_EQ(document.copyOut(99, std::span<char>(buffer, 4)), 0u);

  // Zero copy only inside one piece.
  const std::optional<std::string_view> inside = document.contiguousText(2, 2);
  ASSERT_TRUE(inside.has_value());
  EXPECT_EQ(*inside, "XY");
  EXPECT_FALSE(document.contiguousText(0, 7).has_value());
}

// --- line index -----------------------------------------------------------

TEST(DocumentTest, LineRangesWithLineFeeds) {
  // a0 b1 \n2 c3 d4 e5 \n6 \n7 f8
  Document document("ab\ncde\n\nf");
  ASSERT_EQ(document.size(), 9u);
  ASSERT_EQ(document.lineCount(), 4);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 2, 1}));
  EXPECT_EQ(document.lineAt(1), (LineRange{3, 6, 1}));
  EXPECT_EQ(document.lineAt(2), (LineRange{7, 7, 1}));
  EXPECT_EQ(document.lineAt(3), (LineRange{8, 9, 0}));
  EXPECT_EQ(document.lineAt(0).rawEnd(), 3u);
  EXPECT_EQ(document.lineAt(2).length(), 0u);
  EXPECT_EQ(document.lineAt(99), (LineRange{8, 9, 0}));  // clamped
}

TEST(DocumentTest, LineRangesWithCrLf) {
  Document document("a\r\nb");
  ASSERT_EQ(document.lineCount(), 2);
  // CRLF is one line break, but both bytes stay in the buffer.
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 1, 2}));
  EXPECT_EQ(document.lineAt(0).rawEnd(), 3u);
  EXPECT_EQ(document.lineAt(1), (LineRange{3, 4, 0}));
  EXPECT_EQ(document.text(), "a\r\nb");
  EXPECT_EQ(document.byteAt(1), '\r');
}

TEST(DocumentTest, LineRangesWithMixedEndings) {
  // a0 \r1 \n2 b3 \n4 c5 \r6 \n7
  Document document("a\r\nb\nc\r\n");
  ASSERT_EQ(document.size(), 8u);
  ASSERT_EQ(document.lineCount(), 4);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 1, 2}));
  EXPECT_EQ(document.lineAt(1), (LineRange{3, 4, 1}));
  EXPECT_EQ(document.lineAt(2), (LineRange{5, 6, 2}));
  EXPECT_EQ(document.lineAt(3), (LineRange{8, 8, 0}));
  EXPECT_EQ(document.textRange(0, 8), "a\r\nb\nc\r\n");
}

TEST(DocumentTest, EmptyCrLfOnlyDocument) {
  Document document("\r\n");
  ASSERT_EQ(document.lineCount(), 2);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 0, 2}));
  EXPECT_EQ(document.lineAt(1), (LineRange{2, 2, 0}));
}

TEST(DocumentTest, LoneCarriageReturnIsNotALineBreak) {
  // Classic-Mac endings are out of scope: only LF (optionally preceded by CR)
  // splits lines. The bytes are still preserved exactly.
  Document document("a\rb");
  EXPECT_EQ(document.lineCount(), 1);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 3, 0}));
  EXPECT_EQ(document.text(), "a\rb");
}

TEST(DocumentTest, NoTrailingNewline) {
  Document document("one\ntwo");
  ASSERT_EQ(document.lineCount(), 2);
  EXPECT_EQ(document.lineAt(1), (LineRange{4, 7, 0}));
  EXPECT_EQ(document.textRange(4, 3), "two");
}

TEST(DocumentTest, TrailingNewlineMakesAnEmptyLastLine) {
  Document document("one\n");
  ASSERT_EQ(document.lineCount(), 2);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 3, 1}));
  EXPECT_EQ(document.lineAt(1), (LineRange{4, 4, 0}));
}

TEST(DocumentTest, LineColumnRoundTrip) {
  Document document("ab\ncde\n\nf");
  EXPECT_EQ(document.lineColumnOf(0), (Position{0, 0}));
  EXPECT_EQ(document.lineColumnOf(1), (Position{0, 1}));
  EXPECT_EQ(document.lineColumnOf(2), (Position{0, 2}));
  EXPECT_EQ(document.lineColumnOf(3), (Position{1, 0}));
  EXPECT_EQ(document.lineColumnOf(6), (Position{1, 3}));
  EXPECT_EQ(document.lineColumnOf(7), (Position{2, 0}));
  EXPECT_EQ(document.lineColumnOf(8), (Position{3, 0}));
  EXPECT_EQ(document.lineColumnOf(9), (Position{3, 1}));
  EXPECT_EQ(document.lineColumnOf(1000), (Position{3, 1}));

  EXPECT_EQ(document.offsetOf(0, 0), 0u);
  EXPECT_EQ(document.offsetOf(0, 2), 2u);
  EXPECT_EQ(document.offsetOf(0, 99), 2u);  // clamped to the line content end
  EXPECT_EQ(document.offsetOf(1, 0), 3u);
  EXPECT_EQ(document.offsetOf(1, 3), 6u);
  EXPECT_EQ(document.offsetOf(2, 0), 7u);
  EXPECT_EQ(document.offsetOf(2, 5), 7u);
  EXPECT_EQ(document.offsetOf(3, 1), 9u);
  EXPECT_EQ(document.offsetOf(99, 0), 8u);  // clamped to the last line
  EXPECT_EQ(document.offsetOf(-1, 0), 0u);

  for (int64_t line = 0; line < document.lineCount(); ++line) {
    const size_t start = document.offsetOf(line, 0);
    EXPECT_EQ(document.lineColumnOf(start), (Position{line, 0})) << "line=" << line;
    EXPECT_EQ(document.lineAt(line).start, start) << "line=" << line;
  }
}

TEST(DocumentTest, CaretCannotSitInsideCrLf) {
  Document document("a\r\nb");
  EXPECT_EQ(document.offsetOf(0, 1), 1u);
  EXPECT_EQ(document.offsetOf(0, 2), 1u);  // would land on the LF
  EXPECT_EQ(document.offsetOf(0, 99), 1u);
}

TEST(DocumentTest, InsertingNewlineSplitsALine) {
  Document document("hello world");
  ASSERT_EQ(document.lineCount(), 1);
  document.insert(5, "\n", noCoalesce());
  ASSERT_EQ(document.text(), "hello\n world");
  EXPECT_EQ(document.lineCount(), 2);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 5, 1}));
  EXPECT_EQ(document.lineAt(1), (LineRange{6, 12, 0}));
}

TEST(DocumentTest, ErasingNewlineJoinsLines) {
  Document document("hello\nworld");
  ASSERT_EQ(document.lineCount(), 2);
  document.erase(5, 1, noCoalesce());
  ASSERT_EQ(document.text(), "helloworld");
  EXPECT_EQ(document.lineCount(), 1);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, 10, 0}));
}

TEST(DocumentTest, ManyLineEditsKeepTheIndexConsistent) {
  Document document("l0\nl1\nl2\nl3\nl4\n");
  ASSERT_EQ(document.lineCount(), 6);

  document.insert(3, "X\nY\n", noCoalesce());  // two extra newlines
  ASSERT_EQ(document.text(), "l0\nX\nY\nl1\nl2\nl3\nl4\n");
  EXPECT_EQ(document.lineCount(), 8);
  EXPECT_EQ(document.textRange(document.lineAt(1).start, document.lineAt(1).length()), "X");
  EXPECT_EQ(document.textRange(document.lineAt(2).start, document.lineAt(2).length()), "Y");
  EXPECT_EQ(document.textRange(document.lineAt(3).start, document.lineAt(3).length()), "l1");

  document.erase(3, 4, noCoalesce());  // remove "X\nY\n" again
  ASSERT_EQ(document.text(), "l0\nl1\nl2\nl3\nl4\n");
  EXPECT_EQ(document.lineCount(), 6);
  for (int64_t line = 0; line < 5; ++line) {
    const LineRange range = document.lineAt(line);
    EXPECT_EQ(document.textRange(range.start, range.length()), "l" + std::to_string(line));
    EXPECT_EQ(range.terminatorLength, 1u);
  }
  EXPECT_EQ(document.lineAt(5), (LineRange{15, 15, 0}));
}

TEST(DocumentTest, OneHugeLineWithNoNewline) {
  // 10 MB, one line, no terminator: line queries must not walk the bytes and
  // edits must not care about the size.
  const size_t kSize = 10u * 1024u * 1024u;
  Document document(std::string(kSize, 'a'));
  ASSERT_EQ(document.size(), kSize);
  EXPECT_EQ(document.lineCount(), 1);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, kSize, 0}));
  EXPECT_EQ(document.lineColumnOf(kSize / 2), (Position{0, static_cast<int64_t>(kSize / 2)}));
  EXPECT_EQ(document.offsetOf(0, static_cast<int64_t>(kSize)), kSize);
  EXPECT_EQ(document.offsetOf(0, static_cast<int64_t>(kSize) + 100), kSize);

  document.insert(kSize / 2, "\n", noCoalesce());
  EXPECT_EQ(document.lineCount(), 2);
  EXPECT_EQ(document.lineAt(0), (LineRange{0, kSize / 2, 1}));
  EXPECT_EQ(document.lineAt(1), (LineRange{kSize / 2 + 1, kSize + 1, 0}));
  EXPECT_EQ(document.pieces().pieceCount(), 3u);

  document.erase(kSize / 2, 1, noCoalesce());
  EXPECT_EQ(document.lineCount(), 1);
  EXPECT_EQ(document.size(), kSize);
}

TEST(DocumentTest, ManyLinesLineLookupMatchesNaiveScan) {
  std::string source;
  for (int i = 0; i < 4000; ++i) {
    source += "line ";
    source += std::to_string(i);
    source += '\n';
  }
  Document document(source);
  ASSERT_EQ(document.lineCount(), 4001);

  // Naive reference: collect every line start by scanning.
  std::vector<size_t> starts;
  starts.push_back(0);
  for (size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') {
      starts.push_back(i + 1);
    }
  }
  ASSERT_EQ(starts.size(), 4001u);
  for (size_t i = 0; i < starts.size(); ++i) {
    const LineRange range = document.lineAt(static_cast<int64_t>(i));
    ASSERT_EQ(range.start, starts[i]) << "line=" << i;
    const size_t rawEnd = i + 1 < starts.size() ? starts[i + 1] : source.size();
    ASSERT_EQ(range.rawEnd(), rawEnd) << "line=" << i;
  }
}

// --- UTF-8 ----------------------------------------------------------------

TEST(DocumentTest, CodepointMovementNeverSplitsASequence) {
  const std::string text = std::string("a") + kEAcute + kEuro + kClef + "b";
  Document document(text);
  ASSERT_EQ(document.size(), 11u);

  std::vector<size_t> forward;
  size_t at = 0;
  while (at < document.size()) {
    forward.push_back(at);
    const size_t next = document.nextCodepoint(at);
    ASSERT_GT(next, at);
    at = next;
  }
  EXPECT_EQ(at, 11u);
  const std::vector<size_t> expected = {0, 1, 3, 6, 10};
  EXPECT_EQ(forward, expected);

  std::vector<size_t> backward;
  at = document.size();
  while (at > 0) {
    const size_t previous = document.prevCodepoint(at);
    ASSERT_LT(previous, at);
    backward.push_back(previous);
    at = previous;
  }
  const std::vector<size_t> expectedBackward = {10, 6, 3, 1, 0};
  EXPECT_EQ(backward, expectedBackward);
}

TEST(DocumentTest, CodepointMovementAcrossPieceBoundaries) {
  // Build the 3-byte euro sign out of two separate pieces so the movement code
  // has to read through the piece table, not a contiguous buffer.
  Document document;
  document.insert(0, "\x82\xac", noCoalesce());
  document.insert(0, "\xe2", noCoalesce());
  ASSERT_EQ(document.size(), 3u);
  ASSERT_EQ(document.pieces().pieceCount(), 2u);
  EXPECT_EQ(document.text(), kEuro);
  EXPECT_EQ(document.nextCodepoint(0), 3u);
  EXPECT_EQ(document.prevCodepoint(3), 0u);
  EXPECT_EQ(document.snapToCodepointStart(1), 0u);
  EXPECT_EQ(document.snapToCodepointStart(2), 0u);
}

TEST(DocumentTest, SnapToCodepointStart) {
  const std::string text = std::string("x") + kClef + "y";
  Document document(text);
  EXPECT_EQ(document.snapToCodepointStart(0), 0u);
  EXPECT_EQ(document.snapToCodepointStart(1), 1u);
  EXPECT_EQ(document.snapToCodepointStart(2), 1u);
  EXPECT_EQ(document.snapToCodepointStart(3), 1u);
  EXPECT_EQ(document.snapToCodepointStart(4), 1u);
  EXPECT_EQ(document.snapToCodepointStart(5), 5u);
  EXPECT_EQ(document.snapToCodepointStart(6), 6u);
  EXPECT_EQ(document.snapToCodepointStart(99), 6u);
}

TEST(DocumentTest, OffsetOfSnapsAwayFromMidSequence) {
  const std::string text = std::string("ab") + kClef;  // 2 + 4 bytes
  Document document(text);
  ASSERT_EQ(document.size(), 6u);
  EXPECT_EQ(document.offsetOf(0, 2), 2u);
  EXPECT_EQ(document.offsetOf(0, 3), 2u);  // inside the 4-byte sequence
  EXPECT_EQ(document.offsetOf(0, 4), 2u);
  EXPECT_EQ(document.offsetOf(0, 5), 2u);
  EXPECT_EQ(document.offsetOf(0, 6), 6u);
}

TEST(DocumentTest, CombiningMarkIsItsOwnCodepointButAddsNoWidth) {
  const std::string text = std::string("e") + kCombiningAcute + "x";
  Document document(text);
  ASSERT_EQ(document.size(), 4u);
  EXPECT_EQ(document.nextCodepoint(0), 1u);
  EXPECT_EQ(document.nextCodepoint(1), 3u);
  EXPECT_EQ(document.nextCodepoint(3), 4u);
  EXPECT_EQ(document.displayColumnOf(0, 4), 0);
  EXPECT_EQ(document.displayColumnOf(1, 4), 1);
  EXPECT_EQ(document.displayColumnOf(3, 4), 1);  // the mark takes no cell
  EXPECT_EQ(document.displayColumnOf(4, 4), 2);
  // The caret lands after the mark, not between base and mark.
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 1, 4), 3u);
}

TEST(DocumentTest, DisplayColumnWithTabs) {
  Document document("\tab\tc");
  //  offset: 0 '\t', 1 'a', 2 'b', 3 '\t', 4 'c'
  EXPECT_EQ(document.displayColumnOf(0, 4), 0);
  EXPECT_EQ(document.displayColumnOf(1, 4), 4);
  EXPECT_EQ(document.displayColumnOf(2, 4), 5);
  EXPECT_EQ(document.displayColumnOf(3, 4), 6);
  EXPECT_EQ(document.displayColumnOf(4, 4), 8);  // tab from column 6 -> 8
  EXPECT_EQ(document.displayColumnOf(5, 4), 9);
  // tabWidth 8 and a nonsensical tabWidth both behave sanely.
  EXPECT_EQ(document.displayColumnOf(1, 8), 8);
  EXPECT_EQ(document.displayColumnOf(1, 0), 1);
}

TEST(DocumentTest, DisplayColumnWithWideCharacters) {
  const std::string text = std::string(kCjkDay) + kCjkDay + "x";
  Document document(text);
  ASSERT_EQ(document.size(), 7u);
  EXPECT_EQ(document.displayColumnOf(0, 4), 0);
  EXPECT_EQ(document.displayColumnOf(3, 4), 2);
  EXPECT_EQ(document.displayColumnOf(6, 4), 4);
  EXPECT_EQ(document.displayColumnOf(7, 4), 5);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 0, 4), 0u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 1, 4), 0u);  // inside a wide cell
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 2, 4), 3u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 4, 4), 6u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 99, 4), 7u);
}

TEST(DocumentTest, OffsetOfDisplayColumnRoundsDownInsideTabs) {
  Document document("\tab");
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 0, 4), 0u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 1, 4), 0u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 3, 4), 0u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 4, 4), 1u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 5, 4), 2u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, 6, 4), 3u);
  EXPECT_EQ(document.offsetOfDisplayColumn(0, -3, 4), 0u);
}

TEST(DocumentTest, DisplayColumnIsPerLine) {
  Document document("ab\n\tcd");
  EXPECT_EQ(document.displayColumnOf(4, 4), 4);  // after the tab on line 1
  EXPECT_EQ(document.offsetOfDisplayColumn(1, 4, 4), 4u);
  EXPECT_EQ(document.offsetOfDisplayColumn(1, 0, 4), 3u);
}

TEST(DocumentTest, MultibyteEditsKeepTextIntact) {
  Document document(std::string(kEuro) + kCjkDay);
  ASSERT_EQ(document.size(), 6u);
  document.insert(3, kClef, noCoalesce());
  EXPECT_EQ(document.text(), std::string(kEuro) + kClef + kCjkDay);
  document.erase(3, 4, noCoalesce());
  EXPECT_EQ(document.text(), std::string(kEuro) + kCjkDay);
}

// --- undo / redo ----------------------------------------------------------

TEST(DocumentTest, UndoRedoSingleEdit) {
  Document document("hello");
  document.insert(5, " world", noCoalesce());
  ASSERT_EQ(document.text(), "hello world");
  ASSERT_TRUE(document.canUndo());
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "hello");
  EXPECT_FALSE(document.canUndo());
  ASSERT_TRUE(document.canRedo());
  EXPECT_TRUE(document.redo());
  EXPECT_EQ(document.text(), "hello world");
  EXPECT_FALSE(document.canRedo());
}

TEST(DocumentTest, UndoRedoDeepSequence) {
  Document document;
  std::vector<std::string> states;
  states.push_back("");
  for (int i = 0; i < 200; ++i) {
    document.insert(document.size(), std::to_string(i % 10), noCoalesce(1000 + i));
    states.push_back(document.text());
  }
  ASSERT_EQ(states.size(), 201u);
  for (int i = 199; i >= 0; --i) {
    ASSERT_TRUE(document.undo()) << "i=" << i;
    ASSERT_EQ(document.text(), states[static_cast<size_t>(i)]) << "i=" << i;
  }
  EXPECT_FALSE(document.undo());
  for (int i = 1; i <= 200; ++i) {
    ASSERT_TRUE(document.redo()) << "i=" << i;
    ASSERT_EQ(document.text(), states[static_cast<size_t>(i)]) << "i=" << i;
  }
  EXPECT_FALSE(document.redo());
}

TEST(DocumentTest, UndoRestoresCursorPosition) {
  Document document("hello");
  document.setCursor(CursorState{5, 5});
  document.insert(5, "!", noCoalesce());
  EXPECT_EQ(document.cursor().offset, 6u);
  ASSERT_TRUE(document.undo());
  EXPECT_EQ(document.cursor().offset, 5u);
  ASSERT_TRUE(document.redo());
  EXPECT_EQ(document.cursor().offset, 6u);
}

TEST(DocumentTest, UndoOfEraseRestoresExactBytes) {
  Document document("a\r\nb\xc3\xa9");
  const std::string original = document.text();
  document.erase(1, 4, noCoalesce());
  ASSERT_EQ(document.text(), std::string("a\xa9"));
  ASSERT_TRUE(document.undo());
  EXPECT_EQ(document.text(), original);
  EXPECT_EQ(document.lineCount(), 2);
}

TEST(DocumentTest, NewEditReplacesTheRedoBranchButKeepsItReachable) {
  Document document;
  document.insert(0, "a", noCoalesce());
  document.insert(1, "b", noCoalesce());
  ASSERT_EQ(document.text(), "ab");
  ASSERT_TRUE(document.undo());
  ASSERT_EQ(document.text(), "a");
  ASSERT_TRUE(document.canRedo());

  document.insert(1, "c", noCoalesce());
  EXPECT_EQ(document.text(), "ac");
  EXPECT_FALSE(document.canRedo());  // classic "redo invalidated" behaviour

  ASSERT_TRUE(document.undo());
  ASSERT_EQ(document.text(), "a");
  EXPECT_EQ(document.redoBranchCount(), 2u);
  ASSERT_TRUE(document.redo());  // newest branch
  EXPECT_EQ(document.text(), "ac");

  ASSERT_TRUE(document.undo());
  ASSERT_TRUE(document.redo(0));  // the abandoned branch is still there
  EXPECT_EQ(document.text(), "ab");
  EXPECT_FALSE(document.redo(7));  // out-of-range branch
}

TEST(DocumentTest, TypingCoalescesIntoOneUndoStep) {
  Document document;
  document.insert(0, "a", typedAt(1000));
  document.insert(1, "b", typedAt(1100));
  document.insert(2, "c", typedAt(1200));
  ASSERT_EQ(document.text(), "abc");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "");
  EXPECT_FALSE(document.canUndo());
  EXPECT_TRUE(document.redo());
  EXPECT_EQ(document.text(), "abc");
}

TEST(DocumentTest, TypingPauseStartsANewUndoStep) {
  Document document;
  document.insert(0, "a", typedAt(1000));
  document.insert(1, "b", typedAt(1100));
  document.insert(2, "c", typedAt(5000));  // long pause
  ASSERT_EQ(document.text(), "abc");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "ab");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "");
}

TEST(DocumentTest, MovingTheCaretStartsANewUndoStep) {
  Document document("xy");
  document.setCursor(CursorState{0, 0});
  document.insert(0, "a", typedAt(1000));
  document.setCursor(CursorState{2, 2});  // user clicked elsewhere
  document.insert(2, "b", typedAt(1050));
  ASSERT_EQ(document.text(), "axby");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "axy");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "xy");
}

TEST(DocumentTest, NewlineIsNeverCoalesced) {
  Document document;
  document.insert(0, "a", typedAt(1000));
  document.insert(1, "\n", typedAt(1050));
  document.insert(2, "b", typedAt(1100));
  ASSERT_EQ(document.text(), "a\nb");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "a\n");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "a");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "");
}

TEST(DocumentTest, MultibyteTypingCoalesces) {
  Document document;
  document.insert(0, kEAcute, typedAt(1000));
  document.insert(2, kEuro, typedAt(1050));
  ASSERT_EQ(document.text(), std::string(kEAcute) + kEuro);
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "");
}

TEST(DocumentTest, BackspaceRunCoalesces) {
  Document document("abcdef");
  document.setCursor(CursorState{6, 6});
  document.erase(5, 1, typedAt(1000));
  document.erase(4, 1, typedAt(1050));
  document.erase(3, 1, typedAt(1100));
  ASSERT_EQ(document.text(), "abc");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "abcdef");
  EXPECT_EQ(document.cursor().offset, 6u);
}

TEST(DocumentTest, ForwardDeleteRunCoalesces) {
  Document document("abcdef");
  document.setCursor(CursorState{2, 2});
  document.erase(2, 1, typedAt(1000));
  document.erase(2, 1, typedAt(1050));
  ASSERT_EQ(document.text(), "abef");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "abcdef");
}

TEST(DocumentTest, PasteIsNotCoalescedWithTyping) {
  Document document;
  document.insert(0, "pasted text", typedAt(1000));
  document.insert(11, "x", typedAt(1050));
  ASSERT_EQ(document.text(), "pasted textx");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "pasted text");
  EXPECT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "");
}

TEST(DocumentTest, ReplaceIsOneUndoStep) {
  Document document("hello world");
  document.replace(0, 5, "goodbye", noCoalesce());
  ASSERT_EQ(document.text(), "goodbye world");
  EXPECT_EQ(document.cursor().offset, 7u);
  ASSERT_TRUE(document.undo());
  EXPECT_EQ(document.text(), "hello world");
  ASSERT_TRUE(document.redo());
  EXPECT_EQ(document.text(), "goodbye world");
}

TEST(DocumentTest, ReplaceHandlesDegenerateArguments) {
  Document document("abc");
  const int64_t before = document.version();
  document.replace(1, 0, "", noCoalesce());  // nothing at all
  EXPECT_EQ(document.version(), before);
  document.replace(1, 0, "Z", noCoalesce());  // pure insert
  EXPECT_EQ(document.text(), "aZbc");
  document.replace(0, 2, "", noCoalesce());  // pure erase
  EXPECT_EQ(document.text(), "bc");
  document.replace(99, 5, "!", noCoalesce());  // clamped to the end
  EXPECT_EQ(document.text(), "bc!");
}

TEST(DocumentTest, UndoRedoAcrossPieceHeavyHistory) {
  Document document("0123456789");
  for (int i = 0; i < 50; ++i) {
    document.insert(static_cast<size_t>(i) % (document.size() + 1), "#", noCoalesce(1000 + i));
  }
  const std::string mutated = document.text();
  ASSERT_EQ(mutated.size(), 60u);
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(document.undo()) << "i=" << i;
  }
  EXPECT_EQ(document.text(), "0123456789");
  EXPECT_EQ(document.lineCount(), 1);
  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(document.redo()) << "i=" << i;
  }
  EXPECT_EQ(document.text(), mutated);
}

}  // namespace
