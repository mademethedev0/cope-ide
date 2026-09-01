// A wide regression net for the theme engine: ~40 realistic tokenColors rules
// resolved against 50 scope stacks, checked against a naive reference resolver
// written independently in this file (brute-force placement search, tuple
// comparison, linear-scan interning). The differential check catches ranking and
// interning regressions that hand-written expectations would miss, and a handful
// of literal StyleId assertions pin the id *ordering* so a change in first-touch
// order cannot slip through.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <ide/theme/theme.h>

namespace {

using ide::theme::FontStyle;
using ide::theme::kDefaultStyleId;
using ide::theme::parseColor;
using ide::theme::Rgba;
using ide::theme::Style;
using ide::theme::StyleId;
using ide::theme::Theme;

using Stack = std::vector<std::string_view>;

// --- the theme under test -----------------------------------------------------

struct RuleSpec {
    const char* scope;
    const char* foreground;  ///< nullptr: rule says nothing about the foreground
    const char* fontStyle;   ///< nullptr: rule says nothing about decorations
};

constexpr const char* kDefaultFg = "#E0E0E0";
constexpr const char* kDefaultBg = "#101010";

const RuleSpec kRules[] = {
    /*  0 */ {"comment", "#6A9955", "italic"},
    /*  1 */ {"comment.line.double-slash.js", "#5A8945", nullptr},
    /*  2 */ {"constant", "#B5CEA8", nullptr},
    /*  3 */ {"constant.language", "#569CD6", nullptr},
    /*  4 */ {"constant.numeric", "#B5CEA8", nullptr},
    /*  5 */ {"entity.name.function", "#DCDCAA", nullptr},
    /*  6 */ {"entity.name.type", "#4EC9B0", nullptr},
    /*  7 */ {"entity.name.tag", "#569CD6", nullptr},
    /*  8 */ {"entity.other.attribute-name", "#9CDCFE", nullptr},
    /*  9 */ {"invalid", nullptr, "bold italic underline"},
    /* 10 */ {"keyword", "#569CD6", nullptr},
    /* 11 */ {"keyword.control", "#C586C0", nullptr},
    /* 12 */ {"keyword.operator", "#D4D4D4", nullptr},
    /* 13 */ {"markup.bold", nullptr, "bold"},
    /* 14 */ {"markup.italic", nullptr, "italic"},
    /* 15 */ {"markup.heading", "#569CD6", "bold"},
    /* 16 */ {"meta.embedded, source.groovy.embedded", "#D4D4D4", nullptr},
    /* 17 */ {"punctuation.definition.string", "#CE9178", nullptr},
    /* 18 */ {"storage", "#569CD6", nullptr},
    /* 19 */ {"storage.type", "#569CD6", nullptr},
    /* 20 */ {"storage.modifier", "#569CD6", nullptr},
    /* 21 */ {"string", "#CE9178", nullptr},
    /* 22 */ {"string.quoted.double.js", "#CE9178", nullptr},
    /* 23 */ {"string.regexp", "#D16969", nullptr},
    /* 24 */ {"support.function", "#DCDCAA", nullptr},
    /* 25 */ {"support.type.property-name", "#9CDCFE", nullptr},
    /* 26 */ {"variable", "#9CDCFE", nullptr},
    /* 27 */ {"variable.other.constant", "#4FC1FF", nullptr},
    /* 28 */ {"variable.parameter", "#9CDCFE", nullptr},
    /* 29 */ {"source.js meta.function entity.name", "#FFD700", nullptr},
    /* 30 */ {"source.ts keyword", "#FF8C00", nullptr},
    /* 31 */ {"text.html source.js", "#A0A0A0", nullptr},
    /* 32 */ {"source.js -comment", nullptr, "underline"},
    /* 33 */ {"comment markup.link", "#6A9955", "underline"},
    /* 34 */ {"meta.tag entity.name.tag", "#4EC9B0", nullptr},
    /* 35 */ {"string meta.embedded", "#D7BA7D", nullptr},
    /* 36 */ {"constant.character.escape", "#D7BA7D", nullptr},
    /* 37 */ {"keyword.control.flow.js", "#C586C0", "bold"},
    /* 38 */ {"punctuation", "#808080", nullptr},
    /* 39 */ {"emphasis", nullptr, ""},
};

constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

std::string buildThemeJson() {
    std::string out;
    out += R"({"name":"smoke","type":"dark","colors":{"editor.background":")";
    out += kDefaultBg;
    out += R"(","editor.foreground":")";
    out += kDefaultFg;
    out += R"("},"tokenColors":[)";
    for (size_t i = 0; i < kRuleCount; ++i) {
        if (i != 0) out += ',';
        out += R"({"scope":")";
        out += kRules[i].scope;
        out += R"(","settings":{)";
        bool wrote = false;
        if (kRules[i].foreground != nullptr) {
            out += R"("foreground":")";
            out += kRules[i].foreground;
            out += '"';
            wrote = true;
        }
        if (kRules[i].fontStyle != nullptr) {
            if (wrote) out += ',';
            out += R"("fontStyle":")";
            out += kRules[i].fontStyle;
            out += '"';
        }
        out += "}}";
    }
    out += "]}";
    return out;
}

