#pragma once

// Why this file exists
// -------------------
// The tokenizer produces scope stacks; the renderer needs pixels. This header is
// the pixel half of the theme engine: a packed colour, a font-style bit mask and
// the fully resolved `Style` a token is painted with.
//
// It knows nothing about JSON, selectors or themes on purpose. Phase 4 ships a
// whole screen of tokens over JNI as [start, length, styleId] integer triples
// plus *one* palette transfer, so `Style` has to stay a small, trivially
// copyable POD that can be blitted into a Java int array without translation.
//
// Colour channels are 8-bit, straight (non-premultiplied) alpha, and `Style`
// carries `hasFg`/`hasBg` flags rather than a magic transparent sentinel: a
// theme that says nothing about a background is different from one that asks for
// transparent black, and the renderer must be able to tell them apart.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ide::theme {

/// 8-bit-per-channel colour. Defaults to opaque black so that a
/// default-constructed Style is still paintable rather than invisible.
struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    friend bool operator==(const Rgba& lhs, const Rgba& rhs) = default;
};

/// Font decoration bits. A theme rule's "fontStyle" string is parsed into a
/// mask of these, which is why the values are powers of two and why Style stores
/// a plain uint8_t: masks travel over JNI as an int, enums do not.
enum class FontStyle : uint8_t {
    kNone = 0,
    kBold = 1,
    kItalic = 2,
    kUnderline = 4,
    kStrikethrough = 8,
};

/// All bits the engine ever sets. Useful for asserting no junk sneaks in.
inline constexpr uint8_t kAllFontStyles = 1u | 2u | 4u | 8u;

[[nodiscard]] constexpr uint8_t fontStyleMask(FontStyle style) noexcept {
    return static_cast<uint8_t>(style);
}

/// `FontStyle::kBold | FontStyle::kItalic` yields a uint8_t mask, and the mixed
/// overloads let three or more flags chain without casts.
[[nodiscard]] constexpr uint8_t operator|(FontStyle lhs, FontStyle rhs) noexcept {
    return static_cast<uint8_t>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}
[[nodiscard]] constexpr uint8_t operator|(uint8_t lhs, FontStyle rhs) noexcept {
    return static_cast<uint8_t>(lhs | static_cast<uint8_t>(rhs));
}
[[nodiscard]] constexpr uint8_t operator|(FontStyle lhs, uint8_t rhs) noexcept {
    return static_cast<uint8_t>(static_cast<uint8_t>(lhs) | rhs);
}

/// Tests one decoration bit. Always false for FontStyle::kNone, which has no
/// bit of its own: "no decorations" is simply mask == 0.
[[nodiscard]] constexpr bool hasFontStyle(uint8_t mask, FontStyle style) noexcept {
    return (mask & static_cast<uint8_t>(style)) != 0u;
}

/// A fully resolved visual style: what one token is painted with.
///
/// Member order is part of the public contract (positional aggregate
/// initialisation is used by callers and tests): fg, bg, fontStyle, hasFg, hasBg.
struct Style {
    Rgba fg{};
    Rgba bg{};
    uint8_t fontStyle = 0;  ///< mask of FontStyle bits
    bool hasFg = false;
    bool hasBg = false;

    /// Equality is *visual*, not bitwise: an unspecified channel's colour bytes
    /// are irrelevant. Palette interning depends on this, otherwise two styles
    /// that paint identically could occupy two StyleIds.
    friend bool operator==(const Style& lhs, const Style& rhs) noexcept {
        if (lhs.fontStyle != rhs.fontStyle) return false;
        if (lhs.hasFg != rhs.hasFg || lhs.hasBg != rhs.hasBg) return false;
        if (lhs.hasFg && !(lhs.fg == rhs.fg)) return false;
        if (lhs.hasBg && !(lhs.bg == rhs.bg)) return false;
        return true;
    }
};

/// Index into a Theme's deduplicated style palette. Signed because it crosses
/// JNI into Java `int`, where unsigned does not exist.
using StyleId = int32_t;

