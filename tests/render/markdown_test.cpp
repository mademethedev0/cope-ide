// Markdown parser (M1, docs/design/markdown-renderer.md) — tests.
//
// Three layers of verification, because CI is the only oracle:
//   1. hand-written cases for every construct in the documented subset
//   2. a differential fuzz test against an independently written naive
//      line-scanner that counts block constructs
//   3. a round-trip stability fuzz: serialize(parse(s)) must be a fixed
//      point of serialize(parse(.)) for arbitrary bytes, and parsing must
//      never crash.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <ide/render/markdown.h>

namespace {

using ide::render::Alignment;
using ide::render::Block;
using ide::render::BlockQuote;
using ide::render::BlockSpan;
using ide::render::Code;
using ide::render::CodeBlock;
using ide::render::Doc;
using ide::render::Emph;
using ide::render::Heading;
using ide::render::Image;
using ide::render::Inline;
using ide::render::InlineMath;
using ide::render::Link;
using ide::render::List;
using ide::render::ListItem;
using ide::render::MathBlock;
using ide::render::MermaidBlock;
using ide::render::Paragraph;
using ide::render::Strike;
using ide::render::Strong;
using ide::render::Table;
using ide::render::TableCell;
using ide::render::TargetKind;
using ide::render::Text;
using ide::render::ThematicBreak;

// --- tree-building helpers (keep expectations readable) ---------------------

Inline T(std::string_view s) { return Inline{Text{std::string(s)}}; }
TableCell CdCell(std::string_view s) {
    return s.empty() ? TableCell{} : TableCell{{T(s)}};
}
TableCell Cell(std::vector<Inline> v) { return TableCell{std::move(v)}; }
// Build a Table block without deep nested braces in EXPECT macros.
Block Tbl(std::vector<TableCell> header, std::vector<Alignment> align,
          std::vector<std::vector<TableCell>> rows = {}) {
    return Block{Table{std::move(header), std::move(align), std::move(rows)}};
}
Inline Cd(std::string_view s) { return Inline{Code{std::string(s)}}; }
Inline Mt(std::string_view s) { return Inline{InlineMath{std::string(s)}}; }
Inline E(std::vector<Inline> v) { return Inline{Emph{std::move(v)}}; }
Inline Sg(std::vector<Inline> v) { return Inline{Strong{std::move(v)}}; }
Inline Sk(std::vector<Inline> v) { return Inline{Strike{std::move(v)}}; }
Inline Lk(std::vector<Inline> v, std::string target, std::string title = "",
          TargetKind kind = TargetKind::Relative) {
    return Inline{Link{std::move(v), std::move(target), std::move(title), kind}};
}
Inline Im(std::string alt, std::string target, std::string title = "",
          TargetKind kind = TargetKind::Relative) {
    return Inline{Image{std::move(alt), std::move(target), std::move(title), kind}};
}

Block P(std::vector<Inline> v) { return Block{Paragraph{std::move(v)}}; }
Block H(int level, std::vector<Inline> v) { return Block{Heading{level, std::move(v)}}; }
Block CB(std::string_view lang, std::string_view code) {
    return Block{CodeBlock{std::string(lang), std::string(code)}};
}
Block MB(std::string_view code) { return Block{MathBlock{std::string(code)}}; }
Block MM(std::string_view code) { return Block{MermaidBlock{std::string(code)}}; }
Block BQ(std::vector<Block> v) { return Block{BlockQuote{std::move(v)}}; }
inline Block LS(bool ordered, std::vector<ListItem> items) {
    return Block{List{ordered, std::move(items)}};
}
inline ListItem item(std::vector<Block> v) { return ListItem{std::move(v)}; }

// --- classifyTarget ---------------------------------------------------------

TEST(MarkdownTarget, Classification) {
    EXPECT_EQ(ide::render::classifyTarget("https://ex.com/a"), TargetKind::Https);
    EXPECT_EQ(ide::render::classifyTarget("HTTPS://EX.COM"), TargetKind::Https);
    EXPECT_EQ(ide::render::classifyTarget("http://ex.com"), TargetKind::Http);
    EXPECT_EQ(ide::render::classifyTarget("file:///data/x.txt"), TargetKind::File);
    EXPECT_EQ(ide::render::classifyTarget("mailto:a@b.c"), TargetKind::Other);
    EXPECT_EQ(ide::render::classifyTarget("intent://x"), TargetKind::Other);
    EXPECT_EQ(ide::render::classifyTarget("img.png"), TargetKind::Relative);
    EXPECT_EQ(ide::render::classifyTarget("./dir/f.md"), TargetKind::Relative);
    EXPECT_EQ(ide::render::classifyTarget("../up.png"), TargetKind::Relative);
    EXPECT_EQ(ide::render::classifyTarget("/abs/path.png"), TargetKind::Relative);
    EXPECT_EQ(ide::render::classifyTarget("#anchor"), TargetKind::Relative);
    EXPECT_EQ(ide::render::classifyTarget(""), TargetKind::Relative);
    EXPECT_EQ(ide::render::classifyTarget("example.com:8080"), TargetKind::Other);
}

// --- blocks -----------------------------------------------------------------

TEST(MarkdownParse, EmptyAndBlank) {
    EXPECT_EQ(ide::render::parse("").blocks.size(), 0u);
    EXPECT_EQ(ide::render::parse("\n\n\n").blocks.size(), 0u);
    EXPECT_EQ(ide::render::parse("   \n\t\n").blocks.size(), 0u);
}

TEST(MarkdownParse, AtxHeadings) {
    EXPECT_EQ(ide::render::parse("# hi"), Doc{{H(1, {T("hi")})}});
    EXPECT_EQ(ide::render::parse("###### deep"), Doc{{H(6, {T("deep")})}});
    EXPECT_EQ(ide::render::parse("### spaced ##"), Doc{{H(3, {T("spaced")})}});
    EXPECT_EQ(ide::render::parse("#nospace"), Doc{{P({T("#nospace")})}});
    EXPECT_EQ(ide::render::parse("####### seven"), Doc{{P({T("####### seven")})}});
    EXPECT_EQ(ide::render::parse("   # indented"), Doc{{H(1, {T("indented")})}});
    EXPECT_EQ(ide::render::parse("    # not a heading"), Doc{{P({T("    # not a heading")})}});
    EXPECT_EQ(ide::render::parse("#"), Doc{{H(1, {})}}); // '#' alone is an empty heading
}

TEST(MarkdownParse, SetextHeadings) {
    EXPECT_EQ(ide::render::parse("Title\n==="), Doc{{H(1, {T("Title")})}});
    EXPECT_EQ(ide::render::parse("Title\n---"), Doc{{H(2, {T("Title")})}});
    EXPECT_EQ(ide::render::parse("a\nb\n==="), Doc{{H(1, {T("a\nb")})}});
    // underline without a pending paragraph is not a heading
    EXPECT_EQ(ide::render::parse("---\n==="),
              (Doc{{Block{ThematicBreak{}}, P({T("===")})}}));
}

TEST(MarkdownParse, Paragraphs) {
    EXPECT_EQ(ide::render::parse("one"), Doc{{P({T("one")})}});
    EXPECT_EQ(ide::render::parse("one\ntwo"), Doc{{P({T("one\ntwo")})}});
    EXPECT_EQ(ide::render::parse("a\n\nb"),
              (Doc{{P({T("a")}), P({T("b")})}}));
    // thematic break ends a paragraph; underline after a break is a paragraph
    EXPECT_EQ(ide::render::parse("a\n***\nb"),
              (Doc{{P({T("a")}), Block{ThematicBreak{}}, P({T("b")})}}));
}

TEST(MarkdownParse, FencedCode) {
    EXPECT_EQ(ide::render::parse("```rust\nfn main() {}\n```"),
              Doc{{CB("rust", "fn main() {}")}});
    EXPECT_EQ(ide::render::parse("~~~\nx\n~~~"), Doc{{CB("", "x")}});
    EXPECT_EQ(ide::render::parse("```\nunclosed"), Doc{{CB("", "unclosed")}});
    // inner fence must be at least as long to close
    EXPECT_EQ(ide::render::parse("````\n```\n````"), Doc{{CB("", "```")}});
    // empty content
    EXPECT_EQ(ide::render::parse("```js\n```"), Doc{{CB("js", "")}});
    // info string: first word is the lang
    EXPECT_EQ(ide::render::parse("```c++ highlight\nx\n```"), Doc{{CB("c++", "x")}});
    // CRLF line endings
    EXPECT_EQ(ide::render::parse("```\r\na\r\n```"), Doc{{CB("", "a")}});
}

TEST(MarkdownParse, MathAndMermaidFences) {
    EXPECT_EQ(ide::render::parse("```math\nx = y\n```"), Doc{{MB("x = y")}});
    EXPECT_EQ(ide::render::parse("```mermaid\ngraph TD\n```"), Doc{{MM("graph TD")}});
    EXPECT_EQ(ide::render::parse("$$\nx = y\n$$"), Doc{{MB("x = y")}});
    EXPECT_EQ(ide::render::parse("$$\nunclosed"), Doc{{MB("unclosed")}});
    // inline $$ does not produce a block
    EXPECT_EQ(ide::render::parse("a $$x$$ b"), Doc{{P({T("a "), Mt("x"), T(" b")})}});
}

TEST(MarkdownParse, BlockQuotes) {
    EXPECT_EQ(ide::render::parse("> quote"), Doc{{BQ({P({T("quote")})})}});
    EXPECT_EQ(ide::render::parse("> # head"), Doc{{BQ({H(1, {T("head")})})}});
    EXPECT_EQ(ide::render::parse("> > deep"), Doc{{BQ({BQ({P({T("deep")})})})}});
    EXPECT_EQ(ide::render::parse("> a\n> b"), Doc{{BQ({P({T("a\nb")})})}});
    // blank line ends the quote; two quotes
    EXPECT_EQ(ide::render::parse("> a\n\n> b"),
              (Doc{{BQ({P({T("a")})}), BQ({P({T("b")})})}}));
    EXPECT_EQ(ide::render::parse(">"), Doc{{BQ({})}});
}

TEST(MarkdownParse, Lists) {
    EXPECT_EQ(ide::render::parse("- a\n- b"),
              Doc{{LS(false, {item({P({T("a")})}), item({P({T("b")})})})}});
    EXPECT_EQ(ide::render::parse("* a\n* b"),
              Doc{{LS(false, {item({P({T("a")})}), item({P({T("b")})})})}});
    EXPECT_EQ(ide::render::parse("1. a\n2. b"),
              Doc{{LS(true, {item({P({T("a")})}), item({P({T("b")})})})}});
    EXPECT_EQ(ide::render::parse("- a\n\n- b"), // loose list stays one list
              Doc{{LS(false, {item({P({T("a")})}), item({P({T("b")})})})}});
    // nesting: indented marker is content of the outer item
    EXPECT_EQ(ide::render::parse("- a\n  - b"),
              Doc{{LS(false, {item({P({T("a")}), LS(false, {item({P({T("b")})})})})})}});
    // continuation line belongs to the item
    EXPECT_EQ(ide::render::parse("- a\n  more"),
              Doc{{LS(false, {item({P({T("a\nmore")})})})}});
    // empty item
    EXPECT_EQ(ide::render::parse("-"), Doc{{LS(false, {item({})})}});
    // paragraph ends the list
    EXPECT_EQ(ide::render::parse("- a\nplain"),
              (Doc{{LS(false, {item({P({T("a")})})}), P({T("plain")})}}));
    // ordered lists and bullets don't merge
    {
        const Doc d = ide::render::parse("- a\n1. b");
        ASSERT_EQ(d.blocks.size(), 2u);
    }
    // bullet in a quote
    EXPECT_EQ(ide::render::parse("> - a"),
              Doc{{BQ({LS(false, {item({P({T("a")})})})})}});
}

TEST(MarkdownParse, ThematicBreaks) {
    EXPECT_EQ(ide::render::parse("***"), Doc{{Block{ThematicBreak{}}}});
    EXPECT_EQ(ide::render::parse("___"), Doc{{Block{ThematicBreak{}}}});
    EXPECT_EQ(ide::render::parse("- - -"), Doc{{Block{ThematicBreak{}}}});
    EXPECT_EQ(ide::render::parse("  * * *"), Doc{{Block{ThematicBreak{}}}});
    EXPECT_NE(ide::render::parse("--"), Doc{{Block{ThematicBreak{}}}}); // 2 markers: a list
    EXPECT_NE(ide::render::parse("* a * b"), Doc{{Block{ThematicBreak{}}}});
}

TEST(MarkdownParse, Tables) {
    EXPECT_EQ(ide::render::parse("| a | b |\n|---|---|\n| 1 | 2 |").blocks[0],
              Tbl({CdCell("a"), CdCell("b")}, {Alignment::None, Alignment::None},
                  {{{CdCell("1")}, {CdCell("2")}}}));
    EXPECT_EQ(ide::render::parse("| a | b |\n|:--|--:|").blocks[0],
              Tbl({CdCell("a"), CdCell("b")}, {Alignment::Left, Alignment::Right}));
    EXPECT_EQ(ide::render::parse("| a | b |\n|:-:|---|").blocks[0],
              Tbl({CdCell("a"), CdCell("b")},
                  {Alignment::Center, Alignment::None}));
    // no pipes in header -> not a table
    {
        const Doc d = ide::render::parse("plain\n---|---");
        ASSERT_EQ(d.blocks.size(), 1u);
        EXPECT_TRUE(std::holds_alternative<Paragraph>(d.blocks[0].node));
    }
    // ragged rows are padded/trimmed to the column count
    EXPECT_EQ(ide::render::parse("| a | b |\n|---|---|\n| only |").blocks[0],
              Tbl({CdCell("a"), CdCell("b")}, {Alignment::None, Alignment::None},
                  {{{CdCell("only")}, {CdCell("")}}}));
    // delimiter row without a pending header is a paragraph
    {
        const Doc d = ide::render::parse("|---|---|");
        ASSERT_EQ(d.blocks.size(), 1u);
        EXPECT_TRUE(std::holds_alternative<Paragraph>(d.blocks[0].node));
    }
    // inline content in cells
    EXPECT_EQ(ide::render::parse("| *a* | `b` |\n|---|---|").blocks[0],
              Tbl({Cell({E({T("a")})}), Cell({Cd("b")})},
                  {Alignment::None, Alignment::None}));
}

// --- inline -----------------------------------------------------------------

TEST(MarkdownInline, Emphasis) {
    EXPECT_EQ(ide::render::parse("a *b* c").blocks[0],
              P({T("a "), E({T("b")}), T(" c")}));
    EXPECT_EQ(ide::render::parse("a **b** c").blocks[0],
              P({T("a "), Sg({T("b")}), T(" c")}));
    EXPECT_EQ(ide::render::parse("_b_").blocks[0], P({E({T("b")})}));
    EXPECT_EQ(ide::render::parse("__b__").blocks[0], P({Sg({T("b")})}));
    // intraword underscore is not emphasis
    EXPECT_EQ(ide::render::parse("snake_case_word").blocks[0], P({T("snake_case_word")}));
    // intraword asterisk is
    EXPECT_EQ(ide::render::parse("a*b*c").blocks[0],
              P({T("a"), E({T("b")}), T("c")}));
    // unclosed -> literal
    EXPECT_EQ(ide::render::parse("*unclosed").blocks[0], P({T("*unclosed")}));
    EXPECT_EQ(ide::render::parse("a * b").blocks[0], P({T("a * b")}));
    // nested: strong inside emph
    EXPECT_EQ(ide::render::parse("*a **b** c*").blocks[0],
              P({E({T("a "), Sg({T("b")}), T(" c")})}));
}

TEST(MarkdownInline, Strikethrough) {
    EXPECT_EQ(ide::render::parse("~~gone~~").blocks[0], P({Sk({T("gone")})}));
    EXPECT_EQ(ide::render::parse("a ~single~ b").blocks[0], P({T("a ~single~ b")}));
    EXPECT_EQ(ide::render::parse("~~unclosed").blocks[0], P({T("~~unclosed")}));
}

TEST(MarkdownInline, CodeSpans) {
    EXPECT_EQ(ide::render::parse("`x`").blocks[0], P({Cd("x")}));
    EXPECT_EQ(ide::render::parse("`` a ``").blocks[0], P({Cd("a")}));
    EXPECT_EQ(ide::render::parse("`` ` ``").blocks[0], P({Cd("`")}));
    EXPECT_EQ(ide::render::parse("a ` code ` b").blocks[0],
              P({T("a "), Cd("code"), T(" b")})); // padding spaces are stripped
    EXPECT_EQ(ide::render::parse("`unclosed").blocks[0], P({T("`unclosed")}));
    // backslash is literal inside code spans
    EXPECT_EQ(ide::render::parse("`a\\`b`").blocks[0], P({Cd("a\\"), T("b`")}));
}

TEST(MarkdownInline, Math) {
    EXPECT_EQ(ide::render::parse("$x$").blocks[0], P({Mt("x")}));
    EXPECT_EQ(ide::render::parse("$$x$$").blocks[0], P({Mt("x")}));
    EXPECT_EQ(ide::render::parse("a $x^2$ b").blocks[0],
              P({T("a "), Mt("x^2"), T(" b")}));
    // '$' followed by whitespace never opens math
    EXPECT_EQ(ide::render::parse("costs $5 and $10").blocks[0], P({T("costs $5 and $10")}));
    // no closing '$' -> literal
    EXPECT_EQ(ide::render::parse("$x").blocks[0], P({T("$x")}));
}

TEST(MarkdownInline, LinksAndImages) {
    EXPECT_EQ(ide::render::parse("[t](u)").blocks[0], P({Lk({T("t")}, "u")}));
    EXPECT_EQ(ide::render::parse("[t](https://ex.com/a)").blocks[0],
              P({Lk({T("t")}, "https://ex.com/a", "", TargetKind::Https)}));
    EXPECT_EQ(ide::render::parse("[t](<my file.md> \"the title\")").blocks[0],
              P({Lk({T("t")}, "my file.md", "the title")}));
    EXPECT_EQ(ide::render::parse("[t](u 'ti')").blocks[0], P({Lk({T("t")}, "u", "ti")}));
    EXPECT_EQ(ide::render::parse("![alt](pic.png)").blocks[0], P({Im("alt", "pic.png")}));
    EXPECT_EQ(ide::render::parse("![](./a.png \"t\")").blocks[0],
              P({Im("", "./a.png", "t")}));
    // nested brackets inside link text
    EXPECT_EQ(ide::render::parse("[a [b] c](u)").blocks[0],
              P({Lk({T("a [b] c")}, "u")}));
    // broken syntax degrades to literal
    EXPECT_EQ(ide::render::parse("[broken").blocks[0], P({T("[broken")}));
    EXPECT_EQ(ide::render::parse("[broken](nope").blocks[0], P({T("[broken](nope")}));
    EXPECT_EQ(ide::render::parse("!x").blocks[0], P({T("!x")}));
    // links inside emphasis
    EXPECT_EQ(ide::render::parse("*[t](u)*").blocks[0], P({E({Lk({T("t")}, "u")})}));
}

TEST(MarkdownInline, Escapes) {
    EXPECT_EQ(ide::render::parse("\\*not\\*").blocks[0], P({T("*not*")}));
    EXPECT_EQ(ide::render::parse("\\\\").blocks[0], P({T("\\")}));
    EXPECT_EQ(ide::render::parse("\\a").blocks[0], P({T("\\a")}));
    EXPECT_EQ(ide::render::parse("a\\|b").blocks[0], P({T("a|b")}));
}

TEST(MarkdownInline, MultibyteUtf8) {
    EXPECT_EQ(ide::render::parse("héllo wörld").blocks[0], P({T("héllo wörld")}));
    EXPECT_EQ(ide::render::parse("## タイトル").blocks[0], H(2, {T("タイトル")}));
    EXPECT_EQ(ide::render::parse("héllo →").blocks[0], P({T("héllo →")}));
    // invalid UTF-8 bytes are passed through as literal bytes
    EXPECT_EQ(ide::render::parse("\xc3\xa9\xff").blocks[0], P({T("\xc3\xa9\xff")}));
}

// --- blockSpans -------------------------------------------------------------

TEST(MarkdownSpans, ByteRanges) {
    const std::string src = "# h\n\ntext\n\n- a\n- b\n";
    const std::vector<BlockSpan> spans = ide::render::blockSpans(src);
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0], (BlockSpan{0, 4}));   // "# h\n"
    EXPECT_EQ(spans[1], (BlockSpan{5, 10}));  // "text\n"
    EXPECT_EQ(spans[2], (BlockSpan{11, 19})); // "- a\n- b\n"
}