std::vector<Stack> makeStacks() {
    return {
        /*  0 */ Stack{},
        /*  1 */ Stack{"source.py", "comment.line.number-sign.py"},
        /*  2 */ Stack{"source.py", "keyword.control.import.py"},
        /*  3 */ Stack{"source.py", "comment.block.py"},
        /*  4 */ Stack{"source.py", "string.quoted.single.py"},
        /*  5 */ Stack{"source.js", "comment.line.double-slash.js"},
        /*  6 */ Stack{"source.js", "keyword.control.flow.js"},
        /*  7 */ Stack{"source.js", "meta.function.js", "entity.name.function.js"},
        /*  8 */ Stack{"source.ts", "keyword.control.ts"},
        /*  9 */ Stack{"text.html.basic", "meta.tag.any.html", "entity.name.tag.html"},
        /* 10 */ Stack{"text.html.basic", "source.js.embedded.html", "keyword.control.js"},
        /* 11 */ Stack{"source.css", "meta.property-list.css", "support.type.property-name.css"},
        /* 12 */ Stack{"source.css", "punctuation.separator.key-value.css"},
        /* 13 */ Stack{"source.js", "string.quoted.double.js"},
        /* 14 */ Stack{"source.js", "string.quoted.double.js", "punctuation.definition.string.begin.js"},
        /* 15 */ Stack{"source.js", "invalid.illegal.js"},
        /* 16 */ Stack{"markup.bold"},
        /* 17 */ Stack{"markup.italic"},
        /* 18 */ Stack{"markup.heading.1.markdown"},
        /* 19 */ Stack{"emphasis"},
        /* 20 */ Stack{"source.js", "meta.embedded.block.html", "string.quoted.double.html"},
        /* 21 */ Stack{"text.html.basic", "string.quoted.double.html", "meta.embedded.line.js"},
        /* 22 */ Stack{"source.js", "constant.numeric.decimal.js"},
        /* 23 */ Stack{"source.js", "constant.language.boolean.true.js"},
        /* 24 */ Stack{"source.js", "string.quoted.single.js", "constant.character.escape.js"},
        /* 25 */ Stack{"source.js", "string.regexp.js"},
        /* 26 */ Stack{"source.js", "support.function.console.js"},
        /* 27 */ Stack{"source.js", "variable.other.readwrite.js"},
        /* 28 */ Stack{"source.js", "variable.other.constant.js"},
        /* 29 */ Stack{"source.js", "variable.parameter.js"},
        /* 30 */ Stack{"source.java", "storage.type.java"},
        /* 31 */ Stack{"source.java", "storage.modifier.java"},
        /* 32 */ Stack{"source.java", "entity.name.type.class.java"},
        /* 33 */ Stack{"source.py", "keyword.operator.arithmetic.py"},
        /* 34 */ Stack{"source.html", "entity.other.attribute-name.html"},
        /* 35 */ Stack{"source.unknown", "nothing.matches.here"},
        /* 36 */ Stack{"source.js"},
        /* 37 */ Stack{"source.js", "comment.block.documentation.js", "markup.link.js"},
        /* 38 */ Stack{"source.js", "meta.function.js", "meta.block.js", "entity.name.function.js"},
        /* 39 */ Stack{"source.ts", "meta.class.ts", "entity.name.type.class.ts"},
        /* 40 */ Stack{"source.ts", "keyword.operator.assignment.ts"},
        /* 41 */ Stack{"source.groovy.embedded.html", "keyword.control.groovy"},
        /* 42 */ Stack{"source.md", "markup.heading.markdown", "punctuation.definition.heading.markdown"},
        /* 43 */ Stack{"source.md", "markup.bold.markdown", "markup.italic.markdown"},
        /* 44 */ Stack{"source.js", "invalid.deprecated.js"},
        /* 45 */ Stack{"source.py", "string.quoted.docstring.multi.py", "comment.block.py"},
        /* 46 */ Stack{"source.js", "meta.embedded.block.js"},
        /* 47 */ Stack{"source.py", "entity.name.function.decorator.py"},
        /* 48 */ Stack{"source.js", "punctuation.terminator.statement.js"},
        /* 49 */ Stack{"source.py", "support.type.property-name.py", "string.quoted.single.py"},
    };
}

