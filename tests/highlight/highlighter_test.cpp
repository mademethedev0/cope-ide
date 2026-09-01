#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/highlight/highlighter.h>
#include <ide/theme/theme.h>

#include "highlight_test_util.h"

namespace {

using ember_highlight_test::Config;
using ember_highlight_test::Harness;
using ember_highlight_test::tiles;
using ide::highlight::BatchResult;
using ide::highlight::CarryMode;
using ide::highlight::LineState;
using ide::highlight::ScopedSpan;
using ide::highlight::StyledSpan;
using ide::highlight::Tier;
using ide::theme::Style;
using ide::theme::Theme;

constexpr std::string_view kSmallGrammar = R"json({
  "scopeName": "source.small",
  "fileTypes": ["c"],
  "patterns": [
    { "name": "storage.type.small", "match": "\\bint\\b" },
    { "name": "string.quoted.double.small", "begin": "\"", "end": "\"" }
  ]
})json";

/// Scopes essentially nothing: the case probe() exists to catch.
constexpr std::string_view kUselessGrammar = R"json({
  "scopeName": "source.useless",
  "fileTypes": ["c"],
  "patterns": [
    { "name": "keyword.control.z", "match": "\\bzzz\\b" }
  ]
})json";

/// Two different scopes, one colour: the only way to get two adjacent spans with
/// an identical StyleId, which is what span merging must collapse.
constexpr std::string_view kOneColourThemeJson = R"json({
  "name": "one-colour",
  "colors": { "editor.foreground": "#C0C0C0" },
  "tokenColors": [
    { "scope": "keyword.control", "settings": { "foreground": "#FF0000" } },
    { "scope": "punctuation", "settings": { "foreground": "#FF0000" } }
  ]
})json";

[[nodiscard]] Theme loadTheme(std::string_view json) {
    std::string error;
    std::optional<Theme> theme = Theme::fromJsonText(json, &error);
    EXPECT_TRUE(theme.has_value()) << error;
    return theme.value_or(Theme{});
}

[[nodiscard]] Config withGrammar() {
    Config config;
    config.grammarJson = kSmallGrammar;
    config.fileName = "a.c";
    return config;
}

// --- tier selection ----------------------------------------------------------

TEST(HighlighterTierTest, AGrammarThatResolvesSelectsTierOne) {
    Harness harness(withGrammar());
    EXPECT_EQ(harness.h().tier(), Tier::kGrammar);
    EXPECT_TRUE(harness.h().hasGrammar());
    EXPECT_EQ(harness.h().grammarScope(), "source.small");
}

TEST(HighlighterTierTest, NoGrammarSelectsTierTwo) {
    Config config;
    config.fileName = "a.c";
    Harness harness(config);
    EXPECT_EQ(harness.h().tier(), Tier::kFallback);
    EXPECT_FALSE(harness.h().hasGrammar());
    EXPECT_EQ(harness.h().grammarScope(), "");
    EXPECT_EQ(harness.h().profile().name, "c-family");
}

TEST(HighlighterTierTest, AnUnknownExtensionStillGetsAUsableProfile) {
    Config config;
    config.fileName = "notes.whatever";
    Harness harness(config);
    EXPECT_EQ(harness.h().tier(), Tier::kFallback);
    EXPECT_EQ(harness.h().profile().name, "generic");
}

TEST(HighlighterTierTest, AnOversizedFileSelectsTierThree) {
    Config config = withGrammar();
    config.limits.maxFileBytes = 100;
    config.byteSize = 4096;
    Harness harness(config);
    EXPECT_EQ(harness.h().tier(), Tier::kPlain);

    Config byLines = withGrammar();
    byLines.limits.maxFileLines = 10;
    byLines.lineCount = 5000;
    Harness lineHarness(byLines);
    EXPECT_EQ(lineHarness.h().tier(), Tier::kPlain);
}

