#include <ide/render/markdown.h>

// Why this file exists
// -------------------
// Implementation of the CommonMark-subset markdown parser declared in
// ide/render/markdown.h. Everything here is pure string manipulation: no
// allocation beyond the tree itself, no IO, no exceptions. Every function is
// total — arbitrary bytes produce a tree, never a crash.
//
// Structure: block parsing runs over a flat array of lines (splitLines) and
// recurses for containers (blockquotes, list items) by building sub-line
// arrays. Inline parsing runs over the joined text of one paragraph-shaped
// region. Recursion depth is capped at kMaxDepth so hostile nesting
// (">>>>>>...", deeply indented lists, "***...***") degrades to text instead
// of overflowing the stack.
//
// Block precedence order in the main loop matters and mirrors CommonMark
// closely enough for the subset: fence > blank > setext-underline (with a
// pending paragraph) > ATX heading > blockquote > thematic break > $$
// math fence > list marker > table delimiter > paragraph line.

#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace ide::render {
namespace {

constexpr int kMaxDepth = 64;

// --- small helpers ----------------------------------------------------------

bool isBlank(std::string_view s) {
    for (char c : s) {
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

std::string_view ltrim(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string_view rtrim(std::string_view s) {
    size_t n = s.size();
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) --n;
    return s.substr(0, n);
}

std::string_view trim(std::string_view s) { return rtrim(ltrim(s)); }

bool isAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

bool isAsciiAlnum(char c) { return isAsciiAlpha(c) || isAsciiDigit(c); }

/// ASCII punctuation, the CommonMark escape set.
bool isPunct(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

bool isSpaceLike(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

size_t indentOf(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return i;
}

// --- line splitting ---------------------------------------------------------

struct Line {
    std::string_view text; ///< content without the terminator and any trailing '\r'
    size_t begin = 0;      ///< byte offset of the first content byte
    size_t rawEnd = 0;     ///< byte offset one past the line terminator (or EOF)
};

std::vector<Line> splitLines(std::string_view src) {
    std::vector<Line> lines;
    size_t i = 0;
    while (i <= src.size()) {
        if (i == src.size()) break;
        const size_t start = i;
        const size_t nl = src.find('\n', i);
        const size_t end = (nl == std::string_view::npos) ? src.size() : nl;
        std::string_view text = src.substr(start, end - start);
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        lines.push_back(Line{text, start, (nl == std::string_view::npos) ? src.size() : nl + 1});
        if (nl == std::string_view::npos) break;
        i = nl + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string_view>& parts) {
    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) out += '\n';
        out.append(parts[k]);
    }
    return out;
}

// --- block-level recognizers ------------------------------------------------

struct FenceOpen {
    char ch = '`';
    int count = 3;
    std::string_view info;
};

std::optional<FenceOpen> parseFenceOpen(std::string_view line) {
    if (indentOf(line) > 3) return std::nullopt;
    const size_t ind = indentOf(line);
    if (ind >= line.size()) return std::nullopt;
    const char c = line[ind];
    if (c != '`' && c != '~') return std::nullopt;
    int count = 0;
    size_t i = ind;
    while (i < line.size() && line[i] == c) {
        ++i;
        ++count;
    }
    if (count < 3) return std::nullopt;
    std::string_view info = trim(line.substr(i));
    // CommonMark: the info string of a backtick fence may not contain '`'.
    if (c == '`' && info.find('`') != std::string_view::npos) return std::nullopt;
    return FenceOpen{c, count, info};
}

bool isFenceClose(std::string_view line, char ch, int count) {
    const std::string_view t = trim(line);
    if (static_cast<int>(t.size()) < count) return false;
    for (char c : t) {
        if (c != ch) return false;
    }
    return true;
}

/// ATX heading: 0-3 spaces, 1-6 '#', then space/tab or end of line.
/// On success returns the level and writes the trimmed content (closing
/// '#' run removed) into *content.
std::optional<int> parseAtx(std::string_view line, std::string_view* content) {
    const size_t ind = indentOf(line);
    if (ind > 3 || ind >= line.size()) return std::nullopt;
    if (line[ind] != '#') return std::nullopt;
    size_t i = ind;
    int level = 0;
    while (i < line.size() && line[i] == '#') {
        ++i;
        ++level;
    }
    if (level > 6) return std::nullopt;
    if (i < line.size() && line[i] != ' ' && line[i] != '\t') return std::nullopt;
    std::string_view t = trim(line.substr(i));
    // Remove a closing '#' run, but only if it is separated by whitespace or
    // covers the whole content.
    size_t endp = t.size();
    while (endp > 0 && t[endp - 1] == '#') --endp;
    if (endp < t.size() && (endp == 0 || t[endp - 1] == ' ' || t[endp - 1] == '\t')) {
        t = trim(t.substr(0, endp));
    }
    *content = t;
    return level;
}

/// Thematic break: 3+ occurrences of '-', '_' or '*' (spaces allowed between,
/// all the same marker char).
bool isThematic(std::string_view line) {
    const std::string_view t = trim(line);
    char marker = 0;
    int count = 0;
    for (char c : t) {
        if (c == ' ' || c == '\t') continue;
        if (c != '-' && c != '_' && c != '*') return false;
        if (marker == 0) marker = c;
        else if (c != marker) return false;
        ++count;
    }
    return count >= 3;
}

/// Setext underline: a non-empty line consisting solely of '=' (level 1) or
/// solely of '-' (level 2). Whether it *is* one depends on a pending
/// paragraph — that check lives in the main loop.
int setextLevel(std::string_view line) {
    const std::string_view t = trim(line);
    if (t.empty()) return 0;
    const char c = t[0];
    if (c != '=' && c != '-') return 0;
    for (char ch : t) {
        if (ch != c) return 0;
    }
    return (c == '=') ? 1 : 2;
}

/// Strips one blockquote marker (0-3 spaces, '>', one optional space).
std::optional<std::string_view> stripQuote(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i > 3 || i >= line.size() || line[i] != '>') return std::nullopt;
    ++i;
    if (i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

struct ListMarker {
    bool ordered = false;
    size_t indent = 0;      ///< leading spaces before the marker
    size_t contentCol = 0;  ///< column where the item's content starts
    std::string_view rest;  ///< content of the marker line after the marker
};

std::optional<ListMarker> parseListMarker(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i > 3 || i >= line.size()) return std::nullopt;
    const char c = line[i];
    if (c == '-' || c == '+' || c == '*') {
        const size_t after = i + 1;
        if (after < line.size() && line[after] != ' ' && line[after] != '\t') {
            return std::nullopt; // e.g. "*em" or "-foo" — not a marker
        }
        size_t j = after;
        while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
        const size_t contentCol = (j < line.size()) ? j : after + 1;
        return ListMarker{false, i, contentCol, line.substr(j)};
    }
    if (isAsciiDigit(c)) {
        size_t j = i;
        while (j < line.size() && isAsciiDigit(line[j])) ++j;
        if (j - i > 9 || j >= line.size()) return std::nullopt;
        if (line[j] != '.' && line[j] != ')') return std::nullopt;
        const size_t after = j + 1;
        if (after < line.size() && line[after] != ' ' && line[after] != '\t') {
            return std::nullopt; // e.g. "1.5" — a number, not a marker
        }
        size_t k = after;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        const size_t contentCol = (k < line.size()) ? k : after + 1;
        return ListMarker{true, i, contentCol, line.substr(k)};
    }
    return std::nullopt;
}

// --- table helpers ----------------------------------------------------------

/// Splits a table row on unescaped '|'; drops one leading and one trailing
/// pipe. Cells keep surrounding whitespace (trimmed by the caller when the
/// cell content is parsed). '\|' survives as two raw characters and is
/// unescaped by the inline parser.
std::vector<std::string_view> splitPipes(std::string_view line) {
    std::string_view t = trim(line);
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);
    std::vector<std::string_view> cells;
    size_t start = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '|' && (i == 0 || t[i - 1] != '\\')) {
            cells.push_back(t.substr(start, i - start));
            start = i + 1;
        }
    }
    cells.push_back(t.substr(start));
    return cells;
}

/// GFM delimiter row: trimmed line containing '|', whose cells (after outer
/// pipe stripping) each look like ':---:'.
std::optional<std::vector<Alignment>> parseDelimiterRow(std::string_view line) {
    const std::string_view t = trim(line);
    if (t.find('|') == std::string_view::npos) return std::nullopt;
    const std::vector<std::string_view> cells = splitPipes(t);
    if (cells.empty()) return std::nullopt;
    std::vector<Alignment> aligns;
    for (std::string_view cell : cells) {
        std::string_view c = trim(cell);
        if (c.empty()) return std::nullopt;
        const bool left = (c.front() == ':');
        const bool right = (c.back() == ':');
        if (left && c.size() < 2) return std::nullopt; // lone ':' is not a cell
        if (right && c.size() < 2) return std::nullopt;
        const size_t midBegin = left ? 1 : 0;
        const size_t midEnd = c.size() - (right ? 1 : 0);
        if (midEnd <= midBegin) return std::nullopt;
        for (size_t k = midBegin; k < midEnd; ++k) {
            if (c[k] != '-') return std::nullopt;
        }
        if (left && right) aligns.push_back(Alignment::Center);
        else if (left) aligns.push_back(Alignment::Left);
        else if (right) aligns.push_back(Alignment::Right);
        else aligns.push_back(Alignment::None);
    }
    return aligns;
}

// --- inline parsing ---------------------------------------------------------

std::vector<Inline> parseInlines(std::string_view text, int depth);

/// Finds the start of a closing delimiter run of `c`, searched from `from`.
/// The scan is escape-aware (a backslash pair is skipped) so an escaped
/// delimiter can never close — this keeps serialize/parse round trips
/// stable. A run only closes if the char before it is not whitespace; for
/// '_' the char after the run must not be alphanumeric (no intraword
/// underscore emphasis). `maxLen` caps the accepted run length: emphasis
/// accepts exactly one delimiter (a longer run is strong-emphasis territory
/// and is skipped), strong accepts any run of >= 2. Returns npos if none.
size_t findClosingRun(std::string_view text, char c, size_t from, int minLen, int maxLen,
                       bool underscore) {
    const size_t n = text.size();
    size_t p = from;
    while (p < n) {
        if (text[p] == '\\') {
            p += 2;
            continue;
        }
        if (text[p] == c) {
            size_t r = p;
            while (r < n && text[r] == c) ++r;
            const bool leftOk = (p == 0) ? false : !isSpaceLike(text[p - 1]);
            // after-run boundary for '_': next char (if any) not alphanumeric
            bool rightOk = true;
            if (underscore && r < n && isAsciiAlnum(text[r])) rightOk = false;
            const int len = static_cast<int>(r - p);
            const bool lenOk = (len >= minLen) && (maxLen < 0 || len <= maxLen);
            if (lenOk && leftOk && rightOk) return p;
            p = r;
        } else {
            ++p;
        }
    }
    return std::string_view::npos;
}

/// Unescapes a raw span (used for image alt text): backslash + ASCII
/// punctuation folds to the punctuation.
std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && isPunct(s[i + 1])) {
            out += s[i + 1];
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<Inline> parseInlines(std::string_view text, int depth) {
    std::vector<Inline> out;
    std::string lit;
    const size_t n = text.size();
    const bool capped = depth >= kMaxDepth;

    auto flush = [&]() {
        if (!lit.empty()) {
            out.push_back(Inline{Text{std::move(lit)}});
            lit.clear();
        }
    };

    size_t i = 0;
    while (i < n) {
        const char c = text[i];

        // backslash escape
        if (c == '\\' && i + 1 < n && isPunct(text[i + 1])) {
            lit += text[i + 1];
            i += 2;
            continue;
        }

        // code span: backtick run closed by a run of the same length
        if (c == '`') {
            size_t k = i;
            while (k < n && text[k] == '`') ++k;
            size_t p = k;
            bool found = false;
            while (p < n) {
                if (text[p] == '`') {
                    size_t r = p;
                    while (r < n && text[r] == '`') ++r;
                    if (r - p == k - i) {
                        found = true;
                        break;
                    }
                    p = r;
                } else {
                    ++p;
                }
            }
            if (found) {
                std::string_view content = text.substr(k, p - k);
                if (content.size() >= 2 && content.front() == ' ' &&
                    content.back() == ' ' && content.find_first_not_of(' ') != std::string_view::npos) {
                    content = content.substr(1, content.size() - 2);
                }
                flush();
                out.push_back(Inline{Code{std::string(content)}});
                i = p + (k - i);
                continue;
            }
            lit.append(k - i, '`');
            i = k;
            continue;
        }

        // inline math
        if (c == '$') {
            if (i + 1 < n && text[i + 1] == '$') {
                // `$$...$$` inline math. The content may not start with '$'
                // (that would make the serialization start with "$$\$" and
                // re-parse differently) and may not be empty.
                if (i + 2 < n && text[i + 2] != '$') {
                    const size_t close = text.find("$$", i + 2);
                    if (close != std::string_view::npos && close > i + 2) {
                        flush();
                        out.push_back(Inline{InlineMath{std::string(text.substr(i + 2, close - (i + 2)))}});
                        i = close + 2;
                        continue;
                    }
                }
                lit += "$$";
                i += 2;
                continue;
            }
            // `$...$` — the opening '$' must be followed by a non-space char
            // that is neither '$' (handled above) nor '\\' (an escaped '$'
            // right after would be swallowed as content otherwise).
            if (i + 1 < n && !isSpaceLike(text[i + 1]) && text[i + 1] != '\\') {
                size_t j = i + 2;
                while (j < n) {
                    if (text[j] == '$' && !isSpaceLike(text[j - 1])) break;
                    ++j;
                }
                if (j < n) {
                    flush();
                    out.push_back(Inline{InlineMath{std::string(text.substr(i + 1, j - (i + 1)))}});
                    i = j + 1;
                    continue;
                }
            }
            lit += '$';
            ++i;
            continue;
        }

        // GFM strikethrough
        if (c == '~' && i + 1 < n && text[i + 1] == '~') {
            const size_t close = text.find("~~", i + 2);
            if (close != std::string_view::npos && !capped) {
                flush();
                out.push_back(Inline{Strike{parseInlines(text.substr(i + 2, close - (i + 2)), depth + 1)}});
                i = close + 2;
                continue;
            }
            lit += "~~";
            i += 2;
            continue;
        }

        // link / image
        if (c == '[' || (c == '!' && i + 1 < n && text[i + 1] == '[')) {
            const bool isImage = (c == '!');
            const size_t open = isImage ? i + 1 : i;
            // find matching ']'
            size_t j = open + 1;
            int bracketDepth = 1;
            while (j < n) {
                if (text[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (text[j] == '[') ++bracketDepth;
                else if (text[j] == ']') {
                    --bracketDepth;
                    if (bracketDepth == 0) break;
                }
                ++j;
            }
            if (j < n && text[j] == ']' && j + 1 < n && text[j + 1] == '(') {
                std::string target;
                std::string title;
                bool ok = true;
                size_t q = j + 2;
                while (q < n && (text[q] == ' ' || text[q] == '\t')) ++q;
                if (q < n && text[q] == '<') {
                    size_t r = q + 1;
                    while (r < n && text[r] != '>') ++r;
                    if (r < n) {
                        target.assign(text.substr(q + 1, r - (q + 1)));
                        q = r + 1;
                    } else {
                        ok = false;
                    }
                } else {
                    size_t r = q;
                    while (r < n && text[r] != ' ' && text[r] != '\t' && text[r] != ')' && text[r] != '\n') ++r;
                    if (r > q) target.assign(text.substr(q, r - q));
                    q = r;
                }
                if (ok) {
                    size_t s = q;
                    while (s < n && (text[s] == ' ' || text[s] == '\t')) ++s;
                    if (s < n && (text[s] == '"' || text[s] == '\'')) {
                        const char quote = text[s];
                        size_t r = s + 1;
                        while (r < n && text[r] != quote) {
                            if (text[r] == '\\' && r + 1 < n && isPunct(text[r + 1])) r += 2;
                            else ++r;
                        }
                        if (r < n) {
                            title = unescape(text.substr(s + 1, r - (s + 1)));
                            q = r + 1;
                        } else {
                            ok = false;
                        }
                    }
                }
                if (ok) {
                    size_t u = q;
                    while (u < n && (text[u] == ' ' || text[u] == '\t')) ++u;
                    if (u < n && text[u] == ')') {
                        const std::string_view inner = text.substr(open + 1, j - (open + 1));
                        const TargetKind kind = classifyTarget(target);
                        flush();
                        if (isImage) {
                            out.push_back(Inline{Image{unescape(inner), std::move(target),
                                                       std::move(title), kind}});
                        } else {
                            out.push_back(Inline{Link{parseInlines(inner, depth + 1),
                                                      std::move(target), std::move(title), kind}});
                        }
                        i = u + 1;
                        continue;
                    }
                }
            }
            // not a link/image after all — literal
            lit.append(text.substr(i, isImage ? 2 : 1));
            i += isImage ? 2 : 1;
            continue;
        }

        // emphasis / strong
        if ((c == '*' || c == '_') && !capped) {
            size_t k = i;
            while (k < n && text[k] == c) ++k;
            bool canOpen = true;
            if (c == '_') {
                canOpen = (i == 0) || !isAsciiAlnum(text[i - 1]);
            }
            if (k < n && isSpaceLike(text[k])) canOpen = false;
            if (canOpen) {
                if (k - i >= 2) {
                    const size_t close = findClosingRun(text, c, k, 2, -1, c == '_');
                    if (close != std::string_view::npos) {
                        flush();
                        out.push_back(Inline{Strong{parseInlines(text.substr(i + 2, close - (i + 2)), depth + 1)}});
                        i = close + 2;
                        continue;
                    }
                }
                const size_t close = findClosingRun(text, c, i + 1, 1, 1, c == '_');
                if (close != std::string_view::npos) {
                    flush();
                    out.push_back(Inline{Emph{parseInlines(text.substr(i + 1, close - (i + 1)), depth + 1)}});
                    i = close + 1;
                    continue;
                }
            }
            lit.append(k - i, c);
            i = k;
            continue;
        }

        lit += c;
        ++i;
    }
    flush();
    return out;
}

// --- block parsing ----------------------------------------------------------

/// Parses `lines` into blocks. When `spans` is non-null, a (firstLine,
/// lastLineExclusive) pair is pushed for every block emitted, parallel to
/// `out` — only the top level uses it, so nested calls pass nullptr.
void parseBlocks(std::span<const Line> lines, int depth, std::vector<Block>* out,
                 std::vector<std::pair<size_t, size_t>>* spans) {
    const size_t n = lines.size();
    const bool capped = depth >= kMaxDepth;
    size_t i = 0;

    std::vector<std::string_view> para;     // pending paragraph lines
    std::vector<size_t> paraLineIdx;        // their indices into `lines`

    auto flushPara = [&]() {
        if (!para.empty()) {
            out->push_back(Block{Paragraph{parseInlines(joinLines(para), depth)}});
            if (spans) spans->emplace_back(paraLineIdx.front(), paraLineIdx.back() + 1);
            para.clear();
            paraLineIdx.clear();
        }
    };

    while (i < n) {
        const std::string_view line = lines[i].text;

        // 1. fenced code / math / mermaid
        if (!capped) {
            if (auto f = parseFenceOpen(line)) {
                flushPara();
                const size_t start = i;
                ++i;
                std::vector<std::string_view> content;
                while (i < n && !isFenceClose(lines[i].text, f->ch, f->count)) {
                    content.push_back(lines[i].text);
                    ++i;
                }
                if (i < n) ++i; // consume the closing fence
                const std::string_view info = f->info;
                std::string_view lang = info;
                const size_t sp = info.find_first_of(" \t");
                if (sp != std::string_view::npos) lang = info.substr(0, sp);
                const std::string code = joinLines(content);
                if (lang == "math") {
                    out->push_back(Block{MathBlock{code}});
                } else if (lang == "mermaid") {
                    out->push_back(Block{MermaidBlock{code}});
                } else {
                    out->push_back(Block{CodeBlock{std::string(lang), code}});
                }
                if (spans) spans->emplace_back(start, i);
                continue;
            }
        }

        // 2. blank line
        if (isBlank(line)) {
            flushPara();
            ++i;
            continue;
        }

        // 3. setext underline under a pending paragraph
        if (!para.empty()) {
            const int level = setextLevel(line);
            if (level != 0) {
                out->push_back(Block{Heading{level, parseInlines(joinLines(para), depth)}});
                if (spans) spans->emplace_back(paraLineIdx.front(), i + 1);
                para.clear();
                paraLineIdx.clear();
                ++i;
                continue;
            }
        }

        // 4. ATX heading
        {
            std::string_view content;
            if (auto level = parseAtx(line, &content)) {
                flushPara();
                out->push_back(Block{Heading{*level, parseInlines(content, depth)}});
                if (spans) spans->emplace_back(i, i + 1);
                ++i;
                continue;
            }
        }

        // 5. blockquote
        if (!capped && stripQuote(line).has_value()) {
            flushPara();
            const size_t start = i;
            std::vector<Line> sub;
            while (i < n && stripQuote(lines[i].text).has_value()) {
                sub.push_back(Line{*stripQuote(lines[i].text), 0, 0});
                ++i;
            }
            std::vector<Block> children;
            parseBlocks(sub, depth + 1, &children, nullptr);
            out->push_back(Block{BlockQuote{std::move(children)}});
            if (spans) spans->emplace_back(start, i);
            continue;
        }

        // 6. thematic break
        if (isThematic(line)) {
            flushPara();
            out->push_back(Block{ThematicBreak{}});
            if (spans) spans->emplace_back(i, i + 1);
            ++i;
            continue;
        }

        // 7. $$ ... $$ math block
        if (!capped && trim(line) == "$$") {
            flushPara();
            const size_t start = i;
            ++i;
            std::vector<std::string_view> content;
            while (i < n && trim(lines[i].text) != "$$") {
                content.push_back(lines[i].text);
                ++i;
            }
            if (i < n) ++i; // consume the closing $$
            out->push_back(Block{MathBlock{joinLines(content)}});
            if (spans) spans->emplace_back(start, i);
            continue;
        }

        // 8. list
        if (!capped) {
            if (auto marker = parseListMarker(line)) {
                // A list marker interrupts a paragraph in this subset; the
                // pending paragraph is flushed first.
                flushPara();
                const bool ordered = marker->ordered;
                // --- collect the list region ---
                size_t j = i;
                std::vector<Line> region;
                while (j < n) {
                    const std::string_view t = lines[j].text;
                    if (isBlank(t)) {
                        // blank belongs only if the list continues after it
                        size_t k = j;
                        while (k < n && isBlank(lines[k].text)) ++k;
                        if (k < n) {
                            auto mk = parseListMarker(lines[k].text);
                            const size_t ind = indentOf(lines[k].text);
                            if (ind >= 2 || (mk && mk->indent <= 3)) {
                                region.push_back(lines[j]);
                                ++j;
                                continue;
                            }
                        }
                        break;
                    }
                    auto mk = parseListMarker(t);
                    if (mk && mk->indent <= 3) {
                        if (mk->ordered != ordered) break;
                        region.push_back(lines[j]);
                        ++j;
                        continue;
                    }
                    if (indentOf(t) >= 2) {
                        region.push_back(lines[j]);
                        ++j;
                        continue;
                    }
                    break;
                }
                // --- split region into items ---
                std::vector<ListItem> items;
                size_t r = 0;
                while (r < region.size()) {
                    auto mk = parseListMarker(region[r].text);
                    if (!mk || mk->indent > 3 || mk->ordered != ordered) {
                        // stray continuation line (shouldn't happen) — stop
                        break;
                    }
                    const size_t contentCol = mk->contentCol;
                    std::vector<Line> sub;
                    if (!mk->rest.empty()) {
                        sub.push_back(Line{mk->rest, 0, 0});
                    }
                    ++r;
                    while (r < region.size()) {
                        auto next = parseListMarker(region[r].text);
                        // A marker indented less than this item's content
                        // column is a sibling item; at or beyond it, the line
                        // is indented item content (nested constructs live
                        // there after dedenting).
                        if (next && next->indent < contentCol && next->ordered == ordered) break;
                        std::string_view t = region[r].text;
                        const size_t ind = indentOf(t);
                        const size_t strip = (ind < contentCol) ? ind : contentCol;
                        sub.push_back(Line{t.substr(strip), 0, 0});
                        ++r;
                    }
                    std::vector<Block> children;
                    parseBlocks(sub, depth + 1, &children, nullptr);
                    items.push_back(ListItem{std::move(children)});
                }
                out->push_back(Block{List{ordered, std::move(items)}});
                if (spans) spans->emplace_back(i, j);
                i = j;
                continue;
            }
        }

        // 9. table: delimiter row under a pending paragraph line
        if (!para.empty()) {
            auto aligns = parseDelimiterRow(line);
            if (aligns) {
                const std::string_view headerLine = para.back();
                const std::vector<std::string_view> headerCells = splitPipes(headerLine);
                if (headerCells.size() == aligns->size()) {
                    const size_t headerIdx = paraLineIdx.back();
                    if (para.size() > 1) {
                        // earlier lines are a separate paragraph
                        std::vector<std::string_view> before(para.begin(), para.end() - 1);
                        out->push_back(Block{Paragraph{parseInlines(joinLines(before), depth)}});
                        if (spans) {
                            spans->emplace_back(paraLineIdx.front(),
                                                paraLineIdx[paraLineIdx.size() - 2] + 1);
                        }
                    }
                    para.clear();
                    paraLineIdx.clear();
                    Table table;
                    table.align = *aligns;
                    for (std::string_view cell : headerCells) {
                        table.header.push_back(TableCell{parseInlines(trim(cell), depth)});
                    }
                    ++i; // consume the delimiter row
                    std::string_view atxProbe;
                    while (i < n && !isBlank(lines[i].text) &&
                           lines[i].text.find('|') != std::string_view::npos &&
                           !parseFenceOpen(lines[i].text) &&
                           !parseAtx(lines[i].text, &atxProbe) &&
                           !stripQuote(lines[i].text) && !isThematic(lines[i].text) &&
                           !parseListMarker(lines[i].text)) {
                        auto rowCells = splitPipes(lines[i].text);
                        rowCells.resize(table.align.size());
                        std::vector<TableCell> row;
                        for (std::string_view cell : rowCells) {
                            row.push_back(TableCell{parseInlines(trim(cell), depth)});
                        }
                        table.rows.push_back(std::move(row));
                        ++i;
                    }
                    out->push_back(Block{std::move(table)});
                    if (spans) spans->emplace_back(headerIdx, i);
                    continue;
                }
            }
        }

        // 10. paragraph line
        para.push_back(line);
        paraLineIdx.push_back(i);
        ++i;
    }
    flushPara();
}

} // namespace

// --- public API -------------------------------------------------------------

TargetKind classifyTarget(std::string_view target) {
    if (target.empty()) return TargetKind::Relative;
    if (isAsciiAlpha(target[0])) {
        size_t i = 1;
        while (i < target.size() &&
               (isAsciiAlnum(target[i]) || target[i] == '+' || target[i] == '-' || target[i] == '.')) {
            ++i;
        }
        if (i < target.size() && target[i] == ':') {
            std::string scheme;
            scheme.reserve(i);
            for (size_t k = 0; k < i; ++k) {
                scheme += static_cast<char>(target[k] >= 'A' && target[k] <= 'Z'
                                                ? target[k] - 'A' + 'a'
                                                : target[k]);
            }
            if (scheme == "https") return TargetKind::Https;
            if (scheme == "http") return TargetKind::Http;
            if (scheme == "file") return TargetKind::File;
            return TargetKind::Other;
        }
    }
    return TargetKind::Relative;
}

Doc parse(std::string_view source) {
    Doc doc;
    parseBlocks(splitLines(source), 0, &doc.blocks, nullptr);
    return doc;
}

std::vector<BlockSpan> blockSpans(std::string_view source) {
    const std::vector<Line> lines = splitLines(source);
    std::vector<Block> blocks;
    std::vector<std::pair<size_t, size_t>> spans;
    parseBlocks(lines, 0, &blocks, &spans);
    std::vector<BlockSpan> out;
    out.reserve(spans.size());
    for (const auto& [a, b] : spans) {
        // lastLine = b-1; guard b==0 cannot happen (every span covers >= 1 line)
        const size_t last = (b == 0) ? 0 : b - 1;
        out.push_back(BlockSpan{lines[a].begin, lines[last].rawEnd});
    }
    return out;
}

// --- serialization ----------------------------------------------------------

namespace {

void appendEscaped(std::string& out, std::string_view t) {
    for (char c : t) {
        switch (c) {
            case '\\': case '`': case '*': case '_': case '~':
            case '$': case '[': case ']': case '!': case '|':
                out += '\\';
                break;
            default:
                break;
        }
        out += c;
    }
}

void serializeInlines(std::string& out, const std::vector<Inline>& children);

void serializeInline(std::string& out, const Inline& node) {
    auto text = [&](const std::string& s) { appendEscaped(out, s); };
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Text>) {
                text(v.text);
            } else if constexpr (std::is_same_v<T, Code>) {
                size_t maxRun = 0, run = 0;
                for (char c : v.text) {
                    if (c == '`') {
                        if (++run > maxRun) maxRun = run;
                    } else {
                        run = 0;
                    }
                }
                const size_t fence = maxRun + 1;
                const bool pad = !v.text.empty() && (v.text.front() == '`' || v.text.back() == '`');
                out.append(fence, '`');
                if (pad) out += ' ';
                out += v.text;
                if (pad) out += ' ';
                out.append(fence, '`');
            } else if constexpr (std::is_same_v<T, InlineMath>) {
                out += '$';
                out += v.text;
                out += '$';
            } else if constexpr (std::is_same_v<T, Emph>) {
                out += '*';
                serializeInlines(out, v.children);
                out += '*';
            } else if constexpr (std::is_same_v<T, Strong>) {
                out += "**";
                serializeInlines(out, v.children);
                out += "**";
            } else if constexpr (std::is_same_v<T, Strike>) {
                out += "~~";
                serializeInlines(out, v.children);
                out += "~~";
            } else if constexpr (std::is_same_v<T, Link>) {
                out += '[';
                serializeInlines(out, v.children);
                out += "](";
                if (v.target.find_first_of(" \t)\n") != std::string::npos) {
                    out += '<';
                    out += v.target;
                    out += '>';
                } else {
                    out += v.target;
                }
                if (!v.title.empty()) {
                    out += " \"";
                    for (char c : v.title) {
                        if (c == '"' || c == '\\') out += '\\';
                        out += c;
                    }
                    out += '"';
                }
                out += ')';
            } else if constexpr (std::is_same_v<T, Image>) {
                out += "![";
                appendEscaped(out, v.alt);
                out += "](";
                if (v.target.find_first_of(" \t)\n") != std::string::npos) {
                    out += '<';
                    out += v.target;
                    out += '>';
                } else {
                    out += v.target;
                }
                if (!v.title.empty()) {
                    out += " \"";
                    for (char c : v.title) {
                        if (c == '"' || c == '\\') out += '\\';
                        out += c;
                    }
                    out += '"';
                }
                out += ')';
            }
        },
        node.node);
}