TEST(MarkdownSpans, NoTrailingNewline) {
    const std::vector<BlockSpan> spans = ide::render::blockSpans("# h");
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0], (BlockSpan{0, 3}));
}

TEST(MarkdownSpans, FenceSpanCoversWholeBlock) {
    const std::string src = "```\na\nb\n```\n";
    const std::vector<BlockSpan> spans = ide::render::blockSpans(src);
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0], (BlockSpan{0, 12}));
}

// --- serialize --------------------------------------------------------------

TEST(MarkdownSerialize, RoundTripRichDoc) {
    const std::string src =
        "# Title\n"
        "\n"
        "A *paragraph* with **bold**, `code`, $math$, a [link](https://ex.com) and "
        "an ![img](./pic.png).\n"
        "\n"
        "## Section\n"
        "\n"
        "- item one\n"
        "  - nested\n"
        "- item two\n"
        "\n"
        "1. first\n"
        "2. second\n"
        "\n"
        "> quoted\n"
        "> more\n"
        "\n"
        "```rust\n"
        "fn main() {}\n"
        "```\n"
        "\n"
        "```math\n"
        "x = y\n"
        "```\n"
        "\n"
        "```mermaid\n"
        "graph TD\n"
        "```\n"
        "\n"
        "$$\n"
        "z\n"
        "$$\n"
        "\n"
        "| a | b |\n"
        "|:--|--:|\n"
        "| 1 | 2 |\n"
        "\n"
        "Setext\n"
        "===\n"
        "\n"
        "***\n"
        "\n"
        "~~struck~~\n";
    const Doc d = ide::render::parse(src);
    const std::string s = ide::render::serialize(d);
    EXPECT_EQ(ide::render::parse(s), d);
}

