#include "syntax_test_util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ide/syntax/grammar.h>
#include <ide/syntax/std_regex_engine.h>
#include <ide/syntax/tokenizer.h>
#include <ide/syntax/trace.h>

namespace {

using ember_test::FlatToken;
using ember_test::Harness;
using ide::syntax::GrammarId;
using ide::syntax::GrammarRegistry;
using ide::syntax::kInvalidGrammarId;
using ide::syntax::State;
using ide::syntax::StdRegexEngine;
using ide::syntax::Tokenizer;
using ide::syntax::TokenizeResult;
using ide::syntax::TokenSpan;
using ide::syntax::TraceEvent;
using ide::syntax::TraceEventKind;
using ide::syntax::TraceSink;

using Tokens = std::vector<FlatToken>;

// ---------------------------------------------------------------------------
// match rules
// ---------------------------------------------------------------------------

constexpr std::string_view kMatchGrammar = R"json({
  "scopeName": "source.m",
  "patterns": [ { "match": "a+", "name": "keyword.a" } ]
})json";

TEST(TokenizerTest, MatchRuleSpansAndScopes) {
    Harness h(kMatchGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const State start = h.initialState();
    EXPECT_EQ(start.depth(), 1u);  // the root rule is always on the stack
    EXPECT_TRUE(start.isFirstLine());
    EXPECT_EQ(h.tokenizer().scopeTable().flatten(start.contentScopes()), "source.m");

    const Tokens tokens = h.tokenize("bab", start);
    const Tokens expected = {
        {0u, 1u, "source.m"},
        {1u, 2u, "source.m keyword.a"},
        {2u, 3u, "source.m"},
    };
    EXPECT_EQ(tokens, expected);

    // Raw spans, not just the flattened view.
    ASSERT_EQ(h.last().tokens.size(), 3u);
    EXPECT_EQ(h.last().tokens[1].begin, 1u);
    EXPECT_EQ(h.last().tokens[1].end, 2u);
    EXPECT_NE(h.last().tokens[1].scopes, h.last().tokens[0].scopes);
    EXPECT_EQ(h.last().tokens[0].scopes, h.last().tokens[2].scopes);

    // Nothing degraded.
    EXPECT_FALSE(h.last().bailedOnLineLength);
    EXPECT_FALSE(h.last().hitIterationLimit);
    EXPECT_FALSE(h.last().hitDepthLimit);
    EXPECT_EQ(h.last().forcedAdvances, 0u);

    // A match rule never changes the state, except for the first-line flag.
    EXPECT_EQ(h.endState().depth(), 1u);
    EXPECT_FALSE(h.endState().isFirstLine());
    EXPECT_TRUE(h.endState() == start.withFirstLine(false));
    EXPECT_EQ(h.tokenizer().stats().linesTokenized, 1u);
}

TEST(TokenizerTest, EmptyLineProducesNoTokens) {
    Harness h(kMatchGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();
    const State start = h.initialState();
    const Tokens tokens = h.tokenize("", start);
    EXPECT_TRUE(tokens.empty());
    EXPECT_EQ(h.endState().depth(), 1u);
    EXPECT_FALSE(h.endState().isFirstLine());
}

TEST(TokenizerTest, MultibyteTextIsMeasuredInBytes) {
    Harness h(kMatchGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();
    // "\xC3\xA9" is one codepoint, two bytes; offsets are byte offsets.
    const Tokens tokens = h.tokenize("\xC3\xA9" "aa\xE2\x82\xAC", h.initialState());
    const Tokens expected = {
        {0u, 2u, "source.m"},
        {2u, 4u, "source.m keyword.a"},
        {4u, 7u, "source.m"},
    };
    EXPECT_EQ(tokens, expected);
}

TEST(TokenizerTest, WithoutAGrammarEverythingIsOnePlainToken) {
    Harness h("{not a grammar");
    ASSERT_FALSE(h.loaded());
    EXPECT_EQ(h.gid(), kInvalidGrammarId);

    const State start = h.initialState();
    EXPECT_TRUE(start.empty());
    const Tokens tokens = h.tokenize("abc", start);
    const Tokens expected = {{0u, 3u, ""}};
    EXPECT_EQ(tokens, expected);
    EXPECT_TRUE(h.endState().empty());
}

// ---------------------------------------------------------------------------
// begin/end rules
// ---------------------------------------------------------------------------

constexpr std::string_view kStringGrammar = R"json({
  "scopeName": "source.ml",
  "patterns": [
    { "begin": "\"", "end": "\"", "name": "str", "contentName": "str.body" }
  ]
})json";

TEST(TokenizerTest, BeginEndSpansThreeLines) {
    Harness h(kStringGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const State start = h.initialState();
    const Tokens line1 = h.tokenize("x \"ab", start);
    const Tokens expected1 = {
        {0u, 2u, "source.ml"},
        {2u, 3u, "source.ml str"},        // the begin match itself
        {3u, 5u, "source.ml str str.body"},  // content carries contentName
    };
    EXPECT_EQ(line1, expected1);
    const State afterLine1 = h.endState();
    EXPECT_EQ(afterLine1.depth(), 2u);
    EXPECT_FALSE(afterLine1.isFirstLine());
    ASSERT_NE(afterLine1.top(), nullptr);
    EXPECT_FALSE(afterLine1.top()->endPattern.has_value());  // no backrefs to fill in
    EXPECT_FALSE(afterLine1.top()->beginCapturedEol);

    const Tokens line2 = h.tokenize("cd", afterLine1);
    const Tokens expected2 = {{0u, 2u, "source.ml str str.body"}};
    EXPECT_EQ(line2, expected2);
    const State afterLine2 = h.endState();
    EXPECT_EQ(afterLine2.depth(), 2u);
    EXPECT_TRUE(afterLine2 == afterLine1);  // an unchanged state stops rescanning

    const Tokens line3 = h.tokenize("ef\"g", afterLine2);
    const Tokens expected3 = {
        {0u, 2u, "source.ml str str.body"},
        {2u, 3u, "source.ml str"},  // the end match leaves the content scope
        {3u, 4u, "source.ml"},
    };
    EXPECT_EQ(line3, expected3);
    EXPECT_EQ(h.endState().depth(), 1u);
}

constexpr std::string_view kNestedGrammar = R"json({
  "scopeName": "source.n",
  "patterns": [
    { "begin": "\\(", "end": "\\)", "name": "meta.paren", "contentName": "meta.inner",
      "patterns": [ { "begin": "\\[", "end": "\\]", "name": "meta.brack" } ] }
  ]
})json";

