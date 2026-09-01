#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/syntax/json_lite.h>
#include <ide/theme/theme.h>

namespace {

using ide::theme::FontStyle;
using ide::theme::fontStyleMask;
using ide::theme::Rgba;
using ide::theme::SemanticRule;
using ide::theme::Style;
using ide::theme::StyleId;
using ide::theme::Theme;
using ide::theme::TokenColorRule;
using ide::theme::UiColor;

using Stack = std::vector<std::string_view>;

Theme load(std::string_view json) {
    std::string error;
    std::optional<Theme> theme = Theme::fromJsonText(json, &error);
    EXPECT_TRUE(theme.has_value()) << "load failed: " << error;
    return theme.value_or(Theme{});
}

Style styleFor(const Theme& theme, const Stack& stack) {
    return theme.styleAt(theme.resolve(std::span<const std::string_view>(stack)));
}

constexpr std::string_view kFixture = R"json(
{
  "name": "fixture-dark",
  "displayName": "Fixture Dark",
  "type": "dark",
  "semanticHighlighting": true,
  "colors": {
    "editor.background": "#1E1E1E",
    "editor.foreground": "#D4D4D4",
    "editorLineNumber.foreground": "#858585",
    "editor.selectionBackground": "#264F78",
    "sideBar.background": "#252526",
    "statusBar.background": "#007ACC",
    "statusBar.foreground": "#FFF",
    "editorWhitespace.foreground": null,
    "bad.color": "not-a-color",
    "bad.type": 42
  },
  "tokenColors": [
    { "settings": { "foreground": "#D4D4D4" } },
    { "scope": "comment", "settings": { "foreground": "#6A9955", "fontStyle": "italic" } },
    { "scope": "keyword, storage.type", "settings": { "foreground": "#569CD6" } },
    { "scope": ["string", "constant.character"], "settings": { "foreground": "#CE9178" } },
    { "scope": "invalid", "settings": { "fontStyle": "bold italic underline" } },
    { "scope": "emphasis", "settings": { "fontStyle": "" } },
    { "scope": "nothing", "settings": {} },
    { "scope": "no.settings.here" },
    { "scope": 42, "settings": { "foreground": "#FF0000" } },
    "a string entry, not an object",
    { "scope": "markup.bold", "settings": { "background": "#112233", "foreground": "#445566" } }
  ],
  "semanticTokenColors": {
    "customLiteral": "#DCDCAA",
    "keyword.controlFlow": { "foreground": "#957FB8", "fontStyle": "bold" },
    "broken": 7,
    "badcolor": "nope"
  }
}
)json";

TEST(ThemeLoadTest, TopLevelMetadata) {
    const Theme theme = load(kFixture);
    EXPECT_EQ(theme.name(), "fixture-dark");
    EXPECT_EQ(theme.displayName(), "Fixture Dark");
    EXPECT_TRUE(theme.isDark());
    EXPECT_TRUE(theme.semanticHighlightingEnabled());
}

TEST(ThemeLoadTest, UiColorsSkipUnparsableValues) {
    const Theme theme = load(kFixture);

    EXPECT_EQ(theme.uiColor("editor.background"), (Rgba{30, 30, 30, 255}));
    EXPECT_EQ(theme.uiColor("editor.foreground"), (Rgba{212, 212, 212, 255}));
    EXPECT_EQ(theme.uiColor("editorLineNumber.foreground"), (Rgba{133, 133, 133, 255}));
    EXPECT_EQ(theme.uiColor("editor.selectionBackground"), (Rgba{38, 79, 120, 255}));
    EXPECT_EQ(theme.uiColor("sideBar.background"), (Rgba{37, 37, 38, 255}));
    EXPECT_EQ(theme.uiColor("statusBar.background"), (Rgba{0, 122, 204, 255}));
    EXPECT_EQ(theme.uiColor("statusBar.foreground"), (Rgba{255, 255, 255, 255}));

    EXPECT_FALSE(theme.uiColor("editorWhitespace.foreground").has_value());  // JSON null
    EXPECT_FALSE(theme.uiColor("bad.color").has_value());                    // unparsable
    EXPECT_FALSE(theme.uiColor("bad.type").has_value());                     // not a string
    EXPECT_FALSE(theme.uiColor("does.not.exist").has_value());
    EXPECT_FALSE(theme.uiColor("").has_value());
    EXPECT_FALSE(theme.uiColor("editor").has_value());  // prefixes are not keys

    EXPECT_EQ(theme.uiColors().size(), 7u);
    EXPECT_EQ(theme.uiColors().front().key, "editor.background");
    EXPECT_EQ(theme.uiColors().back().key, "statusBar.foreground");
    for (size_t i = 1; i < theme.uiColors().size(); ++i) {
        EXPECT_LT(theme.uiColors()[i - 1].key, theme.uiColors()[i].key) << "uiColors must be sorted by key";
    }
}

