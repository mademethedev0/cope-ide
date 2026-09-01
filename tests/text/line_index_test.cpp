#include <gtest/gtest.h>

#include <ide/text/line_index.h>

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ide::text::countNewlines;
using ide::text::SourceLineTable;

// --- naive reference implementations --------------------------------------

int64_t naiveCount(std::string_view bytes) {
  int64_t found = 0;
  for (char c : bytes) {
    if (c == '\n') {
      ++found;
    }
  }
  return found;
}

int64_t naiveNewlinesIn(std::string_view bytes, size_t from, size_t to) {
  if (to > bytes.size()) {
    to = bytes.size();
  }
  if (from >= to) {
    return 0;
  }
  int64_t found = 0;
  for (size_t i = from; i < to; ++i) {
    if (bytes[i] == '\n') {
      ++found;
    }
  }
  return found;
}

size_t naiveKthNewline(std::string_view bytes, size_t from, size_t to, int64_t k) {
  int64_t seen = 0;
  for (size_t i = from; i < to && i < bytes.size(); ++i) {
    if (bytes[i] == '\n') {
      if (seen == k) {
        return i;
      }
      ++seen;
    }
  }
  return to;
}

std::string makeRandomText(std::mt19937_64& rng, size_t length, int newlinePercent) {
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const int roll = static_cast<int>(rng() % 100);
    if (roll < newlinePercent) {
      out.push_back('\n');
    } else if (roll < newlinePercent + 3) {
      out.push_back('\r');
    } else {
      out.push_back(static_cast<char>('a' + static_cast<int>(rng() % 26)));
    }
  }
  return out;
}

// --- countNewlines --------------------------------------------------------

TEST(CountNewlinesTest, EmptyAndNoNewlines) {
  EXPECT_EQ(countNewlines(std::string_view{}), 0);
  EXPECT_EQ(countNewlines("abc"), 0);
}

TEST(CountNewlinesTest, CountsEveryLineFeed) {
  EXPECT_EQ(countNewlines("\n"), 1);
  EXPECT_EQ(countNewlines("a\nb\n"), 2);
  EXPECT_EQ(countNewlines("\n\n\n"), 3);
  EXPECT_EQ(countNewlines("a\r\nb\r\n"), 2);  // CR is not counted
  EXPECT_EQ(countNewlines(std::string_view("a\0\nb", 4)), 1);
}

TEST(CountNewlinesTest, MatchesNaiveOnRandomText) {
  std::mt19937_64 rng(12345);
  for (int trial = 0; trial < 50; ++trial) {
    const std::string text = makeRandomText(rng, static_cast<size_t>(1 + rng() % 9000), 7);
    EXPECT_EQ(countNewlines(text), naiveCount(text)) << "trial=" << trial;
  }
}

// --- SourceLineTable basics ----------------------------------------------

TEST(SourceLineTableTest, EmptySource) {
  SourceLineTable table;
  table.extendTo(std::string_view{});
  EXPECT_EQ(table.coveredBytes(), 0u);
  EXPECT_EQ(table.totalNewlines(), 0);
  EXPECT_EQ(table.blockEntryCount(), 1u);
  EXPECT_EQ(table.newlinesBefore(std::string_view{}, 0), 0);
  EXPECT_EQ(table.newlinesIn(std::string_view{}, 0, 0), 0);
}

TEST(SourceLineTableTest, SmallSource) {
  const std::string text = "a\nbb\nccc";
  SourceLineTable table;
  table.extendTo(text);
  EXPECT_EQ(table.coveredBytes(), text.size());
  EXPECT_EQ(table.totalNewlines(), 2);
  EXPECT_EQ(table.newlinesBefore(text, 0), 0);
  EXPECT_EQ(table.newlinesBefore(text, 1), 0);
  EXPECT_EQ(table.newlinesBefore(text, 2), 1);
  EXPECT_EQ(table.newlinesBefore(text, text.size()), 2);
  EXPECT_EQ(table.newlinesIn(text, 2, 5), 1);
  EXPECT_EQ(table.newlinesIn(text, 5, 2), 0);  // inverted range
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 0), 1u);
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 1), 4u);
  // Precondition violation returns `to` instead of misbehaving.
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 2), text.size());
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), -1), text.size());
}

TEST(SourceLineTableTest, ClampsPositionsBeyondCoverage) {
  const std::string text = "a\nb";
  SourceLineTable table;
  table.extendTo(text);
  EXPECT_EQ(table.newlinesBefore(text, 999), 1);
  EXPECT_EQ(table.newlinesIn(text, 0, 999), 1);
}

TEST(SourceLineTableTest, BlockEntryCountInvariant) {
  SourceLineTable table;
  const size_t block = SourceLineTable::kBlockBytes;
  for (size_t length : {size_t{0}, size_t{1}, block - 1, block, block + 1, 3 * block,
                        3 * block + 17}) {
    const std::string text(length, 'x');
    table.clear();
    table.extendTo(text);
    const size_t expected = (length + block - 1) / block + 1;  // ceil(len/B) + 1
    EXPECT_EQ(table.blockEntryCount(), expected) << "length=" << length;
    EXPECT_EQ(table.coveredBytes(), length);
  }
}

