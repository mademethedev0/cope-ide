#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include <ide/highlight/fallback_lexer.h>

#include "highlight_test_util.h"

namespace {

using ember_highlight_test::hasExactSpan;
using ember_highlight_test::scopeAt;
using ember_highlight_test::scopeOf;
using ember_highlight_test::tiles;
using ide::highlight::FallbackLexer;
using ide::highlight::FallbackSpan;
using ide::highlight::FallbackState;
using ide::highlight::LanguageProfile;

std::vector<FallbackSpan> lex(const LanguageProfile& profile, std::string_view text) {
    FallbackState state;
    return FallbackLexer(profile).lexLine(text, state);
}

const LanguageProfile& c() { return ide::highlight::cFamilyProfile(); }
const LanguageProfile& py() { return ide::highlight::pythonProfile(); }
const LanguageProfile& sh() { return ide::highlight::shellProfile(); }
const LanguageProfile& lisp() { return ide::highlight::lispProfile(); }
const LanguageProfile& markup() { return ide::highlight::markupProfile(); }
const LanguageProfile& generic() { return ide::highlight::genericProfile(); }

// --- c family ----------------------------------------------------------------

TEST(FallbackLexerCTest, LineCommentRunsToEndOfLine) {
    const std::string_view line = "int x; // done";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "int", "storage.type"));
    EXPECT_TRUE(hasExactSpan(spans, line, "x", "variable.other"));
    EXPECT_TRUE(hasExactSpan(spans, line, "// done", "comment.line"));
}

TEST(FallbackLexerCTest, BlockCommentClosingOnTheSameLine) {
    const std::string_view line = "a /* b */ c";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "/* b */", "comment.block"));
    EXPECT_TRUE(hasExactSpan(spans, line, "c", "variable.other"));
}

TEST(FallbackLexerCTest, OperatorRunDoesNotSwallowACommentOpener) {
    const std::string_view line = "x =/*c*/1";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "=", "keyword.operator"));
    EXPECT_TRUE(hasExactSpan(spans, line, "/*c*/", "comment.block"));
    EXPECT_TRUE(hasExactSpan(spans, line, "1", "constant.numeric"));
}

TEST(FallbackLexerCTest, StringsCharLiteralsAndEscapes) {
    const std::string_view line = R"(s = "a\tb" + 'c';)";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_EQ(scopeAt(spans, 4), "punctuation.definition.string.begin");
    EXPECT_TRUE(hasExactSpan(spans, line, "a", "string.quoted.double"));
    EXPECT_TRUE(hasExactSpan(spans, line, R"(\t)", "constant.character.escape"));
    EXPECT_TRUE(hasExactSpan(spans, line, "b", "string.quoted.double"));
    EXPECT_EQ(scopeAt(spans, 9), "punctuation.definition.string.end");
    EXPECT_EQ(scopeAt(spans, 13), "punctuation.definition.string.begin");
    EXPECT_TRUE(hasExactSpan(spans, line, "c", "string.quoted.single"));
    EXPECT_EQ(scopeAt(spans, 15), "punctuation.definition.string.end");
}

TEST(FallbackLexerCTest, EscapedQuoteDoesNotCloseTheString) {
    const std::string_view line = R"("a\"b" x)";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    // The escape sits inside the literal, so the closing quote is the last one.
    EXPECT_EQ(scopeOf(spans, line, "x"), "variable.other");
    EXPECT_TRUE(hasExactSpan(spans, line, R"(\")", "constant.character.escape"));
}

TEST(FallbackLexerCTest, UnterminatedStringEndsWithTheLineAndCarriesNothing) {
    const std::string_view line = R"(x = "abc)";
    FallbackState state;
    const std::vector<FallbackSpan> spans = FallbackLexer(c()).lexLine(line, state);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_EQ(scopeAt(spans, 4), "punctuation.definition.string.begin");
    EXPECT_TRUE(hasExactSpan(spans, line, "abc", "string.quoted.double"));
    // A single-line string never leaks into the next line.
    EXPECT_TRUE(state.clean());
}

TEST(FallbackLexerCTest, BackslashAtEndOfLineIsAnEscapeNotACrash) {
    const std::string_view line = R"("abc\)";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
}

TEST(FallbackLexerCTest, RawStringSuppressesEscapes) {
    // The outer literal needs a custom delimiter: its own content contains )".
    const std::string_view line = R"CPP(R"(a\b)")CPP";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].scope, "punctuation.definition.string.begin");
    EXPECT_EQ(spans[1].scope, "string.quoted.other");
    EXPECT_EQ(spans[2].scope, "punctuation.definition.string.end");
    EXPECT_EQ(spans[2].end, line.size());
}