TEST(ThemeLoadTest, MalformedTokenColorEntriesAreSkipped) {
    const Theme theme = load(kFixture);
    // Kept: comment, "keyword, storage.type", ["string", "constant.character"],
    // invalid, emphasis, markup.bold.
    // Dropped: the scope-less default entry (it is not a rule), empty settings,
    // missing settings, a numeric scope, and a non-object entry.
    ASSERT_EQ(theme.rules().size(), 6u);
    EXPECT_EQ(theme.rules()[0].scopeText, "comment");
    EXPECT_EQ(theme.rules()[1].scopeText, "keyword, storage.type");
    EXPECT_EQ(theme.rules()[2].scopeText, "string, constant.character");
    EXPECT_EQ(theme.rules()[3].scopeText, "invalid");
    EXPECT_EQ(theme.rules()[4].scopeText, "emphasis");
    EXPECT_EQ(theme.rules()[5].scopeText, "markup.bold");
}

TEST(ThemeLoadTest, ScopeFormsCommaStringAndArray) {
    const Theme theme = load(kFixture);
    ASSERT_EQ(theme.rules().size(), 6u);

    // A comma separated string is one selector with two alternatives.
    ASSERT_EQ(theme.rules()[1].selectors.size(), 1u);
    EXPECT_EQ(theme.rules()[1].selectors[0].alternativeCount(), 2u);

    // An array is one selector per element.
    ASSERT_EQ(theme.rules()[2].selectors.size(), 2u);
    EXPECT_EQ(theme.rules()[2].selectors[0].alternativeCount(), 1u);
    EXPECT_EQ(theme.rules()[2].selectors[1].alternativeCount(), 1u);

    // ... and all of them resolve.
    const Rgba blue{0x56, 0x9C, 0xD6, 255};
    const Rgba orange{0xCE, 0x91, 0x78, 255};
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "keyword.control.flow.js"}).fg, blue);
    EXPECT_EQ(styleFor(theme, Stack{"source.ts", "storage.type.class.ts"}).fg, blue);
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "string.quoted.double.js"}).fg, orange);
    EXPECT_EQ(styleFor(theme, Stack{"source.js", "constant.character.escape.js"}).fg, orange);
}

TEST(ThemeLoadTest, FontStyleCombinations) {
    const Theme theme = load(kFixture);

    const Style comment = styleFor(theme, Stack{"source.js", "comment.line.double-slash.js"});
    EXPECT_EQ(comment.fontStyle, fontStyleMask(FontStyle::kItalic));
    EXPECT_EQ(comment.fg, (Rgba{0x6A, 0x99, 0x55, 255}));
    EXPECT_TRUE(comment.hasFg);
    EXPECT_TRUE(comment.hasBg);  // inherited from editor.background
    EXPECT_EQ(comment.bg, (Rgba{30, 30, 30, 255}));

    const Style invalid = styleFor(theme, Stack{"source.js", "invalid.illegal.js"});
    EXPECT_EQ(invalid.fontStyle, (FontStyle::kBold | FontStyle::kItalic | FontStyle::kUnderline));
    // No foreground of its own, so it keeps the default one.
    EXPECT_EQ(invalid.fg, theme.defaultStyle().fg);

    // "fontStyle": "" is an explicit "no decorations", not an absent setting.
    const Style emphasis = styleFor(theme, Stack{"emphasis"});
    EXPECT_EQ(emphasis.fontStyle, 0);
    EXPECT_EQ(emphasis, theme.defaultStyle());
}

