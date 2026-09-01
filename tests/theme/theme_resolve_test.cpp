#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/theme/theme.h>

namespace {

using ide::theme::FontStyle;
using ide::theme::fontStyleMask;
using ide::theme::kDefaultStyleId;
using ide::theme::Rgba;
using ide::theme::Style;
using ide::theme::StyleId;
using ide::theme::Theme;

using Stack = std::vector<std::string_view>;

Theme load(std::string_view json) {
    std::string error;
    std::optional<Theme> theme = Theme::fromJsonText(json, &error);
    EXPECT_TRUE(theme.has_value()) << "load failed: " << error;
    return theme.value_or(Theme{});
}

StyleId idFor(const Theme& theme, const Stack& stack) {
    return theme.resolve(std::span<const std::string_view>(stack));
}

Style styleFor(const Theme& theme, const Stack& stack) {
    return theme.styleAt(idFor(theme, stack));
}

const Rgba kRed{0xFF, 0x00, 0x00, 255};
const Rgba kGreen{0x00, 0xFF, 0x00, 255};
const Rgba kBlue{0x00, 0x00, 0xFF, 255};

// --- specificity between competing rules -------------------------------------

TEST(ThemeResolveTest, MoreSegmentsWinsRegardlessOfFileOrder) {
    const std::string_view generalFirst = R"json(
      { "tokenColors": [
        { "scope": "comment", "settings": { "foreground": "#FF0000" } },
        { "scope": "comment.line.double-slash.js", "settings": { "foreground": "#00FF00" } }
      ] }
    )json";
    const std::string_view specificFirst = R"json(
      { "tokenColors": [
        { "scope": "comment.line.double-slash.js", "settings": { "foreground": "#00FF00" } },
        { "scope": "comment", "settings": { "foreground": "#FF0000" } }
      ] }
    )json";

    const Stack stack{"source.js", "comment.line.double-slash.js"};
    EXPECT_EQ(styleFor(load(generalFirst), stack).fg, kGreen);
    EXPECT_EQ(styleFor(load(specificFirst), stack).fg, kGreen);

    // ... and the general rule still wins where the specific one does not apply.
    const Stack blockComment{"source.js", "comment.block.js"};
    EXPECT_EQ(styleFor(load(generalFirst), blockComment).fg, kRed);
    EXPECT_EQ(styleFor(load(specificFirst), blockComment).fg, kRed);
}

TEST(ThemeResolveTest, DeeperInTheStackBeatsMoreSegments) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "source.js", "settings": { "foreground": "#FF0000" } },
        { "scope": "comment", "settings": { "foreground": "#00FF00" } }
      ] }
    )json");
    // "source.js" has two segments and "comment" only one, but "comment" matches
    // deeper in the stack, and depth outranks segment count.
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "comment.line"}).fg, kGreen);
    // Where only the root matches, the root rule applies.
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "keyword.control"}).fg, kRed);
}

TEST(ThemeResolveTest, LongerPathBeatsMoreSegments) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "comment.line.double-slash.js", "settings": { "foreground": "#FF0000" } },
        { "scope": "source.js comment", "settings": { "foreground": "#00FF00" } }
      ] }
    )json");
    // Documented rule 1: path length dominates everything else.
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "comment.line.double-slash.js"}).fg, kGreen);
    // Without the outer source.js the two-element path cannot match.
    EXPECT_EQ(styleFor(theme, Stack{"source.ts", "comment.line.double-slash.js"}).fg, kRed);
}

TEST(ThemeResolveTest, LaterRuleWinsAnExactTie) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "comment", "settings": { "foreground": "#FF0000" } },
        { "scope": "comment", "settings": { "foreground": "#00FF00" } },
        { "scope": "keyword", "settings": { "foreground": "#0000FF" } }
      ] }
    )json");
    EXPECT_EQ(styleFor(theme, Stack{"comment.line"}).fg, kGreen);
    EXPECT_EQ(styleFor(theme, Stack{"keyword.control"}).fg, kBlue);
}

TEST(ThemeResolveTest, DepthDecidesBetweenSiblingScopes) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "meta.a", "settings": { "foreground": "#FF0000" } },
        { "scope": "meta.b", "settings": { "foreground": "#00FF00" } }
      ] }
    )json");
    // On single element stacks only one rule can match at all.
    EXPECT_EQ(styleFor(theme, Stack{"meta.a.x"}).fg, kRed);
    EXPECT_EQ(styleFor(theme, Stack{"meta.b.x"}).fg, kGreen);
    // When both match, the one that matched deeper in the stack wins, whatever
    // the file order is.
    EXPECT_EQ(styleFor(theme, Stack{"meta.a", "meta.b"}).fg, kGreen);
    EXPECT_EQ(styleFor(theme, Stack{"meta.b", "meta.a"}).fg, kRed);
}

