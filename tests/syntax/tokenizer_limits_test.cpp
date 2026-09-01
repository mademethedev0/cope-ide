#include "syntax_test_util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ide/syntax/scope_stack.h>
#include <ide/syntax/tokenizer.h>

namespace {

using cope_test::FlatToken;
using cope_test::Harness;
using cope_test::Lcg;
using ide::syntax::kMaxIterationsPerLine;
using ide::syntax::kMaxLineLength;
using ide::syntax::kMaxStackDepth;
using ide::syntax::kMaxZeroWidthMatchesPerPosition;
using ide::syntax::State;
using ide::syntax::TokenizeResult;
using ide::syntax::TokenSpan;

using Tokens = std::vector<FlatToken>;

/// The invariant every line must satisfy no matter which limit fired: tokens are
/// ordered, non-empty, gap-free and cover exactly [0, line.size()).
void expectCoversLine(const TokenizeResult& result, size_t lineSize) {
    size_t previousEnd = 0;
    for (size_t i = 0; i < result.tokens.size(); ++i) {
        const TokenSpan& token = result.tokens[i];
        ASSERT_EQ(token.begin, previousEnd) << "token " << i;
        ASSERT_LT(token.begin, token.end) << "token " << i;
        ASSERT_LE(token.end, lineSize) << "token " << i;
        previousEnd = token.end;
    }
    ASSERT_EQ(previousEnd, lineSize);
}

// ---------------------------------------------------------------------------
// zero width matches
// ---------------------------------------------------------------------------

constexpr std::string_view kZeroWidthGrammar = R"json({
  "scopeName": "source.z",
  "patterns": [ { "match": "x*", "name": "m.zero" } ]
})json";