TEST(HighlighterTierTest, AnOversizedLineFallsToOnePlainSpan) {
    Config config = withGrammar();
    config.limits.maxLineLength = 10;
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);

    const std::string_view shortLine = "int x;";
    const std::string longLine(40, 'y');
    LineState state = harness.h().initialState();
    std::vector<ScopedSpan> spans;

    harness.h().scopeLine(shortLine, state, spans);
    EXPECT_GT(spans.size(), 1u);
    ASSERT_TRUE(tiles(spans, shortLine.size()));

    harness.h().scopeLine(longLine, state, spans);
    ASSERT_TRUE(tiles(spans, longLine.size()));
    EXPECT_EQ(spans.size(), 1u);
    EXPECT_EQ(harness.h().stats().plainLines, 1u);
}

TEST(HighlighterTierTest, AnOversizedLineKeepsTheIncomingScopes) {
    Config config = withGrammar();
    config.limits.maxLineLength = 12;
    Harness harness(config);

    LineState state = harness.h().initialState();
    std::vector<ScopedSpan> spans;
    harness.h().scopeLine("s = \"open", state, spans);  // opens a grammar string
    ASSERT_TRUE(tiles(spans, 9u));

    const std::string longLine(30, 'z');
    harness.h().scopeLine(longLine, state, spans);
    ASSERT_EQ(spans.size(), 1u);
    // Still inside the string, so the line paints as a string instead of
    // flashing to plain text.
    EXPECT_EQ(harness.flatten(spans).front().scopes,
              "source.small string.quoted.double.small");
}

TEST(HighlighterTierTest, ForceTierOverridesTheSelection) {
    Harness harness(withGrammar());
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);
    harness.h().forceTier(Tier::kPlain);
    EXPECT_EQ(harness.h().tier(), Tier::kPlain);
    harness.h().forceTier(Tier::kGrammar);
    EXPECT_EQ(harness.h().tier(), Tier::kGrammar);
}

TEST(HighlighterTierTest, ForceTierCannotInventAGrammar) {
    Config config;
    config.fileName = "a.c";
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kFallback);
    harness.h().forceTier(Tier::kGrammar);
    EXPECT_EQ(harness.h().tier(), Tier::kFallback);
}

// --- probe -------------------------------------------------------------------

TEST(HighlighterProbeTest, AUselessGrammarIsDemotedToTierTwo) {
    Config config;
    config.grammarJson = kUselessGrammar;
    config.fileName = "a.c";
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);

    const std::vector<std::string_view> lines(30, "int x = 1;");
    EXPECT_EQ(harness.h().probe(std::span<const std::string_view>(lines)), Tier::kFallback);
    EXPECT_EQ(harness.h().tier(), Tier::kFallback);
}

TEST(HighlighterProbeTest, AUsefulGrammarSurvivesTheProbe) {
    Harness harness(withGrammar());
    const std::vector<std::string_view> lines(30, "int x = 1;");
    EXPECT_EQ(harness.h().probe(std::span<const std::string_view>(lines)), Tier::kGrammar);
}

TEST(HighlighterProbeTest, ProbeOnAnEmptyDocumentChangesNothing) {
    Harness harness(withGrammar());
    const std::vector<std::string_view> lines;
    EXPECT_EQ(harness.h().probe(std::span<const std::string_view>(lines)), Tier::kGrammar);
    EXPECT_EQ(harness.h().stats().lines, 0u);
}

// --- span merging ------------------------------------------------------------

TEST(HighlighterMergeTest, AdjacentIdenticalStylesAreMerged) {
    Config config;
    config.fileName = "a.c";  // tier 2
    config.themeJson = kOneColourThemeJson;
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kFallback);

    const std::string_view line = "if(";
    const std::vector<ScopedSpan> scoped = harness.scopeFirstLine(line);
    ASSERT_EQ(scoped.size(), 2u);  // keyword.control then punctuation

    std::vector<StyledSpan> styled;
    harness.h().styleSpans(scoped, styled);
    ASSERT_TRUE(tiles(styled, line.size()));
    ASSERT_EQ(styled.size(), 1u) << "two scopes sharing one colour must merge";
    EXPECT_EQ(styled[0].begin, 0u);
    EXPECT_EQ(styled[0].end, line.size());
}