TEST(ThemeResolveTest, AttributesAreWonIndependently) {
    const Theme theme = load(R"json(
      {
        "colors": { "editor.foreground": "#FFFFFF", "editor.background": "#000000" },
        "tokenColors": [
          { "scope": "comment", "settings": { "foreground": "#FF0000", "fontStyle": "italic" } },
          { "scope": "comment.line", "settings": { "background": "#0000FF" } },
          { "scope": "comment.line.double-slash", "settings": { "fontStyle": "bold" } }
        ]
      }
    )json");
    const Style style = styleFor(theme, Stack{"source.js", "comment.line.double-slash.js"});
    EXPECT_EQ(style.fg, kRed);                                      // only rule 0 sets fg
    EXPECT_EQ(style.bg, kBlue);                                     // only rule 1 sets bg
    EXPECT_EQ(style.fontStyle, fontStyleMask(FontStyle::kBold));    // rule 2 outranks rule 0
    EXPECT_TRUE(style.hasFg);
    EXPECT_TRUE(style.hasBg);
}

TEST(ThemeResolveTest, ExplicitRegularFontStyleClearsInheritedDecoration) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "comment", "settings": { "foreground": "#FF0000", "fontStyle": "italic" } },
        { "scope": "comment.line", "settings": { "fontStyle": "" } }
      ] }
    )json");
    const Style style = styleFor(theme, Stack{"comment.line.js"});
    EXPECT_EQ(style.fg, kRed);
    EXPECT_EQ(style.fontStyle, 0);
    // The italic survives where the clearing rule does not match.
    EXPECT_EQ(styleFor(theme, Stack{"comment.block.js"}).fontStyle, fontStyleMask(FontStyle::kItalic));
}

TEST(ThemeResolveTest, ExclusionsParticipateInResolution) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "source.js", "settings": { "foreground": "#FF0000" } },
        { "scope": "source.js -comment", "settings": { "foreground": "#00FF00" } }
      ] }
    )json");
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "keyword.control"}).fg, kGreen);
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "comment.line"}).fg, kRed);
}

TEST(ThemeResolveTest, UnmatchedStacksGetTheDefaultStyle) {
    const Theme theme = load(R"json(
      {
        "colors": { "editor.foreground": "#FFFFFF", "editor.background": "#000000" },
        "tokenColors": [ { "scope": "comment", "settings": { "foreground": "#FF0000" } } ]
      }
    )json");
    EXPECT_EQ(idFor(theme, Stack{}), kDefaultStyleId);
    EXPECT_EQ(idFor(theme, Stack{"source.js"}), kDefaultStyleId);
    EXPECT_EQ(idFor(theme, Stack{"source.js", "keyword.control"}), kDefaultStyleId);
    EXPECT_EQ(idFor(theme, Stack{"", ""}), kDefaultStyleId);
    EXPECT_EQ(theme.defaultStyle().fg, (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(theme.defaultStyle().bg, (Rgba{0, 0, 0, 255}));
}

// --- palette interning --------------------------------------------------------

TEST(ThemePaletteTest, IdenticalStylesShareAnId) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "keyword", "settings": { "foreground": "#569CD6" } },
        { "scope": "storage", "settings": { "foreground": "#569CD6" } },
        { "scope": "string", "settings": { "foreground": "#CE9178" } }
      ] }
    )json");
    const StyleId keyword = idFor(theme, Stack{"source.js", "keyword.control.js"});
    const StyleId storage = idFor(theme, Stack{"source.js", "storage.type.js"});
    const StyleId string = idFor(theme, Stack{"source.js", "string.quoted.js"});

    EXPECT_EQ(keyword, storage);
    EXPECT_NE(keyword, string);
    EXPECT_NE(keyword, kDefaultStyleId);
    EXPECT_EQ(theme.paletteSize(), 3u);  // default + two distinct styles
    EXPECT_EQ(theme.styleAt(keyword).fg, (Rgba{0x56, 0x9C, 0xD6, 255}));
    EXPECT_EQ(theme.styleAt(string).fg, (Rgba{0xCE, 0x91, 0x78, 255}));
}

