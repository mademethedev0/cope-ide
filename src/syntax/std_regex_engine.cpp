#include <ide/syntax/std_regex_engine.h>

#include <algorithm>
#include <cstring>
#include <regex>
#include <utility>

namespace ide::syntax {
namespace {

// Zero width, always true. Mirror of kNeverMatchAssertion from regex.h; both use
// nothing but a lookahead and a negated class so every dialect accepts them.
constexpr std::string_view kAlwaysMatchAssertion = "(?![^\\s\\S])";
// Consumes one character but can never match: the ASCII-approximation of an
// empty unicode property set.
constexpr std::string_view kNeverChar = "[^\\s\\S]";
constexpr std::string_view kAnyChar = "[\\s\\S]";

constexpr bool isDigitChar(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isOctalChar(char c) noexcept { return c >= '0' && c <= '7'; }
constexpr bool isAlnumChar(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool isAsciiChar(char c) noexcept { return static_cast<unsigned char>(c) < 0x80u; }

bool isEcmaMeta(char c) noexcept { return std::strchr("^$\\.*+?()[]{}|", c) != nullptr; }

bool parseHexDigits(std::string_view hex, uint32_t& out) noexcept {
    if (hex.empty() || hex.size() > 8) return false;
    uint32_t v = 0;
    for (char c : hex) {
        uint32_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A') + 10u;
        } else {
            return false;
        }
        v = v * 16u + d;
    }
    out = v;
    return true;
}

/// Lowercases and drops '_', '-' and ' ' so that \p{Uppercase_Letter} and
/// \p{uppercaseletter} normalise to the same key.
std::string normalisePropertyName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '_' || c == '-' || c == ' ') continue;
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

/// ASCII approximation of a unicode property. Returns false for unknown names
/// (which are then rejected, never guessed). An empty `ranges` with a true
/// return means "no ASCII character has this property", which is a perfectly
/// good answer for e.g. \p{Mn}.
bool asciiRangesForProperty(std::string_view rawName, std::string& ranges) {
    std::string n = normalisePropertyName(rawName);
    // Oniguruma allows the optional "Is"/"In" prefix, e.g. \p{IsAlpha}.
    struct Entry {
        std::string_view name;
        std::string_view ranges;
    };
    static constexpr Entry kTable[] = {
        // letters
        {"l", "A-Za-z"},          {"letter", "A-Za-z"},
        {"alpha", "A-Za-z"},      {"alphabetic", "A-Za-z"},
        {"lu", "A-Z"},            {"uppercaseletter", "A-Z"},
        {"upper", "A-Z"},         {"uppercase", "A-Z"},
        {"ll", "a-z"},            {"lowercaseletter", "a-z"},
        {"lower", "a-z"},         {"lowercase", "a-z"},
        {"lt", "A-Z"},            {"titlecaseletter", "A-Z"},
        {"lm", ""},               {"modifierletter", ""},
        {"lo", ""},               {"otherletter", ""},
        {"lc", "A-Za-z"},
        // numbers
        {"n", "0-9"},             {"number", "0-9"},
        {"nd", "0-9"},            {"decimalnumber", "0-9"},
        {"digit", "0-9"},         {"nl", ""},
        {"letternumber", ""},     {"no", ""},
        {"othernumber", ""},
        // marks (none in ASCII)
        {"m", ""},   {"mark", ""}, {"mn", ""}, {"mc", ""}, {"me", ""},
        {"nonspacingmark", ""}, {"spacingmark", ""}, {"enclosingmark", ""},
        // punctuation
        {"p", "\\x21-\\x23\\x25-\\x2A\\x2C-\\x2F\\x3A\\x3B\\x3F\\x40\\x5B-\\x5D\\x5F\\x7B\\x7D"},
        {"punct", "\\x21-\\x2F\\x3A-\\x40\\x5B-\\x60\\x7B-\\x7E"},
        {"punctuation", "\\x21-\\x23\\x25-\\x2A\\x2C-\\x2F\\x3A\\x3B\\x3F\\x40\\x5B-\\x5D\\x5F\\x7B\\x7D"},
        {"pc", "_"},              {"connectorpunctuation", "_"},
        {"pd", "\\x2D"},          {"dashpunctuation", "\\x2D"},
        {"ps", "\\x28\\x5B\\x7B"},{"pe", "\\x29\\x5D\\x7D"},
        {"pi", ""},               {"pf", ""},
        {"po", "\\x21\\x22\\x23\\x25\\x26\\x27\\x2A\\x2C\\x2E\\x2F\\x3A\\x3B\\x3F\\x40\\x5C"},
        // symbols
        {"s", "\\x24\\x2B\\x3C-\\x3E\\x5E\\x60\\x7C\\x7E"},
        {"symbol", "\\x24\\x2B\\x3C-\\x3E\\x5E\\x60\\x7C\\x7E"},
        {"sm", "\\x2B\\x3C-\\x3E\\x7C\\x7E"},
        {"mathsymbol", "\\x2B\\x3C-\\x3E\\x7C\\x7E"},
        {"sc", "\\x24"},          {"currencysymbol", "\\x24"},
        {"sk", "\\x5E\\x60"},     {"modifiersymbol", "\\x5E\\x60"},
        {"so", ""},               {"othersymbol", ""},
        // separators / control / other
        {"z", " "},               {"separator", " "},
        {"zs", " "},              {"zl", ""}, {"zp", ""},
        {"c", "\\x00-\\x1F\\x7F"},{"cc", "\\x00-\\x1F\\x7F"},
        {"control", "\\x00-\\x1F\\x7F"},
        {"cf", ""},               {"format", ""},
        {"cn", ""},               {"co", ""}, {"cs", ""},
        // posix-style names Oniguruma also accepts inside \p{}
        {"alnum", "0-9A-Za-z"},   {"word", "0-9A-Za-z_"},
        {"w", "0-9A-Za-z_"},      {"space", "\\s"},
        {"whitespace", "\\s"},    {"blank", " \\t"},
        {"xdigit", "0-9A-Fa-f"},  {"asciihexdigit", "0-9A-Fa-f"},
        {"ascii", "\\x00-\\x7F"}, {"print", "\\x20-\\x7E"},
        {"graph", "\\x21-\\x7E"}, {"cntrl", "\\x00-\\x1F\\x7F"},
        {"any", "\\s\\S"},
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (const Entry& e : kTable) {
            if (e.name == n) {
                ranges.assign(e.ranges);
                return true;
            }
        }
        if (n.size() > 2 && (n.compare(0, 2, "is") == 0 || n.compare(0, 2, "in") == 0)) {
            n = n.substr(2);
        } else {
            break;
        }
    }
    return false;
}

/// Explicit ASCII expansion of a POSIX bracket class. std::regex's support for
/// [[:alpha:]] is implementation-defined in practice, and the corpus uses these
/// 6,396 times, so we expand them ourselves and depend on nothing.
bool asciiRangesForPosix(std::string_view name, std::string& ranges) {
    struct Entry {
        std::string_view name;
        std::string_view ranges;
    };
    static constexpr Entry kTable[] = {
        {"alpha", "A-Za-z"},
        {"alnum", "0-9A-Za-z"},
        {"digit", "0-9"},
        {"xdigit", "0-9A-Fa-f"},
        {"upper", "A-Z"},
        {"lower", "a-z"},
        {"space", " \\t\\r\\n\\v\\f"},
        {"blank", " \\t"},
        {"word", "0-9A-Za-z_"},
        {"punct", "\\x21-\\x2F\\x3A-\\x40\\x5B-\\x60\\x7B-\\x7E"},
        {"print", "\\x20-\\x7E"},
        {"graph", "\\x21-\\x7E"},
        {"cntrl", "\\x00-\\x1F\\x7F"},
        {"ascii", "\\x00-\\x7F"},
    };
    for (const Entry& e : kTable) {
        if (e.name == name) {
            ranges.assign(e.ranges);
            return true;
        }
    }
    return false;
}

/// Index one past the ']' that closes the class starting at s[i] == '['.
/// Understands the leading-']'-is-a-literal rule and [:posix:] members.
size_t scanClassEnd(std::string_view s, size_t i) noexcept {
    size_t j = i + 1;
    if (j < s.size() && s[j] == '^') ++j;
    if (j < s.size() && s[j] == ']') ++j;
    while (j < s.size()) {
        const char c = s[j];
        if (c == '\\') {
            j += 2;
            continue;
        }
        if (c == '[' && j + 1 < s.size() && s[j + 1] == ':') {
            const size_t k = s.find(":]", j + 2);
            if (k == std::string_view::npos) return std::string_view::npos;
            j = k + 2;
            continue;
        }
        if (c == ']') return j + 1;
        ++j;
    }
    return std::string_view::npos;
}

/// Oniguruma -> ECMAScript source translator.
///
/// Single pass, no backtracking, no recursion except one self-call for a
/// leading lookbehind body. Emits into two buffers simultaneously so the \G
/// variants stay in lockstep; every other construct is written to both.
class Translator {
public:
    explicit Translator(std::string_view src) noexcept : src_(src) {}

