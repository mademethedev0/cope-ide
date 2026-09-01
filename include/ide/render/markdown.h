#pragma once

// Why this file exists
// -------------------
// The markdown renderer (design: docs/design/markdown-renderer.md) splits the
// world into a pure string->tree parser that lives in core (this file) and
// frontends (Compose renderer, CLI ANSI renderer, WebView math islands) that
// walk the tree. Keeping the tree here means one parser, many renderers, and
// the parser stays testable on CI with zero Android / JNI / filesystem /
// network dependencies.
//
// Scope of the parser (documented CommonMark subset + GFM):
//   blocks  - ATX + setext headings, paragraphs, fenced code blocks (``` and
//             ~~~) with info strings, blockquotes, nested UL/OL lists,
//             thematic breaks, GFM pipe tables, `$$`-fenced math blocks
//   special - a fence whose info string is `math` or `mermaid` produces a
//             MathBlock / MermaidBlock carrying the raw source for the
//             WebView islands to render
//   inline  - emphasis, strong, GFM strikethrough, code spans, links and
//             images with optional titles, inline math $...$ / $$...$$,
//             backslash escapes
//
// Deliberately NOT in this subset: reference links, HTML blocks, link
// reference definitions, tabs as indentation, lazy blockquote/list
// continuation. Anything unrecognized degrades to literal text, never to a
// crash: the parser is total over arbitrary bytes.
//
// All offsets in this API are byte offsets into the source string. Line
// numbering is 0-based. The tree is value-semantic (plain structs, defaulted
// equality) so it can be copied, diffed and compared in tests.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ide::render {

/// How a link/image target *looks*, classified purely from the raw string
/// shape (scheme prefix / path shape). Classification carries no policy: the
/// UI layer decides what Relative / Https / etc. means for the user (see the
/// permission-gate design). No filesystem access happens here.
enum class TargetKind : uint8_t {
    Relative, ///< no scheme: relative path, anchor, or empty
    File,     ///< file: scheme
    Https,    ///< https: scheme
    Http,     ///< http: scheme
    Other,    ///< any other scheme (mailto:, intent:, unknown...)
};

/// Column alignment of one GFM table column, from the delimiter row.
enum class Alignment : uint8_t { None, Left, Center, Right };

struct Inline;
struct Block;

// --- Inline nodes -----------------------------------------------------------

/// A run of literal text. Backslash escapes have already been folded in, so
/// `\*` parses to Text("*"). Soft line breaks survive as '\n' inside Text.
struct Text {
    std::string text;
    bool operator==(const Text&) const = default;
};

/// An inline code span. Content is verbatim (no escape processing inside).
struct Code {
    std::string text;
    bool operator==(const Code&) const = default;
};

/// Inline math `$...$` (or `$$...$$`), raw source for the math island.
struct InlineMath {
    std::string text;
    bool operator==(const InlineMath&) const = default;
};

/// Emphasis `*x*` / `_x_`.
struct Emph {
    std::vector<Inline> children;
    bool operator==(const Emph&) const = default;
};

/// Strong emphasis `**x**` / `__x__`.
struct Strong {
    std::vector<Inline> children;
    bool operator==(const Strong&) const = default;
};

/// GFM strikethrough `~~x~~`.
struct Strike {
    std::vector<Inline> children;
    bool operator==(const Strike&) const = default;
};

/// A link. `target` is the raw destination string; `title` may be empty.
struct Link {
    std::vector<Inline> children;
    std::string target;
    std::string title;
    TargetKind kind = TargetKind::Relative;
    bool operator==(const Link&) const = default;
};

/// An image. The alt text is kept as a plain string (not parsed as inline
/// nodes) — it is used for placeholders and accessibility, not styled.
struct Image {
    std::string alt;
    std::string target;
    std::string title;
    TargetKind kind = TargetKind::Relative;
    bool operator==(const Image&) const = default;
};

/// Value-semantic inline node: a tagged union of the inline types above.
struct Inline {
    std::variant<Text, Code, InlineMath, Emph, Strong, Strike, Link, Image> node;
    bool operator==(const Inline&) const = default;
};

// --- Block nodes ------------------------------------------------------------

/// A thematic break (`---`, `***`, `___`). Carries no data.
struct ThematicBreak {
    bool operator==(const ThematicBreak&) const = default;
};