TEST(ThemePaletteTest, IdZeroIsAlwaysTheDefaultStyle) {
    const Theme theme = load(R"json(
      {
        "colors": { "editor.foreground": "#D4D4D4" },
        "tokenColors": [
          { "settings": { "foreground": "#ABCDEF", "fontStyle": "bold" } },
          { "scope": "comment", "settings": { "foreground": "#FF0000" } }
        ]
      }
    )json");
    EXPECT_EQ(theme.styleAt(kDefaultStyleId), theme.defaultStyle());
    EXPECT_EQ(theme.palette()[0], theme.defaultStyle());
    EXPECT_EQ(theme.defaultStyle().fg, (Rgba{0xAB, 0xCD, 0xEF, 255}));
    EXPECT_EQ(theme.defaultStyle().fontStyle, fontStyleMask(FontStyle::kBold));

    // A rule that reproduces the default style exactly must reuse id 0.
    const Theme same = load(R"json(
      { "tokenColors": [
        { "settings": { "foreground": "#ABCDEF" } },
        { "scope": "comment", "settings": { "foreground": "#ABCDEF" } }
      ] }
    )json");
    EXPECT_EQ(idFor(same, Stack{"comment.line"}), kDefaultStyleId);
    EXPECT_EQ(same.paletteSize(), 1u);
}

TEST(ThemePaletteTest, OutOfRangeIdsFallBackToTheDefault) {
    const Theme theme = load(R"json(
      { "tokenColors": [ { "scope": "comment", "settings": { "foreground": "#FF0000" } } ] }
    )json");
    EXPECT_EQ(theme.styleAt(-1), theme.defaultStyle());
    EXPECT_EQ(theme.styleAt(9999), theme.defaultStyle());
    EXPECT_EQ(theme.styleAt(static_cast<StyleId>(theme.paletteSize())), theme.defaultStyle());
}

TEST(ThemePaletteTest, StaysSmallAcrossManyResolves) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "comment", "settings": { "foreground": "#FF0000" } },
        { "scope": "keyword", "settings": { "foreground": "#00FF00" } },
        { "scope": "string", "settings": { "foreground": "#0000FF" } }
      ] }
    )json");

    std::vector<std::string> owned;
    owned.reserve(3000);
    for (int i = 0; i < 1000; ++i) {
        owned.push_back("comment.line.n" + std::to_string(i));
        owned.push_back("keyword.control.n" + std::to_string(i));
        owned.push_back("string.quoted.n" + std::to_string(i));
    }

    StyleId commentId = kDefaultStyleId;
    StyleId keywordId = kDefaultStyleId;
    StyleId stringId = kDefaultStyleId;
    for (size_t i = 0; i < owned.size(); i += 3) {
        const Stack commentStack{"source.js", owned[i]};
        const Stack keywordStack{"source.js", owned[i + 1]};
        const Stack stringStack{"source.js", owned[i + 2]};
        if (i == 0) {
            commentId = idFor(theme, commentStack);
            keywordId = idFor(theme, keywordStack);
            stringId = idFor(theme, stringStack);
        } else {
            EXPECT_EQ(idFor(theme, commentStack), commentId);
            EXPECT_EQ(idFor(theme, keywordStack), keywordId);
            EXPECT_EQ(idFor(theme, stringStack), stringId);
        }
    }
    EXPECT_EQ(theme.paletteSize(), 4u);  // default + three
    EXPECT_EQ(commentId, 1);
    EXPECT_EQ(keywordId, 2);
    EXPECT_EQ(stringId, 3);
}

TEST(ThemePaletteTest, PaletteIsDenseAndIterableInIdOrder) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "a", "settings": { "foreground": "#010101" } },
        { "scope": "b", "settings": { "foreground": "#020202" } },
        { "scope": "c", "settings": { "foreground": "#030303" } }
      ] }
    )json");
    (void)idFor(theme, Stack{"c.x"});
    (void)idFor(theme, Stack{"a.x"});
    (void)idFor(theme, Stack{"b.x"});
    ASSERT_EQ(theme.paletteSize(), 4u);
    EXPECT_EQ(theme.palette().size(), theme.paletteSize());
    // Ids are handed out in first-resolve order.
    EXPECT_EQ(theme.palette()[1].fg, (Rgba{3, 3, 3, 255}));
    EXPECT_EQ(theme.palette()[2].fg, (Rgba{1, 1, 1, 255}));
    EXPECT_EQ(theme.palette()[3].fg, (Rgba{2, 2, 2, 255}));
    for (size_t i = 0; i < theme.paletteSize(); ++i) {
        EXPECT_EQ(theme.styleAt(static_cast<StyleId>(i)), theme.palette()[i]);
    }
}

// --- memoisation --------------------------------------------------------------