TEST(TokenizerLimits, ZeroWidthMatchesForceProgressAndTerminate) {
    Harness h(kZeroWidthGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    // "x*" matches empty at offsets 0, 2 and 3, and "x" at offset 1.
    const Tokens tokens = h.tokenize("yxy", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.z"},
        {1u, 2u, "source.z m.zero"},
        {2u, 3u, "source.z"},
    };
    EXPECT_EQ(tokens, expected);
    // One forced advance at offset 0 and one at offset 2; the streak at the end
    // of the line ends the scan instead of advancing.
    EXPECT_EQ(h.last().forcedAdvances, 2u);
    EXPECT_EQ(h.tokenizer().stats().forcedAdvances, 2u);
    EXPECT_FALSE(h.last().hitIterationLimit);
    EXPECT_FALSE(h.last().hitDepthLimit);
    expectCoversLine(h.last(), 3u);
}

TEST(TokenizerLimits, PureLookaheadRuleCannotHangTheLine) {
    Harness h(R"json({
      "scopeName": "source.zw",
      "patterns": [ { "match": "(?=x)", "name": "look" } ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    // A zero-width match produces no token at all, so both bytes end up in
    // plain root-scope tokens - but the line always finishes.
    const Tokens tokens = h.tokenize("xx", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.zw"},
        {1u, 2u, "source.zw"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.last().forcedAdvances, 2u);
    EXPECT_FALSE(h.last().hitIterationLimit);
    expectCoversLine(h.last(), 2u);
}

TEST(TokenizerLimits, ForcedAdvanceMovesWholeUtf8Characters) {
    Harness h(R"json({
      "scopeName": "source.zm",
      "patterns": [ { "match": "z*", "name": "m.z" } ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    // Two bytes, one codepoint: a byte-wise advance would split it into two
    // tokens, a codepoint-wise advance produces exactly one.
    const Tokens tokens = h.tokenize("\xC3\xA9", h.initialState());
    const Tokens expected = {{0u, 2u, "source.zm"}};
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.last().forcedAdvances, 1u);
    expectCoversLine(h.last(), 2u);
}

TEST(TokenizerLimits, IterationLimitEndsAPathologicalLine) {
    Harness h(kZeroWidthGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const size_t kLineSize = 2000u;
    ASSERT_LT(kLineSize, kMaxLineLength);  // must not bail on length instead
    ASSERT_EQ(kMaxIterationsPerLine % kMaxZeroWidthMatchesPerPosition, 0u)
        << "the token arithmetic below assumes the two limits divide evenly";
    const std::string line(kLineSize, 'y');
    const Tokens tokens = h.tokenize(line, h.initialState());

    // Every position costs kMaxZeroWidthMatchesPerPosition iterations before the
    // forced advance, so the scan dies after that many positions: one one-byte
    // token per position after the first, plus one token for the untokenized
    // tail.
    const size_t positions = kMaxIterationsPerLine / kMaxZeroWidthMatchesPerPosition;
    EXPECT_TRUE(h.last().hitIterationLimit);
    EXPECT_EQ(h.tokenizer().stats().iterationLimitHits, 1u);
    EXPECT_EQ(h.last().forcedAdvances, positions);
    ASSERT_EQ(tokens.size(), positions);
    EXPECT_EQ(tokens.front().begin, 0u);
    EXPECT_EQ(tokens.front().end, 1u);
    EXPECT_EQ(tokens.back().begin, positions - 1u);
    EXPECT_EQ(tokens.back().end, kLineSize);
    expectCoversLine(h.last(), kLineSize);
    EXPECT_EQ(h.endState().depth(), 1u);
}

// ---------------------------------------------------------------------------
// line length
// ---------------------------------------------------------------------------

constexpr std::string_view kLengthGrammar = R"json({
  "scopeName": "source.len",
  "patterns": [
    { "match": "a+", "name": "zz" },
    { "begin": "\\(", "end": "\\)", "name": "m.p", "contentName": "m.in" }
  ]
})json";

TEST(TokenizerLimits, ExactlyMaxLineLengthIsStillTokenized) {
    Harness h(kLengthGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const std::string line(kMaxLineLength, 'a');
    const Tokens tokens = h.tokenize(line, h.initialState());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].begin, 0u);
    EXPECT_EQ(tokens[0].end, kMaxLineLength);
    EXPECT_EQ(tokens[0].scopes, "source.len zz");
    EXPECT_FALSE(h.last().bailedOnLineLength);
    EXPECT_EQ(h.tokenizer().stats().linesBailedOnLength, 0u);
}

TEST(TokenizerLimits, LongLineBailsToOnePlainToken) {
    Harness h(kLengthGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const size_t kSize = 20000u;
    ASSERT_GT(kSize, kMaxLineLength);
    const std::string line(kSize, 'a');
    const State start = h.initialState();
    const Tokens tokens = h.tokenize(line, start);

    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].begin, 0u);
    EXPECT_EQ(tokens[0].end, kSize);
    // The incoming scopes, not the scopes of any rule: nothing was matched.
    EXPECT_EQ(tokens[0].scopes, "source.len");
    EXPECT_EQ(h.last().tokens[0].scopes, start.contentScopes());
    EXPECT_TRUE(h.last().bailedOnLineLength);
    EXPECT_FALSE(h.last().hitIterationLimit);
    EXPECT_EQ(h.tokenizer().stats().linesBailedOnLength, 1u);
    EXPECT_EQ(h.tokenizer().stats().linesTokenized, 1u);
    expectCoversLine(h.last(), kSize);

    // The state passes through untouched apart from the first-line flag.
    EXPECT_TRUE(h.endState() == start.withFirstLine(false));
}

TEST(TokenizerLimits, LongLineInsideAnOpenRuleKeepsTheStack) {
    Harness h(kLengthGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens opener = h.tokenize("(", h.initialState());
    const Tokens expectedOpener = {{0u, 1u, "source.len m.p"}};
    EXPECT_EQ(opener, expectedOpener);
    const State inRule = h.endState();
    ASSERT_EQ(inRule.depth(), 2u);

    const std::string line(kMaxLineLength + 1u, 'b');
    const Tokens tokens = h.tokenize(line, inRule);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].end, line.size());
    EXPECT_EQ(tokens[0].scopes, "source.len m.p m.in");  // the content scopes
    EXPECT_TRUE(h.last().bailedOnLineLength);
    EXPECT_TRUE(h.endState() == inRule);  // still inside the rule, unchanged
    EXPECT_EQ(h.endState().depth(), 2u);
}

// ---------------------------------------------------------------------------
// stack depth
// ---------------------------------------------------------------------------

constexpr std::string_view kRecursiveGrammar = R"json({
  "scopeName": "source.d",
  "patterns": [
    { "begin": "\\(", "end": "\\)", "name": "p", "patterns": [ { "include": "$self" } ] }
  ]
})json";