TEST(ThemeLoadTest, BackgroundAndForegroundTogether) {
    const Theme theme = load(kFixture);
    const Style bold = styleFor(theme, Stack{"markup.bold"});
    EXPECT_EQ(bold.fg, (Rgba{0x44, 0x55, 0x66, 255}));
    EXPECT_EQ(bold.bg, (Rgba{0x11, 0x22, 0x33, 255}));
    EXPECT_TRUE(bold.hasFg);
    EXPECT_TRUE(bold.hasBg);
}

TEST(ThemeLoadTest, ScopelessEntryDefinesTheDefaultStyle) {
    const Theme theme = load(kFixture);
    const Style def = theme.defaultStyle();
    EXPECT_TRUE(def.hasFg);
    EXPECT_TRUE(def.hasBg);
    EXPECT_EQ(def.fg, (Rgba{212, 212, 212, 255}));
    EXPECT_EQ(def.bg, (Rgba{30, 30, 30, 255}));
    EXPECT_EQ(def.fontStyle, 0);
    EXPECT_EQ(theme.styleAt(ide::theme::kDefaultStyleId), def);
    EXPECT_EQ(theme.resolve(std::span<const std::string_view>()), ide::theme::kDefaultStyleId);
}

TEST(ThemeLoadTest, SemanticTokenColorsAreParsedButNotApplied) {
    const Theme theme = load(kFixture);
    ASSERT_EQ(theme.semanticRules().size(), 2u);
    EXPECT_EQ(theme.semanticRules()[0].selector, "customLiteral");
    EXPECT_TRUE(theme.semanticRules()[0].delta.hasFg);
    EXPECT_EQ(theme.semanticRules()[0].delta.fg, (Rgba{0xDC, 0xDC, 0xAA, 255}));
    EXPECT_FALSE(theme.semanticRules()[0].delta.hasFontStyle);

    EXPECT_EQ(theme.semanticRules()[1].selector, "keyword.controlFlow");
    EXPECT_EQ(theme.semanticRules()[1].delta.fg, (Rgba{0x95, 0x7F, 0xB8, 255}));
    EXPECT_TRUE(theme.semanticRules()[1].delta.hasFontStyle);
    EXPECT_EQ(theme.semanticRules()[1].delta.fontStyle, fontStyleMask(FontStyle::kBold));

    // Nothing semantic influences resolve() in this phase.
    EXPECT_EQ(styleFor(theme, Stack{"source.py", "customLiteral"}), theme.defaultStyle());
}

TEST(ThemeLoadTest, AcceptsAlreadyParsedJsonValue) {
    ide::json::ParseResult parsed = ide::json::parse(kFixture);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    std::string error = "untouched";
    const std::optional<Theme> theme = Theme::fromJson(parsed.root, &error);
    ASSERT_TRUE(theme.has_value());
    EXPECT_EQ(error, "untouched");  // only written on failure
    EXPECT_EQ(theme->name(), "fixture-dark");
    EXPECT_EQ(theme->rules().size(), 6u);
}

TEST(ThemeLoadTest, NonObjectRootFails) {
    std::string error;
    EXPECT_FALSE(Theme::fromJsonText("[1, 2, 3]", &error).has_value());
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(Theme::fromJsonText("\"just a string\"", &error).has_value());
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(Theme::fromJsonText("42", &error).has_value());
    EXPECT_FALSE(error.empty());
    // A null error pointer is allowed.
    EXPECT_FALSE(Theme::fromJsonText("null", nullptr).has_value());
}