void serializeInlines(std::string& out, const std::vector<Inline>& children) {
    for (const Inline& c : children) serializeInline(out, c);
}

/// Fenced block emitter shared by CodeBlock / MathBlock / MermaidBlock.
/// The fence is always backticks and always longer than any backtick run
/// inside the content, so the content can never terminate the fence early.
void appendFenced(std::string& out, std::string_view lang, const std::string& code) {
    size_t maxRun = 0, run = 0;
    for (char c : code) {
        if (c == '`') {
            if (++run > maxRun) maxRun = run;
        } else {
            run = 0;
        }
    }
    const size_t fence = maxRun + 3;
    out.append(fence, '`');
    out += lang;
    out += '\n';
    size_t start = 0;
    for (;;) {
        const size_t nl = code.find('\n', start);
        if (nl == std::string::npos) {
            out.append(code, start, std::string::npos);
            out += '\n';
            break;
        }
        out.append(code, start, nl - start);
        out += '\n';
        start = nl + 1;
    }
    out.append(fence, '`');
}

/// Paragraphs need a leading-character guard so the serialized first line
/// cannot be re-read as a heading / quote / list / fence underline. Every
/// guard inserts a backslash before ASCII punctuation, which the inline
/// parser folds back, so the tree is preserved exactly.
std::string guardParagraphStart(std::string_view s) {
    if (s.empty()) return std::string(s);
    const char c = s[0];
    if (c == '#' || c == '>' || c == '=') {
        return "\\" + std::string(s);
    }
    if (c == '-' || c == '+') {
        if (s.size() == 1 || s[1] == ' ' || s[1] == '\t' || s[1] == '\n') {
            return "\\" + std::string(s);
        }
    }
    if (isAsciiDigit(c)) {
        size_t d = 0;
        while (d < s.size() && isAsciiDigit(s[d])) ++d;
        if (d <= 9 && d < s.size() && (s[d] == '.' || s[d] == ')') &&
            (s.size() == d + 1 || s[d + 1] == ' ' || s[d + 1] == '\t' || s[d + 1] == '\n')) {
            std::string out(s.substr(0, d));
            out += '\\';
            out += s[d];
            out.append(s.substr(d + 1));
            return out;
        }
    }
    return std::string(s);
}