TEST(MarkdownSerialize, EscapedParagraphStarts) {
    // paragraph text that would re-parse as a block gets a leading escape
    const Doc d1 = ide::render::parse("\\# not a heading");
    EXPECT_EQ(d1, Doc{{P({T("# not a heading")})}});
    EXPECT_EQ(ide::render::parse(ide::render::serialize(d1)), d1);

    const Doc d2 = ide::render::parse("\\- not a list");
    EXPECT_EQ(ide::render::parse(ide::render::serialize(d2)), d2);

    const Doc d3 = ide::render::parse("1\\. not a list");
    EXPECT_EQ(ide::render::parse(ide::render::serialize(d3)), d3);
}

TEST(MarkdownSerialize, CodeWithBackticks) {
    const Doc d = ide::render::parse("````\n```\n````");
    EXPECT_EQ(ide::render::parse(ide::render::serialize(d)), d);
    const Doc d2 = ide::render::parse("`` `code` ``");
    EXPECT_EQ(ide::render::parse(ide::render::serialize(d2)), d2);
}

TEST(MarkdownSerialize, FenceLangWithBacktick) {
    // only ~~~ fences accept a backtick in the info string
    const Doc d = ide::render::parse("~~~ `x\ny\n~~~");
    EXPECT_EQ(d, Doc{{CB("`x", "y")}});
    const std::string s = ide::render::serialize(d);
    EXPECT_EQ(ide::render::parse(s), d);
    EXPECT_EQ(ide::render::serialize(ide::render::parse(s)), s);
}

