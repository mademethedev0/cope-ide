// The most important test in the module: Document is driven with thousands of
// random operations side by side with a naive std::string model, and *every*
// observable is compared after *every* single operation. Since nobody can
// compile locally, this differential test is the real verification that the
// treap splits, joins, line aggregates and undo tree agree with reality.
//
// Coalescing is switched off in the main fuzz so that one edit == one undo
// step, which is what makes the string-stack model exact. Coalescing has its
// own dedicated tests in undo_test.cpp and document_test.cpp.

#include <gtest/gtest.h>

#include <ide/text/document.h>
#include <ide/text/utf8.h>

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ide::text::Document;
using ide::text::EditOptions;
using ide::text::LineRange;
using ide::text::Position;

EditOptions fuzzOptions() {
  EditOptions options;
  options.timestampMs = 0;  // pinned: no clock calls, fully reproducible
  options.coalesce = false;
  return options;
}

// --- naive reference model ------------------------------------------------

int64_t refLineCount(const std::string& text) {
  int64_t lines = 1;
  for (char c : text) {
    if (c == '\n') {
      ++lines;
    }
  }
  return lines;
}

std::vector<size_t> refLineStarts(const std::string& text) {
  std::vector<size_t> starts;
  starts.push_back(0);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      starts.push_back(i + 1);
    }
  }
  return starts;
}

LineRange refLineAt(const std::string& text, const std::vector<size_t>& starts, int64_t line) {
  if (line < 0) {
    line = 0;
  }
  if (line >= static_cast<int64_t>(starts.size())) {
    line = static_cast<int64_t>(starts.size()) - 1;
  }
  const size_t index = static_cast<size_t>(line);
  const size_t start = starts[index];
  const size_t rawEnd = index + 1 < starts.size() ? starts[index + 1] : text.size();
  size_t terminator = 0;
  if (index + 1 < starts.size()) {
    terminator = 1;
    if (rawEnd >= start + 2 && text[rawEnd - 2] == '\r') {
      terminator = 2;
    }
  }
  LineRange range;
  range.start = start;
  range.end = rawEnd - terminator;
  range.terminatorLength = terminator;
  return range;
}

// Snippets deliberately mix ASCII, both line endings, tabs, 2/3/4-byte UTF-8
// and a combining mark. Hex escapes are never followed by a hex digit.
const char* const kSnippets[] = {
    "a",
    "b",
    "xy",
    "\n",
    "\r\n",
    "\t",
    " ",
    "\xc3\xa9",              // U+00E9
    "\xe2\x82\xac",          // U+20AC
    "\xf0\x9d\x84\x9e",      // U+1D11E
    "e\xcc\x81",             // e + combining acute
    "hello world",
    "line1\nline2\n",
    "\n\n\n",
    "\xe6\x97\xa5\xe6\x9c\xac",  // CJK, wide
    "}",
    "{",
    "0123456789",
};
constexpr size_t kSnippetCount = sizeof(kSnippets) / sizeof(kSnippets[0]);