    PatternTranslation run() {
        tryStripLeadingLookbehind();
        while (i_ < src_.size() && !failed_) step();
        if (!failed_ && inClass_) fail("unterminated character class");
        if (!failed_ && depth_ != 0) fail("unbalanced parenthesis");
        if (failed_) {
            t_.ok = false;
            return std::move(t_);
        }
        t_.ok = true;
        t_.ecma = std::move(out_);
        t_.ecmaNoG = std::move(outNoG_);
        return std::move(t_);
    }

private:
    void emit(std::string_view s) {
        out_.append(s);
        outNoG_.append(s);
    }
    void emitCh(char c) {
        out_.push_back(c);
        outNoG_.push_back(c);
    }
    void emitLiteral(char c) {
        if (isEcmaMeta(c)) emitCh('\\');
        emitCh(c);
    }
    void emitHexByte(uint32_t v) {
        static const char kHex[] = "0123456789ABCDEF";
        emit("\\x");
        emitCh(kHex[(v >> 4) & 0xFu]);
        emitCh(kHex[v & 0xFu]);
    }
    void fail(std::string reason) {
        if (failed_) return;
        failed_ = true;
        t_.reason = std::move(reason);
    }
    void note(std::string_view text) {
        for (const std::string& existing : t_.notes) {
            if (existing == text) return;
        }
        t_.notes.emplace_back(text);
    }