/// ATX (`# x`) or setext (`x\n===`) heading. `level` is 1..6.
struct Heading {
    int level = 1;
    std::vector<Inline> children;
    bool operator==(const Heading&) const = default;
};

/// A paragraph. Multi-line paragraphs keep their soft breaks as '\n' inside
/// the Text children; the lines are not merged.
struct Paragraph {
    std::vector<Inline> children;
    bool operator==(const Paragraph&) const = default;
};

/// A fenced code block. `lang` is the first word of the info string (may be
/// empty); `code` is the verbatim content between the fences, lines joined
/// with '\n' (no trailing newline).
struct CodeBlock {
    std::string lang;
    std::string code;
    bool operator==(const CodeBlock&) const = default;
};

/// Math block: fence with info `math`, or a `$$` ... `$$` block. `code` is
/// the raw LaTeX source for the WebView math island.
struct MathBlock {
    std::string code;
    bool operator==(const MathBlock&) const = default;
};

/// Diagram block: fence with info `mermaid`. Raw source for the diagram
/// island.
struct MermaidBlock {
    std::string code;
    bool operator==(const MermaidBlock&) const = default;
};

/// A blockquote. Children are the blocks parsed from the quoted region.
struct BlockQuote {
    std::vector<Block> children;
    bool operator==(const BlockQuote&) const = default;
};

/// One list item: the blocks parsed from the item's content lines.
struct ListItem {
    std::vector<Block> children;
    bool operator==(const ListItem&) const = default;
};

/// A bullet (`ordered == false`) or ordered list. Ordered numbering restarts
/// per list; the serializer emits 1., 2., ... regardless of source numbers.
struct List {
    bool ordered = false;
    std::vector<ListItem> items;
    bool operator==(const List&) const = default;
};

/// One table cell (header or body); inline content only.
struct TableCell {
    std::vector<Inline> children;
    bool operator==(const TableCell&) const = default;
};

/// A GFM pipe table. `align` has one entry per column; every body row has
/// exactly `align.size()` cells (rows are not ragged: short rows are padded
/// with empty cells by the parser, long rows are trimmed).
struct Table {
    std::vector<TableCell> header;
    std::vector<Alignment> align;
    std::vector<std::vector<TableCell>> rows;
    bool operator==(const Table&) const = default;
};

/// Value-semantic block node: a tagged union of the block types above.
struct Block {
    std::variant<ThematicBreak, Heading, Paragraph, CodeBlock, MathBlock,
                 MermaidBlock, BlockQuote, List, Table>
        node;
    bool operator==(const Block&) const = default;
};

/// The whole document: a flat list of top-level blocks.
struct Doc {
    std::vector<Block> blocks;
    bool operator==(const Doc&) const = default;
};

// --- API --------------------------------------------------------------------

/// Parses `source` (arbitrary bytes, UTF-8 or not) into a document tree.
/// Total: never crashes, never throws for any input; unrecognized syntax
/// degrades to literal text.
[[nodiscard]] Doc parse(std::string_view source);

/// Serializes a document back to canonical markdown. Intended for round-trip
/// stability testing and the CLI renderer's degrade paths; `parse(serialize(d))`
/// reproduces `d` for every tree `parse` can produce (see tests). Known
/// non-representable shapes (empty inline math, '|' inside code spans in
/// table cells, '$' inside inline math) never arise from `parse` itself.
[[nodiscard]] std::string serialize(const Doc& doc);

/// Classifies a raw link/image target by its string shape alone. Pure
/// function, no filesystem or network access, no policy.
[[nodiscard]] TargetKind classifyTarget(std::string_view target);

/// Byte range of one top-level block. `begin` is the first byte of the
/// block's first line; `end` is one past the block's last byte and includes
/// that line's terminator when one exists. Blocks tile the non-blank source
/// in order; blank lines between blocks belong to no span.
struct BlockSpan {
    size_t begin = 0;
    size_t end = 0;
    bool operator==(const BlockSpan&) const = default;
};

/// Byte ranges of the top-level blocks of `source`, in document order.
/// This is the hook future incremental re-parsing will key off: an edit that
/// stays inside one span only requires re-parsing that block.
[[nodiscard]] std::vector<BlockSpan> blockSpans(std::string_view source);

} // namespace ide::render