TEST(TokenizerTest, NestedBeginEndRules) {
    Harness h(kNestedGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("(a[b]c)", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.n meta.paren"},
        {1u, 2u, "source.n meta.paren meta.inner"},
        {2u, 3u, "source.n meta.paren meta.inner meta.brack"},
        {3u, 4u, "source.n meta.paren meta.inner meta.brack"},
        {4u, 5u, "source.n meta.paren meta.inner meta.brack"},
        {5u, 6u, "source.n meta.paren meta.inner"},
        {6u, 7u, "source.n meta.paren"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 1u);  // both rules closed on this line
}

TEST(TokenizerTest, NestedBeginEndAcrossLines) {
    Harness h(kNestedGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens line1 = h.tokenize("([", h.initialState());
    const Tokens expected1 = {
        {0u, 1u, "source.n meta.paren"},
        {1u, 2u, "source.n meta.paren meta.inner meta.brack"},
    };
    EXPECT_EQ(line1, expected1);
    const State inner = h.endState();
    EXPECT_EQ(inner.depth(), 3u);
    ASSERT_NE(inner.frameAt(0), nullptr);
    ASSERT_NE(inner.frameAt(1), nullptr);
    ASSERT_NE(inner.frameAt(2), nullptr);
    EXPECT_TRUE(inner.top()->beginCapturedEol);  // the begin match ended the line

    const Tokens line2 = h.tokenize("])", inner);
    const Tokens expected2 = {
        {0u, 1u, "source.n meta.paren meta.inner meta.brack"},
        {1u, 2u, "source.n meta.paren"},
    };
    EXPECT_EQ(line2, expected2);
    EXPECT_EQ(h.endState().depth(), 1u);
}

// ---------------------------------------------------------------------------
// captures
// ---------------------------------------------------------------------------

constexpr std::string_view kCaptureGrammar = R"json({
  "scopeName": "source.cap",
  "patterns": [
    { "match": "(a+)-(b+)", "name": "meta.m",
      "captures": { "1": { "name": "c.one" }, "2": { "name": "c.two" } } },
    { "match": "(k)", "name": "kw.$1" }
  ]
})json";

TEST(TokenizerTest, MatchCapturesDecorateSubRanges) {
    Harness h(kCaptureGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("aa-bb", h.initialState());
    const Tokens expected = {
        {0u, 2u, "source.cap meta.m c.one"},
        {2u, 3u, "source.cap meta.m"},  // the gap between the two captures
        {3u, 5u, "source.cap meta.m c.two"},
    };
    EXPECT_EQ(tokens, expected);
}

TEST(TokenizerTest, ScopeNameCaptureSubstitution) {
    Harness h(kCaptureGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();
    const Tokens tokens = h.tokenize("k", h.initialState());
    const Tokens expected = {{0u, 1u, "source.cap kw.k"}};
    EXPECT_EQ(tokens, expected);
}

constexpr std::string_view kTagGrammar = R"json({
  "scopeName": "source.tag",
  "patterns": [
    { "begin": "(<)(tag)", "end": "(>)", "name": "meta.tag", "contentName": "meta.inside",
      "beginCaptures": { "1": { "name": "punct.open" }, "2": { "name": "entity.name" } },
      "endCaptures": { "1": { "name": "punct.close" } } }
  ]
})json";

TEST(TokenizerTest, BeginAndEndCaptures) {
    Harness h(kTagGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("<tag a>", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.tag meta.tag punct.open"},
        {1u, 4u, "source.tag meta.tag entity.name"},
        {4u, 6u, "source.tag meta.tag meta.inside"},
        {6u, 7u, "source.tag meta.tag punct.close"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 1u);
}

constexpr std::string_view kSubCaptureGrammar = R"json({
  "scopeName": "source.sub",
  "patterns": [
    { "match": "(a+)-(b+)", "name": "meta.m",
      "captures": {
        "1": { "name": "g.one", "patterns": [ { "match": "a", "name": "single.a" } ] }
      } }
  ]
})json";

TEST(TokenizerTest, CaptureWithItsOwnPatternsIsRetokenized) {
    Harness h(kSubCaptureGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("aa-bb", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.sub meta.m g.one single.a"},
        {1u, 2u, "source.sub meta.m g.one single.a"},
        {2u, 5u, "source.sub meta.m"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 1u);  // the capture stack does not leak out
}

// ---------------------------------------------------------------------------
// applyEndPatternLast
// ---------------------------------------------------------------------------

constexpr std::string_view kEndFirstGrammar = R"json({
  "scopeName": "source.ep",
  "patterns": [
    { "begin": "B", "end": "-", "name": "meta.b",
      "patterns": [ { "match": "-x", "name": "dash.x" } ] }
  ]
})json";

constexpr std::string_view kEndLastGrammar = R"json({
  "scopeName": "source.ep",
  "patterns": [
    { "begin": "B", "end": "-", "name": "meta.b", "applyEndPatternLast": true,
      "patterns": [ { "match": "-x", "name": "dash.x" } ] }
  ]
})json";