TEST(MarkdownSerialize, SoftBreakSecondLineGuards) {
    // The line guard must apply to every line of a paragraph, not just the
    // first: a soft break whose second line would re-read as a list / heading
    // / quote / setext underline / thematic break is escaped and folds back.
    const char* const sources[] = {
        "a\\n\\- b",    "a\\n\\+ b",     "a\\n\\1. b",  "a\\n\\# b",
        "a\\n\\> b",    "a\\n\\=",       "a\\n\\===",   "a\\n\\- - -",
        "x y\\n2\\) z", " \\# x",   "a\\n\\:--|--",
    };
    for (const char* src : sources) {
        const Doc d = ide::render::parse(src);
        const std::string s1 = ide::render::serialize(d);
        EXPECT_EQ(ide::render::parse(s1), d) << "tree lost, source:\n" << src;
        EXPECT_EQ(ide::render::serialize(ide::render::parse(s1)), s1)
            << "not a fixed point, source:\n" << src;
    }
}

TEST(MarkdownSerialize, SoftBreakInsideEmphasis) {
    const Doc d = ide::render::parse("*a\\n\\- b*");
    EXPECT_EQ(d, Doc{{P({E({T("a\n- b")})})}});
    const std::string s = ide::render::serialize(d);
    EXPECT_EQ(ide::render::parse(s), d);
    EXPECT_EQ(ide::render::serialize(ide::render::parse(s)), s);
}