void serializeBlocks(std::string& out, const std::vector<Block>& blocks);

void serializeBlock(std::string& out, const Block& block) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, ThematicBreak>) {
                out += "---";
            } else if constexpr (std::is_same_v<T, Heading>) {
                out.append(static_cast<size_t>(v.level), '#');
                out += ' ';
                serializeInlines(out, v.children);
            } else if constexpr (std::is_same_v<T, Paragraph>) {
                std::string inner;
                serializeInlines(inner, v.children);
                out += guardParagraphStart(inner);
            } else if constexpr (std::is_same_v<T, CodeBlock>) {
                appendFenced(out, v.lang, v.code);
            } else if constexpr (std::is_same_v<T, MathBlock>) {
                appendFenced(out, "math", v.code);
            } else if constexpr (std::is_same_v<T, MermaidBlock>) {
                appendFenced(out, "mermaid", v.code);
            } else if constexpr (std::is_same_v<T, BlockQuote>) {
                std::string inner;
                serializeBlocks(inner, v.children);
                if (inner.empty()) {
                    out += '>';
                } else {
                    size_t start = 0;
                    for (;;) {
                        const size_t nl = inner.find('\n', start);
                        const size_t end = (nl == std::string::npos) ? inner.size() : nl;
                        out += '>';
                        if (end > start) out += ' ';
                        out.append(inner, start, end - start);
                        if (nl == std::string::npos) break;
                        out += '\n';
                        start = nl + 1;
                    }
                }
            } else if constexpr (std::is_same_v<T, List>) {
                for (size_t idx = 0; idx < v.items.size(); ++idx) {
                    std::string marker = v.ordered ? std::to_string(idx + 1) + ". " : "- ";
                    std::string content;
                    serializeBlocks(content, v.items[idx].children);
                    if (idx > 0) out += '\n';
                    if (content.empty()) {
                        // bare marker (rtrim the trailing space)
                        out.append(marker, 0, marker.size() - 1);
                        continue;
                    }
                    size_t start = 0;
                    bool first = true;
                    for (;;) {
                        const size_t nl = content.find('\n', start);
                        const size_t end = (nl == std::string::npos) ? content.size() : nl;
                        if (first) {
                            out += marker;
                            first = false;
                        } else {
                            out.append(marker.size(), ' ');
                        }
                        out.append(content, start, end - start);
                        if (nl == std::string::npos) break;
                        out += '\n';
                        start = nl + 1;
                    }
                }
            } else if constexpr (std::is_same_v<T, Table>) {
                const auto alignCell = [](Alignment a) -> const char* {
                    switch (a) {
                        case Alignment::Left: return ":--";
                        case Alignment::Center: return ":-:";
                        case Alignment::Right: return "--:";
                        default: return "---";
                    }
                };
                const auto emitRow = [&](const std::vector<TableCell>& row) {
                    out += '|';
                    for (const TableCell& cell : row) {
                        out += ' ';
                        serializeInlines(out, cell.children);
                        out += " |";
                    }
                    out += '\n';
                };
                emitRow(v.header);
                out += '|';
                for (Alignment a : v.align) {
                    out += ' ';
                    out += alignCell(a);
                    out += " |";
                }
                out += '\n';
                for (const auto& row : v.rows) emitRow(row);
                // trim one trailing newline (blocks are joined with \n\n)
                if (!out.empty() && out.back() == '\n') out.pop_back();
            }
        },
        block.node);
}

void serializeBlocks(std::string& out, const std::vector<Block>& blocks) {
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i > 0) out += "\n\n";
        serializeBlock(out, blocks[i]);
    }
}

} // namespace

std::string serialize(const Doc& doc) {
    std::string out;
    serializeBlocks(out, doc.blocks);
    return out;
}

} // namespace ide::render
