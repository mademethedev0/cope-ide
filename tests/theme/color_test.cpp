#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <ide/theme/style.h>

namespace {

using ide::theme::applyDelta;
using ide::theme::FontStyle;
using ide::theme::fontStyleMask;
using ide::theme::hasFontStyle;
using ide::theme::normalizeStyle;
using ide::theme::packRgba;
using ide::theme::parseColor;
using ide::theme::parseFontStyleMask;
using ide::theme::Rgba;
using ide::theme::Style;
using ide::theme::StyleDelta;
using ide::theme::styleKey;
using ide::theme::StyleKey;
using ide::theme::StyleKeyHash;
using ide::theme::toHex;
using ide::theme::unpackRgba;

Rgba mustParse(std::string_view text) {
    const std::optional<Rgba> parsed = parseColor(text);
    EXPECT_TRUE(parsed.has_value()) << "expected a colour for " << text;
    return parsed.value_or(Rgba{0, 0, 0, 0});
}

TEST(ThemeColorTest, ThreeDigitFormDuplicatesNibbles) {
    EXPECT_EQ(mustParse("#fff"), (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(mustParse("#000"), (Rgba{0, 0, 0, 255}));
    EXPECT_EQ(mustParse("#abc"), (Rgba{0xAA, 0xBB, 0xCC, 255}));
    EXPECT_EQ(mustParse("#ABC"), (Rgba{0xAA, 0xBB, 0xCC, 255}));
    EXPECT_EQ(mustParse("#123"), (Rgba{0x11, 0x22, 0x33, 255}));
}

TEST(ThemeColorTest, FourDigitFormCarriesAlpha) {
    EXPECT_EQ(mustParse("#0000"), (Rgba{0, 0, 0, 0}));
    EXPECT_EQ(mustParse("#ccc3"), (Rgba{0xCC, 0xCC, 0xCC, 0x33}));
    EXPECT_EQ(mustParse("#FFFF"), (Rgba{255, 255, 255, 255}));
}

TEST(ThemeColorTest, SixDigitForm) {
    EXPECT_EQ(mustParse("#1E1E1E"), (Rgba{30, 30, 30, 255}));
    EXPECT_EQ(mustParse("#1e1e1e"), (Rgba{30, 30, 30, 255}));
    EXPECT_EQ(mustParse("#AbCdEf"), (Rgba{0xAB, 0xCD, 0xEF, 255}));
    EXPECT_EQ(mustParse("#007ACC"), (Rgba{0x00, 0x7A, 0xCC, 255}));
}

TEST(ThemeColorTest, EightDigitForm) {
    EXPECT_EQ(mustParse("#ADD6FF26"), (Rgba{173, 214, 255, 38}));
    EXPECT_EQ(mustParse("#ffffffa0"), (Rgba{255, 255, 255, 160}));
    EXPECT_EQ(mustParse("#00000000"), (Rgba{0, 0, 0, 0}));
}

TEST(ThemeColorTest, AlphaDefaultsToOpaque) {
    EXPECT_EQ(mustParse("#abc").a, 255);
    EXPECT_EQ(mustParse("#aabbcc").a, 255);
}

TEST(ThemeColorTest, SurroundingWhitespaceTolerated) {
    EXPECT_EQ(mustParse("  #fff"), (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(mustParse("#fff\t\n"), (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(mustParse("\n  #1E1E1E  "), (Rgba{30, 30, 30, 255}));
}

TEST(ThemeColorTest, InvalidInputYieldsNullopt) {
    EXPECT_FALSE(parseColor("").has_value());
    EXPECT_FALSE(parseColor(" ").has_value());
    EXPECT_FALSE(parseColor("#").has_value());
    EXPECT_FALSE(parseColor("#f").has_value());
    EXPECT_FALSE(parseColor("#ff").has_value());
    EXPECT_FALSE(parseColor("#fffff").has_value());       // 5 digits
    EXPECT_FALSE(parseColor("#fffffff").has_value());     // 7 digits
    EXPECT_FALSE(parseColor("#fffffffff").has_value());   // 9 digits
    EXPECT_FALSE(parseColor("fff").has_value());          // no '#'
    EXPECT_FALSE(parseColor("0x112233").has_value());
    EXPECT_FALSE(parseColor("#gggggg").has_value());
    EXPECT_FALSE(parseColor("#12 34 56").has_value());    // 8 chars, not 8 digits
    EXPECT_FALSE(parseColor("# fff").has_value());
    EXPECT_FALSE(parseColor("#-12345").has_value());
    EXPECT_FALSE(parseColor("rgb(1,2,3)").has_value());
    EXPECT_FALSE(parseColor("red").has_value());
    EXPECT_FALSE(parseColor("#ff\0ff").has_value());
}

TEST(ThemeColorTest, MultibyteInputIsRejectedNotMisread) {
    // "#ffé" is 5 bytes: '#', 'f', 'f', 0xC3, 0xA9.
    EXPECT_FALSE(parseColor("#ff\xC3\xA9").has_value());
    EXPECT_FALSE(parseColor("\xE2\x80\x8B#ffffff").has_value());  // zero-width space prefix
}

TEST(ThemeColorTest, ToHexRoundTrips) {
    EXPECT_EQ(toHex(Rgba{30, 30, 30, 255}), "#1e1e1e");
    EXPECT_EQ(toHex(Rgba{173, 214, 255, 38}), "#add6ff26");
    EXPECT_EQ(toHex(Rgba{0, 0, 0, 0}), "#00000000");
    EXPECT_EQ(toHex(Rgba{255, 255, 255, 255}), "#ffffff");

    for (const std::string_view text : {"#fff", "#0000", "#1E1E1E", "#ADD6FF26", "#ffffffa0"}) {
        const Rgba colour = mustParse(text);
        EXPECT_EQ(mustParse(toHex(colour)), colour) << text;
    }
}

TEST(ThemeColorTest, PackUnpackRoundTrips) {
    const Rgba colour{0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(packRgba(colour), 0x12345678u);
    EXPECT_EQ(unpackRgba(0x12345678u), colour);
    EXPECT_EQ(unpackRgba(packRgba(Rgba{1, 2, 3, 4})), (Rgba{1, 2, 3, 4}));
}

TEST(ThemeFontStyleTest, ParsesSingleWords) {
    EXPECT_EQ(parseFontStyleMask("bold"), fontStyleMask(FontStyle::kBold));
    EXPECT_EQ(parseFontStyleMask("italic"), fontStyleMask(FontStyle::kItalic));
    EXPECT_EQ(parseFontStyleMask("underline"), fontStyleMask(FontStyle::kUnderline));
    EXPECT_EQ(parseFontStyleMask("strikethrough"), fontStyleMask(FontStyle::kStrikethrough));
}

TEST(ThemeFontStyleTest, ParsesCombinationsInAnyOrder) {
    EXPECT_EQ(parseFontStyleMask("bold italic"), (FontStyle::kBold | FontStyle::kItalic));
    EXPECT_EQ(parseFontStyleMask("italic bold"), (FontStyle::kBold | FontStyle::kItalic));
    EXPECT_EQ(parseFontStyleMask("italic underline"), (FontStyle::kItalic | FontStyle::kUnderline));
    EXPECT_EQ(parseFontStyleMask(" italic bold underline"),
              (FontStyle::kItalic | FontStyle::kBold | FontStyle::kUnderline));
    EXPECT_EQ(parseFontStyleMask("bold\titalic\nunderline  strikethrough"), ide::theme::kAllFontStyles);
}

TEST(ThemeFontStyleTest, UnknownAndEmptyMeanNoDecoration) {
    EXPECT_EQ(parseFontStyleMask(""), 0);
    EXPECT_EQ(parseFontStyleMask("   "), 0);
    EXPECT_EQ(parseFontStyleMask("regular"), 0);
    EXPECT_EQ(parseFontStyleMask("normal"), 0);
    EXPECT_EQ(parseFontStyleMask("wavy-underline"), 0);
    EXPECT_EQ(parseFontStyleMask("bolditalic"), 0);  // one unknown word, not two
    EXPECT_EQ(parseFontStyleMask("regular bold"), fontStyleMask(FontStyle::kBold));
}

TEST(ThemeFontStyleTest, CaseInsensitiveAndIdempotent) {
    EXPECT_EQ(parseFontStyleMask("BOLD Italic"), (FontStyle::kBold | FontStyle::kItalic));
    EXPECT_EQ(parseFontStyleMask("bold bold bold"), fontStyleMask(FontStyle::kBold));
}

TEST(ThemeFontStyleTest, HasFontStyleTestsBits) {
    const uint8_t mask = FontStyle::kBold | FontStyle::kUnderline;
    EXPECT_TRUE(hasFontStyle(mask, FontStyle::kBold));
    EXPECT_TRUE(hasFontStyle(mask, FontStyle::kUnderline));
    EXPECT_FALSE(hasFontStyle(mask, FontStyle::kItalic));
    EXPECT_FALSE(hasFontStyle(mask, FontStyle::kStrikethrough));
    EXPECT_FALSE(hasFontStyle(mask, FontStyle::kNone));
    EXPECT_FALSE(hasFontStyle(0, FontStyle::kBold));
}

TEST(ThemeStyleTest, EqualityIgnoresUnspecifiedChannels) {
    const Style a{Rgba{1, 2, 3, 4}, Rgba{5, 6, 7, 8}, 0, false, false};
    const Style b{Rgba{9, 9, 9, 9}, Rgba{9, 9, 9, 9}, 0, false, false};
    EXPECT_EQ(a, b);
    EXPECT_EQ(styleKey(a), styleKey(b));

    const Style withFg{Rgba{1, 2, 3, 4}, Rgba{}, 0, true, false};
    const Style otherFg{Rgba{1, 2, 3, 5}, Rgba{}, 0, true, false};
    EXPECT_NE(withFg, a);
    EXPECT_NE(withFg, otherFg);
    EXPECT_FALSE(styleKey(withFg) == styleKey(otherFg));
}

TEST(ThemeStyleTest, EqualityDistinguishesFontStyleAndFlags) {
    const Style plain{Rgba{}, Rgba{}, 0, false, false};
    const Style bold{Rgba{}, Rgba{}, fontStyleMask(FontStyle::kBold), false, false};
    const Style transparentBg{Rgba{0, 0, 0, 0}, Rgba{0, 0, 0, 0}, 0, false, true};
    EXPECT_NE(plain, bold);
    EXPECT_NE(plain, transparentBg);  // "no background" != "transparent background"
    EXPECT_FALSE(styleKey(plain) == styleKey(bold));
    EXPECT_FALSE(styleKey(plain) == styleKey(transparentBg));
}

TEST(ThemeStyleTest, NormalizeZeroesUnspecifiedChannels) {
    const Style messy{Rgba{1, 2, 3, 4}, Rgba{5, 6, 7, 8}, 0, false, false};
    const Style clean = normalizeStyle(messy);
    EXPECT_EQ(clean.fg, (Rgba{0, 0, 0, 0}));
    EXPECT_EQ(clean.bg, (Rgba{0, 0, 0, 0}));
    EXPECT_EQ(clean, messy);  // normalisation is visually invisible

    const Style specified{Rgba{1, 2, 3, 4}, Rgba{5, 6, 7, 8}, 3, true, true};
    EXPECT_EQ(normalizeStyle(specified).fg, (Rgba{1, 2, 3, 4}));
    EXPECT_EQ(normalizeStyle(specified).bg, (Rgba{5, 6, 7, 8}));
    EXPECT_EQ(normalizeStyle(specified).fontStyle, 3);
}

TEST(ThemeStyleTest, ApplyDeltaOverlaysSpecifiedPartsOnly) {
    Style base;
    base.fg = Rgba{10, 10, 10, 255};
    base.hasFg = true;
    base.bg = Rgba{20, 20, 20, 255};
    base.hasBg = true;
    base.fontStyle = fontStyleMask(FontStyle::kItalic);

    StyleDelta empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(applyDelta(base, empty), base);

    StyleDelta fgOnly;
    fgOnly.fg = Rgba{30, 30, 30, 255};
    fgOnly.hasFg = true;
    EXPECT_FALSE(fgOnly.empty());
    const Style overlaid = applyDelta(base, fgOnly);
    EXPECT_EQ(overlaid.fg, (Rgba{30, 30, 30, 255}));
    EXPECT_EQ(overlaid.bg, base.bg);
    EXPECT_EQ(overlaid.fontStyle, fontStyleMask(FontStyle::kItalic));

    StyleDelta clearFont;
    clearFont.fontStyle = 0;
    clearFont.hasFontStyle = true;
    EXPECT_FALSE(clearFont.empty());
    EXPECT_EQ(applyDelta(base, clearFont).fontStyle, 0);  // "regular" beats inherited italic

    StyleDelta bgOnly;
    bgOnly.bg = Rgba{40, 40, 40, 128};
    bgOnly.hasBg = true;
    Style bare;
    const Style gained = applyDelta(bare, bgOnly);
    EXPECT_TRUE(gained.hasBg);
    EXPECT_FALSE(gained.hasFg);
    EXPECT_EQ(gained.bg, (Rgba{40, 40, 40, 128}));
}

TEST(ThemeStyleTest, StyleKeyWorksAsAHashMapKey) {
    std::unordered_map<StyleKey, int, StyleKeyHash> table;
    const Style a{Rgba{1, 2, 3, 255}, Rgba{}, 0, true, false};
    const Style b{Rgba{1, 2, 3, 255}, Rgba{9, 9, 9, 9}, 0, true, false};  // bg bytes are noise
    const Style c{Rgba{1, 2, 3, 255}, Rgba{9, 9, 9, 9}, 0, true, true};
    table[styleKey(a)] = 1;
    table[styleKey(b)] = 2;
    table[styleKey(c)] = 3;
    EXPECT_EQ(table.size(), 2u);
    EXPECT_EQ(table[styleKey(a)], 2);
    EXPECT_EQ(table[styleKey(c)], 3);
}

TEST(ThemeStyleTest, DefaultStyleIdIsZero) {
    EXPECT_EQ(ide::theme::kDefaultStyleId, 0);
}

}  // namespace