// --- naive reference implementation -------------------------------------------

using Segments = std::vector<std::string>;

std::vector<std::string_view> splitOn(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        const size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

Segments toSegments(std::string_view scope) {
    Segments segments;
    for (const std::string_view part : splitOn(scope, '.')) segments.emplace_back(part);
    return segments;
}

/// One comma branch of a rule's scope text. The smoke rules deliberately use only
/// the syntax this simple parser understands: commas, spaces and leading '-'.
struct RefAlternative {
    std::vector<Segments> path;
    std::vector<Segments> exclusions;
};

std::vector<RefAlternative> refParse(std::string_view scopeText) {
    std::vector<RefAlternative> alternatives;
    for (const std::string_view branch : splitOn(scopeText, ',')) {
        RefAlternative alternative;
        for (const std::string_view word : splitOn(branch, ' ')) {
            if (word.empty()) continue;
            if (word.front() == '-') {
                if (word.size() > 1) alternative.exclusions.push_back(toSegments(word.substr(1)));
            } else {
                alternative.path.push_back(toSegments(word));
            }
        }
        if (!alternative.path.empty()) alternatives.push_back(std::move(alternative));
    }
    return alternatives;
}

bool refPatternMatch(const Segments& pattern, const Segments& scope) {
    if (pattern.size() > scope.size()) return false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != scope[i]) return false;
    }
    return true;
}

/// Can path[0 .. count-1] be placed, in order, at stack indices below `limit`?
bool refFits(const std::vector<Segments>& path, size_t count, const std::vector<Segments>& stack, size_t limit) {
    if (count == 0) return true;
    for (size_t i = limit; i-- > 0;) {
        if (refPatternMatch(path[count - 1], stack[i]) && refFits(path, count - 1, stack, i)) return true;
    }
    return false;
}

int64_t refDeepest(const std::vector<Segments>& path, const std::vector<Segments>& stack) {
    if (path.empty()) return -1;
    for (size_t last = stack.size(); last-- > 0;) {
        if (!refPatternMatch(path.back(), stack[last])) continue;
        if (refFits(path, path.size() - 1, stack, last)) return static_cast<int64_t>(last);
    }
    return -1;
}

struct RefKey {
    size_t path = 0;
    size_t depth = 0;
    size_t segments = 0;
};

bool refKeyLess(const RefKey& lhs, const RefKey& rhs) {
    return std::tie(lhs.path, lhs.depth, lhs.segments) < std::tie(rhs.path, rhs.depth, rhs.segments);
}

