// Tests for ide::text::search.
//
// The interesting cases are not "does it find a substring" but the four ways a
// windowed search over a piece table can go wrong: a match straddling a window
// boundary, a match straddling a *piece* boundary, backwards scanning, and word
// boundaries evaluated against the document rather than the window.

#include <ide/text/search.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <ide/text/document.h>

namespace {

using ide::text::Document;
using ide::text::findAll;
using ide::text::findNext;
using ide::text::findPrev;
using ide::text::kSearchWindowBytes;
using ide::text::Match;
using ide::text::replaceAll;
using ide::text::SearchOptions;

// Byte layout, indexed by hand (this is the class of expectation that has been
// wrong twice in this project, so it is spelled out):
//   "the quick brown fox\n"       [0, 20)   'the' at 0
//   "jumps over the lazy dog\n"   [20, 44)  'the' at 31
//   "THE END\n"                   [44, 52)  'THE' at 44
//   "lathering\n"                 [52, 62)  'the' at 54, inside a word
constexpr const char* kSample =
    "the quick brown fox\n"
    "jumps over the lazy dog\n"
    "THE END\n"
    "lathering\n";

std::vector<size_t> offsetsOf(const std::vector<Match>& matches) {
  std::vector<size_t> out;
  out.reserve(matches.size());
  for (const Match& match : matches) {
    out.push_back(match.offset);
  }
  return out;
}

TEST(Search, SampleLayoutIsWhatTheCommentClaims) {
  Document doc(kSample);
  EXPECT_EQ(doc.size(), 62u);
  EXPECT_EQ(doc.textRange(0, 3), "the");
  EXPECT_EQ(doc.textRange(31, 3), "the");
  EXPECT_EQ(doc.textRange(44, 3), "THE");
  EXPECT_EQ(doc.textRange(52, 9), "lathering");
  EXPECT_EQ(doc.textRange(54, 3), "the");
}

TEST(Search, CaseSensitiveFindsExactBytesOnly) {
  Document doc(kSample);
  EXPECT_EQ(offsetsOf(findAll(doc, "the")), (std::vector<size_t>{0, 31, 54}));
}

TEST(Search, CaseInsensitiveFoldsAsciiOnly) {
  Document doc(kSample);
  SearchOptions options;
  options.caseSensitive = false;
  EXPECT_EQ(offsetsOf(findAll(doc, "the", options)), (std::vector<size_t>{0, 31, 44, 54}));
}

TEST(Search, WholeWordRejectsMatchInsideAWord) {
  Document doc(kSample);
  SearchOptions options;
  options.wholeWord = true;
  EXPECT_EQ(offsetsOf(findAll(doc, "the", options)), (std::vector<size_t>{0, 31}));
}

TEST(Search, WholeWordAcceptsMatchAtDocumentStartAndEnd) {
  Document doc("the end");
  SearchOptions options;
  options.wholeWord = true;
  EXPECT_EQ(findNext(doc, "the", 0, options), std::optional<Match>(Match{0, 3}));
  EXPECT_EQ(findNext(doc, "end", 0, options), std::optional<Match>(Match{4, 3}));
}

TEST(Search, WholeWordTreatsHighBytesAsWordContent) {
  // "thé" is 't','h','é' where 'é' is two bytes (0xC3 0xA9). A whole-word search
  // for "th" must refuse it.
  Document doc("th\xC3\xA9");
  SearchOptions options;
  options.wholeWord = true;
  EXPECT_FALSE(findNext(doc, "th", 0, options).has_value());
  EXPECT_TRUE(findNext(doc, "th", 0).has_value());
}

TEST(Search, FindNextRespectsStartOffset) {
  Document doc(kSample);
  EXPECT_EQ(findNext(doc, "the", 1), std::optional<Match>(Match{31, 3}));
  EXPECT_EQ(findNext(doc, "the", 31), std::optional<Match>(Match{31, 3}));
  EXPECT_EQ(findNext(doc, "the", 32), std::optional<Match>(Match{54, 3}));
  EXPECT_FALSE(findNext(doc, "the", 55).has_value());
}

TEST(Search, FindPrevIsStrictlyBeforeFrom) {
  Document doc(kSample);
  EXPECT_EQ(findPrev(doc, "the", 31), std::optional<Match>(Match{0, 3}));
  EXPECT_EQ(findPrev(doc, "the", 32), std::optional<Match>(Match{31, 3}));
  EXPECT_EQ(findPrev(doc, "the", 62), std::optional<Match>(Match{54, 3}));
  EXPECT_FALSE(findPrev(doc, "the", 0).has_value());
}

TEST(Search, EmptyNeedleAndOversizedNeedleFindNothing) {
  Document doc("abc");
  EXPECT_FALSE(findNext(doc, "", 0).has_value());
  EXPECT_FALSE(findPrev(doc, "", 3).has_value());
  EXPECT_FALSE(findNext(doc, "abcd", 0).has_value());
  EXPECT_TRUE(findAll(doc, "").empty());
}

TEST(Search, MatchesDoNotOverlap) {
  Document doc("aaaa");
  EXPECT_EQ(offsetsOf(findAll(doc, "aa")), (std::vector<size_t>{0, 2}));
}

TEST(Search, MaxMatchesCaps) {
  Document doc("x.x.x.x");
  EXPECT_EQ(offsetsOf(findAll(doc, "x", {}, 2u)), (std::vector<size_t>{0, 2}));
}

TEST(Search, FindsMatchStraddlingAWindowBoundary) {
  // "needle" starts 2 bytes before the first window ends, so the first window
  // holds only "ne" and the overlap logic is what makes the hit possible.
  const size_t at = kSearchWindowBytes - 2u;
  std::string bytes(at, '.');
  bytes += "needle";
  bytes += std::string(100u, '.');
  Document doc(bytes);

  ASSERT_EQ(doc.textRange(at, 6), "needle");
  EXPECT_EQ(findNext(doc, "needle", 0), std::optional<Match>(Match{at, 6}));
  EXPECT_EQ(findPrev(doc, "needle", doc.size()), std::optional<Match>(Match{at, 6}));
  EXPECT_EQ(offsetsOf(findAll(doc, "needle")), (std::vector<size_t>{at}));
}

TEST(Search, FindsMatchStraddlingAPieceBoundary) {
  // Built by insertion so the needle spans three pieces of the piece table.
  Document doc("nee");
  doc.insert(3, "dl");
  doc.insert(5, "e tail");
  doc.insert(0, "head ");
  ASSERT_EQ(doc.text(), "head needle tail");
  EXPECT_EQ(findNext(doc, "needle", 0), std::optional<Match>(Match{5, 6}));
  EXPECT_EQ(findPrev(doc, "needle", doc.size()), std::optional<Match>(Match{5, 6}));
}

TEST(Search, FindPrevScansPastManyWindows) {
  // One match at the very start of a document several windows long: the backward
  // scan must keep walking instead of giving up after the first window.
  std::string bytes = "needle";
  bytes += std::string(kSearchWindowBytes * 3u, '.');
  Document doc(bytes);
  EXPECT_EQ(findPrev(doc, "needle", doc.size()), std::optional<Match>(Match{0, 6}));
}

TEST(Search, ReplaceAllRewritesEverythingAsOneUndoGroup) {
  Document doc("aXbXc");
  const ide::text::ReplaceResult result = replaceAll(doc, "X", "YY");
  EXPECT_EQ(result.replacements, 2u);
  EXPECT_FALSE(result.tooLarge);
  EXPECT_EQ(doc.text(), "aYYbYYc");

  ASSERT_TRUE(doc.canUndo());
  EXPECT_TRUE(doc.undo());
  EXPECT_EQ(doc.text(), "aXbXc");
  EXPECT_FALSE(doc.canUndo());  // exactly one group was pushed
}

TEST(Search, ReplaceAllWithLongerAndShorterReplacements) {
  Document doc("one two one two");
  EXPECT_EQ(replaceAll(doc, "one", "1").replacements, 2u);
  EXPECT_EQ(doc.text(), "1 two 1 two");

  Document grow("ab ab");
  EXPECT_EQ(replaceAll(grow, "ab", "abcd").replacements, 2u);
  EXPECT_EQ(grow.text(), "abcd abcd");
}

TEST(Search, ReplaceAllHonoursCaseAndWordOptions) {
  Document doc("Cat cat catalog");
  SearchOptions options;
  options.caseSensitive = false;
  options.wholeWord = true;
  EXPECT_EQ(replaceAll(doc, "cat", "dog", options).replacements, 2u);
  EXPECT_EQ(doc.text(), "dog dog catalog");
}

TEST(Search, ReplaceAllRefusesAnOversizedSpanAndChangesNothing) {
  Document doc("X" + std::string(64u, '.') + "X");
  const int64_t before = doc.version();
  const ide::text::ReplaceResult result = replaceAll(doc, "X", "Y", {}, 8u);
  EXPECT_TRUE(result.tooLarge);
  EXPECT_EQ(result.replacements, 0u);
  EXPECT_EQ(doc.version(), before);
  EXPECT_EQ(doc.text(), "X" + std::string(64u, '.') + "X");
}

TEST(Search, ReplaceAllOnNoMatchIsANoOp) {
  Document doc("abc");
  const int64_t before = doc.version();
  const ide::text::ReplaceResult result = replaceAll(doc, "zzz", "y");
  EXPECT_EQ(result.replacements, 0u);
  EXPECT_FALSE(result.tooLarge);
  EXPECT_EQ(doc.version(), before);
}

}  // namespace