/// Every Theme's palette entry 0 is the theme's default style. Phase 4 relies on
/// this: a token whose scopes match nothing costs no palette slot.
inline constexpr StyleId kDefaultStyleId = 0;

/// A partially specified style: exactly what one theme rule's `settings` block
/// can express. Kept separate from Style so "this rule says nothing about the
/// background" survives loading instead of collapsing into a default.
struct StyleDelta {
    Rgba fg{};
    Rgba bg{};
    uint8_t fontStyle = 0;
    bool hasFg = false;
    bool hasBg = false;
    bool hasFontStyle = false;

    [[nodiscard]] bool empty() const noexcept { return !hasFg && !hasBg && !hasFontStyle; }
};

/// Overlays the specified parts of `delta` onto `base`. Note that a delta which
/// specifies fontStyle == 0 (theme JSON "fontStyle": "" / "regular") *clears*
/// the base decorations: saying "regular" explicitly has to be able to beat an
/// inherited italic.
[[nodiscard]] Style applyDelta(const Style& base, const StyleDelta& delta) noexcept;

/// Zeroes the colour bytes of unspecified channels so that visually equal styles
/// are also bitwise equal. Interning normalises, so palette entries are
/// canonical and can be compared byte-wise on the Java side.
[[nodiscard]] Style normalizeStyle(const Style& style) noexcept;

[[nodiscard]] constexpr uint32_t packRgba(Rgba colour) noexcept {
    return (static_cast<uint32_t>(colour.r) << 24) | (static_cast<uint32_t>(colour.g) << 16) |
           (static_cast<uint32_t>(colour.b) << 8) | static_cast<uint32_t>(colour.a);
}

[[nodiscard]] constexpr Rgba unpackRgba(uint32_t packed) noexcept {
    return Rgba{static_cast<uint8_t>((packed >> 24) & 0xFFu), static_cast<uint8_t>((packed >> 16) & 0xFFu),
                static_cast<uint8_t>((packed >> 8) & 0xFFu), static_cast<uint8_t>(packed & 0xFFu)};
}

/// Hash key for palette interning: a canonical, comparable projection of Style.
struct StyleKey {
    uint32_t fg = 0;
    uint32_t bg = 0;
    uint32_t flags = 0;  ///< fontStyle | hasFg << 8 | hasBg << 9

    friend bool operator==(const StyleKey& lhs, const StyleKey& rhs) = default;
};

[[nodiscard]] StyleKey styleKey(const Style& style) noexcept;

struct StyleKeyHash {
    [[nodiscard]] size_t operator()(const StyleKey& key) const noexcept {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(key.fg) * 0xFF51AFD7ED558CCDull;
        h = (h << 13) | (h >> 51);
        h ^= static_cast<uint64_t>(key.bg) * 0xC4CEB9FE1A85EC53ull;
        h = (h << 17) | (h >> 47);
        h ^= static_cast<uint64_t>(key.flags) * 0xD6E8FEB86659FD93ull;
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

/// Parses a CSS-style hex colour: #RGB, #RGBA, #RRGGBB or #RRGGBBAA,
/// case-insensitive, surrounding ASCII whitespace tolerated. Short forms expand
/// by nibble duplication (#abc -> #aabbcc), and a missing alpha defaults to 0xFF.
///
/// Returns nullopt for anything else — no leading '#', a bad digit count, a
/// non-hex digit, named colours. Callers *skip the key* on nullopt; a malformed
/// colour never fails a theme load, because real themes contain them.
[[nodiscard]] std::optional<Rgba> parseColor(std::string_view text);

/// Lowercase "#rrggbb", or "#rrggbbaa" when alpha is not fully opaque. Inverse
/// of parseColor for every value parseColor can produce. Diagnostics only.
[[nodiscard]] std::string toHex(Rgba colour);

/// Parses a theme "fontStyle" value: a whitespace-separated list of bold,
/// italic, underline, strikethrough (case-insensitive, any order, duplicates
/// allowed). Unknown words — including the common "regular"/"normal"/"" — are
/// ignored, so "" yields 0, meaning "explicitly no decorations".
[[nodiscard]] uint8_t parseFontStyleMask(std::string_view text);

}  // namespace ide::theme