TEST(MarkdownSerialize, MultiLineSetextHeading) {
    // soft breaks in heading text are only representable in setext form
    const Doc d = ide::render::parse("a\nb\n===");
    EXPECT_EQ(d, Doc{{H(1, {T("a\nb")})}});
    const std::string s = ide::render::serialize(d);
    EXPECT_EQ(ide::render::parse(s), d);
    EXPECT_EQ(ide::render::serialize(ide::render::parse(s)), s);

    const Doc d2 = ide::render::parse("a\nb\n---");
    EXPECT_EQ(d2, Doc{{H(2, {T("a\nb")})}});
    const std::string s2 = ide::render::serialize(d2);
    EXPECT_EQ(ide::render::parse(s2), d2);
    EXPECT_EQ(ide::render::serialize(ide::render::parse(s2)), s2);
}

TEST(MarkdownSerialize, InlineMathEdges) {
    // math text with leading/trailing space only round-trips via $$
    const Doc d = ide::render::parse("$$ x $$");
    EXPECT_EQ(d, Doc{{P({Mt(" x ")})}});
    const std::string s = ide::render::serialize(d);
    EXPECT_EQ(ide::render::parse(s), d);
    EXPECT_EQ(ide::render::serialize(ide::render::parse(s)), s);

    // math never spans lines: this is literal text, and must stay stable
    const Doc d2 = ide::render::parse("$$a\nb$$");
    EXPECT_EQ(d2, Doc{{P({T("$$a\nb$$")})}});
    const std::string s2 = ide::render::serialize(d2);
    EXPECT_EQ(ide::render::parse(s2), d2);
    EXPECT_EQ(ide::render::serialize(ide::render::parse(s2)), s2);
}