TEST(TokenizerTest, EndPatternWinsByDefault) {
    Harness h(kEndFirstGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("B-xC", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.ep meta.b"},  // begin
        {1u, 2u, "source.ep meta.b"},  // end wins over the sub-pattern at "-x"
        {2u, 4u, "source.ep"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 1u);
}

TEST(TokenizerTest, ApplyEndPatternLastLetsSubPatternsWin) {
    Harness h(kEndLastGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("B-xC", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.ep meta.b"},
        {1u, 3u, "source.ep meta.b dash.x"},  // sub-pattern consumed the end text
        {3u, 4u, "source.ep meta.b"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 2u);  // still inside the rule
}

// ---------------------------------------------------------------------------
// while rules
// ---------------------------------------------------------------------------

constexpr std::string_view kQuoteGrammar = R"json({
  "scopeName": "source.q",
  "patterns": [
    { "begin": "^> ", "while": "^> ", "name": "markup.quote", "contentName": "meta.q" }
  ]
})json";

TEST(TokenizerTest, WhileRuleContinuesUntilItFails) {
    Harness h(kQuoteGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens line1 = h.tokenize("> a", h.initialState());
    const Tokens expected1 = {
        {0u, 2u, "source.q markup.quote"},
        {2u, 3u, "source.q markup.quote meta.q"},
    };
    EXPECT_EQ(line1, expected1);
    const State inQuote = h.endState();
    EXPECT_EQ(inQuote.depth(), 2u);

    // The while match is consumed with the content scopes.
    const Tokens line2 = h.tokenize("> b", inQuote);
    const Tokens expected2 = {
        {0u, 2u, "source.q markup.quote meta.q"},
        {2u, 3u, "source.q markup.quote meta.q"},
    };
    EXPECT_EQ(line2, expected2);
    EXPECT_EQ(h.endState().depth(), 2u);
    EXPECT_TRUE(h.endState() == inQuote);

    // A line that fails the while condition pops the frame before scanning.
    const Tokens line3 = h.tokenize("c", h.endState());
    const Tokens expected3 = {{0u, 1u, "source.q"}};
    EXPECT_EQ(line3, expected3);
    EXPECT_EQ(h.endState().depth(), 1u);
}

constexpr std::string_view kWhileBackrefGrammar = R"json({
  "scopeName": "source.qb",
  "patterns": [
    { "begin": "^(\\w+)>", "while": "^\\1>", "name": "m.w", "contentName": "m.body" }
  ]
})json";

TEST(TokenizerTest, WhilePatternBackreferencesAreSubstitutedFromTheBeginMatch) {
    Harness h(kWhileBackrefGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens line1 = h.tokenize("ab>x", h.initialState());
    const Tokens expected1 = {
        {0u, 3u, "source.qb m.w"},
        {3u, 4u, "source.qb m.w m.body"},
    };
    EXPECT_EQ(line1, expected1);
    const State inBlock = h.endState();
    ASSERT_NE(inBlock.top(), nullptr);
    ASSERT_TRUE(inBlock.top()->whilePattern.has_value());
    EXPECT_EQ(*inBlock.top()->whilePattern, "^ab>");
    EXPECT_FALSE(inBlock.top()->endPattern.has_value());

    // The same prefix continues the block.
    const Tokens line2 = h.tokenize("ab>y", inBlock);
    const Tokens expected2 = {
        {0u, 3u, "source.qb m.w m.body"},
        {3u, 4u, "source.qb m.w m.body"},
    };
    EXPECT_EQ(line2, expected2);
    EXPECT_EQ(h.endState().depth(), 2u);

    // A line without the captured prefix ends it.
    const Tokens line3 = h.tokenize("done", h.endState());
    const Tokens expected3 = {{0u, 4u, "source.qb"}};
    EXPECT_EQ(line3, expected3);
    EXPECT_EQ(h.endState().depth(), 1u);
}

// ---------------------------------------------------------------------------
// end pattern backreferences
// ---------------------------------------------------------------------------

constexpr std::string_view kHeredocGrammar = R"json({
  "scopeName": "source.hd",
  "patterns": [
    { "begin": "<<(\\w+)", "end": "\\1", "name": "string.heredoc", "contentName": "hd.body" }
  ]
})json";