TEST(SourceLineTableTest, NewlinesExactlyOnBlockBoundaries) {
  const size_t block = SourceLineTable::kBlockBytes;
  std::string text(3 * block, 'x');
  text[0] = '\n';
  text[block - 1] = '\n';
  text[block] = '\n';
  text[2 * block - 1] = '\n';
  text[3 * block - 1] = '\n';

  SourceLineTable table;
  table.extendTo(text);
  ASSERT_EQ(table.totalNewlines(), 5);
  EXPECT_EQ(table.newlinesBefore(text, 0), 0);
  EXPECT_EQ(table.newlinesBefore(text, 1), 1);
  EXPECT_EQ(table.newlinesBefore(text, block - 1), 1);
  EXPECT_EQ(table.newlinesBefore(text, block), 2);
  EXPECT_EQ(table.newlinesBefore(text, block + 1), 3);
  EXPECT_EQ(table.newlinesBefore(text, 2 * block), 4);
  EXPECT_EQ(table.newlinesBefore(text, 3 * block), 5);
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 0), 0u);
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 1), block - 1);
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 2), block);
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 3), 2 * block - 1);
  EXPECT_EQ(table.kthNewline(text, 0, text.size(), 4), 3 * block - 1);
}

TEST(SourceLineTableTest, AllNewlines) {
  const std::string text(SourceLineTable::kBlockBytes * 2 + 5, '\n');
  SourceLineTable table;
  table.extendTo(text);
  ASSERT_EQ(table.totalNewlines(), static_cast<int64_t>(text.size()));
  for (size_t i = 0; i < text.size(); i += 97) {
    EXPECT_EQ(table.newlinesBefore(text, i), static_cast<int64_t>(i));
    EXPECT_EQ(table.kthNewline(text, 0, text.size(), static_cast<int64_t>(i)), i);
  }
}

TEST(SourceLineTableTest, IncrementalExtendMatchesFullRebuild) {
  std::mt19937_64 rng(777);
  std::string text;
  SourceLineTable incremental;
  for (int step = 0; step < 40; ++step) {
    const std::string chunk = makeRandomText(rng, static_cast<size_t>(1 + rng() % 900), 6);
    text += chunk;
    incremental.extendTo(text);

    SourceLineTable rebuilt;
    rebuilt.extendTo(text);

    ASSERT_EQ(incremental.coveredBytes(), text.size()) << "step=" << step;
    ASSERT_EQ(incremental.totalNewlines(), rebuilt.totalNewlines()) << "step=" << step;
    ASSERT_EQ(incremental.blockEntryCount(), rebuilt.blockEntryCount()) << "step=" << step;
    for (size_t position = 0; position <= text.size(); position += 137) {
      ASSERT_EQ(incremental.newlinesBefore(text, position),
                rebuilt.newlinesBefore(text, position))
          << "step=" << step << " position=" << position;
    }
  }
}

TEST(SourceLineTableTest, ShrinkForcesRebuild) {
  SourceLineTable table;
  const std::string big = "a\nb\nc\nd\n";
  table.extendTo(big);
  ASSERT_EQ(table.totalNewlines(), 4);
  const std::string small = "a\n";
  table.extendTo(small);
  EXPECT_EQ(table.coveredBytes(), small.size());
  EXPECT_EQ(table.totalNewlines(), 1);
}

// --- differential fuzz ----------------------------------------------------

TEST(SourceLineTableTest, DifferentialFuzzAgainstNaive) {
  for (uint64_t seed : {uint64_t{1}, uint64_t{2}, uint64_t{99}}) {
    std::mt19937_64 rng(seed);
    // Straddle several blocks so the block-prefix arithmetic is exercised.
    const std::string text = makeRandomText(rng, SourceLineTable::kBlockBytes * 3 + 271, 5);
    SourceLineTable table;
    table.extendTo(text);
    ASSERT_EQ(table.totalNewlines(), naiveCount(text)) << "seed=" << seed;

    for (int trial = 0; trial < 3000; ++trial) {
      const size_t a = static_cast<size_t>(rng() % (text.size() + 1));
      const size_t b = static_cast<size_t>(rng() % (text.size() + 1));
      const size_t from = a < b ? a : b;
      const size_t to = a < b ? b : a;

      ASSERT_EQ(table.newlinesBefore(text, from), naiveNewlinesIn(text, 0, from))
          << "seed=" << seed << " trial=" << trial << " from=" << from;
      const int64_t inRange = table.newlinesIn(text, from, to);
      ASSERT_EQ(inRange, naiveNewlinesIn(text, from, to))
          << "seed=" << seed << " trial=" << trial << " [" << from << "," << to << ")";
      if (inRange > 0) {
        const int64_t k = static_cast<int64_t>(rng() % static_cast<uint64_t>(inRange));
        ASSERT_EQ(table.kthNewline(text, from, to, k), naiveKthNewline(text, from, to, k))
            << "seed=" << seed << " trial=" << trial << " k=" << k;
        ASSERT_EQ(table.kthNewline(text, from, to, inRange - 1),
                  naiveKthNewline(text, from, to, inRange - 1))
            << "seed=" << seed << " trial=" << trial << " last";
      }
    }
  }
}

}  // namespace