TEST(ThemeCacheTest, RepeatedResolvesHitTheCache) {
    const Theme theme = load(R"json(
      { "tokenColors": [ { "scope": "comment", "settings": { "foreground": "#FF0000" } } ] }
    )json");
    const Stack stack{"source.js", "comment.line.js"};

    EXPECT_EQ(theme.cacheEntryCount(), 0u);
    const StyleId first = idFor(theme, stack);
    EXPECT_EQ(theme.cacheEntryCount(), 1u);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(idFor(theme, stack), first);
    EXPECT_EQ(theme.cacheEntryCount(), 1u);
    EXPECT_EQ(theme.paletteSize(), 2u);

    // A different stack is a different entry, even with the same resulting style.
    const Stack other{"source.ts", "comment.block.ts"};
    EXPECT_EQ(idFor(theme, other), first);
    EXPECT_EQ(theme.cacheEntryCount(), 2u);
    EXPECT_EQ(theme.paletteSize(), 2u);

    theme.clearCache();
    EXPECT_EQ(theme.cacheEntryCount(), 0u);
    EXPECT_EQ(idFor(theme, stack), first);  // ids survive a cache drop
    EXPECT_EQ(theme.paletteSize(), 2u);
}

TEST(ThemeCacheTest, StacksThatDifferOnlyInBoundariesAreNotConfused) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "ab", "settings": { "foreground": "#FF0000" } },
        { "scope": "a", "settings": { "foreground": "#00FF00" } },
        { "scope": "bc", "settings": { "foreground": "#0000FF" } }
      ] }
    )json");
    const StyleId abThenC = idFor(theme, Stack{"ab", "c"});
    const StyleId aThenBc = idFor(theme, Stack{"a", "bc"});
    EXPECT_NE(abThenC, aThenBc);
    EXPECT_EQ(theme.styleAt(abThenC).fg, kRed);
    EXPECT_EQ(theme.styleAt(aThenBc).fg, kBlue);
    EXPECT_EQ(theme.cacheEntryCount(), 2u);

    // Repeat in the opposite order to be sure both cache entries are correct.
    EXPECT_EQ(idFor(theme, Stack{"a", "bc"}), aThenBc);
    EXPECT_EQ(idFor(theme, Stack{"ab", "c"}), abThenC);
    EXPECT_EQ(theme.cacheEntryCount(), 2u);
}

TEST(ThemeCacheTest, CacheIsBoundedAndKeepsAnsweringCorrectly) {
    const Theme theme = load(R"json(
      { "tokenColors": [ { "scope": "comment", "settings": { "foreground": "#FF0000" } } ] }
    )json");

    const size_t total = Theme::kMaxCacheEntries + 64;
    std::vector<std::string> owned;
    owned.reserve(total);
    for (size_t i = 0; i < total; ++i) owned.push_back("comment.line.n" + std::to_string(i));

    StyleId expected = kDefaultStyleId;
    for (size_t i = 0; i < total; ++i) {
        const Stack stack{"source.js", owned[i]};
        const StyleId id = idFor(theme, stack);
        if (i == 0) expected = id;
        EXPECT_EQ(id, expected);
    }
    EXPECT_LE(theme.cacheEntryCount(), Theme::kMaxCacheEntries);
    EXPECT_EQ(theme.paletteSize(), 2u);  // eviction must never grow the palette
    EXPECT_NE(expected, kDefaultStyleId);
}

TEST(ThemeCacheTest, CopyingAThemeKeepsIdsStable) {
    const Theme theme = load(R"json(
      { "tokenColors": [
        { "scope": "comment", "settings": { "foreground": "#FF0000" } },
        { "scope": "keyword", "settings": { "foreground": "#00FF00" } }
      ] }
    )json");
    const StyleId commentId = idFor(theme, Stack{"comment.line"});
    const Theme copy = theme;
    EXPECT_EQ(copy.paletteSize(), theme.paletteSize());
    EXPECT_EQ(idFor(copy, Stack{"comment.line"}), commentId);
    EXPECT_EQ(idFor(copy, Stack{"keyword.control"}), idFor(theme, Stack{"keyword.control"}));
    EXPECT_EQ(copy.styleAt(commentId), theme.styleAt(commentId));
}

TEST(ThemeResolveTest, EmptyThemeResolvesEverythingToTheDefault) {
    const Theme empty;
    EXPECT_EQ(empty.paletteSize(), 1u);
    EXPECT_EQ(idFor(empty, Stack{}), kDefaultStyleId);
    EXPECT_EQ(idFor(empty, Stack{"source.js", "comment.line"}), kDefaultStyleId);
    EXPECT_FALSE(empty.defaultStyle().hasFg);
    EXPECT_FALSE(empty.defaultStyle().hasBg);
    EXPECT_EQ(empty.defaultStyle().fontStyle, 0);
    EXPECT_TRUE(empty.rules().empty());
    EXPECT_FALSE(empty.uiColor("editor.background").has_value());
}

}  // namespace
