#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include <ide/highlight/highlighter.h>

#include "highlight_test_util.h"

// The repair layer is the reason this module exists, so it gets its own file.
//
// Each test uses a DELIBERATELY CRIPPLED grammar: it scopes a couple of token
// kinds and leaves everything else at the bare root scope, which is exactly what
// a real grammar looks like once the regex engine has refused 15% of its rules.

namespace {

using cope_highlight_test::Config;
using cope_highlight_test::FlatSpan;
using cope_highlight_test::Harness;
using cope_highlight_test::tiles;
using ide::highlight::ScopedSpan;
using ide::highlight::Tier;

/// Scopes only "int" and double quoted strings. Everything else is a hole.
constexpr std::string_view kCrippledGrammar = R"json({
  "scopeName": "source.crippled",
  "fileTypes": ["c"],
  "patterns": [
    { "name": "storage.type.crippled", "match": "\\bint\\b" },
    { "name": "string.quoted.double.crippled", "begin": "\"", "end": "\"" }
  ]
})json";

/// Scopes a braced body with a meta scope and nothing else. meta scopes are
/// structure, not colour, so this grammar produces text that *looks* unstyled.
constexpr std::string_view kMetaOnlyGrammar = R"json({
  "scopeName": "source.meta",
  "fileTypes": ["c"],
  "patterns": [
    { "name": "meta.body.x", "begin": "\\{", "end": "\\}" }
  ]
})json";

[[nodiscard]] Config crippled() {
    Config config;
    config.grammarJson = kCrippledGrammar;
    config.fileName = "a.c";
    return config;
}

TEST(HighlightRepairTest, FallbackFillsTheHolesAndKeepsTheGrammarSpans) {
    Harness harness(crippled());
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);
    ASSERT_EQ(harness.h().profile().name, "c-family");

    const std::string_view line = "int x = foo(1); // hi";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));

    // What the grammar produced is untouched, boundaries included.
    const std::vector<FlatSpan> flat = harness.flatten(spans);
    ASSERT_FALSE(flat.empty());
    EXPECT_EQ(flat.front(), (FlatSpan{0u, 3u, "source.crippled storage.type.crippled"}));

    // The holes are filled, and the fallback's scopes sit UNDER the root scope.
    EXPECT_EQ(harness.scopesOf(spans, line, "x"), "source.crippled variable.other");
    EXPECT_EQ(harness.scopesOf(spans, line, "="), "source.crippled keyword.operator");
    EXPECT_EQ(harness.scopesOf(spans, line, "foo"), "source.crippled entity.name.function");
    EXPECT_EQ(harness.scopesOf(spans, line, "1"), "source.crippled constant.numeric");
    // The trailing comment is repaired as ONE comment even though "//" and "hi"
    // are separated by whitespace: a repair region spans its interior blanks.
    EXPECT_EQ(harness.scopesOf(spans, line, "// hi"), "source.crippled comment.line");
    EXPECT_EQ(harness.scopesOf(spans, line, "hi"), "source.crippled comment.line");
}

TEST(HighlightRepairTest, WhitespaceOnlyGapsAreLeftAlone) {
    Harness harness(crippled());
    const std::string_view line = "int  int";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));

    const std::vector<FlatSpan> flat = harness.flatten(spans);
    const std::vector<FlatSpan> expected{
        FlatSpan{0u, 3u, "source.crippled storage.type.crippled"},
        FlatSpan{3u, 5u, "source.crippled"},
        FlatSpan{5u, 8u, "source.crippled storage.type.crippled"},
    };
    EXPECT_EQ(flat, expected);
    EXPECT_EQ(harness.h().stats().repairRegions, 0u);
    EXPECT_EQ(harness.h().stats().repairedBytes, 0u);
}

TEST(HighlightRepairTest, IndentationIsNotRepairedButTheCodeAfterItIs) {
    Harness harness(crippled());
    const std::string_view line = "    int x";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));

    const std::vector<FlatSpan> flat = harness.flatten(spans);
    ASSERT_EQ(flat.size(), 4u);
    EXPECT_EQ(flat[0], (FlatSpan{0u, 4u, "source.crippled"}));
    EXPECT_EQ(flat[1], (FlatSpan{4u, 7u, "source.crippled storage.type.crippled"}));
    EXPECT_EQ(flat[2], (FlatSpan{7u, 8u, "source.crippled"}));
    EXPECT_EQ(flat[3], (FlatSpan{8u, 9u, "source.crippled variable.other"}));
}

TEST(HighlightRepairTest, RepairNeverReachesInsideAGrammarString) {
    Harness harness(crippled());
    // The apostrophe inside the string would start a character literal if the
    // fallback ever saw it. It must not: the grammar owns those bytes.
    const std::string_view line = "int s = \"it's fine\";";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));

    EXPECT_EQ(harness.scopesOf(spans, line, "\"it's"),
              "source.crippled string.quoted.double.crippled");
    EXPECT_EQ(harness.scopesOf(spans, line, "fine"),
              "source.crippled string.quoted.double.crippled");
    EXPECT_EQ(harness.scopesOf(spans, line, ";"), "source.crippled punctuation.terminator");
    for (const FlatSpan& span : harness.flatten(spans)) {
        EXPECT_EQ(span.scopes.find("string.quoted.single"), std::string::npos)
            << "the fallback leaked a character literal into " << span.scopes;
    }
}