TEST(TokenizerTest, EndPatternBackreferencesAreSubstitutedFromTheBeginMatch) {
    Harness h(kHeredocGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    // The captured terminator closes the rule.
    const Tokens closed = h.tokenize("<<AB;AB", h.initialState());
    const Tokens expectedClosed = {
        {0u, 4u, "source.hd string.heredoc"},
        {4u, 5u, "source.hd string.heredoc hd.body"},
        {5u, 7u, "source.hd string.heredoc"},
    };
    EXPECT_EQ(closed, expectedClosed);
    EXPECT_EQ(h.endState().depth(), 1u);

    // A different terminator does not: the substituted pattern is remembered in
    // the state so the next line keeps looking for "AB".
    const Tokens open = h.tokenize("<<AB;CD", h.initialState());
    const Tokens expectedOpen = {
        {0u, 4u, "source.hd string.heredoc"},
        {4u, 7u, "source.hd string.heredoc hd.body"},
    };
    EXPECT_EQ(open, expectedOpen);
    const State inHeredoc = h.endState();
    EXPECT_EQ(inHeredoc.depth(), 2u);
    ASSERT_NE(inHeredoc.top(), nullptr);
    ASSERT_TRUE(inHeredoc.top()->endPattern.has_value());
    EXPECT_EQ(*inHeredoc.top()->endPattern, "AB");

    const Tokens next = h.tokenize("zAB", inHeredoc);
    const Tokens expectedNext = {
        {0u, 1u, "source.hd string.heredoc hd.body"},
        {1u, 3u, "source.hd string.heredoc"},
    };
    EXPECT_EQ(next, expectedNext);
    EXPECT_EQ(h.endState().depth(), 1u);
}

TEST(TokenizerTest, BeginMatchTextIsEscapedBeforeItBecomesAnEndPattern) {
    // The captured terminator contains a regex metacharacter. Escaped, "a."
    // matches only the literal two bytes; unescaped it would match "ay" first
    // and the token boundaries would differ.
    Harness h(R"json({
      "scopeName": "source.esc",
      "patterns": [
        { "begin": "<<(.\\.)", "end": "\\1", "name": "lit", "contentName": "lit.body" }
      ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("<<a.zaya.", h.initialState());
    const Tokens expected = {
        {0u, 4u, "source.esc lit"},           // "<<a."
        {4u, 7u, "source.esc lit lit.body"},  // "zay" - "ay" must not terminate
        {7u, 9u, "source.esc lit"},           // the literal "a."
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 1u);
}

// ---------------------------------------------------------------------------
// anchors
// ---------------------------------------------------------------------------

constexpr std::string_view kFirstLineGrammar = R"json({
  "scopeName": "source.fl",
  "patterns": [
    { "match": "\\Afoo", "name": "first.line" },
    { "match": "foo", "name": "any.foo" }
  ]
})json";

TEST(TokenizerTest, AnchorAOnlyMatchesOnTheFirstLine) {
    Harness h(kFirstLineGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens line1 = h.tokenize("foo", h.initialState());
    const Tokens expected1 = {{0u, 3u, "source.fl first.line"}};
    EXPECT_EQ(line1, expected1);

    const Tokens line2 = h.tokenize("foo", h.endState());
    const Tokens expected2 = {{0u, 3u, "source.fl any.foo"}};
    EXPECT_EQ(line2, expected2);
}

constexpr std::string_view kAnchorGGrammar = R"json({
  "scopeName": "source.g",
  "patterns": [
    { "begin": "\\(", "end": "\\)", "name": "m.p",
      "patterns": [ { "match": "\\G[a-z]+", "name": "first.word" } ] }
  ]
})json";