    // --- top level dispatch -------------------------------------------------

    void step() {
        const char c = src_[i_];
        if (inClass_) {
            stepInClass(c);
            return;
        }
        switch (c) {
            case '\\':
                stepEscape();
                return;
            case '[':
                openClass();
                return;
            case '(':
                stepGroupOpen();
                return;
            case ')':
                if (depth_ == 0) {
                    fail("unbalanced ')'");
                    return;
                }
                --depth_;
                emitCh(')');
                ++i_;
                return;  // a following quantifier is handled by the main loop
            case '|':
                if (depth_ == 0) t_.topLevelAlternation = true;
                emitCh('|');
                ++i_;
                return;
            case '$':
                // Oniguruma/Ruby $ is end-of-line; ECMAScript $ (no multiline)
                // is end-of-text. Widen to "end, or just before a final \n".
                emit("(?=\\n?$)");
                ++i_;
                return;
            case '{':
                stepBrace();
                return;
            case '}':
                emit("\\}");
                ++i_;
                return;
            case ']':
                // A bare ] outside a class is Annex B only; std::regex may
                // reject it. Real grammars contain it (ini: "^(\[)(.*?)(])").
                emit("\\]");
                ++i_;
                return;
            case '*':
            case '+':
            case '?':
                emitCh(c);
                ++i_;
                consumeQuantifierModifier();
                return;
            default:
                emitCh(c);
                ++i_;
                return;
        }
    }

    /// Turns a possessive quantifier into a greedy one and keeps lazy ones.
    void consumeQuantifierModifier() {
        if (i_ >= src_.size()) return;
        const char c = src_[i_];
        if (c == '?') {
            emitCh('?');
            ++i_;
            return;
        }
        if (c == '+') {
            ++i_;
            note("possessive quantifier relaxed to greedy");
        }
    }

    void stepBrace() {
        size_t p = i_ + 1;
        std::string lo;
        std::string hi;
        bool comma = false;
        while (p < src_.size() && isDigitChar(src_[p])) lo.push_back(src_[p++]);
        if (p < src_.size() && src_[p] == ',') {
            comma = true;
            ++p;
            while (p < src_.size() && isDigitChar(src_[p])) hi.push_back(src_[p++]);
        }
        if (p >= src_.size() || src_[p] != '}' || (lo.empty() && hi.empty())) {
            emit("\\{");  // a literal brace, not a quantifier
            ++i_;
            return;
        }
        if (lo.empty()) lo = "0";  // {,m} is not valid ECMAScript
        std::string q = "{";
        q += lo;
        if (comma) {
            q += ",";
            q += hi;
        }
        q += "}";
        emit(q);
        i_ = p + 1;
        consumeQuantifierModifier();
    }

    void openClass() {
        inClass_ = true;
        ++i_;
        emitCh('[');
        if (i_ < src_.size() && src_[i_] == '^') {
            emitCh('^');
            ++i_;
        }
        if (i_ < src_.size() && src_[i_] == ']') {
            emit("\\]");  // POSIX/Oniguruma: a leading ] is a member, not the end
            ++i_;
        }
    }

    void stepInClass(char c) {
        if (c == ']') {
            emitCh(']');
            inClass_ = false;
            ++i_;
            return;
        }
        if (c == '\\') {
            stepEscape();
            return;
        }
        if (c == '&' && i_ + 1 < src_.size() && src_[i_ + 1] == '&') {
            fail("character class set intersection [a&&b]");
            return;
        }
        if (c == '[') {
            if (i_ + 1 < src_.size() && src_[i_ + 1] == ':') {
                stepPosixClass();
                return;
            }
            note("nested character class flattened to a literal '['");
            emit("\\[");
            ++i_;
            return;
        }
        emitCh(c);  // ranges, '-', literals, raw UTF-8 bytes
        ++i_;
    }

    void stepPosixClass() {
        const size_t close = src_.find(":]", i_ + 2);
        if (close == std::string_view::npos) {
            fail("unterminated POSIX bracket class");
            return;
        }
        std::string_view name = src_.substr(i_ + 2, close - (i_ + 2));
        if (!name.empty() && name.front() == '^') {
            fail("negated POSIX bracket class [[:^name:]]");
            return;
        }
        std::string ranges;
        if (!asciiRangesForPosix(name, ranges)) {
            fail("unknown POSIX bracket class [[:" + std::string(name) + ":]]");
            return;
        }
        emit(ranges);
        i_ = close + 2;
    }

