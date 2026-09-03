#pragma once

// Markdown tree -> a flat instruction stream for the Kotlin preview renderer.
//
// Why a stream and not JNI objects: the tree has 9 block kinds and 8 inline
// kinds, and building it as Java objects would mean ~17 class lookups, ~17
// constructor ids and one JNI call per node — hundreds of calls for one README.
// A preorder byte stream crosses in a single call and the Compose side rebuilds
// it with a stack, which is less code on both sides and immeasurably faster.
//
// Format: one record per line, fields tab-separated. Text payloads are escaped
// (\\ \n \t) so the framing can never be broken by document content.
//
//   b <kind> <arg>   begin block   ("b\th\t2", "b\tcode\trust", "b\tcell\tc")
//   e                end block
//   t <mask> <href> <text>   an inline run; mask is kStyle* bits OR'd
//
// The parser on the other side is dev/cope/ide/markdown/MarkdownStream.kt; the
// two must be changed together.

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <ide/render/markdown.h>

namespace cope::md {

// Style bits for a `t` record. Must match MarkdownStream.kt.
inline constexpr unsigned kStyleBold = 1u;
inline constexpr unsigned kStyleItalic = 2u;
inline constexpr unsigned kStyleStrike = 4u;
inline constexpr unsigned kStyleCode = 8u;
inline constexpr unsigned kStyleLink = 16u;
inline constexpr unsigned kStyleImage = 32u;
inline constexpr unsigned kStyleMath = 64u;

inline void escapeInto(std::string_view text, std::string& out) {
    for (const char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                break;  // dropped: CR is never meaningful inside a run
            default:
                out.push_back(c);
        }
    }
}

inline void beginBlock(std::string& out, std::string_view kind, std::string_view arg) {
    out += "b\t";
    out.append(kind);
    out.push_back('\t');
    escapeInto(arg, out);
    out.push_back('\n');
}

inline void endBlock(std::string& out) { out += "e\n"; }

inline void run(std::string& out, unsigned mask, std::string_view href, std::string_view text) {
    out += "t\t";
    out += std::to_string(mask);
    out.push_back('\t');
    escapeInto(href, out);
    out.push_back('\t');
    escapeInto(text, out);
    out.push_back('\n');
}

inline void emitInlines(const std::vector<ide::render::Inline>& nodes, unsigned mask,
                        std::string_view href, std::string& out);

inline void emitInline(const ide::render::Inline& node, unsigned mask, std::string_view href,
                       std::string& out) {
    using namespace ide::render;
    if (const auto* text = std::get_if<Text>(&node.node)) {
        run(out, mask, href, text->text);
    } else if (const auto* code = std::get_if<Code>(&node.node)) {
        run(out, mask | kStyleCode, href, code->text);
    } else if (const auto* math = std::get_if<InlineMath>(&node.node)) {
        run(out, mask | kStyleMath, href, math->text);
    } else if (const auto* emph = std::get_if<Emph>(&node.node)) {
        emitInlines(emph->children, mask | kStyleItalic, href, out);
    } else if (const auto* strong = std::get_if<Strong>(&node.node)) {
        emitInlines(strong->children, mask | kStyleBold, href, out);
    } else if (const auto* strike = std::get_if<Strike>(&node.node)) {
        emitInlines(strike->children, mask | kStyleStrike, href, out);
    } else if (const auto* link = std::get_if<Link>(&node.node)) {
        if (link->children.empty()) {
            run(out, mask | kStyleLink, link->target, link->target);
        } else {
            emitInlines(link->children, mask | kStyleLink, link->target, out);
        }
    } else if (const auto* image = std::get_if<Image>(&node.node)) {
        run(out, mask | kStyleImage, image->target, image->alt);
    }
}

inline void emitInlines(const std::vector<ide::render::Inline>& nodes, unsigned mask,
                        std::string_view href, std::string& out) {
    for (const ide::render::Inline& node : nodes) {
        emitInline(node, mask, href, out);
    }
}

inline void emitBlocks(const std::vector<ide::render::Block>& blocks, std::string& out);

inline char alignChar(ide::render::Alignment align) {
    switch (align) {
        case ide::render::Alignment::Left:
            return 'l';
        case ide::render::Alignment::Center:
            return 'c';
        case ide::render::Alignment::Right:
            return 'r';
        case ide::render::Alignment::None:
        default:
            return 'n';
    }
}

inline void emitCells(const std::vector<ide::render::TableCell>& cells,
                      const std::vector<ide::render::Alignment>& align, std::string& out) {
    for (size_t i = 0; i < cells.size(); ++i) {
        const char a = i < align.size() ? alignChar(align[i]) : 'n';
        beginBlock(out, "cell", std::string_view(&a, 1));
        emitInlines(cells[i].children, 0u, std::string_view(), out);
        endBlock(out);
    }
}

inline void emitBlock(const ide::render::Block& block, std::string& out) {
    using namespace ide::render;
    if (std::get_if<ThematicBreak>(&block.node) != nullptr) {
        beginBlock(out, "hr", std::string_view());
        endBlock(out);
    } else if (const auto* heading = std::get_if<Heading>(&block.node)) {
        beginBlock(out, "h", std::to_string(heading->level));
        emitInlines(heading->children, 0u, std::string_view(), out);
        endBlock(out);
    } else if (const auto* paragraph = std::get_if<Paragraph>(&block.node)) {
        beginBlock(out, "p", std::string_view());
        emitInlines(paragraph->children, 0u, std::string_view(), out);
        endBlock(out);
    } else if (const auto* code = std::get_if<CodeBlock>(&block.node)) {
        beginBlock(out, "code", code->lang);
        run(out, kStyleCode, std::string_view(), code->code);
        endBlock(out);
    } else if (const auto* math = std::get_if<MathBlock>(&block.node)) {
        beginBlock(out, "math", std::string_view());
        run(out, kStyleMath, std::string_view(), math->code);
        endBlock(out);
    } else if (const auto* mermaid = std::get_if<MermaidBlock>(&block.node)) {
        beginBlock(out, "mermaid", std::string_view());
        run(out, kStyleCode, std::string_view(), mermaid->code);
        endBlock(out);
    } else if (const auto* quote = std::get_if<BlockQuote>(&block.node)) {
        beginBlock(out, "quote", std::string_view());
        emitBlocks(quote->children, out);
        endBlock(out);
    } else if (const auto* list = std::get_if<List>(&block.node)) {
        beginBlock(out, list->ordered ? "ol" : "ul", std::string_view());
        for (const ListItem& item : list->items) {
            beginBlock(out, "li", std::string_view());
            emitBlocks(item.children, out);
            endBlock(out);
        }
        endBlock(out);
    } else if (const auto* table = std::get_if<Table>(&block.node)) {
        beginBlock(out, "table", std::string_view());
        beginBlock(out, "thead", std::string_view());
        emitCells(table->header, table->align, out);
        endBlock(out);
        for (const std::vector<TableCell>& row : table->rows) {
            beginBlock(out, "trow", std::string_view());
            emitCells(row, table->align, out);
            endBlock(out);
        }
        endBlock(out);
    }
}

inline void emitBlocks(const std::vector<ide::render::Block>& blocks, std::string& out) {
    for (const ide::render::Block& block : blocks) {
        emitBlock(block, out);
    }
}

/// Whole-document entry point.
[[nodiscard]] inline std::string streamOf(const ide::render::Doc& doc) {
    std::string out;
    out.reserve(4096);
    emitBlocks(doc.blocks, out);
    return out;
}

}  // namespace cope::md