RefKey refRuleKey(const std::vector<RefAlternative>& alternatives, const std::vector<Segments>& stack) {
    RefKey best;
    for (const RefAlternative& alternative : alternatives) {
        const int64_t deepest = refDeepest(alternative.path, stack);
        if (deepest < 0) continue;

        bool excluded = false;
        for (const Segments& clause : alternative.exclusions) {
            for (const Segments& element : stack) {
                if (refPatternMatch(clause, element)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) break;
        }
        if (excluded) continue;

        RefKey key;
        key.path = alternative.path.size();
        key.depth = static_cast<size_t>(deepest) + 1u;
        for (const Segments& pattern : alternative.path) key.segments += pattern.size();
        if (refKeyLess(best, key)) best = key;
    }
    return best;
}

uint8_t refFontMask(std::string_view spec) {
    uint8_t mask = 0;
    for (const std::string_view word : splitOn(spec, ' ')) {
        if (word == "bold") mask = static_cast<uint8_t>(mask | 1u);
        if (word == "italic") mask = static_cast<uint8_t>(mask | 2u);
        if (word == "underline") mask = static_cast<uint8_t>(mask | 4u);
        if (word == "strikethrough") mask = static_cast<uint8_t>(mask | 8u);
    }
    return mask;
}

struct RefStyle {
    Rgba fg{};
    Rgba bg{};
    uint8_t fontStyle = 0;
    bool hasFg = false;
    bool hasBg = false;
};

bool refSameStyle(const RefStyle& lhs, const RefStyle& rhs) {
    if (lhs.fontStyle != rhs.fontStyle) return false;
    if (lhs.hasFg != rhs.hasFg || lhs.hasBg != rhs.hasBg) return false;
    if (lhs.hasFg && !(lhs.fg == rhs.fg)) return false;
    if (lhs.hasBg && !(lhs.bg == rhs.bg)) return false;
    return true;
}

Rgba refColor(const char* hex) {
    const std::optional<Rgba> parsed = parseColor(hex);
    EXPECT_TRUE(parsed.has_value()) << "bad fixture colour " << hex;
    return parsed.value_or(Rgba{0, 0, 0, 0});
}

RefStyle refResolve(const std::vector<std::vector<RefAlternative>>& parsedRules, const Stack& stack,
                    const RefStyle& base) {
    std::vector<Segments> stackSegments;
    stackSegments.reserve(stack.size());
    for (const std::string_view scope : stack) stackSegments.push_back(toSegments(scope));

    RefKey fgKey;
    RefKey fontKey;
    const RuleSpec* fgRule = nullptr;
    const RuleSpec* fontRule = nullptr;

    for (size_t i = 0; i < kRuleCount; ++i) {
        const RefKey key = refRuleKey(parsedRules[i], stackSegments);
        if (key.path == 0) continue;  // no match
        if (kRules[i].foreground != nullptr && !refKeyLess(key, fgKey)) {
            fgKey = key;
            fgRule = &kRules[i];
        }
        if (kRules[i].fontStyle != nullptr && !refKeyLess(key, fontKey)) {
            fontKey = key;
            fontRule = &kRules[i];
        }
    }

    RefStyle style = base;
    if (fgRule != nullptr) {
        style.fg = refColor(fgRule->foreground);
        style.hasFg = true;
    }
    if (fontRule != nullptr) style.fontStyle = refFontMask(fontRule->fontStyle);
    return style;
}

// --- the test -----------------------------------------------------------------

TEST(ThemeSmokeTest, FortyRulesAgainstFiftyStacks) {
    const std::string json = buildThemeJson();
    std::string error;
    const std::optional<Theme> loaded = Theme::fromJsonText(json, &error);
    ASSERT_TRUE(loaded.has_value()) << error << "\n" << json;
    const Theme& theme = *loaded;
    ASSERT_EQ(theme.rules().size(), kRuleCount);

    std::vector<std::vector<RefAlternative>> parsedRules;
    parsedRules.reserve(kRuleCount);
    for (size_t i = 0; i < kRuleCount; ++i) parsedRules.push_back(refParse(kRules[i].scope));

    RefStyle base;
    base.fg = refColor(kDefaultFg);
    base.hasFg = true;
    base.bg = refColor(kDefaultBg);
    base.hasBg = true;

    std::vector<RefStyle> refPalette{base};
    const std::vector<Stack> stacks = makeStacks();
    ASSERT_EQ(stacks.size(), 50u);

    std::vector<StyleId> ids;
    ids.reserve(stacks.size());

    for (size_t s = 0; s < stacks.size(); ++s) {
        const Stack& stack = stacks[s];
        const RefStyle expected = refResolve(parsedRules, stack, base);

        StyleId expectedId = -1;
        for (size_t p = 0; p < refPalette.size(); ++p) {
            if (refSameStyle(refPalette[p], expected)) {
                expectedId = static_cast<StyleId>(p);
                break;
            }
        }
        if (expectedId < 0) {
            expectedId = static_cast<StyleId>(refPalette.size());
            refPalette.push_back(expected);
        }

        const StyleId actualId = theme.resolve(std::span<const std::string_view>(stack));
        ids.push_back(actualId);
        EXPECT_EQ(actualId, expectedId) << "stack #" << s;

        const Style actual = theme.styleAt(actualId);
        EXPECT_EQ(actual.hasFg, expected.hasFg) << "stack #" << s;
        EXPECT_EQ(actual.hasBg, expected.hasBg) << "stack #" << s;
        EXPECT_EQ(actual.fontStyle, expected.fontStyle) << "stack #" << s;
        if (expected.hasFg) EXPECT_EQ(actual.fg, expected.fg) << "stack #" << s;
        if (expected.hasBg) EXPECT_EQ(actual.bg, expected.bg) << "stack #" << s;
    }

    EXPECT_EQ(theme.paletteSize(), refPalette.size());

    // Literal anchors: ids are handed out in first-resolve order, so these are
    // fixed by construction. Stack 3 must *reuse* stack 1's id (same style), and
    // anything that resolves to the theme default must be id 0.
    ASSERT_EQ(ids.size(), 50u);
    EXPECT_EQ(ids[0], 0);   // empty stack -> default
    EXPECT_EQ(ids[1], 1);   // comment: first non-default style seen
    EXPECT_EQ(ids[2], 2);   // keyword.control
    EXPECT_EQ(ids[3], 1);   // another comment -> interned to the same id
    EXPECT_EQ(ids[4], 3);   // string
    EXPECT_EQ(ids[19], 0);  // "emphasis" only clears decorations -> default
    EXPECT_EQ(ids[35], 0);  // nothing matches -> default

    // The palette must stay far smaller than the number of stacks.
    EXPECT_LT(theme.paletteSize(), stacks.size());
    EXPECT_GT(theme.paletteSize(), 10u);

    // Cached and uncached paths must agree, and re-resolving must not grow the
    // palette or change a single id.
    const size_t paletteSize = theme.paletteSize();
    for (size_t s = 0; s < stacks.size(); ++s) {
        EXPECT_EQ(theme.resolve(std::span<const std::string_view>(stacks[s])), ids[s]) << "cached stack #" << s;
    }
    theme.clearCache();
    for (size_t s = 0; s < stacks.size(); ++s) {
        EXPECT_EQ(theme.resolve(std::span<const std::string_view>(stacks[s])), ids[s]) << "recomputed stack #" << s;
    }
    EXPECT_EQ(theme.paletteSize(), paletteSize);

    // Resolving in reverse order must also be stable.
    theme.clearCache();
    for (size_t s = stacks.size(); s-- > 0;) {
        EXPECT_EQ(theme.resolve(std::span<const std::string_view>(stacks[s])), ids[s]) << "reversed stack #" << s;
    }
    EXPECT_EQ(theme.paletteSize(), paletteSize);
}

TEST(ThemeSmokeTest, SpotChecksOnTheSmokeTheme) {
    const std::string json = buildThemeJson();
    std::string error;
    const std::optional<Theme> loaded = Theme::fromJsonText(json, &error);
    ASSERT_TRUE(loaded.has_value()) << error;
    const Theme& theme = *loaded;

    const auto styleOf = [&theme](const Stack& stack) {
        return theme.styleAt(theme.resolve(std::span<const std::string_view>(stack)));
    };

    // The js-specific comment rule beats the generic one, and the exclusion in
    // rule 32 keeps its underline away from comments.
    const Style jsLineComment = styleOf(Stack{"source.js", "comment.line.double-slash.js"});
    EXPECT_EQ(jsLineComment.fg, refColor("#5A8945"));
    EXPECT_EQ(jsLineComment.fontStyle, ide::theme::fontStyleMask(FontStyle::kItalic));

    // A three element descendant path beats a three segment single scope.
    const Style functionName = styleOf(Stack{"source.js", "meta.function.js", "entity.name.function.js"});
    EXPECT_EQ(functionName.fg, refColor("#FFD700"));

    // Depth beats segment count: "string" at depth 3 beats "meta.embedded" at 2.
    const Style embeddedString = styleOf(Stack{"source.js", "meta.embedded.block.html", "string.quoted.double.html"});
    EXPECT_EQ(embeddedString.fg, refColor("#CE9178"));

    // Everything inherits the theme background.
    for (const Stack& stack : makeStacks()) {
        const Style style = styleOf(stack);
        EXPECT_TRUE(style.hasBg);
        EXPECT_EQ(style.bg, refColor(kDefaultBg));
        EXPECT_TRUE(style.hasFg);
    }
}

}  // namespace