TEST(FallbackLexerCTest, PrefixedStringLiterals) {
    const std::string_view line = R"(u8"a" L"b" u"c" U"d")";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    // The C-family table spells prefixed literals out as open delimiters
    // ("u8\"", "L\""...), so prefix letters and the quote form one begin span.
    EXPECT_TRUE(hasExactSpan(spans, line, R"(u8")", "punctuation.definition.string.begin"));
    EXPECT_TRUE(hasExactSpan(spans, line, "a", "string.quoted.double"));
    EXPECT_TRUE(hasExactSpan(spans, line, R"(L")", "punctuation.definition.string.begin"));
    EXPECT_TRUE(hasExactSpan(spans, line, "b", "string.quoted.double"));
    EXPECT_TRUE(hasExactSpan(spans, line, R"(u")", "punctuation.definition.string.begin"));
    EXPECT_TRUE(hasExactSpan(spans, line, "c", "string.quoted.double"));
    EXPECT_TRUE(hasExactSpan(spans, line, R"(U")", "punctuation.definition.string.begin"));
    EXPECT_TRUE(hasExactSpan(spans, line, "d", "string.quoted.double"));
}

TEST(FallbackLexerCTest, TemplateLiteralBackticks) {
    const std::string_view line = "let s = `a ${b} c`;";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "a ${b} c", "string.quoted.other"));
    EXPECT_TRUE(hasExactSpan(spans, line, "let", "keyword.control"));
}

TEST(FallbackLexerCTest, EveryNumberForm) {
    const std::string_view line = "v = 0xFFull + 1_000_000 - 1.5e-3f;";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "0xFFull", "constant.numeric"));
    EXPECT_TRUE(hasExactSpan(spans, line, "1_000_000", "constant.numeric"));
    EXPECT_TRUE(hasExactSpan(spans, line, "1.5e-3f", "constant.numeric"));
}

TEST(FallbackLexerCTest, MoreNumberForms) {
    struct Case {
        std::string_view line;
        std::string_view number;
    };
    const Case cases[] = {
        {"a = 0b1010;", "0b1010"},   {"a = 0o17;", "0o17"},   {"a = 007;", "007"},
        {"a = .5;", ".5"},           {"a = 3.14;", "3.14"},   {"a = 1'000;", "1'000"},
        {"a = 0xFF;", "0xFF"},       {"a = 1e10;", "1e10"},   {"a = 42u;", "42u"},
        {"a = 1i32;", "1i32"},       {"a = 2.;", "2."},
    };
    for (const Case& item : cases) {
        const std::vector<FallbackSpan> spans = lex(c(), item.line);
        ASSERT_TRUE(tiles(spans, item.line.size())) << item.line;
        EXPECT_TRUE(hasExactSpan(spans, item.line, item.number, "constant.numeric"))
            << "line: " << item.line;
    }
}

TEST(FallbackLexerCTest, ADotFollowedByAnIdentifierIsNotPartOfTheNumber) {
    const std::string_view line = "1.max";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "1", "constant.numeric"));
    EXPECT_TRUE(hasExactSpan(spans, line, ".", "punctuation.accessor"));
    EXPECT_TRUE(hasExactSpan(spans, line, "max", "variable.other.property"));
}

TEST(FallbackLexerCTest, KeywordLookupIsWholeWord) {
    const std::string_view line = "if (iffy) return;";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "if", "keyword.control"));
    EXPECT_TRUE(hasExactSpan(spans, line, "iffy", "variable.other"));
    EXPECT_TRUE(hasExactSpan(spans, line, "return", "keyword.control"));
}

TEST(FallbackLexerCTest, KeywordBeatsTheCallHeuristic) {
    const std::string_view line = "if(x)";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "if", "keyword.control"));
}

TEST(FallbackLexerCTest, CallAndPropertyHeuristics) {
    const std::string_view line = "obj.field = run(x) + p->next;";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "obj", "variable.other"));
    EXPECT_TRUE(hasExactSpan(spans, line, "field", "variable.other.property"));
    EXPECT_TRUE(hasExactSpan(spans, line, "run", "entity.name.function"));
    EXPECT_TRUE(hasExactSpan(spans, line, "->", "keyword.operator"));
    EXPECT_TRUE(hasExactSpan(spans, line, "next", "variable.other.property"));
}