TEST(HighlighterMergeTest, DifferentStylesAreNotMerged) {
    Config config;
    config.fileName = "a.c";  // tier 2, distinct colour per scope
    Harness harness(config);

    const std::string_view line = "if(";
    const std::vector<ScopedSpan> scoped = harness.scopeFirstLine(line);
    ASSERT_EQ(scoped.size(), 2u);
    std::vector<StyledSpan> styled;
    harness.h().styleSpans(scoped, styled);
    ASSERT_TRUE(tiles(styled, line.size()));
    ASSERT_EQ(styled.size(), 2u);
    EXPECT_NE(styled[0].style, styled[1].style);
}

TEST(HighlighterMergeTest, UnscopedRunsCollapseIntoTheDefaultStyle) {
    Config config;
    config.fileName = "notes.whatever";  // generic profile, tier 2
    Harness harness(config);
    // Prose-like text with no keywords: identifiers and blanks alternate, so the
    // styled output must still be short.
    const std::string_view line = "aaa bbb ccc";
    std::vector<StyledSpan> styled;
    LineState state = harness.h().initialState();
    harness.h().highlightLine(line, state, styled);
    ASSERT_TRUE(tiles(styled, line.size()));
    for (size_t i = 1; i < styled.size(); ++i) {
        EXPECT_NE(styled[i - 1u].style, styled[i].style);
    }
}

// --- theme swapping ----------------------------------------------------------

TEST(HighlighterThemeTest, SwappingTheThemeRestylesWithoutRetokenizing) {
    Config config;
    config.fileName = "a.c";  // tier 2
    Harness harness(config);

    const std::string_view line = "if x";
    const std::vector<ScopedSpan> scoped = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(scoped, line.size()));
    const size_t linesBefore = harness.h().stats().lines;

    std::vector<StyledSpan> before;
    harness.h().styleSpans(scoped, before);
    ASSERT_FALSE(before.empty());
    const Style beforeStyle = harness.h().theme().styleAt(before.front().style);

    Theme alternative = loadTheme(ember_highlight_test::kAltThemeJson);
    harness.h().setTheme(alternative);

    std::vector<StyledSpan> after;
    harness.h().styleSpans(scoped, after);
    const Style afterStyle = harness.h().theme().styleAt(after.front().style);

    EXPECT_EQ(before.size(), after.size());
    EXPECT_NE(beforeStyle.fg, afterStyle.fg);
    // Restyling touched no line: it is a re-resolve, not a retokenization.
    EXPECT_EQ(harness.h().stats().lines, linesBefore);
}

TEST(HighlighterThemeTest, DefaultStyleAndIdAreExposed) {
    Harness harness(withGrammar());
    EXPECT_EQ(ide::highlight::Highlighter::defaultStyleId(), ide::theme::kDefaultStyleId);
    const Style fallbackStyle = harness.h().defaultStyle();
    EXPECT_TRUE(fallbackStyle.hasFg);
    EXPECT_EQ(fallbackStyle.fg, harness.h().theme().defaultStyle().fg);
}

TEST(HighlighterThemeTest, StyleIdsAreStablePerScopeStack) {
    Harness harness(withGrammar());
    const std::string_view line = "int x;";
    const std::vector<ScopedSpan> scoped = harness.scopeFirstLine(line);
    ASSERT_GE(scoped.size(), 2u);
    const ide::theme::StyleId first = harness.h().styleFor(scoped.front().scopes);
    EXPECT_EQ(harness.h().styleFor(scoped.front().scopes), first);
    // "int" is storage.type in the test theme, which is not the default style.
    EXPECT_NE(first, ide::theme::kDefaultStyleId);
}

// --- batching ----------------------------------------------------------------