    void stepEscape() {
        if (i_ + 1 >= src_.size()) {
            fail("trailing backslash");
            return;
        }
        const char d = src_[i_ + 1];
        switch (d) {
            case 'd':
            case 'D':
            case 's':
            case 'S':
            case 'w':
            case 'W':
            case 'n':
            case 'r':
            case 't':
            case 'f':
            case 'v':
                emitCh('\\');
                emitCh(d);
                i_ += 2;
                return;
            case '0':
                stepOctalEscape();
                return;
            case 'b':  // word boundary outside a class, backspace inside
                emit("\\b");
                i_ += 2;
                return;
            case 'B':
                if (inClass_) {
                    fail("\\B inside a character class");
                    return;
                }
                emit("\\B");
                i_ += 2;
                return;
            case 'e':
                emit("\\x1B");
                i_ += 2;
                return;
            case 'a':
                emit("\\x07");
                i_ += 2;
                return;
            case 'N':
                if (inClass_) {
                    fail("\\N inside a character class");
                    return;
                }
                emit("[^\\n]");
                i_ += 2;
                return;
            case 'O':
                emit(inClass_ ? "\\s\\S" : "[\\s\\S]");
                i_ += 2;
                return;
            case 'R':
                if (inClass_) {
                    fail("\\R inside a character class");
                    return;
                }
                emit("(?:\\r\\n|[\\r\\n])");
                i_ += 2;
                return;
            case 'h':
                emit(inClass_ ? "0-9a-fA-F" : "[0-9a-fA-F]");
                i_ += 2;
                return;
            case 'H':
                if (inClass_) {
                    fail("\\H inside a character class");
                    return;
                }
                emit("[^0-9a-fA-F]");
                i_ += 2;
                return;
            case 'A':
                if (inClass_) {
                    fail("\\A inside a character class");
                    return;
                }
                emit("^");
                note("\\A treated as start of line");
                i_ += 2;
                return;
            case 'z':
                if (inClass_) {
                    fail("\\z inside a character class");
                    return;
                }
                emit("$");
                i_ += 2;
                return;
            case 'Z':
                if (inClass_) {
                    fail("\\Z inside a character class");
                    return;
                }
                emit("(?=\\n?$)");
                note("\\Z treated as end of line");
                i_ += 2;
                return;
            case 'G':
                stepAnchorG();
                return;
            case 'p':
            case 'P':
                stepUnicodeProperty();
                return;
            case 'Q':
                stepQuoteSpan();
                return;
            case 'E':
                i_ += 2;  // stray \E
                return;
            case 'x':
                stepHexEscape();
                return;
            case 'u':
                stepUnicodeEscape();
                return;
            case 'c':
                if (i_ + 2 >= src_.size()) {
                    fail("truncated \\c escape");
                    return;
                }
                emitCh('\\');
                emitCh('c');
                emitCh(src_[i_ + 2]);
                i_ += 3;
                return;
            case 'k':
                stepNamedBackref();
                return;
            case 'g':
                fail("\\g<> subroutine call");
                return;
            case 'K':
                fail("\\K match reset");
                return;
            case 'X':
                fail("\\X grapheme cluster");
                return;
            case 'o':
                fail("\\o{} octal escape");
                return;
            default:
                break;
        }
        if (isDigitChar(d)) {
            if (inClass_) {
                fail("backreference inside a character class");
                return;
            }
            size_t j = i_ + 1;
            std::string digits;
            while (j < src_.size() && isDigitChar(src_[j])) digits.push_back(src_[j++]);
            emitCh('\\');
            emit(digits);
            i_ = j;
            return;
        }
        if (isAlnumChar(d)) {
            fail(std::string("unsupported escape \\") + d);
            return;
        }
        // Punctuation identity escape: always legal, keep it verbatim.
        emitCh('\\');
        emitCh(d);
        i_ += 2;
    }

    void stepOctalEscape() {
        size_t j = i_ + 2;
        uint32_t v = 0;
        int count = 0;
        while (j < src_.size() && isOctalChar(src_[j]) && count < 2) {
            v = v * 8u + static_cast<uint32_t>(src_[j] - '0');
            ++j;
            ++count;
        }
        if (count == 0) {
            emit("\\x00");
            i_ += 2;
            return;
        }
        emitHexByte(v);
        i_ = j;
    }

    void stepAnchorG() {
        if (inClass_) {
            fail("\\G inside a character class");
            return;
        }
        if (i_ == 0) t_.leadingG = true;
        t_.hasG = true;
        out_.append(kAlwaysMatchAssertion);
        outNoG_.append(kNeverMatchAssertion);
        i_ += 2;
        if (i_ < src_.size()) {
            const char c = src_[i_];
            if (c == '*' || c == '+' || c == '?' || c == '{') {
                fail("quantifier applied to \\G");
            }
        }
    }