TEST(ThemeLoadTest, BrokenJsonFailsWithAnOffset) {
    std::string error;
    EXPECT_FALSE(Theme::fromJsonText("{", &error).has_value());
    EXPECT_NE(error.find("byte"), std::string::npos) << error;
    error.clear();
    EXPECT_FALSE(Theme::fromJsonText("", &error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(ThemeLoadTest, EmptyObjectLoadsAsAnEmptyTheme) {
    const Theme theme = load("{}");
    EXPECT_TRUE(theme.name().empty());
    EXPECT_TRUE(theme.displayName().empty());
    EXPECT_TRUE(theme.isDark());  // nothing to infer from: dark is the default
    EXPECT_FALSE(theme.semanticHighlightingEnabled());
    EXPECT_TRUE(theme.rules().empty());
    EXPECT_TRUE(theme.semanticRules().empty());
    EXPECT_TRUE(theme.uiColors().empty());
    EXPECT_EQ(theme.paletteSize(), 1u);
    EXPECT_FALSE(theme.defaultStyle().hasFg);
    EXPECT_FALSE(theme.defaultStyle().hasBg);
    EXPECT_EQ(theme.resolve(std::span<const std::string_view>(Stack{"source.js", "comment"})), 0);
}

TEST(ThemeLoadTest, WrongTypesForWholeSectionsAreIgnored) {
    const Theme theme = load(R"json(
      {
        "name": 7,
        "displayName": ["nope"],
        "type": 1,
        "semanticHighlighting": "yes",
        "colors": "not an object",
        "tokenColors": { "scope": "comment" },
        "semanticTokenColors": [1, 2, 3]
      }
    )json");
    EXPECT_TRUE(theme.name().empty());
    EXPECT_TRUE(theme.uiColors().empty());
    EXPECT_TRUE(theme.rules().empty());
    EXPECT_TRUE(theme.semanticRules().empty());
    EXPECT_FALSE(theme.semanticHighlightingEnabled());
    EXPECT_EQ(theme.paletteSize(), 1u);
}

TEST(ThemeLoadTest, NameFallsBackBothWays) {
    const Theme onlyName = load(R"json({ "name": "only-name" })json");
    EXPECT_EQ(onlyName.name(), "only-name");
    EXPECT_EQ(onlyName.displayName(), "only-name");

    const Theme onlyDisplay = load(R"json({ "displayName": "Only Display" })json");
    EXPECT_EQ(onlyDisplay.name(), "Only Display");
    EXPECT_EQ(onlyDisplay.displayName(), "Only Display");
}

TEST(ThemeLoadTest, TypeFieldWinsOverInference) {
    const Theme light = load(R"json({ "type": "light", "colors": { "editor.background": "#000000" } })json");
    EXPECT_FALSE(light.isDark());

    const Theme dark = load(R"json({ "type": "dark", "colors": { "editor.background": "#FFFFFF" } })json");
    EXPECT_TRUE(dark.isDark());
}

TEST(ThemeLoadTest, TypeIsInferredFromBackgroundWhenMissing) {
    const Theme light = load(R"json({ "colors": { "editor.background": "#FFFFFF" } })json");
    EXPECT_FALSE(light.isDark());

    const Theme dark = load(R"json({ "colors": { "editor.background": "#1E1E1E" } })json");
    EXPECT_TRUE(dark.isDark());

    const Theme unknown = load(R"json({ "type": "solarized" })json");
    EXPECT_TRUE(unknown.isDark());
}

TEST(ThemeLoadTest, BlankScopesBecomeTheDefaultEntry) {
    const Theme emptyString = load(R"json(
      { "tokenColors": [ { "scope": "", "settings": { "foreground": "#010203" } } ] }
    )json");
    EXPECT_TRUE(emptyString.rules().empty());
    EXPECT_EQ(emptyString.defaultStyle().fg, (Rgba{1, 2, 3, 255}));

    const Theme emptyArray = load(R"json(
      { "tokenColors": [ { "scope": [], "settings": { "foreground": "#040506" } } ] }
    )json");
    EXPECT_TRUE(emptyArray.rules().empty());
    EXPECT_EQ(emptyArray.defaultStyle().fg, (Rgba{4, 5, 6, 255}));

    const Theme nullScope = load(R"json(
      { "tokenColors": [ { "scope": null, "settings": { "foreground": "#070809" } } ] }
    )json");
    EXPECT_TRUE(nullScope.rules().empty());
    EXPECT_EQ(nullScope.defaultStyle().fg, (Rgba{7, 8, 9, 255}));

    const Theme blanks = load(R"json(
      { "tokenColors": [ { "scope": ["", "   ", 5], "settings": { "foreground": "#0A0B0C" } } ] }
    )json");
    EXPECT_TRUE(blanks.rules().empty());
    EXPECT_EQ(blanks.defaultStyle().fg, (Rgba{10, 11, 12, 255}));
}

TEST(ThemeLoadTest, LaterDefaultEntriesMergePerAttribute) {
    const Theme theme = load(R"json(
      {
        "colors": { "editor.foreground": "#FFFFFF", "editor.background": "#000000" },
        "tokenColors": [
          { "settings": { "foreground": "#010203", "fontStyle": "bold" } },
          { "settings": { "foreground": "#040506" } }
        ]
      }
    )json");
    const Style def = theme.defaultStyle();
    EXPECT_EQ(def.fg, (Rgba{4, 5, 6, 255}));                        // later entry wins
    EXPECT_EQ(def.fontStyle, fontStyleMask(FontStyle::kBold));      // kept from the earlier one
    EXPECT_EQ(def.bg, (Rgba{0, 0, 0, 255}));                        // from editor.background
}

TEST(ThemeLoadTest, UnparsableSettingsColoursAreDropped) {
    const Theme theme = load(R"json(
      {
        "tokenColors": [
          { "scope": "comment", "settings": { "foreground": "green", "fontStyle": "italic" } },
          { "scope": "keyword", "settings": { "foreground": null } },
          { "scope": "string", "settings": { "background": "#12345" } },
          { "scope": "invalid", "settings": { "fontStyle": 3 } }
        ]
      }
    )json");
    // Only the comment rule survives, and only its fontStyle.
    ASSERT_EQ(theme.rules().size(), 1u);
    EXPECT_EQ(theme.rules()[0].scopeText, "comment");
    EXPECT_FALSE(theme.rules()[0].delta.hasFg);
    EXPECT_TRUE(theme.rules()[0].delta.hasFontStyle);
    EXPECT_EQ(styleFor(theme, Stack{"comment.line"}).fontStyle, fontStyleMask(FontStyle::kItalic));
}

TEST(ThemeLoadTest, UnparsableSelectorsDropTheirRule) {
    const Theme theme = load(R"json(
      {
        "tokenColors": [
          { "scope": "-comment", "settings": { "foreground": "#010203" } },
          { "scope": ["(", ")"], "settings": { "foreground": "#040506" } },
          { "scope": ["-x", "keyword"], "settings": { "foreground": "#070809" } }
        ]
      }
    )json");
    // The first two selectors parse to zero alternatives, so their rules vanish;
    // the third keeps only its usable selector.
    ASSERT_EQ(theme.rules().size(), 1u);
    EXPECT_EQ(theme.rules()[0].scopeText, "keyword");
    EXPECT_EQ(styleFor(theme, Stack{"keyword.control"}).fg, (Rgba{7, 8, 9, 255}));
}

TEST(ThemeLoadTest, Utf8AndLargeInputSurvive) {
    const Theme theme = load(R"json(
      {
        "name": "th\u00e8me \u6f22\u5b57",
        "colors": { "editor.background": "#101010" },
        "tokenColors": [
          { "scope": "keyword.\u00e9t\u00e9", "settings": { "foreground": "#ABCDEF" } }
        ]
      }
    )json");
    EXPECT_EQ(theme.name(), "th\xC3\xA8me \xE6\xBC\xA2\xE5\xAD\x97");
    ASSERT_EQ(theme.rules().size(), 1u);
    EXPECT_EQ(styleFor(theme, Stack{"keyword.\xC3\xA9t\xC3\xA9.x"}).fg, (Rgba{0xAB, 0xCD, 0xEF, 255}));

    // A theme with many rules loads and keeps them in order.
    std::string bigJson = R"json({ "tokenColors": [ )json";
    for (int i = 0; i < 500; ++i) {
        if (i != 0) bigJson += ", ";
        bigJson += R"json({ "scope": "scope.n)json";
        bigJson += std::to_string(i);
        bigJson += R"json(", "settings": { "foreground": "#0000)json";
        const char* digits = "0123456789abcdef";
        bigJson.push_back(digits[(i / 16) % 16]);
        bigJson.push_back(digits[i % 16]);
        bigJson += R"json(" } })json";
    }
    bigJson += " ] }";
    const Theme big = load(bigJson);
    ASSERT_EQ(big.rules().size(), 500u);
    EXPECT_EQ(big.rules()[0].scopeText, "scope.n0");
    EXPECT_EQ(big.rules()[499].scopeText, "scope.n499");
    EXPECT_EQ(styleFor(big, Stack{"scope.n7.inner"}).fg, (Rgba{0, 0, 7, 255}));
}

}  // namespace
