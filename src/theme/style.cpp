#include <ide/theme/style.h>

namespace ide::theme {
namespace {

[[nodiscard]] constexpr bool isAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[nodiscard]] constexpr char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lowerAscii(lhs[i]) != lowerAscii(rhs[i])) return false;
    }
    return true;
}

/// -1 when `c` is not a hex digit.
[[nodiscard]] constexpr int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

[[nodiscard]] std::string_view trimAscii(std::string_view text) noexcept {
    size_t begin = 0;
    while (begin < text.size() && isAsciiSpace(text[begin])) ++begin;
    size_t end = text.size();
    while (end > begin && isAsciiSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

}  // namespace

Style applyDelta(const Style& base, const StyleDelta& delta) noexcept {
    Style out = base;
    if (delta.hasFg) {
        out.fg = delta.fg;
        out.hasFg = true;
    }
    if (delta.hasBg) {
        out.bg = delta.bg;
        out.hasBg = true;
    }
    // Note the asymmetry: fontStyle has no "clear" flag of its own, so a delta
    // that specifies it always wins, including when it specifies 0 decorations.
    if (delta.hasFontStyle) out.fontStyle = delta.fontStyle;
    return out;
}

Style normalizeStyle(const Style& style) noexcept {
    Style out = style;
    if (!out.hasFg) out.fg = Rgba{0, 0, 0, 0};
    if (!out.hasBg) out.bg = Rgba{0, 0, 0, 0};
    return out;
}

StyleKey styleKey(const Style& style) noexcept {
    const Style canonical = normalizeStyle(style);
    StyleKey key;
    key.fg = packRgba(canonical.fg);
    key.bg = packRgba(canonical.bg);
    key.flags = static_cast<uint32_t>(canonical.fontStyle) | (canonical.hasFg ? 0x100u : 0u) |
                (canonical.hasBg ? 0x200u : 0u);
    return key;
}

std::optional<Rgba> parseColor(std::string_view text) {
    const std::string_view trimmed = trimAscii(text);
    if (trimmed.size() < 2 || trimmed.front() != '#') return std::nullopt;

    const std::string_view digits = trimmed.substr(1);
    const size_t count = digits.size();
    if (count != 3 && count != 4 && count != 6 && count != 8) return std::nullopt;

    int nibble[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < count; ++i) {
        const int value = hexDigit(digits[i]);
        if (value < 0) return std::nullopt;
        nibble[i] = value;
    }

    Rgba colour;  // alpha defaults to 0xFF, only the 4/8 digit forms override it
    if (count <= 4) {
        // Short form: each nibble is duplicated, so 'a' becomes 0xAA.
        colour.r = static_cast<uint8_t>(nibble[0] * 17);
        colour.g = static_cast<uint8_t>(nibble[1] * 17);
        colour.b = static_cast<uint8_t>(nibble[2] * 17);
        if (count == 4) colour.a = static_cast<uint8_t>(nibble[3] * 17);
    } else {
        colour.r = static_cast<uint8_t>(nibble[0] * 16 + nibble[1]);
        colour.g = static_cast<uint8_t>(nibble[2] * 16 + nibble[3]);
        colour.b = static_cast<uint8_t>(nibble[4] * 16 + nibble[5]);
        if (count == 8) colour.a = static_cast<uint8_t>(nibble[6] * 16 + nibble[7]);
    }
    return colour;
}

std::string toHex(Rgba colour) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(9);
    out.push_back('#');
    const auto put = [&out](uint8_t value) {
        out.push_back(kDigits[(value >> 4) & 0x0Fu]);
        out.push_back(kDigits[value & 0x0Fu]);
    };
    put(colour.r);
    put(colour.g);
    put(colour.b);
    if (colour.a != 255) put(colour.a);
    return out;
}

uint8_t parseFontStyleMask(std::string_view text) {
    uint8_t mask = 0;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isAsciiSpace(text[i])) ++i;
        const size_t start = i;
        while (i < text.size() && !isAsciiSpace(text[i])) ++i;
        if (i == start) break;  // trailing whitespace only

        const std::string_view word = text.substr(start, i - start);
        if (equalsIgnoreCase(word, "bold")) {
            mask = mask | FontStyle::kBold;
        } else if (equalsIgnoreCase(word, "italic")) {
            mask = mask | FontStyle::kItalic;
        } else if (equalsIgnoreCase(word, "underline")) {
            mask = mask | FontStyle::kUnderline;
        } else if (equalsIgnoreCase(word, "strikethrough")) {
            mask = mask | FontStyle::kStrikethrough;
        }
        // Anything else — "regular", "normal", vendor extensions — is ignored.
    }
    return mask;
}

}  // namespace ide::theme