TEST(TokenizerLimits, StackDepthOverflowDegradesToMatchRules) {
    Harness h(kRecursiveGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const size_t kOpens = 150u;
    ASSERT_GT(kOpens, kMaxStackDepth);
    const std::string line(kOpens, '(');
    const Tokens tokens = h.tokenize(line, h.initialState());

    // Every '(' still produces its own one-byte token with the rule's scope...
    ASSERT_EQ(tokens.size(), kOpens);
    expectCoversLine(h.last(), kOpens);
    for (size_t i = 0; i < kOpens; ++i) {
        EXPECT_EQ(tokens[i].begin, i);
        EXPECT_EQ(tokens[i].end, i + 1u);
    }

    // ...but the stack stops growing at the limit, and the surplus rules are
    // treated as plain match rules instead.
    EXPECT_TRUE(h.last().hitDepthLimit);
    EXPECT_EQ(h.endState().depth(), kMaxStackDepth);
    const size_t pushed = kMaxStackDepth - 1u;  // the root frame occupies one slot
    EXPECT_EQ(h.tokenizer().stats().depthLimitHits, kOpens - pushed);

    // Scope stacks grow with the pushes and then stay flat.
    const ide::syntax::ScopeStackTable& table = h.tokenizer().scopeTable();
    EXPECT_EQ(table.depth(h.last().tokens[0].scopes), 2u);
    EXPECT_EQ(table.depth(h.last().tokens[pushed - 1u].scopes), kMaxStackDepth);
    EXPECT_EQ(table.depth(h.last().tokens[pushed].scopes), kMaxStackDepth + 1u);
    EXPECT_EQ(table.depth(h.last().tokens[kOpens - 1u].scopes), kMaxStackDepth + 1u);
    EXPECT_EQ(h.last().tokens[pushed].scopes, h.last().tokens[kOpens - 1u].scopes);

    // The rest of the file still tokenizes: closing them all unwinds cleanly.
    const std::string closers(pushed, ')');
    const Tokens closed = h.tokenize(closers, h.endState());
    ASSERT_EQ(closed.size(), pushed);
    expectCoversLine(h.last(), closers.size());
    EXPECT_EQ(h.endState().depth(), 1u);
}

// ---------------------------------------------------------------------------
// fuzz-style structural invariants
// ---------------------------------------------------------------------------

constexpr std::string_view kFuzzGrammar = R"json({
  "scopeName": "source.fz",
  "patterns": [
    { "match": "(a)(b)?", "name": "m.ab",
      "captures": { "1": { "name": "c.one" }, "2": { "name": "c.two" } } },
    { "begin": "\\(", "end": "\\)", "name": "m.paren", "contentName": "m.inner",
      "patterns": [ { "include": "$self" } ] },
    { "begin": "\"", "end": "\"", "name": "m.str" },
    { "begin": "^>", "while": "^>", "name": "m.quote" },
    { "match": "x*", "name": "m.zero" }
  ]
})json";

TEST(TokenizerLimits, RandomLinesAlwaysProduceAValidTokenization) {
    Harness h(kFuzzGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    static const char kAlphabet[] = {'a', 'b', 'x', '(', ')', '"', '>', ' ', 'z', '\n'};
    constexpr size_t kAlphabetSize = sizeof(kAlphabet) / sizeof(kAlphabet[0]);

    Lcg rng(0x5EEDu);
    State state = h.initialState();
    for (int line = 0; line < 60; ++line) {
        std::string text;
        const size_t length = rng.below(20u);
        for (size_t i = 0; i < length; ++i) text.push_back(kAlphabet[rng.below(kAlphabetSize)]);

        const Tokens tokens = h.tokenize(text, state);
        const TokenizeResult first = h.last();
        expectCoversLine(first, text.size());
        ASSERT_FALSE(first.bailedOnLineLength);
        ASSERT_FALSE(first.hitIterationLimit) << "line " << line << ": " << text;
        ASSERT_LE(first.endState.depth(), kMaxStackDepth);

        // Tokenizing the same line from the same state twice must give exactly
        // the same answer: the per-line caches must not leak across lines.
        const Tokens again = h.tokenize(text, state);
        EXPECT_EQ(tokens, again) << "line " << line << ": " << text;
        EXPECT_TRUE(first.endState == h.last().endState) << "line " << line;

        state = first.endState;
    }
}

}  // namespace