    void stepUnicodeProperty() {
        const bool negatedByP = (src_[i_ + 1] == 'P');
        size_t j = i_ + 2;
        if (j >= src_.size() || src_[j] != '{') {
            fail("\\p not followed by {name}");
            return;
        }
        const size_t close = src_.find('}', j + 1);
        if (close == std::string_view::npos) {
            fail("unterminated \\p{...}");
            return;
        }
        std::string_view name = src_.substr(j + 1, close - j - 1);
        bool negated = negatedByP;
        if (!name.empty() && name.front() == '^') {
            negated = !negated;
            name.remove_prefix(1);
        }
        std::string ranges;
        if (!asciiRangesForProperty(name, ranges)) {
            fail("unsupported unicode property \\p{" + std::string(name) + "}");
            return;
        }
        note("\\p{...} approximated by ASCII ranges");
        if (inClass_) {
            if (!negated) {
                emit(ranges);  // may contribute nothing, which is correct
            } else if (ranges.empty()) {
                emit("\\s\\S");  // complement of the empty set is everything
            } else {
                fail("negated \\P{...} inside a character class");
                return;
            }
        } else if (ranges.empty()) {
            emit(negated ? kAnyChar : kNeverChar);
        } else {
            emit(negated ? "[^" : "[");
            emit(ranges);
            emit("]");
        }
        i_ = close + 1;
    }

    void stepQuoteSpan() {
        size_t j = i_ + 2;
        const size_t end = src_.find("\\E", j);
        const size_t stop = (end == std::string_view::npos) ? src_.size() : end;
        for (; j < stop; ++j) {
            const char ch = src_[j];
            if (inClass_) {
                if (ch == ']' || ch == '\\' || ch == '^' || ch == '-' || ch == '[') emitCh('\\');
                emitCh(ch);
            } else {
                emitLiteral(ch);
            }
        }
        i_ = (end == std::string_view::npos) ? src_.size() : end + 2;
    }