TEST(HighlightRepairTest, AFullyUnscopedLineIsRepairedEndToEnd) {
    Harness harness(crippled());
    const std::string_view line = "foo bar baz";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));

    EXPECT_EQ(harness.scopesOf(spans, line, "foo"), "source.crippled variable.other");
    EXPECT_EQ(harness.scopesOf(spans, line, "bar"), "source.crippled variable.other");
    EXPECT_EQ(harness.scopesOf(spans, line, "baz"), "source.crippled variable.other");
    // Hand computed: three words of three bytes, whitespace excluded.
    EXPECT_EQ(harness.h().stats().repairRegions, 1u);
    EXPECT_EQ(harness.h().stats().repairedBytes, 9u);
}

TEST(HighlightRepairTest, RepairIsDisabledWhenAskedTo) {
    Config config = crippled();
    config.limits.repairWithFallback = false;
    Harness harness(config);

    const std::string_view line = "int x = foo(1);";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(harness.scopesOf(spans, line, "foo"), "source.crippled");
    EXPECT_EQ(harness.h().stats().repairedBytes, 0u);
}

TEST(HighlightRepairTest, MetaOnlyStretchesAreRepairedByDefault) {
    Config config;
    config.grammarJson = kMetaOnlyGrammar;
    config.fileName = "a.c";
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);

    const std::string_view line = "{ int x; }";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));
    // The meta scope survives and the fallback's scope is appended to it.
    EXPECT_EQ(harness.scopesOf(spans, line, "int"), "source.meta meta.body.x storage.type");
    EXPECT_EQ(harness.scopesOf(spans, line, "x"), "source.meta meta.body.x variable.other");
    EXPECT_EQ(harness.scopesOf(spans, line, "{"),
              "source.meta meta.body.x punctuation.definition.brackets");
}

TEST(HighlightRepairTest, MetaOnlyRepairCanBeTurnedOff) {
    Config config;
    config.grammarJson = kMetaOnlyGrammar;
    config.fileName = "a.c";
    config.limits.repairMetaOnlyRuns = false;
    Harness harness(config);

    const std::string_view line = "{ int x; }";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));
    // Tier 1 is absolute: the whole line keeps the meta scope and merges into one
    // span, because every token has the same interned stack.
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(harness.scopesOf(spans, line, "int"), "source.meta meta.body.x");
    EXPECT_EQ(harness.h().stats().repairedBytes, 0u);
}

TEST(HighlightRepairTest, MultiLineGrammarStateSurvivesRepair) {
    Harness harness(crippled());
    const std::string_view lines[] = {"int s = \"open", "still string", "closed\"; int y"};
    ide::highlight::LineState state = harness.h().initialState();
    std::vector<ScopedSpan> spans;

    harness.h().scopeLine(lines[0], state, spans);
    ASSERT_TRUE(tiles(spans, lines[0].size()));
    EXPECT_EQ(harness.scopesOf(spans, lines[0], "\"open"),
              "source.crippled string.quoted.double.crippled");

    harness.h().scopeLine(lines[1], state, spans);
    ASSERT_TRUE(tiles(spans, lines[1].size()));
    // Inside the grammar's string: no repair at all, so no fallback scope.
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(harness.scopesOf(spans, lines[1], "still"),
              "source.crippled string.quoted.double.crippled");

    harness.h().scopeLine(lines[2], state, spans);
    ASSERT_TRUE(tiles(spans, lines[2].size()));
    EXPECT_EQ(harness.scopesOf(spans, lines[2], "closed"),
              "source.crippled string.quoted.double.crippled");
    EXPECT_EQ(harness.scopesOf(spans, lines[2], "int"),
              "source.crippled storage.type.crippled");
    EXPECT_EQ(harness.scopesOf(spans, lines[2], "y"), "source.crippled variable.other");
}

TEST(HighlightRepairTest, EmptyLineProducesNoSpans) {
    Harness harness(crippled());
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine("");
    EXPECT_TRUE(spans.empty());
    EXPECT_TRUE(tiles(spans, 0u));
}

TEST(HighlightRepairTest, WhitespaceOnlyLineIsNotRepaired) {
    Harness harness(crippled());
    const std::string_view line = "   \t";
    const std::vector<ScopedSpan> spans = harness.scopeFirstLine(line);
    ASSERT_TRUE(tiles(spans, line.size()));
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(harness.flatten(spans).front().scopes, "source.crippled");
    EXPECT_EQ(harness.h().stats().repairRegions, 0u);
}

}  // namespace
