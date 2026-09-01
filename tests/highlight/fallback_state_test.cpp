#include <gtest/gtest.h>

#include <string_view>
#include <type_traits>
#include <vector>

#include <ide/highlight/fallback_lexer.h>

#include "highlight_test_util.h"

namespace {

using ember_highlight_test::tiles;
using ide::highlight::CarryMode;
using ide::highlight::FallbackLexer;
using ide::highlight::FallbackSpan;
using ide::highlight::FallbackState;

// --- the value itself --------------------------------------------------------

TEST(FallbackStateTest, IsASmallTriviallyCopyableValue) {
    static_assert(std::is_trivially_copyable_v<FallbackState>);
    static_assert(sizeof(FallbackState) <= 4u);
    EXPECT_TRUE(FallbackState{}.clean());
}

TEST(FallbackStateTest, EqualityComparesEveryField) {
    const FallbackState clean;
    FallbackState other;
    EXPECT_EQ(clean, other);

    other.mode = CarryMode::kBlockComment;
    EXPECT_NE(clean, other);

    FallbackState a{CarryMode::kString, 2, false};
    FallbackState b{CarryMode::kString, 2, false};
    EXPECT_EQ(a, b);
    b.delimiter = 3;
    EXPECT_NE(a, b);
    b.delimiter = 2;
    b.rawString = true;
    EXPECT_NE(a, b);
}

// --- block comments ----------------------------------------------------------

TEST(FallbackStateTest, BlockCommentSpansThreeLines) {
    const FallbackLexer lexer(ide::highlight::cFamilyProfile());
    FallbackState state;

    const std::string_view first = "x /* start";
    std::vector<FallbackSpan> spans = lexer.lexLine(first, state);
    ASSERT_TRUE(tiles(spans, first.size()));
    EXPECT_EQ(state.mode, CarryMode::kBlockComment);
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, first, "/* start", "comment.block"));

    const FallbackState afterFirst = state;

    const std::string_view second = "middle";
    spans = lexer.lexLine(second, state);
    ASSERT_TRUE(tiles(spans, second.size()));
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].scope, "comment.block");
    // Nothing changed, which is exactly what lets incremental relayout stop.
    EXPECT_EQ(state, afterFirst);

    const std::string_view third = "end */ y";
    spans = lexer.lexLine(third, state);
    ASSERT_TRUE(tiles(spans, third.size()));
    EXPECT_TRUE(state.clean());
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, third, "end */", "comment.block"));
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, third, "y", "variable.other"));
}

TEST(FallbackStateTest, EmptyLineInsideABlockCommentKeepsItOpen) {
    const FallbackLexer lexer(ide::highlight::cFamilyProfile());
    FallbackState state;
    std::vector<FallbackSpan> spans = lexer.lexLine("/* open", state);
    ASSERT_TRUE(tiles(spans, 7u));
    ASSERT_EQ(state.mode, CarryMode::kBlockComment);

    spans = lexer.lexLine("", state);
    EXPECT_TRUE(spans.empty());
    EXPECT_EQ(state.mode, CarryMode::kBlockComment);

    spans = lexer.lexLine("*/", state);
    ASSERT_TRUE(tiles(spans, 2u));
    EXPECT_TRUE(state.clean());
}

TEST(FallbackStateTest, CommentCloserAtTheVeryStartOfALine) {
    const FallbackLexer lexer(ide::highlight::cFamilyProfile());
    FallbackState state;
    const std::vector<FallbackSpan> opening = lexer.lexLine("/*", state);
    ASSERT_TRUE(tiles(opening, 2u));
    ASSERT_EQ(state.mode, CarryMode::kBlockComment);
    const std::string_view line = "*/ int";
    const std::vector<FallbackSpan> spans = lexer.lexLine(line, state);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(state.clean());
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, line, "*/", "comment.block"));
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, line, "int", "storage.type"));
}

// --- multi-line strings ------------------------------------------------------

TEST(FallbackStateTest, TripleQuotedStringSpansLines) {
    const FallbackLexer lexer(ide::highlight::pythonProfile());
    FallbackState state;

    const std::string_view first = R"(x = """doc)";
    std::vector<FallbackSpan> spans = lexer.lexLine(first, state);
    ASSERT_TRUE(tiles(spans, first.size()));
    EXPECT_EQ(state.mode, CarryMode::kString);
    EXPECT_FALSE(state.rawString);

    const std::string_view second = "more";
    spans = lexer.lexLine(second, state);
    ASSERT_TRUE(tiles(spans, second.size()));
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].scope, "string.quoted.other");
    EXPECT_EQ(state.mode, CarryMode::kString);

    const std::string_view third = R"(text""" + y)";
    spans = lexer.lexLine(third, state);
    ASSERT_TRUE(tiles(spans, third.size()));
    EXPECT_TRUE(state.clean());
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, third, "text",
                                                   "string.quoted.other"));
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, third, "y", "variable.other"));
}