void checkInvariants(const Document& document, const std::string& reference, std::mt19937_64& rng,
                     uint64_t seed, int iteration) {
  ASSERT_EQ(document.size(), reference.size()) << "seed=" << seed << " it=" << iteration;
  ASSERT_EQ(document.text(), reference) << "seed=" << seed << " it=" << iteration;
  ASSERT_EQ(document.lineCount(), refLineCount(reference))
      << "seed=" << seed << " it=" << iteration;

  const std::vector<size_t> starts = refLineStarts(reference);
  ASSERT_EQ(static_cast<size_t>(document.lineCount()), starts.size())
      << "seed=" << seed << " it=" << iteration;

  // Every line, when the document is small; a random sample otherwise.
  const int64_t lines = document.lineCount();
  const int samples = lines <= 12 ? static_cast<int>(lines) : 6;
  for (int s = 0; s < samples; ++s) {
    const int64_t line = lines <= 12 ? s : static_cast<int64_t>(rng() % static_cast<uint64_t>(lines));
    const LineRange actual = document.lineAt(line);
    const LineRange expected = refLineAt(reference, starts, line);
    ASSERT_EQ(actual.start, expected.start)
        << "seed=" << seed << " it=" << iteration << " line=" << line;
    ASSERT_EQ(actual.end, expected.end)
        << "seed=" << seed << " it=" << iteration << " line=" << line;
    ASSERT_EQ(actual.terminatorLength, expected.terminatorLength)
        << "seed=" << seed << " it=" << iteration << " line=" << line;

    // Line start <-> (line, 0) round trip.
    ASSERT_EQ(document.offsetOf(line, 0), expected.start)
        << "seed=" << seed << " it=" << iteration << " line=" << line;
    const Position position = document.lineColumnOf(expected.start);
    ASSERT_EQ(position.line, line) << "seed=" << seed << " it=" << iteration;
    ASSERT_EQ(position.column, 0) << "seed=" << seed << " it=" << iteration;
  }

  if (!reference.empty()) {
    // Random read-back through both extraction paths.
    const size_t offset = static_cast<size_t>(rng() % reference.size());
    const size_t length =
        1 + static_cast<size_t>(rng() % static_cast<uint64_t>(reference.size() - offset));
    ASSERT_EQ(document.textRange(offset, length), reference.substr(offset, length))
        << "seed=" << seed << " it=" << iteration << " [" << offset << "+" << length << ")";
    const std::optional<std::string_view> view = document.contiguousText(offset, length);
    if (view.has_value()) {
      ASSERT_EQ(*view, reference.substr(offset, length))
          << "seed=" << seed << " it=" << iteration;
    }
    std::string buffer(length, '\0');
    ASSERT_EQ(document.copyOut(offset, std::span<char>(buffer.data(), buffer.size())), length);
    ASSERT_EQ(buffer, reference.substr(offset, length))
        << "seed=" << seed << " it=" << iteration;

    // Byte-for-byte column arithmetic at a random offset.
    const Position position = document.lineColumnOf(offset);
    const size_t lineStart = starts[static_cast<size_t>(position.line)];
    ASSERT_EQ(static_cast<size_t>(position.column), offset - lineStart)
        << "seed=" << seed << " it=" << iteration << " offset=" << offset;

    // Caret movement always makes progress and never leaves the document.
    const size_t next = document.nextCodepoint(offset);
    ASSERT_GT(next, offset) << "seed=" << seed << " it=" << iteration;
    ASSERT_LE(next, reference.size()) << "seed=" << seed << " it=" << iteration;
    const size_t previous = document.prevCodepoint(offset == 0 ? 1 : offset);
    ASSERT_LT(previous, offset == 0 ? 1u : offset) << "seed=" << seed << " it=" << iteration;
  }
}