    void emitCodepoint(uint32_t cp) {
        if (cp < 0x80u) {
            emitHexByte(cp);
            return;
        }
        if (inClass_) {
            fail("non-ASCII codepoint escape inside a character class");
            return;
        }
        // Byte oriented engine: emit the UTF-8 bytes, wrapped so a following
        // quantifier applies to the whole codepoint and not to the last byte.
        char buf[4];
        int len = 0;
        if (cp < 0x800u) {
            buf[0] = static_cast<char>(0xC0u | (cp >> 6));
            buf[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
            len = 2;
        } else if (cp < 0x10000u) {
            buf[0] = static_cast<char>(0xE0u | (cp >> 12));
            buf[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            buf[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
            len = 3;
        } else if (cp <= 0x10FFFFu) {
            buf[0] = static_cast<char>(0xF0u | (cp >> 18));
            buf[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            buf[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            buf[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
            len = 4;
        } else {
            fail("codepoint escape out of range");
            return;
        }
        emit("(?:");
        emit(std::string_view(buf, static_cast<size_t>(len)));
        emit(")");
        note("codepoint escape emitted as UTF-8 bytes");
    }

    void stepHexEscape() {
        size_t j = i_ + 2;
        if (j < src_.size() && src_[j] == '{') {
            const size_t close = src_.find('}', j + 1);
            if (close == std::string_view::npos) {
                fail("unterminated \\x{...}");
                return;
            }
            const std::string_view hex = src_.substr(j + 1, close - j - 1);
            uint32_t cp = 0;
            if (hex.find(' ') != std::string_view::npos) {
                fail("\\x{...} with multiple codepoints");
                return;
            }
            if (!parseHexDigits(hex, cp)) {
                fail("malformed \\x{...}");
                return;
            }
            emitCodepoint(cp);
            i_ = close + 1;
            return;
        }
        size_t count = 0;
        uint32_t v = 0;
        while (j + count < src_.size() && count < 2) {
            const char c = src_[j + count];
            uint32_t d = 0;
            if (!parseHexDigits(std::string_view(&c, 1), d)) break;
            v = v * 16u + d;
            ++count;
        }
        if (count == 0) {
            fail("malformed \\x escape");
            return;
        }
        emitHexByte(v);
        i_ = j + count;
    }

    void stepUnicodeEscape() {
        size_t j = i_ + 2;
        if (j < src_.size() && src_[j] == '{') {
            stepHexEscape();  // \u{...} behaves like \x{...}
            return;
        }
        if (j + 4 > src_.size()) {
            fail("truncated \\u escape");
            return;
        }
        uint32_t cp = 0;
        if (!parseHexDigits(src_.substr(j, 4), cp)) {
            fail("malformed \\u escape");
            return;
        }
        emitCodepoint(cp);
        i_ = j + 4;
    }

    void stepNamedBackref() {
        if (inClass_) {
            fail("\\k inside a character class");
            return;
        }
        size_t j = i_ + 2;
        if (j >= src_.size()) {
            fail("truncated \\k");
            return;
        }
        const char open = src_[j];
        char close = 0;
        if (open == '<') close = '>';
        else if (open == '\'') close = '\'';
        if (close == 0) {
            fail("\\k must be followed by <name> or 'name'");
            return;
        }
        const size_t e = src_.find(close, j + 1);
        if (e == std::string_view::npos) {
            fail("unterminated \\k<name>");
            return;
        }
        const std::string_view nm = src_.substr(j + 1, e - j - 1);
        int index = -1;
        for (const std::pair<std::string, int>& g : t_.groupNames) {
            if (g.first == nm) index = g.second;
        }
        if (index < 0) {
            bool digits = !nm.empty();
            for (char c : nm) {
                if (!isDigitChar(c)) digits = false;
            }
            if (digits) {
                uint32_t v = 0;
                for (char c : nm) v = v * 10u + static_cast<uint32_t>(c - '0');
                index = static_cast<int>(v);
            }
        }
        if (index < 0) {
            fail("\\k<" + std::string(nm) + "> names an unknown group");
            return;
        }
        emitCh('\\');
        emit(std::to_string(index));
        i_ = e + 1;
    }

    void stepGroupOpen() {
        const size_t n = src_.size();
        if (i_ + 1 >= n || src_[i_ + 1] != '?') {
            ++t_.groupCount;
            ++depth_;
            emitCh('(');
            ++i_;
            return;
        }
        if (i_ + 2 >= n) {
            fail("truncated group");
            return;
        }
        const char c2 = src_[i_ + 2];
        if (c2 == ':') {
            emit("(?:");
            ++depth_;
            i_ += 3;
            return;
        }
        if (c2 == '=') {
            emit("(?=");
            ++depth_;
            i_ += 3;
            return;
        }
        if (c2 == '!') {
            emit("(?!");
            ++depth_;
            i_ += 3;
            return;
        }
        if (c2 == '>') {
            note("atomic group (?>...) relaxed to (?:...)");
            emit("(?:");
            ++depth_;
            i_ += 3;
            return;
        }
        if (c2 == '#') {
            const size_t close = src_.find(')', i_ + 3);
            if (close == std::string_view::npos) {
                fail("unterminated (?#comment)");
                return;
            }
            i_ = close + 1;
            return;
        }
        if (c2 == '(') {
            fail("conditional group (?(cond)yes|no)");
            return;
        }
        if (c2 == 'R' || c2 == '&' || c2 == '+' || c2 == '~') {
            fail("subroutine call or absent operator");
            return;
        }
        if (c2 == '<' || c2 == '\'' || (c2 == 'P' && i_ + 3 < n && src_[i_ + 3] == '<')) {
            size_t j = i_ + 2;
            if (c2 == 'P') ++j;
            const char open = src_[j];
            if (open == '<' && j + 1 < n && (src_[j + 1] == '=' || src_[j + 1] == '!')) {
                fail("lookbehind (?<=...)/(?<!...) is not expressible in std::regex");
                return;
            }
            const char close = (open == '<') ? '>' : '\'';
            const size_t e = src_.find(close, j + 1);
            if (e == std::string_view::npos) {
                fail("unterminated group name");
                return;
            }
            std::string name(src_.substr(j + 1, e - j - 1));
            if (name.empty()) {
                fail("empty group name");
                return;
            }
            ++t_.groupCount;
            ++depth_;
            t_.groupNames.emplace_back(std::move(name), t_.groupCount);
            emitCh('(');
            i_ = e + 1;
            return;
        }
        // Inline flag group: (?flags) or (?flags:...)
        size_t j = i_ + 2;
        bool negating = false;
        bool sawI = false;
        bool sawX = false;
        bool sawAny = false;
        while (j < n) {
            const char f = src_[j];
            if (f == '-') {
                negating = true;
                sawAny = true;
                ++j;
                continue;
            }
            if (f == 'i' || f == 'm' || f == 's' || f == 'x' || f == 'a' || f == 'd' || f == 'u' ||
                f == 'l' || f == 'n' || f == 'p') {
                if (!negating && f == 'i') sawI = true;
                if (!negating && f == 'x') sawX = true;
                sawAny = true;
                ++j;
                continue;
            }
            break;
        }
        if (sawAny && j < n && (src_[j] == ':' || src_[j] == ')')) {
            if (sawX) {
                fail("extended mode (?x) is not supported");
                return;
            }
            if (sawI) {
                t_.icase = true;
                note("inline (?i) widened to the whole pattern");
            }
            if (negating) note("inline flag reset (?-...) ignored");
            if (src_[j] == ':') {
                emit("(?:");
                ++depth_;
            }
            i_ = j + 1;
            return;
        }
        fail(std::string("unsupported group construct (?") + c2 + ")");
    }

    /// Detects "(?<=X)" / "(?<!X)" at offset 0 where X is exactly one character
    /// wide, strips it, and records the byte test to run outside the regex.
    void tryStripLeadingLookbehind() {
        if (src_.size() < 6) return;  // shortest form is (?<=x)
        if (src_.compare(0, 3, "(?<") != 0) return;
        const char kind = src_[3];
        if (kind != '=' && kind != '!') return;
        const size_t bodyStart = 4;
        size_t bodyEnd = 0;
        const char c = src_[bodyStart];
        if (c == '[') {
            const size_t e = scanClassEnd(src_, bodyStart);
            if (e == std::string_view::npos) return;
            bodyEnd = e;
        } else if (c == '\\') {
            if (bodyStart + 1 >= src_.size()) return;
            const char d = src_[bodyStart + 1];
            // Assertions, spans and multi-char constructs are not one character.
            if (std::strchr("GAzZbBKgRXQEk", d) != nullptr) return;
            if (isDigitChar(d)) return;  // backreference, unknown width
            if (d == 'p' || d == 'P') {
                const size_t open = bodyStart + 2;
                if (open >= src_.size() || src_[open] != '{') return;
                const size_t close = src_.find('}', open + 1);
                if (close == std::string_view::npos) return;
                bodyEnd = close + 1;
            } else if ((d == 'x' || d == 'u') && bodyStart + 2 < src_.size() &&
                       src_[bodyStart + 2] == '{') {
                const size_t close = src_.find('}', bodyStart + 3);
                if (close == std::string_view::npos) return;
                bodyEnd = close + 1;
            } else if (d == 'x') {
                bodyEnd = bodyStart + 2;
                while (bodyEnd < src_.size() && bodyEnd < bodyStart + 4) {
                    uint32_t ignored = 0;
                    const char h = src_[bodyEnd];
                    if (!parseHexDigits(std::string_view(&h, 1), ignored)) break;
                    ++bodyEnd;
                }
            } else if (d == 'u') {
                bodyEnd = bodyStart + 6;
                if (bodyEnd > src_.size()) return;
            } else if (d == 'c') {
                bodyEnd = bodyStart + 3;
                if (bodyEnd > src_.size()) return;
            } else {
                bodyEnd = bodyStart + 2;
            }
        } else if (isAsciiChar(c) && !isEcmaMeta(c)) {
            bodyEnd = bodyStart + 1;
        } else {
            return;
        }
        if (bodyEnd >= src_.size() || src_[bodyEnd] != ')') return;
        const std::string_view body = src_.substr(bodyStart, bodyEnd - bodyStart);
        const PatternTranslation bt = translateOnigToEcma(body);
        if (!bt.ok || bt.groupCount != 0 || bt.hasG) return;
        if (bt.lookbehind != LookbehindKind::None) return;
        for (char ch : bt.ecma) {
            if (!isAsciiChar(ch)) return;  // multi-byte comparison is out of scope
        }
        t_.lookbehind = (kind == '=') ? LookbehindKind::Positive : LookbehindKind::Negative;
        t_.lookbehindTest = bt.ecma;
        if (bt.icase) t_.icase = true;
        for (const std::string& n : bt.notes) note(n);
        note("leading single-character lookbehind emulated outside the regex");
        i_ = bodyEnd + 1;
    }

    std::string_view src_;
    size_t i_ = 0;
    std::string out_;
    std::string outNoG_;
    PatternTranslation t_;
    int depth_ = 0;
    bool inClass_ = false;
    bool failed_ = false;
};

/// std::regex backed IRegex. Holds up to three compiled regexes: the primary,
/// the "\G is impossible here" variant, and the one-byte lookbehind test.
class StdRegex final : public IRegex {
public:
    StdRegex(std::string source, std::regex primary, std::regex noG, std::regex lbTest,
             const PatternTranslation& t, std::shared_ptr<RegexEngineStats> stats)
        : source_(std::move(source)),
          re_(std::move(primary)),
          reNoG_(std::move(noG)),
          lbTest_(std::move(lbTest)),
          stats_(std::move(stats)),
          groupCount_(t.groupCount),
          hasG_(t.hasG),
          skipForwardScan_(t.leadingG && !t.topLevelAlternation),
          lookbehind_(t.lookbehind) {}

    std::optional<MatchResult> search(std::string_view text, size_t startPos) noexcept override {
        if (stats_) ++stats_->searchCalls;
        if (startPos > text.size()) return std::nullopt;
        if (hasG_) {
            // \G can only hold at the search position itself.
            std::optional<MatchResult> m = rawSearch(text, startPos, re_, true);
            if (m && lookbehindOk(text, m->begin)) return m;
            if (skipForwardScan_) return std::nullopt;
            if (startPos >= text.size()) return std::nullopt;
            return scanForward(text, startPos + 1, reNoG_);
        }
        return scanForward(text, startPos, re_);
    }

    [[nodiscard]] int groupCount() const noexcept override { return groupCount_; }
    [[nodiscard]] std::string_view pattern() const noexcept override { return source_; }

private:
    /// Repeats the search until the emulated lookbehind is satisfied. Each
    /// retry starts strictly later, so this always terminates.
    std::optional<MatchResult> scanForward(std::string_view text, size_t from,
                                           const std::regex& re) noexcept {
        size_t cur = from;
        while (cur <= text.size()) {
            std::optional<MatchResult> m = rawSearch(text, cur, re, false);
            if (!m) return std::nullopt;
            if (lookbehindOk(text, m->begin)) return m;
            if (m->begin >= text.size()) return std::nullopt;
            cur = m->begin + 1;
        }
        return std::nullopt;
    }

    bool lookbehindOk(std::string_view text, size_t pos) const noexcept {
        if (lookbehind_ == LookbehindKind::None) return true;
        bool present = false;
        if (pos > 0 && pos <= text.size() && !text.empty()) {
            const char* p = text.data() + (pos - 1);
            try {
                present = std::regex_match(p, p + 1, lbTest_);
            } catch (...) {
                present = false;
            }
        }
        return (lookbehind_ == LookbehindKind::Positive) ? present : !present;
    }

    std::optional<MatchResult> rawSearch(std::string_view text, size_t pos, const std::regex& re,
                                         bool continuous) noexcept {
        static const char kEmptyBuffer[1] = {'\0'};
        const char* base = text.empty() ? kEmptyBuffer : text.data();
        const char* first = base + pos;
        const char* last = base + text.size();
        std::regex_constants::match_flag_type flags = std::regex_constants::match_default;
        if (pos > 0) flags |= std::regex_constants::match_prev_avail;
        if (continuous) flags |= std::regex_constants::match_continuous;
        try {
            std::cmatch m;
            if (!std::regex_search(first, last, m, re, flags)) return std::nullopt;
            MatchResult out;
            out.begin = static_cast<size_t>(m[0].first - base);
            out.end = static_cast<size_t>(m[0].second - base);
            const size_t groups =
                std::max<size_t>(m.size(), static_cast<size_t>(groupCount_) + 1u);
            out.captures.resize(groups);
            for (size_t g = 0; g < groups; ++g) {
                Capture& c = out.captures[g];
                c.index = static_cast<int>(g);
                if (g < m.size() && m[g].matched) {
                    c.begin = static_cast<size_t>(m[g].first - base);
                    c.end = static_cast<size_t>(m[g].second - base);
                }
            }
            return out;
        } catch (...) {
            if (stats_) ++stats_->searchErrors;
            return std::nullopt;
        }
    }

    std::string source_;
    std::regex re_;
    std::regex reNoG_;
    std::regex lbTest_;
    std::shared_ptr<RegexEngineStats> stats_;
    int groupCount_ = 0;
    bool hasG_ = false;
    bool skipForwardScan_ = false;
    LookbehindKind lookbehind_ = LookbehindKind::None;
};

}  // namespace

PatternTranslation translateOnigToEcma(std::string_view pattern) {
    Translator tr(pattern);
    return tr.run();
}

StdRegexEngine::StdRegexEngine() : stats_(std::make_shared<RegexEngineStats>()) {}
StdRegexEngine::~StdRegexEngine() = default;

std::shared_ptr<IRegex> StdRegexEngine::compile(std::string_view pattern) noexcept {
    ++stats_->compileCalls;
    std::shared_ptr<IRegex> result;
    std::string failureReason;
    bool failed = false;
    try {
        const std::string key(pattern);
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            ++stats_->cacheHits;
            if (it->second) {
                lastError_.clear();
            } else {
                const auto reason = reasons_.find(key);
                lastError_ = (reason == reasons_.end()) ? std::string("unsupported pattern")
                                                        : reason->second;
            }
            return it->second;
        }
        PatternTranslation t = translateOnigToEcma(pattern);
        if (!t.ok) {
            cache_.emplace(key, nullptr);
            reasons_[key] = t.reason;
            lastError_ = t.reason;
            ++stats_->rejected;
            return nullptr;
        }
        std::regex_constants::syntax_option_type flags =
            std::regex_constants::ECMAScript | std::regex_constants::optimize;
        if (t.icase) flags |= std::regex_constants::icase;
        std::regex primary(t.ecma, flags);
        std::regex noG;
        if (t.hasG) noG.assign(t.ecmaNoG, flags);
        std::regex lbTest;
        if (t.lookbehind != LookbehindKind::None) lbTest.assign(t.lookbehindTest, flags);
        auto re = std::make_shared<StdRegex>(std::string(pattern), std::move(primary),
                                             std::move(noG), std::move(lbTest), t, stats_);
        cache_.emplace(key, re);
        ++stats_->compiled;
        if (t.lossy()) {
            lossy_[key] = t.notes.front();
            ++stats_->lossy;
        }
        lastError_.clear();
        result = std::move(re);
    } catch (const std::regex_error& e) {
        failureReason = std::string("std::regex rejected the translation: ") + e.what();
        failed = true;
    } catch (const std::exception& e) {
        failureReason = std::string("compile failed: ") + e.what();
        failed = true;
    } catch (...) {
        failureReason = "compile failed: unknown error";
        failed = true;
    }
    if (failed) {
        // Recording must never throw out of a noexcept function either.
        try {
            const std::string key(pattern);
            cache_[key] = nullptr;
            reasons_[key] = failureReason;
            lastError_ = failureReason;
        } catch (...) {
        }
        ++stats_->rejected;
        return nullptr;
    }
    return result;
}

std::string StdRegexEngine::errorFor(std::string_view pattern) const {
    const auto it = reasons_.find(std::string(pattern));
    return (it == reasons_.end()) ? std::string() : it->second;
}

RegexEngineCaps StdRegexEngine::caps() const noexcept {
    RegexEngineCaps c;
    c.anchorG = true;       // emulated; exact for anchors at the match start
    c.posixClasses = true;  // expanded to explicit ASCII ranges
    c.namedGroups = true;   // lowered to numbered groups
    return c;
}

void StdRegexEngine::clearCache() noexcept {
    try {
        cache_.clear();
        reasons_.clear();
        lossy_.clear();
        lastError_.clear();
    } catch (...) {
    }
}

}  // namespace ide::syntax