TEST(TokenizerTest, AnchorGOnlyMatchesAtTheAnchorPosition) {
    Harness h(kAnchorGGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("(ab cd)", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.g m.p"},
        {1u, 3u, "source.g m.p first.word"},  // \G holds right after the begin match
        {3u, 6u, "source.g m.p"},             // " cd" is no longer at the anchor
        {6u, 7u, "source.g m.p"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 1u);
}

// ---------------------------------------------------------------------------
// includes
// ---------------------------------------------------------------------------

TEST(TokenizerTest, SelfIncludeCycleTerminates) {
    Harness h(R"json({
      "scopeName": "source.c1",
      "patterns": [ { "include": "$self" }, { "match": "q", "name": "hit.q" } ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("zqz", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.c1"},
        {1u, 2u, "source.c1 hit.q"},
        {2u, 3u, "source.c1"},
    };
    EXPECT_EQ(tokens, expected);
}

TEST(TokenizerTest, RepositoryIncludeCycleTerminates) {
    Harness h(R"json({
      "scopeName": "source.cyc",
      "patterns": [ { "include": "#a" } ],
      "repository": {
        "a": { "patterns": [ { "include": "#b" }, { "match": "x", "name": "hit.x" } ] },
        "b": { "patterns": [ { "include": "#a" } ] }
      }
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("zxz", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.cyc"},
        {1u, 2u, "source.cyc hit.x"},
        {2u, 3u, "source.cyc"},
    };
    EXPECT_EQ(tokens, expected);
}

TEST(TokenizerTest, UnresolvedExternalIncludeIsSkipped) {
    Harness h(R"json({
      "scopeName": "source.ext",
      "patterns": [ { "include": "source.nowhere" }, { "match": "e", "name": "hit.e" } ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("ze", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.ext"},
        {1u, 2u, "source.ext hit.e"},
    };
    EXPECT_EQ(tokens, expected);
    ASSERT_EQ(h.registry().missingScopes().size(), 1u);
    EXPECT_EQ(h.registry().missingScopes()[0], "source.nowhere");
    ASSERT_NE(h.grammar(), nullptr);
    ASSERT_EQ(h.grammar()->externalScopeRefs().size(), 1u);
    EXPECT_EQ(h.grammar()->externalScopeRefs()[0], "source.nowhere");
}

TEST(TokenizerTest, ExternalRuleFromAnotherGrammarApplies) {
    GrammarRegistry registry;
    std::string error;
    const GrammarId a = registry.addGrammarJson(R"json({
      "scopeName": "source.a",
      "repository": { "kw": { "match": "KA", "name": "keyword.a" } }
    })json",
                                                &error);
    ASSERT_NE(a, kInvalidGrammarId) << error;
    const GrammarId b = registry.addGrammarJson(R"json({
      "scopeName": "source.b",
      "patterns": [ { "include": "source.a#kw" } ]
    })json",
                                                &error);
    ASSERT_NE(b, kInvalidGrammarId) << error;

    StdRegexEngine engine;
    Tokenizer tokenizer(registry, engine, b);
    const TokenizeResult result = tokenizer.tokenizeLine("xKAy", tokenizer.initialState());
    ASSERT_EQ(result.tokens.size(), 3u);
    EXPECT_EQ(tokenizer.scopeTable().flatten(result.tokens[0].scopes), "source.b");
    EXPECT_EQ(result.tokens[0].begin, 0u);
    EXPECT_EQ(result.tokens[0].end, 1u);
    EXPECT_EQ(tokenizer.scopeTable().flatten(result.tokens[1].scopes), "source.b keyword.a");
    EXPECT_EQ(result.tokens[1].begin, 1u);
    EXPECT_EQ(result.tokens[1].end, 3u);
    EXPECT_EQ(tokenizer.scopeTable().flatten(result.tokens[2].scopes), "source.b");
    EXPECT_EQ(result.tokens[2].begin, 3u);
    EXPECT_EQ(result.tokens[2].end, 4u);
    EXPECT_TRUE(registry.missingScopes().empty());
}

// ---------------------------------------------------------------------------
// degradation
// ---------------------------------------------------------------------------

TEST(TokenizerTest, RuleWithAnUncompilableEndPatternIsNotEntered) {
    Harness h(R"json({
      "scopeName": "source.bad",
      "patterns": [ { "begin": "B", "end": "\\K", "name": "bad" } ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    // "\K" is refused by the engine, so the rule degrades to a match rule
    // instead of swallowing the rest of the file.
    const Tokens tokens = h.tokenize("aBc", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.bad"},
        {1u, 2u, "source.bad bad"},
        {2u, 3u, "source.bad"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.endState().depth(), 1u);
    EXPECT_EQ(h.tokenizer().stats().regexCompileFailures, 1u);
    EXPECT_EQ(h.tokenizer().stats().rulesDisabledByEndPattern, 1u);
    EXPECT_FALSE(h.engine().errorFor("\\K").empty());
}

TEST(TokenizerTest, UncompilableMatchPatternIsIgnored) {
    Harness h(R"json({
      "scopeName": "source.badm",
      "patterns": [
        { "match": "(?<=abc)x", "name": "unsupported" },
        { "match": "x", "name": "plain.x" }
      ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    const Tokens tokens = h.tokenize("zx", h.initialState());
    const Tokens expected = {
        {0u, 1u, "source.badm"},
        {1u, 2u, "source.badm plain.x"},
    };
    EXPECT_EQ(tokens, expected);
    EXPECT_EQ(h.tokenizer().stats().regexCompileFailures, 1u);
}

// ---------------------------------------------------------------------------
// statistics and tracing
// ---------------------------------------------------------------------------

TEST(TokenizerTest, StatsAccumulateAndReset) {
    Harness h(kMatchGrammar);
    ASSERT_TRUE(h.loaded()) << h.error();
    State state = h.initialState();
    for (int i = 0; i < 3; ++i) {
        h.tokenize("aab", state);
        state = h.endState();
    }
    EXPECT_EQ(h.tokenizer().stats().linesTokenized, 3u);
    EXPECT_EQ(h.tokenizer().stats().linesBailedOnLength, 0u);
    EXPECT_GT(h.tokenizer().stats().matchCacheMisses, 0u);
    h.tokenizer().resetStats();
    EXPECT_EQ(h.tokenizer().stats().linesTokenized, 0u);
    EXPECT_EQ(h.tokenizer().stats().matchCacheMisses, 0u);
}

TEST(TokenizerTest, TraceSinkSeesEveryDecision) {
    Harness h(R"json({
      "scopeName": "source.tr",
      "patterns": [ { "begin": "\\(", "end": "\\)", "name": "p" } ]
    })json");
    ASSERT_TRUE(h.loaded()) << h.error();

    std::vector<TraceEventKind> kinds;
    std::vector<std::string> details;
    h.tokenizer().setTrace([&kinds, &details](const TraceEvent& event) {
        kinds.push_back(event.kind);
        details.emplace_back(event.detail);
    });

    h.tokenize("(x)", h.initialState());

    const auto seen = [&kinds](TraceEventKind kind) {
        for (const TraceEventKind k : kinds) {
            if (k == kind) return true;
        }
        return false;
    };
    EXPECT_TRUE(seen(TraceEventKind::LineStart));
    EXPECT_TRUE(seen(TraceEventKind::RuleTried));
    EXPECT_TRUE(seen(TraceEventKind::RuleWon));
    EXPECT_TRUE(seen(TraceEventKind::Push));
    EXPECT_TRUE(seen(TraceEventKind::Pop));
    EXPECT_TRUE(seen(TraceEventKind::NoMatch));

    bool sawDebugName = false;
    for (const std::string& detail : details) {
        if (detail == "$self.patterns[0]") sawDebugName = true;
    }
    EXPECT_TRUE(sawDebugName);

    EXPECT_EQ(std::string_view(ide::syntax::traceEventKindName(TraceEventKind::Push)), "push");
    EXPECT_EQ(std::string_view(ide::syntax::traceEventKindName(TraceEventKind::Limit)), "limit");

    // Removing the sink silences it completely.
    const size_t before = kinds.size();
    EXPECT_GT(before, 0u);
    h.tokenizer().setTrace(TraceSink{});
    h.tokenize("(y)", h.initialState());
    EXPECT_EQ(kinds.size(), before);
}

}  // namespace