void runFuzz(uint64_t seed, int iterations) {
  std::mt19937_64 rng(seed);
  Document document;
  std::string reference;

  // Linear-history model: redo always follows the newest branch, and a new edit
  // makes the current node a childless leaf, so a stack of states is exact.
  std::vector<std::string> states;
  states.push_back(reference);
  size_t current = 0;
  int64_t lastVersion = document.version();
  const EditOptions options = fuzzOptions();

  for (int iteration = 0; iteration < iterations; ++iteration) {
    // Grow first so the treap really reaches thousands of pieces, then settle
    // into a mixed workload and bias towards erasing if it gets unwieldy (the
    // naive model is O(n) per check).
    const bool growPhase = iteration < iterations / 3;
    const int insertWeight = growPhase ? 70 : (reference.size() > 6000 ? 20 : 45);
    const int roll = static_cast<int>(rng() % 100);

    if (roll < insertWeight) {
      const size_t offset = static_cast<size_t>(rng() % (reference.size() + 1));
      const std::string snippet = kSnippets[rng() % kSnippetCount];
      document.insert(offset, snippet, options);
      reference.insert(offset, snippet);
      states.resize(current + 1);
      states.push_back(reference);
      current = states.size() - 1;
      ASSERT_GT(document.version(), lastVersion) << "seed=" << seed << " it=" << iteration;
      lastVersion = document.version();
    } else if (roll < 80) {
      if (reference.empty()) {
        continue;
      }
      const size_t offset = static_cast<size_t>(rng() % reference.size());
      const size_t maxLength = reference.size() - offset;
      const size_t capped = maxLength > 8 ? 8 : maxLength;
      const size_t length = 1 + static_cast<size_t>(rng() % static_cast<uint64_t>(capped));
      document.erase(offset, length, options);
      reference.erase(offset, length);
      states.resize(current + 1);
      states.push_back(reference);
      current = states.size() - 1;
      ASSERT_GT(document.version(), lastVersion) << "seed=" << seed << " it=" << iteration;
      lastVersion = document.version();
    } else if (roll < 90) {
      const bool expected = current > 0;
      ASSERT_EQ(document.undo(), expected) << "seed=" << seed << " it=" << iteration;
      if (expected) {
        --current;
        reference = states[current];
        ASSERT_GT(document.version(), lastVersion) << "seed=" << seed << " it=" << iteration;
        lastVersion = document.version();
      }
    } else {
      const bool expected = current + 1 < states.size();
      ASSERT_EQ(document.redo(), expected) << "seed=" << seed << " it=" << iteration;
      if (expected) {
        ++current;
        reference = states[current];
        ASSERT_GT(document.version(), lastVersion) << "seed=" << seed << " it=" << iteration;
        lastVersion = document.version();
      }
    }

    ASSERT_EQ(document.canUndo(), current > 0) << "seed=" << seed << " it=" << iteration;
    ASSERT_EQ(document.canRedo(), current + 1 < states.size())
        << "seed=" << seed << " it=" << iteration;

    checkInvariants(document, reference, rng, seed, iteration);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

TEST(DocumentFuzzTest, DifferentialSeed1) { runFuzz(1ULL, 2500); }
TEST(DocumentFuzzTest, DifferentialSeed2) { runFuzz(2ULL, 2500); }
TEST(DocumentFuzzTest, DifferentialSeed12345) { runFuzz(12345ULL, 2500); }
TEST(DocumentFuzzTest, DifferentialSeed0xDEADBEEF) { runFuzz(0xDEADBEEFULL, 2500); }

// --- UTF-8 clean fuzz -----------------------------------------------------

// Offsets in `text` that start a codepoint, plus text.size(). Valid only when
// `text` is well-formed UTF-8, which this fuzz maintains by construction.
std::vector<size_t> boundariesOf(const std::string& text) {
  std::vector<size_t> boundaries;
  for (size_t i = 0; i < text.size(); ++i) {
    if (!ide::text::utf8::isContinuation(text[i])) {
      boundaries.push_back(i);
    }
  }
  boundaries.push_back(text.size());
  return boundaries;
}

const char* const kCodepoints[] = {
    "a",  "Z",  "\n", "\t", " ",
    "\xc3\xa9",          // 2 bytes
    "\xe2\x82\xac",      // 3 bytes
    "\xe6\x97\xa5",      // 3 bytes, wide
    "\xf0\x9d\x84\x9e",  // 4 bytes
    "\xcc\x81",          // combining mark
};
constexpr size_t kCodepointCount = sizeof(kCodepoints) / sizeof(kCodepoints[0]);

TEST(DocumentFuzzTest, CodepointMovementNeverLandsMidSequence) {
  for (uint64_t seed : {uint64_t{7}, uint64_t{8}, uint64_t{9}}) {
    std::mt19937_64 rng(seed);
    Document document;
    std::string reference;
    const EditOptions options = fuzzOptions();

    for (int iteration = 0; iteration < 700; ++iteration) {
      const std::vector<size_t> boundaries = boundariesOf(reference);
      const bool doInsert = reference.size() < 400 || (rng() % 100) < 55;
      if (doInsert) {
        const size_t offset = boundaries[rng() % boundaries.size()];
        const std::string snippet = kCodepoints[rng() % kCodepointCount];
        document.insert(offset, snippet, options);
        reference.insert(offset, snippet);
      } else if (boundaries.size() > 2) {
        // Erase a whole number of codepoints so the text stays well-formed.
        const size_t startIndex = static_cast<size_t>(rng() % (boundaries.size() - 1));
        const size_t remaining = boundaries.size() - 1 - startIndex;
        const size_t take = 1 + static_cast<size_t>(rng() % (remaining > 4 ? 4 : remaining));
        const size_t from = boundaries[startIndex];
        const size_t to = boundaries[startIndex + take];
        document.erase(from, to - from, options);
        reference.erase(from, to - from);
      }

      ASSERT_EQ(document.text(), reference) << "seed=" << seed << " it=" << iteration;

      // Walking forwards must visit exactly the reference boundaries.
      const std::vector<size_t> expected = boundariesOf(reference);
      std::vector<size_t> walked;
      size_t at = 0;
      while (at < document.size()) {
        walked.push_back(at);
        const size_t next = document.nextCodepoint(at);
        ASSERT_GT(next, at) << "seed=" << seed << " it=" << iteration;
        at = next;
      }
      walked.push_back(document.size());
      ASSERT_EQ(walked, expected) << "seed=" << seed << " it=" << iteration;

      // Walking backwards must visit the same boundaries in reverse.
      std::vector<size_t> back;
      at = document.size();
      back.push_back(at);
      while (at > 0) {
        const size_t previous = document.prevCodepoint(at);
        ASSERT_LT(previous, at) << "seed=" << seed << " it=" << iteration;
        back.push_back(previous);
        at = previous;
      }
      std::vector<size_t> reversed(back.rbegin(), back.rend());
      ASSERT_EQ(reversed, expected) << "seed=" << seed << " it=" << iteration;

      // Snapping is idempotent and never moves a real boundary.
      for (size_t boundary : expected) {
        ASSERT_EQ(document.snapToCodepointStart(boundary), boundary)
            << "seed=" << seed << " it=" << iteration << " boundary=" << boundary;
      }
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }
}

// --- line structure fuzz --------------------------------------------------

// Newline-heavy fragments for the line-structure fuzz.
const char* const kLinePieces[] = {"\n", "\r\n", "a\nb", "\n\n", "x", "\r", "y\r\n"};
constexpr size_t kLinePieceCount = sizeof(kLinePieces) / sizeof(kLinePieces[0]);

// This fuzz targets the line index specifically: after every operation the
// whole line table is compared against a naive scan of the actual bytes. For
// undo/redo the reference is re-synced from the document (content correctness
// of undo/redo is covered exhaustively by the differential fuzz above), so what
// is verified here is that the newline aggregates in the piece tree always
// agree with the bytes, however violently the text is churned.
TEST(DocumentFuzzTest, LineIndexSurvivesNewlineChurn) {
  for (uint64_t seed : {uint64_t{31}, uint64_t{32}}) {
    std::mt19937_64 rng(seed);
    Document document;
    std::string reference;
    const EditOptions options = fuzzOptions();

    for (int iteration = 0; iteration < 1200; ++iteration) {
      const int roll = static_cast<int>(rng() % 100);
      if (roll < 40 || reference.empty()) {
        // Insert something newline-heavy.
        const std::string snippet = kLinePieces[rng() % kLinePieceCount];
        const size_t offset = static_cast<size_t>(rng() % (reference.size() + 1));
        document.insert(offset, snippet, options);
        reference.insert(offset, snippet);
      } else if (roll < 80) {
        const size_t offset = static_cast<size_t>(rng() % reference.size());
        const size_t maxLength = reference.size() - offset;
        const size_t length = 1 + static_cast<size_t>(rng() % (maxLength > 3 ? 3 : maxLength));
        document.erase(offset, length, options);
        reference.erase(offset, length);
      } else if (roll < 90) {
        if (document.undo()) {
          reference = document.text();
        }
      } else {
        if (document.redo()) {
          reference = document.text();
        }
      }

      ASSERT_EQ(document.lineCount(), refLineCount(reference))
          << "seed=" << seed << " it=" << iteration;
      const std::vector<size_t> starts = refLineStarts(reference);
      ASSERT_EQ(static_cast<size_t>(document.lineCount()), starts.size())
          << "seed=" << seed << " it=" << iteration;
      for (size_t index = 0; index < starts.size(); ++index) {
        const LineRange actual = document.lineAt(static_cast<int64_t>(index));
        const LineRange expected = refLineAt(reference, starts, static_cast<int64_t>(index));
        ASSERT_EQ(actual.start, expected.start)
            << "seed=" << seed << " it=" << iteration << " line=" << index;
        ASSERT_EQ(actual.end, expected.end)
            << "seed=" << seed << " it=" << iteration << " line=" << index;
        ASSERT_EQ(actual.terminatorLength, expected.terminatorLength)
            << "seed=" << seed << " it=" << iteration << " line=" << index;
      }
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }
}

}  // namespace