TEST(MarkdownSerialize, EmptyDoc) {
    EXPECT_EQ(ide::render::serialize(ide::render::parse("")), "");
    EXPECT_EQ(ide::render::serialize(ide::render::parse("\n\n")), "");
}

// --- totality: hostile inputs must not crash -------------------------------

TEST(MarkdownParse, HugeInput) {
    std::string big(500000, 'a');
    big += "\n# h\n";
    const Doc d = ide::render::parse(big);
    ASSERT_EQ(d.blocks.size(), 2u);
}

TEST(MarkdownParse, DeepNestingIsCapped) {
    const std::string quotes(20000, '>');
    const Doc d = ide::render::parse(quotes + " x");
    // depth is capped; the innermost level degrades to a paragraph
    const Block* b = &d.blocks.at(0);
    int depth = 0;
    while (const BlockQuote* q = std::get_if<BlockQuote>(&b->node)) {
        ASSERT_FALSE(q->children.empty());
        b = &q->children.at(0);
        ++depth;
    }
    EXPECT_LE(depth, 66);
    EXPECT_TRUE(std::holds_alternative<Paragraph>(b->node));

    const std::string stars(5000, '*');
    const Doc d2 = ide::render::parse(stars + "x" + stars); // inline depth cap
    EXPECT_FALSE(d2.blocks.empty());

    const std::string indented(3000, ' ');
    const Doc d3 = ide::render::parse("- a\n" + indented + "- deep"); // list depth cap
    EXPECT_FALSE(d3.blocks.empty());
}

// --- fuzz infrastructure ----------------------------------------------------

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint64_t pick(uint64_t n) { return next() % n; }
};

const char* const kWords[] = {
    "lorem", "ipsum", "dolor", "x", "y", "alpha", "beta", "v2",
    "hello", "naive", "h\xc3\xa9llo", "w\xc3\xb6rld", "ok", "zz", "a", "i",
};

std::string word(Rng& rng) { return kWords[rng.pick(sizeof(kWords) / sizeof(kWords[0]))]; }

std::string inlineText(Rng& rng, int tokens) {
    std::string out;
    for (int k = 0; k < tokens; ++k) {
        if (k > 0) out += ' ';
        switch (rng.pick(11)) {
            case 0: out += word(rng); break;
            case 1: out += "*" + word(rng) + "*"; break;
            case 2: out += "**" + word(rng) + "**"; break;
            case 3: out += "~~" + word(rng) + "~~"; break;
            case 4: out += "`" + word(rng) + "`"; break;
            case 5: out += "$" + word(rng) + "$"; break;
            case 6: out += "[" + word(rng) + "](https://ex.com/" + word(rng) + ")"; break;
            case 7: out += "[" + word(rng) + "](pic" + std::to_string(rng.pick(9)) + ".png)"; break;
            case 8: out += "![" + word(rng) + "](./img/" + word(rng) + ".png)"; break;
            case 9: out += "[" + word(rng) + "](<" + word(rng) + " " + word(rng) +
                            ".md> \"the title\")"; break;
            case 10: out += word(rng) + "\\*"; break;
        }
    }
    return out;
}

std::string cellText(Rng& rng) {
    switch (rng.pick(5)) {
        case 0: return word(rng);
        case 1: return "*" + word(rng) + "*";
        case 2: return "**" + word(rng) + "**";
        case 3: return "`" + word(rng) + "`";
        default: return "$" + word(rng) + "$";
    }
}

