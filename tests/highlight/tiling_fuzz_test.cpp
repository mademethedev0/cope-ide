#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/highlight/fallback_lexer.h>
#include <ide/highlight/highlighter.h>

#include "highlight_test_util.h"

// THE most important test in this module.
//
// Every tier of the cascade, and above all the splice of tier 2 into tier 1,
// must produce spans that tile the line exactly: sorted, non-overlapping,
// gapless, covering [0, line.size()). The renderer indexes into the line with
// these offsets, so a gap is a missing glyph run and an overlap is a double
// paint. Hand written cases cannot cover the interaction of unbalanced quotes,
// unclosed comments, truncated UTF-8 and carried grammar state, so this is a
// seeded fuzz test over hundreds of generated lines.

namespace {

using ember_highlight_test::Config;
using ember_highlight_test::Harness;
using ember_highlight_test::Lcg;
using ember_highlight_test::tiles;
using ide::highlight::FallbackLexer;
using ide::highlight::FallbackSpan;
using ide::highlight::FallbackState;
using ide::highlight::LineState;
using ide::highlight::ScopedSpan;
using ide::highlight::StyledSpan;
using ide::highlight::Tier;

/// A grammar with enough shape to exercise the interesting paths: begin/end rules
/// that carry state across lines, match rules, and a meta-only rule whose scope
/// no theme colours - which is exactly the case the repair layer exists for.
constexpr std::string_view kFuzzGrammar = R"json({
  "scopeName": "source.fuzz",
  "fileTypes": ["c"],
  "patterns": [
    { "name": "comment.block.fuzz", "begin": "/\\*", "end": "\\*/" },
    { "name": "comment.line.fuzz", "match": "//.*" },
    { "name": "string.quoted.double.fuzz", "begin": "\"", "end": "\"" },
    { "name": "keyword.control.fuzz", "match": "\\b(if|else|while|return)\\b" },
    { "name": "constant.numeric.fuzz", "match": "\\b[0-9]+\\b" },
    { "name": "meta.parens.fuzz", "begin": "\\(", "end": "\\)" }
  ]
})json";

/// Fragments chosen to break a lexer: unbalanced delimiters, comment openers
/// with no closer, truncated UTF-8, digit separators, literal prefixes.
constexpr std::string_view kFragments[] = {
    "if",     "else",    "while",   "return", "x",      "foo",     "bar_baz", "Ident",
    " ",      "  ",      "\t",      "(",      ")",      "{",       "}",       "[",
    "]",      ";",       ",",       ".",      "\"",     "'",       "`",       "\\",
    "/*",     "*/",      "//",      "#",      "@",      "$",       "0",       "42",
    "0x1F",   "1_0",     "1.5e-3",  ".5",     "+",      "-",       "==",      "->",
    "::",     "?",       ":",       "é",      "€",      "\xC3",    "\x80",    "\"abc\"",
    "'c'",    "/* c */", "// tail", "\"\"\"", "'''",    "R\"(",    ")\"",     "<div>",
    "&amp;",  "u8\"",    "1'000",   "@Attr",  "[Attr]", "#define", "obj.f",   "p->q",
};

constexpr size_t kFragmentCount = std::size(kFragments);
constexpr size_t kIterations = 400;

[[nodiscard]] std::string generateLine(Lcg& rng) {
    const size_t count = rng.below(12u);
    std::string line;
    for (size_t i = 0; i < count; ++i) line += kFragments[rng.below(kFragmentCount)];
    return line;
}

/// Flattened scope stack of every byte, so two highlighters with independent
/// interning tables can be compared by text instead of by id.
[[nodiscard]] std::vector<std::string> scopesPerByte(Harness& harness,
                                                     const std::vector<ScopedSpan>& spans,
                                                     size_t length) {
    std::vector<std::string> out(length);
    for (const ScopedSpan& span : spans) {
        const std::string flat = harness.h().scopeTable().flatten(span.scopes);
        for (size_t i = span.begin; i < span.end && i < length; ++i) out[i] = flat;
    }
    return out;
}