TEST(FallbackStateTest, RawTripleQuotedStringCarriesTheRawFlag) {
    const FallbackLexer lexer(ide::highlight::pythonProfile());
    FallbackState state;

    const std::string_view first = R"(x = r"""a\)";
    std::vector<FallbackSpan> spans = lexer.lexLine(first, state);
    ASSERT_TRUE(tiles(spans, first.size()));
    ASSERT_EQ(state.mode, CarryMode::kString);
    EXPECT_TRUE(state.rawString);

    // Raw: the backslash on the continuation line is NOT an escape, so the body
    // is one string span; the closing """ is its own end-delimiter span.
    const std::string_view second = R"(b\n""")";
    spans = lexer.lexLine(second, state);
    ASSERT_TRUE(tiles(spans, second.size()));
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].scope, "string.quoted.other");
    EXPECT_EQ(spans[1].scope, "punctuation.definition.string.end");
    EXPECT_TRUE(state.clean());
}

TEST(FallbackStateTest, CookedMultilineStringStillHandlesEscapes) {
    const FallbackLexer lexer(ide::highlight::pythonProfile());
    FallbackState state;
    const std::vector<FallbackSpan> opening = lexer.lexLine(R"(x = """a)", state);
    ASSERT_FALSE(opening.empty());
    ASSERT_EQ(state.mode, CarryMode::kString);
    EXPECT_FALSE(state.rawString);

    const std::string_view second = R"(b\n""")";
    const std::vector<FallbackSpan> spans = lexer.lexLine(second, state);
    ASSERT_TRUE(tiles(spans, second.size()));
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[1].scope, "constant.character.escape");
    EXPECT_TRUE(state.clean());
}

TEST(FallbackStateTest, CppRawStringSpansLines) {
    const FallbackLexer lexer(ide::highlight::cFamilyProfile());
    FallbackState state;

    const std::string_view first = R"(s = R"(abc)";
    std::vector<FallbackSpan> spans = lexer.lexLine(first, state);
    ASSERT_TRUE(tiles(spans, first.size()));
    EXPECT_EQ(state.mode, CarryMode::kString);

    const std::string_view second = R"CPP(def)";)CPP";
    spans = lexer.lexLine(second, state);
    ASSERT_TRUE(tiles(spans, second.size()));
    EXPECT_TRUE(state.clean());
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, second, "def",
                                                   "string.quoted.other"));
}

TEST(FallbackStateTest, UnterminatedSingleLineStringDoesNotCarry) {
    const FallbackLexer lexer(ide::highlight::cFamilyProfile());
    FallbackState state;
    const std::string_view first = R"(s = "oops)";
    const std::vector<FallbackSpan> spans = lexer.lexLine(first, state);
    ASSERT_TRUE(tiles(spans, first.size()));
    EXPECT_TRUE(state.clean());

    // The next line starts from scratch: no string colour leaks downwards.
    const std::string_view second = "int y = 1;";
    const std::vector<FallbackSpan> next = lexer.lexLine(second, state);
    ASSERT_TRUE(tiles(next, second.size()));
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(next, second, "int", "storage.type"));
}

// --- defensive ---------------------------------------------------------------

TEST(FallbackStateTest, StaleDelimiterIndexRecoversInsteadOfHanging) {
    // A caller that swaps the profile between lines (or a corrupted state from
    // the Java side later) must not put the lexer in a state it cannot leave.
    FallbackState state;
    state.mode = CarryMode::kBlockComment;
    state.delimiter = 250;
    const std::string_view line = "echo hi";
    const std::vector<FallbackSpan> spans =
        FallbackLexer(ide::highlight::shellProfile()).lexLine(line, state);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(state.clean());
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, line, "echo", "storage.type"));
}

TEST(FallbackStateTest, StaleStringDelimiterIndexRecovers) {
    FallbackState state;
    state.mode = CarryMode::kString;
    state.delimiter = 99;
    const std::string_view line = "int x;";
    const std::vector<FallbackSpan> spans =
        FallbackLexer(ide::highlight::cFamilyProfile()).lexLine(line, state);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(state.clean());
    EXPECT_TRUE(ember_highlight_test::hasExactSpan(spans, line, "int", "storage.type"));
}

}  // namespace