std::string construct(Rng& rng) {
    switch (rng.pick(9)) {
        case 0: { // ATX heading
            const int level = 1 + static_cast<int>(rng.pick(6));
            return std::string(static_cast<size_t>(level), '#') + " " +
                   inlineText(rng, 1 + static_cast<int>(rng.pick(2)));
        }
        case 1: // setext heading
            return word(rng) + (rng.pick(2) == 0 ? "\n===" : "\n---");
        case 2: { // fenced block
            const char ch = (rng.pick(2) == 0) ? '`' : '~';
            const int count = 3 + static_cast<int>(rng.pick(2));
            const char* const infos[] = {"", "math", "mermaid", "rust", "c++"};
            std::string out(static_cast<size_t>(count), ch);
            out += infos[rng.pick(5)];
            out += '\n';
            const int lines = static_cast<int>(rng.pick(4));
            for (int k = 0; k < lines; ++k) {
                out += word(rng);
                out += '\n';
            }
            out.append(static_cast<size_t>(count), ch);
            return out;
        }
        case 3: { // thematic break
            switch (rng.pick(3)) {
                case 0: return "***";
                case 1: return "___";
                default: return "- - -";
            }
        }
        case 4: { // paragraph (first token is a plain word)
            std::string out = word(rng);
            const int extra = static_cast<int>(rng.pick(5));
            if (extra > 0) out += " " + inlineText(rng, extra);
            if (rng.pick(3) == 0) out += "\n" + word(rng);
            return out;
        }
        case 5: { // blockquote: plain words and ATX headings only
            std::string out;
            const int lines = 1 + static_cast<int>(rng.pick(3));
            for (int k = 0; k < lines; ++k) {
                if (k > 0) out += '\n';
                out += "> ";
                if (rng.pick(4) == 0) {
                    out += std::string(1 + rng.pick(3), '#');
                    out += ' ';
                }
                out += word(rng);
            }
            return out;
        }
        case 6: { // list
            const bool ordered = rng.pick(2) == 0;
            std::string out;
            const int items = 1 + static_cast<int>(rng.pick(4));
            for (int k = 0; k < items; ++k) {
                if (k > 0) out += '\n';
                out += ordered ? std::to_string(k + 1) + ". " : "- ";
                out += word(rng);
                if (rng.pick(4) == 0) out += "\n  - " + word(rng); // nested
                if (rng.pick(6) == 0) out += "\n  " + word(rng);   // continuation
            }
            return out;
        }
        case 7: { // table
            std::string out = "| " + cellText(rng) + " | " + cellText(rng) + " |";
            out += "\n|";
            for (int c = 0; c < 2; ++c) {
                switch (rng.pick(4)) {
                    case 0: out += " --- |"; break;
                    case 1: out += " :-- |"; break;
                    case 2: out += " :-: |"; break;
                    default: out += " --: |"; break;
                }
            }
            const int rows = static_cast<int>(rng.pick(4));
            for (int r = 0; r < rows; ++r) {
                out += "\n| " + cellText(rng) + " | " + cellText(rng) + " |";
            }
            return out;
        }
        default: // $$ math block
            return "$$\n" + word(rng) + "\n$$";
    }
}

std::string genDoc(Rng& rng) {
    std::string out;
    const int n = 1 + static_cast<int>(rng.pick(10));
    for (int k = 0; k < n; ++k) {
        if (k > 0) out += "\n\n";
        out += construct(rng);
    }
    return out;
}

std::string randomString(Rng& rng) {
    static const char alph[] = "ab \n#>`~*$[]()-_.|=!\\1'\":;,\r\txyz0";
    std::string out;
    const size_t len = 1 + rng.pick(150);
    out.reserve(len);
    for (size_t k = 0; k < len; ++k) {
        if (rng.pick(4) == 0) {
            out += static_cast<char>(rng.next() & 0xff);
        } else {
            out += alph[rng.pick(sizeof(alph) - 1)];
        }
    }
    return out;
}

// --- the naive reference: counts block constructs by line scanning ----------

struct NaiveCounts {
    int headings = 0, thematic = 0, code = 0, math = 0, mermaid = 0;
};

std::string trimCopy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