[[nodiscard]] ::testing::AssertionResult stylesAreMerged(const std::vector<StyledSpan>& spans) {
    for (size_t i = 1; i < spans.size(); ++i) {
        if (spans[i - 1u].style == spans[i].style) {
            return ::testing::AssertionFailure()
                   << "spans " << (i - 1u) << " and " << i << " share style " << spans[i].style
                   << " and should have been merged";
        }
    }
    return ::testing::AssertionSuccess();
}

[[nodiscard]] Config grammarConfig() {
    Config config;
    config.grammarJson = kFuzzGrammar;
    config.fileName = "fuzz.c";
    return config;
}

// --- tier 1 ------------------------------------------------------------------

TEST(HighlightTilingFuzzTest, GrammarTierTilesEveryLine) {
    Config config = grammarConfig();
    config.limits.repairWithFallback = false;
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);

    Lcg rng(0xC0FFEEu);
    LineState state = harness.h().initialState();
    std::vector<ScopedSpan> scoped;
    std::vector<StyledSpan> styled;
    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::string line = generateLine(rng);
        harness.h().scopeLine(line, state, scoped);
        ASSERT_TRUE(tiles(scoped, line.size())) << "iteration " << iteration << ": \"" << line
                                                << '"';
        ASSERT_TRUE(ide::highlight::spansTile(std::span<const ScopedSpan>(scoped), line.size()));
        harness.h().styleSpans(scoped, styled);
        ASSERT_TRUE(tiles(styled, line.size())) << "iteration " << iteration << ": \"" << line
                                                << '"';
        ASSERT_TRUE(stylesAreMerged(styled)) << "iteration " << iteration;
    }
}

// --- tier 2 ------------------------------------------------------------------

TEST(HighlightTilingFuzzTest, FallbackTierTilesEveryLine) {
    Config config;
    config.fileName = "fuzz.c";  // no grammar registered: tier 2 by construction
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kFallback);
    ASSERT_EQ(harness.h().profile().name, "c-family");

    Lcg rng(0xC0FFEEu);
    LineState state = harness.h().initialState();
    std::vector<StyledSpan> styled;
    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::string line = generateLine(rng);
        harness.h().highlightLine(line, state, styled);
        ASSERT_TRUE(tiles(styled, line.size())) << "iteration " << iteration << ": \"" << line
                                                << '"';
        ASSERT_TRUE(stylesAreMerged(styled)) << "iteration " << iteration;
    }
}

// --- tier 3 ------------------------------------------------------------------

TEST(HighlightTilingFuzzTest, PlainTierTilesEveryLine) {
    Config config = grammarConfig();
    Harness harness(config);
    harness.h().forceTier(Tier::kPlain);
    ASSERT_EQ(harness.h().tier(), Tier::kPlain);

    Lcg rng(0xC0FFEEu);
    LineState state = harness.h().initialState();
    std::vector<ScopedSpan> scoped;
    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::string line = generateLine(rng);
        harness.h().scopeLine(line, state, scoped);
        ASSERT_TRUE(tiles(scoped, line.size())) << "iteration " << iteration;
        EXPECT_LE(scoped.size(), 1u);
    }
}

TEST(HighlightTilingFuzzTest, MixedGrammarAndOversizedLinesTile) {
    Config config = grammarConfig();
    config.limits.maxLineLength = 8;  // most generated lines fall through to tier 3
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);

    Lcg rng(0x5EEDu);
    LineState state = harness.h().initialState();
    std::vector<ScopedSpan> scoped;
    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::string line = generateLine(rng);
        harness.h().scopeLine(line, state, scoped);
        ASSERT_TRUE(tiles(scoped, line.size())) << "iteration " << iteration << ": \"" << line
                                                << '"';
    }
    EXPECT_GT(harness.h().stats().plainLines, 0u);
    EXPECT_GT(harness.h().stats().grammarLines, 0u);
}

// --- the repaired combination ------------------------------------------------