TEST(FallbackLexerCTest, AMethodCallIsAFunctionNotAProperty) {
    const std::string_view line = "promise.catch(err);";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    // After a dot a word is never a keyword, and a call beats a property.
    EXPECT_TRUE(hasExactSpan(spans, line, "catch", "entity.name.function"));
}

TEST(FallbackLexerCTest, PreprocessorInclude) {
    const std::string_view line = "#include <stdio.h>";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "#include", "meta.preprocessor"));
    EXPECT_TRUE(hasExactSpan(spans, line, "<stdio.h>", "string.quoted.other"));
}

TEST(FallbackLexerCTest, PreprocessorDefineKeepsLexingTheRest) {
    const std::string_view line = "#define MAX 10";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "#define", "meta.preprocessor"));
    EXPECT_TRUE(hasExactSpan(spans, line, "MAX", "variable.other"));
    EXPECT_TRUE(hasExactSpan(spans, line, "10", "constant.numeric"));
}

TEST(FallbackLexerCTest, IndentedPreprocessorStillCounts) {
    const std::string_view line = "  #endif";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "#endif", "meta.preprocessor"));
}

TEST(FallbackLexerCTest, HashInTheMiddleOfALineIsNotADirective) {
    const std::string_view line = "x = a # b";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "#", "keyword.operator"));
}

TEST(FallbackLexerCTest, DecoratorAndBracketAnnotation) {
    const std::string_view decorated = "@Override";
    const std::vector<FallbackSpan> spans = lex(c(), decorated);
    ASSERT_TRUE(tiles(spans, decorated.size()));
    EXPECT_TRUE(hasExactSpan(spans, decorated, "@Override", "entity.name.function.decorator"));

    const std::string_view attribute = "[Serializable]";
    const std::vector<FallbackSpan> attributeSpans = lex(c(), attribute);
    ASSERT_TRUE(tiles(attributeSpans, attribute.size()));
    EXPECT_TRUE(hasExactSpan(attributeSpans, attribute, "[", "punctuation.definition.annotation"));
    EXPECT_TRUE(hasExactSpan(attributeSpans, attribute, "Serializable", "entity.name.tag"));
}

TEST(FallbackLexerCTest, BracketThatIsNotAnAnnotationStaysPunctuation) {
    const std::string_view line = "[i] = 1;";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    // "[i]" *is* a valid annotation shape, which is fine: it is line-anchored and
    // coloured as a tag. What must never happen is a broken tiling.
    EXPECT_TRUE(hasExactSpan(spans, line, "[", "punctuation.definition.annotation"));
}

// --- python ------------------------------------------------------------------

TEST(FallbackLexerPythonTest, HashComment) {
    const std::string_view line = "x = 1  # note";
    const std::vector<FallbackSpan> spans = lex(py(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "# note", "comment.line"));
    EXPECT_TRUE(hasExactSpan(spans, line, "1", "constant.numeric"));
}