NaiveCounts naiveCount(const std::string& src) {
    std::vector<std::string> lines;
    {
        size_t start = 0;
        for (;;) {
            const size_t nl = src.find('\n', start);
            std::string line = src.substr(start, (nl == std::string::npos)
                                                     ? std::string::npos
                                                     : nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    NaiveCounts c;
    bool inFence = false;
    char fch = 0;
    int fcount = 0;
    bool inMath = false;
    bool prevContent = false;
    for (std::string line : lines) {
        // strip blockquote markers (all levels, like the real parser recurses)
        for (;;) {
            size_t i = 0;
            while (i < line.size() && line[i] == ' ') ++i;
            if (i <= 3 && i < line.size() && line[i] == '>') {
                size_t j = i + 1;
                if (j < line.size() && line[j] == ' ') ++j;
                line = line.substr(j);
            } else {
                break;
            }
        }
        const std::string t = trimCopy(line);
        if (inFence) {
            if (!t.empty()) {
                bool all = true;
                for (char ch : t) {
                    if (ch != fch) {
                        all = false;
                        break;
                    }
                }
                if (all && static_cast<int>(t.size()) >= fcount) inFence = false;
            }
            continue;
        }
        if (inMath) {
            if (t == "$$") inMath = false;
            continue;
        }
        if (t.empty()) {
            prevContent = false;
            continue;
        }
        // fence open?
        {
            size_t i = 0;
            while (i < line.size() && line[i] == ' ') ++i;
            if (i <= 3 && i < line.size() && (line[i] == '`' || line[i] == '~')) {
                const char fc = line[i];
                size_t j = i;
                while (j < line.size() && line[j] == fc) ++j;
                if (static_cast<int>(j - i) >= 3) {
                    const std::string info = trimCopy(line.substr(j));
                    if (!(fc == '`' && info.find('`') != std::string::npos)) {
                        const size_t sp = info.find_first_of(" \t");
                        const std::string lang = (sp == std::string::npos)
                                                     ? info
                                                     : info.substr(0, sp);
                        if (lang == "math") ++c.math;
                        else if (lang == "mermaid") ++c.mermaid;
                        else ++c.code;
                        inFence = true;
                        fch = fc;
                        fcount = static_cast<int>(j - i);
                        prevContent = false;
                        continue;
                    }
                }
            }
        }
        if (t == "$$") {
            ++c.math;
            inMath = true;
            prevContent = false;
            continue;
        }
        // setext underline under content?
        if (prevContent && (t[0] == '=' || t[0] == '-') &&
            t.find_first_not_of(t[0]) == std::string::npos) {
            ++c.headings;
            prevContent = false;
            continue;
        }
        // ATX?
        if (t[0] == '#') {
            size_t k = 0;
            while (k < t.size() && t[k] == '#') ++k;
            if (k <= 6 && (k == t.size() || t[k] == ' ')) {
                ++c.headings;
                prevContent = false;
                continue;
            }
        }
        // thematic?
        {
            char m = 0;
            int cnt = 0;
            bool ok = true;
            for (char ch : t) {
                if (ch == ' ') continue;
                if (ch != '-' && ch != '_' && ch != '*') {
                    ok = false;
                    break;
                }
                if (m == 0) m = ch;
                else if (ch != m) {
                    ok = false;
                    break;
                }
                ++cnt;
            }
            if (ok && cnt >= 3) {
                ++c.thematic;
                prevContent = false;
                continue;
            }
        }
        prevContent = true;
    }
    return c;
}

// deep count over the parsed tree
struct TreeCounts {
    int headings = 0, thematic = 0, code = 0, math = 0, mermaid = 0;
};

void countBlocks(const std::vector<Block>& blocks, TreeCounts& c) {
    for (const Block& b : blocks) {
        std::visit(
            [&](const auto& v) {
                using Ty = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<Ty, ThematicBreak>) {
                    ++c.thematic;
                } else if constexpr (std::is_same_v<Ty, Heading>) {
                    ++c.headings;
                } else if constexpr (std::is_same_v<Ty, CodeBlock>) {
                    ++c.code;
                } else if constexpr (std::is_same_v<Ty, MathBlock>) {
                    ++c.math;
                } else if constexpr (std::is_same_v<Ty, MermaidBlock>) {
                    ++c.mermaid;
                } else if constexpr (std::is_same_v<Ty, BlockQuote>) {
                    countBlocks(v.children, c);
                } else if constexpr (std::is_same_v<Ty, List>) {
                    for (const ListItem& it : v.items) countBlocks(it.children, c);
                }
            },
            b.node);
    }
}

// --- fuzz tests -------------------------------------------------------------

TEST(MarkdownFuzz, DifferentialAgainstNaiveReference) {
    Rng rng(0xC0FE1234ull);
    for (int iter = 0; iter < 600; ++iter) {
        const std::string src = genDoc(rng);
        const Doc d = ide::render::parse(src);
        TreeCounts tc;
        countBlocks(d.blocks, tc);
        const NaiveCounts nc = naiveCount(src);
        EXPECT_EQ(tc.headings, nc.headings) << "headings differ, source:\n" << src;
        EXPECT_EQ(tc.thematic, nc.thematic) << "thematic differ, source:\n" << src;
        EXPECT_EQ(tc.code, nc.code) << "code fences differ, source:\n" << src;
        EXPECT_EQ(tc.math, nc.math) << "math blocks differ, source:\n" << src;
        EXPECT_EQ(tc.mermaid, nc.mermaid) << "mermaid blocks differ, source:\n" << src;
    }
}

TEST(MarkdownFuzz, GeneratedRoundTripStability) {
    Rng rng(0xBEEF0001ull);
    for (int iter = 0; iter < 600; ++iter) {
        const std::string src = genDoc(rng);
        const std::string s1 = ide::render::serialize(ide::render::parse(src));
        const std::string s2 = ide::render::serialize(ide::render::parse(s1));
        EXPECT_EQ(s1, s2) << "round trip unstable, source:\n" << src;
    }
}

TEST(MarkdownFuzz, RandomBytesRoundTripStability) {
    Rng rng(0xDEAD0002ull);
    for (int iter = 0; iter < 1500; ++iter) {
        const std::string src = randomString(rng);
        const std::string s1 = ide::render::serialize(ide::render::parse(src));
        const std::string s2 = ide::render::serialize(ide::render::parse(s1));
        EXPECT_EQ(s1, s2) << "round trip unstable, source:\n" << src;
    }
}

} // namespace