TEST(HighlighterBatchTest, BatchMatchesPerLineCalls) {
    Harness harness(withGrammar());
    const std::vector<std::string_view> lines{"int a = 1;", "s = \"open", "still string",
                                              "closed\"; int b", ""};

    LineState state = harness.h().initialState();
    std::vector<std::vector<StyledSpan>> expected;
    std::vector<StyledSpan> lineSpans;
    for (const std::string_view line : lines) {
        harness.h().highlightLine(line, state, lineSpans);
        expected.push_back(lineSpans);
    }

    const BatchResult batch =
        harness.h().highlightLines(std::span<const std::string_view>(lines),
                                   harness.h().initialState());
    ASSERT_EQ(batch.lineCount(), lines.size());
    ASSERT_EQ(batch.lineOffsets.size(), lines.size() + 1u);
    ASSERT_EQ(batch.endStates.size(), lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::span<const StyledSpan> actual = batch.lineSpans(i);
        ASSERT_EQ(actual.size(), expected[i].size()) << "line " << i;
        for (size_t j = 0; j < actual.size(); ++j) {
            EXPECT_EQ(actual[j], expected[i][j]) << "line " << i << " span " << j;
        }
    }
    EXPECT_EQ(batch.endStates.back(), state);
}

TEST(HighlighterBatchTest, OutOfRangeLineYieldsNoSpans) {
    Harness harness(withGrammar());
    const std::vector<std::string_view> lines{"int a;"};
    const BatchResult batch =
        harness.h().highlightLines(std::span<const std::string_view>(lines),
                                   harness.h().initialState());
    EXPECT_TRUE(batch.lineSpans(1u).empty());
    EXPECT_TRUE(batch.lineSpans(99u).empty());
    const BatchResult empty;
    EXPECT_TRUE(empty.lineSpans(0u).empty());
    EXPECT_EQ(empty.lineCount(), 0u);
}

TEST(HighlighterBatchTest, AllocatingAndOutParamOverloadsAgree) {
    Harness harness(withGrammar());
    const std::string_view line = "int x = 1; // c";
    LineState first = harness.h().initialState();
    LineState second = harness.h().initialState();
    std::vector<StyledSpan> out;
    harness.h().highlightLine(line, first, out);
    const std::vector<StyledSpan> returned = harness.h().highlightLine(line, second);
    EXPECT_EQ(out, returned);
    EXPECT_EQ(first, second);
}

// --- state -------------------------------------------------------------------

TEST(HighlighterStateTest, StateIsDeterministicAndComparable) {
    Config config;
    config.fileName = "a.c";  // tier 2 carries the fallback state
    Harness harness(config);

    std::vector<StyledSpan> spans;
    LineState first = harness.h().initialState();
    harness.h().highlightLine("/* open", first, spans);
    LineState second = harness.h().initialState();
    harness.h().highlightLine("/* open", second, spans);

    EXPECT_EQ(first, second);
    EXPECT_NE(first, harness.h().initialState());
    EXPECT_EQ(first.fallback.mode, CarryMode::kBlockComment);
}

TEST(HighlighterStateTest, GrammarStateAdvancesAcrossLines) {
    Harness harness(withGrammar());
    std::vector<StyledSpan> spans;
    LineState state = harness.h().initialState();
    harness.h().highlightLine("s = \"open", state, spans);
    const LineState insideString = state;
    EXPECT_NE(insideString, harness.h().initialState());

    harness.h().highlightLine("closed\";", state, spans);
    EXPECT_NE(state, insideString);
}

TEST(HighlighterStateTest, StatsCountEachTier) {
    Harness harness(withGrammar());
    std::vector<StyledSpan> spans;
    LineState state = harness.h().initialState();
    harness.h().highlightLine("int x = 1;", state, spans);
    EXPECT_EQ(harness.h().stats().lines, 1u);
    EXPECT_EQ(harness.h().stats().grammarLines, 1u);
    EXPECT_EQ(harness.h().stats().fallbackLines, 0u);
    EXPECT_EQ(harness.h().stats().plainLines, 0u);
    harness.h().resetStats();
    EXPECT_EQ(harness.h().stats().lines, 0u);
}

}  // namespace