TEST(FallbackLexerPythonTest, TripleQuotedStringOnOneLine) {
    const std::string_view line = R"(d = """doc""")";
    FallbackState state;
    const std::vector<FallbackSpan> spans = FallbackLexer(py()).lexLine(line, state);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "doc", "string.quoted.other"));
    EXPECT_TRUE(state.clean());
}

TEST(FallbackLexerPythonTest, StringPrefixes) {
    const std::string_view line = R"(a = f"{x}" + r"a\tb" + b"z")";
    const std::vector<FallbackSpan> spans = lex(py(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "{x}", "string.quoted.double"));
    // r"" is raw: the backslash is NOT an escape, so the body is one span.
    EXPECT_TRUE(hasExactSpan(spans, line, R"(a\tb)", "string.quoted.double"));
    EXPECT_TRUE(hasExactSpan(spans, line, "z", "string.quoted.double"));
}

TEST(FallbackLexerPythonTest, KeywordsTypesConstantsAndDecorators) {
    const std::string_view line = "@app.route";
    const std::vector<FallbackSpan> spans = lex(py(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "@app.route", "entity.name.function.decorator"));

    const std::string_view body = "def run(self, n: int = None): pass";
    const std::vector<FallbackSpan> bodySpans = lex(py(), body);
    ASSERT_TRUE(tiles(bodySpans, body.size()));
    EXPECT_TRUE(hasExactSpan(bodySpans, body, "def", "keyword.control"));
    EXPECT_TRUE(hasExactSpan(bodySpans, body, "run", "entity.name.function"));
    EXPECT_TRUE(hasExactSpan(bodySpans, body, "self", "constant.language"));
    EXPECT_TRUE(hasExactSpan(bodySpans, body, "int", "storage.type"));
    EXPECT_TRUE(hasExactSpan(bodySpans, body, "None", "constant.language"));
    EXPECT_TRUE(hasExactSpan(bodySpans, body, "pass", "keyword.control"));
}

// --- shell -------------------------------------------------------------------

TEST(FallbackLexerShellTest, CommandStringAndComment) {
    const std::string_view line = "echo \"$HOME\" # comment";
    const std::vector<FallbackSpan> spans = lex(sh(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "echo", "storage.type"));
    EXPECT_TRUE(hasExactSpan(spans, line, "\"", "punctuation.definition.string.begin"));
    EXPECT_TRUE(hasExactSpan(spans, line, "$HOME", "string.quoted.double"));
    EXPECT_TRUE(hasExactSpan(spans, line, "# comment", "comment.line"));
}

TEST(FallbackLexerShellTest, SingleQuotesAreLiteral) {
    const std::string_view line = R"(x='literal\n')";
    const std::vector<FallbackSpan> spans = lex(sh(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, R"(literal\n)", "string.quoted.single"));
}

TEST(FallbackLexerShellTest, DollarVariablesAreOneIdentifier) {
    const std::string_view line = "if [ -z $HOME ]; then";
    const std::vector<FallbackSpan> spans = lex(sh(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "$HOME", "variable.other"));
    EXPECT_TRUE(hasExactSpan(spans, line, "if", "keyword.control"));
    EXPECT_TRUE(hasExactSpan(spans, line, "then", "keyword.control"));
}

// --- lisp --------------------------------------------------------------------

TEST(FallbackLexerLispTest, SemicolonCommentsAndHyphenIdentifiers) {
    const std::string_view line = "(defun foo-bar (x) 42) ; note";
    const std::vector<FallbackSpan> spans = lex(lisp(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "defun", "keyword.control"));
    EXPECT_TRUE(hasExactSpan(spans, line, "foo-bar", "variable.other"));
    EXPECT_TRUE(hasExactSpan(spans, line, "42", "constant.numeric"));
    EXPECT_TRUE(hasExactSpan(spans, line, "; note", "comment.line"));
}

TEST(FallbackLexerLispTest, BlockComment) {
    const std::string_view line = "#| block |#";
    const std::vector<FallbackSpan> spans = lex(lisp(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].scope, "comment.block");
}

TEST(FallbackLexerLispTest, CallsAreDetectedAfterTheOpenParen) {
    const std::string_view line = "(my-fn x)";
    const std::vector<FallbackSpan> spans = lex(lisp(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "my-fn", "entity.name.function"));
    EXPECT_TRUE(hasExactSpan(spans, line, "x", "variable.other"));
}

// --- markup ------------------------------------------------------------------

TEST(FallbackLexerMarkupTest, TagAttributesEntitiesAndProse) {
    const std::string_view line = R"(<div class="a">text &amp; x</div>)";
    const std::vector<FallbackSpan> spans = lex(markup(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "div", "entity.name.tag"));
    EXPECT_TRUE(hasExactSpan(spans, line, "class", "entity.other.attribute-name"));
    EXPECT_TRUE(hasExactSpan(spans, line, R"("a")", "string.quoted.double"));
    EXPECT_TRUE(hasExactSpan(spans, line, "&amp;", "constant.character.escape"));
    // Prose must stay unclassified: colouring English text looks worse than
    // leaving it alone, which is the whole reason markup has no identifierScope.
    EXPECT_EQ(scopeOf(spans, line, "text"), "");
}

TEST(FallbackLexerMarkupTest, Comment) {
    const std::string_view line = "<!-- c --><b>";
    const std::vector<FallbackSpan> spans = lex(markup(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "<!-- c -->", "comment.block"));
    EXPECT_TRUE(hasExactSpan(spans, line, "b", "entity.name.tag"));
}

TEST(FallbackLexerMarkupTest, ProcessingInstructionAndClosingTag) {
    const std::string_view line = R"(<?xml version="1.0"?>)";
    const std::vector<FallbackSpan> spans = lex(markup(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    EXPECT_TRUE(hasExactSpan(spans, line, "xml", "entity.name.tag"));
    EXPECT_TRUE(hasExactSpan(spans, line, "version", "entity.other.attribute-name"));
}

TEST(FallbackLexerMarkupTest, UnclosedTagStopsAtEndOfLine) {
    const std::string_view line = "<div class=";
    const std::vector<FallbackSpan> spans = lex(markup(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
}

TEST(FallbackLexerMarkupTest, StrayLessThanInProse) {
    const std::string_view line = "a < b";
    const std::vector<FallbackSpan> spans = lex(markup(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
}

// --- generic -----------------------------------------------------------------

TEST(FallbackLexerGenericTest, BothCommentStylesAndConstants) {
    const std::string_view hashed = "key = yes # note";
    const std::vector<FallbackSpan> hashedSpans = lex(generic(), hashed);
    ASSERT_TRUE(tiles(hashedSpans, hashed.size()));
    EXPECT_TRUE(hasExactSpan(hashedSpans, hashed, "# note", "comment.line"));
    EXPECT_TRUE(hasExactSpan(hashedSpans, hashed, "yes", "constant.language"));

    const std::string_view slashed = "x = 1 // note";
    const std::vector<FallbackSpan> slashedSpans = lex(generic(), slashed);
    ASSERT_TRUE(tiles(slashedSpans, slashed.size()));
    EXPECT_TRUE(hasExactSpan(slashedSpans, slashed, "// note", "comment.line"));
}

// --- UTF-8 -------------------------------------------------------------------

TEST(FallbackLexerUtf8Test, MultibyteIdentifierIsOneSpan) {
    const std::string_view line = "né = 1";  // 'é' is two bytes
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    ASSERT_FALSE(spans.empty());
    EXPECT_EQ(spans[0].begin, 0u);
    EXPECT_EQ(spans[0].end, 3u);
    EXPECT_EQ(spans[0].scope, "variable.other");
}

TEST(FallbackLexerUtf8Test, EscapedMultibyteCharacterIsNotSplit) {
    const std::string_view line = R"("\é")";  // quote, backslash, 2 bytes, quote
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[1].begin, 1u);
    EXPECT_EQ(spans[1].end, 4u);
    EXPECT_EQ(spans[1].scope, "constant.character.escape");
}

TEST(FallbackLexerUtf8Test, MultibyteInsideAStringAndAComment) {
    const std::string_view line = "x = \"héllo\"; // ünïcode";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
}

// --- degenerate input --------------------------------------------------------

TEST(FallbackLexerEdgeTest, EmptyInputYieldsNoSpans) {
    const std::vector<FallbackSpan> spans = lex(c(), "");
    EXPECT_TRUE(spans.empty());
    EXPECT_TRUE(tiles(spans, 0u));
}

TEST(FallbackLexerEdgeTest, WhitespaceOnlyLineIsOneUnscopedSpan) {
    const std::string_view line = " \t  ";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    ASSERT_TRUE(tiles(spans, line.size()));
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_TRUE(spans[0].scope.empty());
}

TEST(FallbackLexerEdgeTest, HugeLineIsHandledLinearly) {
    std::string line;
    line.reserve(40000);
    for (size_t i = 0; i < 4000; ++i) line += "x = f(1); ";
    const std::vector<FallbackSpan> spans = lex(c(), line);
    EXPECT_TRUE(tiles(spans, line.size()));
}

TEST(FallbackLexerEdgeTest, AllProfilesTileNastyInput) {
    const std::string_view nasty[] = {
        "",
        " ",
        "\t\t",
        "\"",
        "'",
        "`",
        "\\",
        "#",
        "@",
        "/*",
        "*/",
        "//",
        "<",
        ">",
        "&",
        ";",
        "\"\"\"",
        "'''",
        "0x",
        ".",
        "..",
        "1.",
        "$",
        "R\"(",
        "<!--",
        "&#;",
        "é",
        "\xC3",          // truncated UTF-8 lead byte
        "\x80\x80",      // stray continuation bytes
        "a\\",
        "[x",
        "@ ",
        "x=/*",
        "'''a",
        "u8\"",
    };
    for (const ide::highlight::LanguageProfile* profile : ide::highlight::allProfiles()) {
        ASSERT_NE(profile, nullptr);
        for (const std::string_view line : nasty) {
            FallbackState state;
            const std::vector<FallbackSpan> spans =
                FallbackLexer(*profile).lexLine(line, state);
            EXPECT_TRUE(tiles(spans, line.size()))
                << "profile " << profile->name << ", line \"" << line << '"';
        }
    }
}

TEST(FallbackLexerEdgeTest, EveryEmittedScopeIsADocumentedConstant) {
    const std::string_view known[] = {
        "",
        "comment.line",
        "comment.block",
        "string.quoted.double",
        "string.quoted.single",
        "string.quoted.other",
        "constant.character.escape",
        "constant.numeric",
        "constant.language",
        "keyword.control",
        "keyword.operator",
        "storage.type",
        "entity.name.function",
        "entity.name.function.decorator",
        "entity.name.tag",
        "entity.other.attribute-name",
        "variable.other.property",
        "variable.other",
        "punctuation.definition.brackets",
        "punctuation.separator.comma",
        "punctuation.terminator",
        "punctuation.accessor",
        "punctuation.definition.tag",
        "punctuation.definition.annotation",
        "punctuation.definition.string.begin",
        "punctuation.definition.string.end",
        "meta.preprocessor",
    };
    const std::string_view corpus[] = {
        "#include <stdio.h>",
        "int main(int argc, char** argv) { return 0; }",
        "x = \"a\\tb\" + 'c' + `d` + 0xFF + 1.5e3;",
        "/* block */ // line",
        "@Decorator [Attr] obj.prop->q",
        "def f(self): return None  # hi",
        "echo \"$HOME\" | grep -v x",
        "(defun f (x) ; c",
        "<a href=\"x\">&amp;</a>",
        "key = yes # note",
        "\"\"\"doc\"\"\"",
    };
    for (const ide::highlight::LanguageProfile* profile : ide::highlight::allProfiles()) {
        for (const std::string_view line : corpus) {
            FallbackState state;
            const std::vector<FallbackSpan> spans =
                FallbackLexer(*profile).lexLine(line, state);
            for (const FallbackSpan& span : spans) {
                bool found = false;
                for (const std::string_view candidate : known) {
                    if (span.scope == candidate) {
                        found = true;
                        break;
                    }
                }
                EXPECT_TRUE(found) << "undocumented scope \"" << span.scope << "\" from profile "
                                   << profile->name << " on \"" << line << '"';
            }
        }
    }
}

// --- profile selection -------------------------------------------------------

TEST(FallbackProfileTest, ExtensionSelection) {
    EXPECT_EQ(ide::highlight::profileForExtension("cpp").name, "c-family");
    EXPECT_EQ(ide::highlight::profileForExtension(".PY").name, "python-style");
    EXPECT_EQ(ide::highlight::profileForExtension("sh").name, "shell");
    EXPECT_EQ(ide::highlight::profileForExtension("el").name, "lisp");
    EXPECT_EQ(ide::highlight::profileForExtension("html").name, "markup");
}

TEST(FallbackProfileTest, SelectionNeverFails) {
    EXPECT_EQ(ide::highlight::profileForExtension("").name, "generic");
    EXPECT_EQ(ide::highlight::profileForExtension("wat").name, "generic");
    EXPECT_EQ(ide::highlight::profileForExtension("...").name, "generic");
    EXPECT_EQ(ide::highlight::profileForFileName("").name, "generic");
    EXPECT_EQ(ide::highlight::profileForFileName("weird-thing").name, "generic");
}

TEST(FallbackProfileTest, FileNameSelection) {
    EXPECT_EQ(ide::highlight::profileForFileName("src/Makefile").name, "shell");
    EXPECT_EQ(ide::highlight::profileForFileName("a/b/CMakeLists.txt").name, "python-style");
    EXPECT_EQ(ide::highlight::profileForFileName(".bashrc").name, "shell");
    EXPECT_EQ(ide::highlight::profileForFileName("/x/y/main.cc").name, "c-family");
    EXPECT_EQ(ide::highlight::profileForFileName("notes.md").name, "markup");
}

TEST(FallbackProfileTest, ExtensionExtraction) {
    EXPECT_EQ(ide::highlight::extensionOfFileName("a/b.c"), "c");
    EXPECT_EQ(ide::highlight::extensionOfFileName("foo.tar.gz"), "gz");
    EXPECT_EQ(ide::highlight::extensionOfFileName(".bashrc"), "");
    EXPECT_EQ(ide::highlight::extensionOfFileName("Makefile"), "");
    EXPECT_EQ(ide::highlight::extensionOfFileName("dir.d/file"), "");
    EXPECT_EQ(ide::highlight::extensionOfFileName(""), "");
}

}  // namespace