TEST(HighlightTilingFuzzTest, RepairedOutputTilesAndOnlyEverExtendsTheGrammar) {
    Config plainConfig = grammarConfig();
    plainConfig.limits.repairWithFallback = false;
    Harness grammarOnly(plainConfig);
    Harness repaired(grammarConfig());
    ASSERT_EQ(grammarOnly.h().tier(), Tier::kGrammar);
    ASSERT_EQ(repaired.h().tier(), Tier::kGrammar);

    Lcg rng(0xC0FFEEu);
    LineState grammarState = grammarOnly.h().initialState();
    LineState repairState = repaired.h().initialState();
    std::vector<ScopedSpan> grammarSpans;
    std::vector<ScopedSpan> repairedSpans;
    std::vector<StyledSpan> styled;

    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::string line = generateLine(rng);
        grammarOnly.h().scopeLine(line, grammarState, grammarSpans);
        repaired.h().scopeLine(line, repairState, repairedSpans);

        ASSERT_TRUE(tiles(grammarSpans, line.size())) << "iteration " << iteration;
        ASSERT_TRUE(tiles(repairedSpans, line.size())) << "iteration " << iteration << ": \""
                                                       << line << '"';
        repaired.h().styleSpans(repairedSpans, styled);
        ASSERT_TRUE(tiles(styled, line.size())) << "iteration " << iteration;
        ASSERT_TRUE(stylesAreMerged(styled)) << "iteration " << iteration;

        // The repair layer may only ever ADD scopes on top of what the grammar
        // produced: byte for byte, the repaired stack must have the grammar's
        // stack as a prefix. That is the formal statement of "the grammar always
        // wins where it produced something".
        const std::vector<std::string> before =
            scopesPerByte(grammarOnly, grammarSpans, line.size());
        const std::vector<std::string> after = scopesPerByte(repaired, repairedSpans, line.size());
        for (size_t i = 0; i < line.size(); ++i) {
            ASSERT_EQ(after[i].compare(0, before[i].size(), before[i]), 0)
                << "byte " << i << " of \"" << line << "\": grammar said \"" << before[i]
                << "\", repaired said \"" << after[i] << '"';
        }
    }
    // The corpus is full of unscoped words, so repair must actually have run.
    EXPECT_GT(repaired.h().stats().repairRegions, 0u);
    EXPECT_GT(repaired.h().stats().repairedBytes, 0u);
}

// --- the lexer on its own, for every profile ---------------------------------

TEST(HighlightTilingFuzzTest, FallbackLexerTilesEveryLineForEveryProfile) {
    for (const ide::highlight::LanguageProfile* profile : ide::highlight::allProfiles()) {
        ASSERT_NE(profile, nullptr);
        const FallbackLexer lexer(*profile);
        Lcg rng(0xABCDEFu);
        FallbackState state;
        std::vector<FallbackSpan> spans;
        for (size_t iteration = 0; iteration < kIterations; ++iteration) {
            const std::string line = generateLine(rng);
            lexer.lex(line, state, spans);
            ASSERT_TRUE(tiles(spans, line.size()))
                << "profile " << profile->name << ", iteration " << iteration << ": \"" << line
                << '"';
            ASSERT_TRUE(
                ide::highlight::spansTile(std::span<const FallbackSpan>(spans), line.size()));
        }
    }
}

/// Fragments are also fed one at a time from a clean state, which is the case
/// most likely to hit an off-by-one at position 0 or at the end of the input.
TEST(HighlightTilingFuzzTest, EveryFragmentAloneTilesForEveryProfile) {
    for (const ide::highlight::LanguageProfile* profile : ide::highlight::allProfiles()) {
        for (const std::string_view fragment : kFragments) {
            FallbackState state;
            const std::vector<FallbackSpan> spans =
                FallbackLexer(*profile).lexLine(fragment, state);
            EXPECT_TRUE(tiles(spans, fragment.size()))
                << "profile " << profile->name << ", fragment \"" << fragment << '"';
        }
    }
}

}  // namespace
