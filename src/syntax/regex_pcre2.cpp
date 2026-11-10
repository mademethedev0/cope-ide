// PCRE2 adapter for IRegexEngine. The entire TU is guarded: with COPE_USE_PCRE2
// off this file compiles to nothing (a translation unit containing no
// declarations is well-formed) and the build is behaviorally identical to
// before phase 5.
#include <ide/syntax/regex_pcre2.h>

#ifdef COPE_HAS_PCRE2

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// The CMake interface target cope::regex_pcre2 defines PCRE2_CODE_UNIT_WIDTH=8
// on the command line; the local fallback keeps the include correct even if
// this TU is compiled outside that target (an identical redefinition is legal).
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include <pcre2.h>

namespace ide::syntax {
namespace {

constexpr bool isDigitChar(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isHexDigitChar(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// PCRE2 marks a non-participating capture with all-ones in the ovector. This
/// is intentionally the same bit pattern as kNoPosition, but restated so the
/// comparison does not depend on that coincidence.
constexpr PCRE2_SIZE kPcre2Unset = static_cast<PCRE2_SIZE>(~static_cast<PCRE2_SIZE>(0));

bool parseHex(std::string_view hex, uint32_t& out) noexcept {
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

bool parseOctal(std::string_view digits, uint32_t& out) noexcept {
    if (digits.empty()) return false;
    uint32_t v = 0;
    for (char c : digits) {
        if (c < '0' || c > '7') return false;
        v = v * 8u + static_cast<uint32_t>(c - '0');
    }
    out = v;
    return true;
}

/// Oniguruma -> PCRE2 source translator. Single pass, no recursion. Anything
/// PCRE2 already understands is copied verbatim; only the constructs listed
/// on translateOnigToPcre2's doc comment are rewritten. Malformed or
/// untranslatable input is copied verbatim and left for pcre2_compile to
/// refuse (a compile error is a normal refusal, never a crash).
class Pcre2Translator {
public:
    explicit Pcre2Translator(std::string_view src) noexcept : src_(src) {}

    bool run(std::string& out, std::string& reason) {
        out.clear();
        reason.clear();
        out_.reserve(src_.size() + 8u);
        const size_t n = src_.size();
        while (i_ < n && ok_) {
            const char c = src_[i_];
            if (inClass_) {
                if (c == '\\') {
                    stepEscape();
                    classFirst_ = false;
                } else if (c == '^' && classFirst_) {
                    // Negation sign; still "first" for the literal-']' rule.
                    out_.push_back(c);
                    ++i_;
                } else if (c == ']' && classFirst_) {
                    // A ']' as the first data character is a literal.
                    out_.push_back(c);
                    classFirst_ = false;
                    ++i_;
                } else if (c == '[' && i_ + 1 < n &&
                           (src_[i_ + 1] == ':' || src_[i_ + 1] == '.' || src_[i_ + 1] == '=')) {
                    // POSIX class / collating element / equivalence class:
                    // copy through its own ']' so it cannot close the outer
                    // class early.
                    const size_t close = src_.find(']', i_ + 2);
                    if (close == std::string_view::npos) {
                        fail("unterminated POSIX character class");
                    } else {
                        out_.append(src_.substr(i_, close - i_ + 1));
                        i_ = close + 1;
                    }
                    classFirst_ = false;
                } else if (c == '[' && i_ + 1 < n && src_[i_ + 1] == '^') {
                    // Oniguruma supports nested classes as set union; a
                    // *negated* inner class has to be expanded to its
                    // complement because PCRE2 has no nested classes.
                    stepNestedNegatedClass();
                    classFirst_ = false;
                } else if (c == '[') {
                    // Positive nested class: union means its brackets simply
                    // disappear. Depth keeps the inner ']' from closing us.
                    ++nestedClassDepth_;
                    ++i_;
                    classFirst_ = false;
                } else if (c == ']') {
                    if (nestedClassDepth_ > 0) {
                        --nestedClassDepth_;
                    } else {
                        inClass_ = false;
                        out_.push_back(c);
                    }
                    ++i_;
                    classFirst_ = false;
                } else {
                    out_.push_back(c);
                    ++i_;
                    classFirst_ = false;
                }
                continue;
            }
            if (c == '\\') {
                stepEscape();
            } else if (c == '[' && tryHandleExtendedClass()) {
                // The whole class was emitted as a PCRE2 extended class.
            } else if (c == '[') {
                inClass_ = true;
                classNegated_ = (i_ + 1 < n && src_[i_ + 1] == '^');
                classFirst_ = true;
                nestedClassDepth_ = 0;
                out_.push_back(c);
                ++i_;
            } else if (c == '(' && i_ + 1 < n && src_[i_ + 1] == '?') {
                stepGroupQuestion();
            } else if (c == '{') {
                stepBrace();
            } else {
                out_.push_back(c);
                ++i_;
            }
        }
        if (!ok_) {
            reason = reason_;
            return false;
        }
        out = std::move(out_);
        return true;
    }

private:
    void fail(std::string_view reason) {
        if (!ok_) return;
        ok_ = false;
        reason_ = std::string(reason);
    }

    /// PCRE2 is compiled with PCRE2_UTF|PCRE2_UCP, so a codepoint is emitted
    /// natively. This matches Oniguruma's UTF-8 semantics exactly and works
    /// inside character classes too.
    void emitCodepoint(uint32_t cp) { emitSmallCodepoint(cp); }

    void emitSmallCodepoint(uint32_t cp) {
        static const char kHex[] = "0123456789abcdef";
        out_ += "\\x{";
        char digits[8];
        int n = 0;
        do {
            digits[n++] = kHex[cp & 0xFu];
            cp >>= 4;
        } while (cp != 0u);
        while (n > 0) out_.push_back(digits[--n]);
        out_ += '}';
    }

    /// Decodes one UTF-8 codepoint at `pos`. Returns bytes consumed or 0.
    static size_t decodeUtf8(std::string_view s, size_t pos, uint32_t& cp) {
        if (pos >= s.size()) return 0;
        const auto b0 = static_cast<unsigned char>(s[pos]);
        if (b0 < 0x80u) {
            cp = b0;
            return 1;
        }
        size_t len = 0;
        uint32_t value = 0;
        if ((b0 & 0xE0u) == 0xC0u) {
            len = 2;
            value = b0 & 0x1Fu;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            len = 3;
            value = b0 & 0x0Fu;
        } else if ((b0 & 0xF8u) == 0xF0u) {
            len = 4;
            value = b0 & 0x07u;
        } else {
            return 0;
        }
        if (pos + len > s.size()) return 0;
        for (size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(s[pos + k]);
            if ((b & 0xC0u) != 0x80u) return 0;
            value = (value << 6) | (b & 0x3Fu);
        }
        cp = value;
        return len;
    }

    /// One simple codepoint token in a class body: a literal (UTF-8 decoded),
    /// \xHH, \x{...}, \o{...}, \uXXXX or a single-char escape. Returns the
    /// bytes consumed, or 0 when the token is not a plain codepoint.
    static size_t classAtom(std::string_view s, size_t pos, uint32_t& cp) {
        if (pos >= s.size()) return 0;
        if (s[pos] != '\\') return decodeUtf8(s, pos, cp);
        if (pos + 1 >= s.size()) return 0;
        const char d = s[pos + 1];
        if (d == 'x') {  // lowercase only: \X is a grapheme cluster in Onig
            if (pos + 2 < s.size() && s[pos + 2] == '{') {
                const size_t close = s.find('}', pos + 3);
                if (close == std::string_view::npos) return 0;
                return parseHex(s.substr(pos + 3, close - pos - 3), cp) ? close - pos + 1 : 0;
            }
            if (pos + 3 < s.size() && isHexDigitChar(s[pos + 2]) && isHexDigitChar(s[pos + 3])) {
                const auto hv = [](char c) -> uint32_t {
                    if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
                    return static_cast<uint32_t>((c | 0x20) - 'a' + 10);
                };
                cp = hv(s[pos + 2]) * 16u + hv(s[pos + 3]);
                return 4;
            }
            return 0;
        }
        if (d == 'o' && pos + 2 < s.size() && s[pos + 2] == '{') {
            const size_t close = s.find('}', pos + 3);
            if (close == std::string_view::npos) return 0;
            return parseOctal(s.substr(pos + 3, close - pos - 3), cp) ? close - pos + 1 : 0;
        }
        if (d == 'u') {
            if (pos + 2 < s.size() && s[pos + 2] == '{') {
                const size_t close = s.find('}', pos + 3);
                if (close == std::string_view::npos) return 0;
                return parseHex(s.substr(pos + 3, close - pos - 3), cp) ? close - pos + 1 : 0;
            }
            if (pos + 5 < s.size()) {
                bool hex = true;
                for (size_t k = 0; k < 4; ++k) {
                    if (!isHexDigitChar(s[pos + 2 + k])) hex = false;
                }
                if (hex && parseHex(s.substr(pos + 2, 4), cp)) return 6;
            }
            return 0;
        }
        switch (d) {
            case 'n': cp = 0x0Au; return 2;
            case 't': cp = 0x09u; return 2;
            case 'r': cp = 0x0Du; return 2;
            case 'f': cp = 0x0Cu; return 2;
            case 'v': cp = 0x0Bu; return 2;
            case 'a': cp = 0x07u; return 2;
            case 'e': cp = 0x1Bu; return 2;
            case '0':
                // \0 followed by a digit is an octal escape on Oniguruma;
                // do not guess, let the pattern be refused instead.
                if (pos + 2 < s.size() && isDigitChar(s[pos + 2])) return 0;
                cp = 0x00u;
                return 2;
            default: break;
        }
        const bool alphanumeric = (d >= '0' && d <= '9') || (d >= 'a' && d <= 'z') ||
                                  (d >= 'A' && d <= 'Z');
        if (alphanumeric) {
            // \w \d \s \h \p \P \R \X \k ... are character sets or
            // assertions, not literals. Evaluating them as the literal letter
            // would silently change the class; refuse instead.
            return 0;
        }
        // An escaped punctuation character is that literal (\-, \., \] ...).
        return decodeUtf8(s, pos + 1, cp) ? 2 : 0;
    }

    /// Parses a class body into codepoint ranges. Rejects anything that is not
    /// a plain literal/escape/range (properties, POSIX classes, set algebra).
    static bool parseSimpleClassBody(std::string_view body,
                                     std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
        size_t pos = 0;
        while (pos < body.size()) {
            uint32_t lo = 0;
            size_t used = classAtom(body, pos, lo);
            if (used == 0) return false;
            pos += used;
            uint32_t hi = lo;
            if (pos + 1 < body.size() && body[pos] == '-') {
                uint32_t next = 0;
                const size_t nextUsed = classAtom(body, pos + 1, next);
                if (nextUsed != 0) {
                    hi = next;
                    pos += 1 + nextUsed;
                }
            }
            if (hi < lo) return false;
            ranges.emplace_back(lo, hi);
        }
        return !ranges.empty();
    }

    /// Appends the ranges as PCRE2 class content, or fails when empty.
    void emitComplementRanges(std::vector<std::pair<uint32_t, uint32_t>> ranges) {
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        for (const auto& r : ranges) {
            if (!merged.empty() && r.first <= merged.back().second + 1u) {
                if (r.second > merged.back().second) merged.back().second = r.second;
            } else {
                merged.push_back(r);
            }
        }
        uint32_t cursor = 0;
        bool any = false;
        for (const auto& r : merged) {
            if (r.first > cursor) {
                emitCodepointRange(cursor, r.first - 1u);
                any = true;
            }
            if (r.second + 1u > cursor) cursor = r.second + 1u;
        }
        if (cursor <= 0x10FFFFu) {
            emitCodepointRange(cursor, 0x10FFFFu);
            any = true;
        }
        if (!any) fail("empty nested character class complement");
    }

    void emitCodepointRange(uint32_t lo, uint32_t hi) {
        emitCodepoint(lo);
        if (hi > lo) {
            out_.push_back('-');
            emitCodepoint(hi);
        }
    }

    /// Start of the class whose closing ']' is at `classEnd`. Scans forward so
    /// a leading literal ']' (`[]*\[]`) and POSIX items cannot confuse it.
    static size_t classStartFor(std::string_view out, size_t classEnd) {
        size_t start = 0;
        bool inClass = false;
        bool first = false;
        for (size_t i = 0; i <= classEnd && i < out.size(); ++i) {
            const char c = out[i];
            if (!inClass) {
                if (c == '\\') {
                    ++i;
                    continue;
                }
                if (c == '[') {
                    inClass = true;
                    start = i;
                    first = true;
                }
                continue;
            }
            if (c == '\\') {
                ++i;
                first = false;
                continue;
            }
            if (c == '[' && i + 1 < out.size() &&
                (out[i + 1] == ':' || out[i + 1] == '.' || out[i + 1] == '=')) {
                const size_t close = out.find(']', i + 2);
                if (close == std::string_view::npos) return start;
                i = close;
                first = false;
                continue;
            }
            if (c == ']') {
                if (first) {
                    first = false;
                    continue;
                }
                if (i == classEnd) return start;
                inClass = false;
                continue;
            }
            first = false;
        }
        return classEnd;
    }

    /// `[` outside a class. Most classes stream through the normal
    /// in-class path. A class that contains a top-level nested *negated*
    /// class we cannot enumerate (`[^\s\p{S}\p{P}]`) is emitted whole as a
    /// PCRE2 extended class, because there the union of the positive part and
    /// the negated part is expressible: `(?[ [pos] | [^neg] ])`.
    /// Returns true when it consumed the class (successfully or by failing).
    bool tryHandleExtendedClass() {
        const size_t open = i_;
        const size_t close = findClassEnd(open);
        if (close == std::string_view::npos) return false;
        const std::string_view body = src_.substr(open + 1, close - open - 1);
        if (body.empty() || body.front() == '^') return false;  // streaming handles it

        std::vector<std::pair<size_t, size_t>> negated;
        for (size_t k = 0; k < body.size();) {
            const char c = body[k];
            if (c == '\\') {
                k += 2;
                continue;
            }
            if (c == '[') {
                const size_t inner = classCloseIn(body, k);
                if (inner == std::string_view::npos) return false;
                if (k + 1 < body.size() && body[k + 1] == '^') {
                    std::vector<std::pair<uint32_t, uint32_t>> ranges;
                    if (!parseSimpleClassBody(body.substr(k + 2, inner - k - 2), ranges)) {
                        negated.emplace_back(k, inner);
                    }
                }
                k = inner + 1;
                continue;
            }
            ++k;
        }
        if (negated.empty()) return false;  // nothing the simple path cannot do

        std::string alt;
        bool firstAlt = true;
        const auto appendChunk = [&](std::string_view chunk) -> bool {
            if (chunk.empty()) return true;
            std::string translated;
            std::string reason;
            if (!translateOnigToPcre2(std::string("[") + std::string(chunk) + "]", translated,
                                      reason)) {
                fail(reason);
                return false;
            }
            if (translated.size() < 2u || translated.front() != '[' || translated.back() != ']') {
                fail("class rewrite produced a non-class fragment");
                return false;
            }
            if (!firstAlt) alt.push_back('|');
            firstAlt = false;
            alt += translated;
            return true;
        };

        size_t pos = 0;
        for (const auto& [begin, end] : negated) {
            if (!appendChunk(body.substr(pos, begin - pos))) break;
            std::string translated;
            std::string reason;
            if (!translateOnigToPcre2(std::string(body.substr(begin, end - begin + 1)),
                                      translated, reason)) {
                fail(reason);
                break;
            }
            if (!firstAlt) alt.push_back('|');
            firstAlt = false;
            alt += translated;
            pos = end + 1;
        }
        if (!ok_) {
            i_ = close + 1;
            return true;
        }
        if (!appendChunk(body.substr(pos))) {
            i_ = close + 1;
            return true;
        }
        if (firstAlt) {
            fail("empty extended character class");
            i_ = close + 1;
            return true;
        }
        out_ += "(?[";
        out_ += alt;
        out_ += "])";
        i_ = close + 1;
        return true;
    }

    /// Finds the ']' closing the class opened at `open` in `s` (relative scan).
    static size_t classCloseIn(std::string_view s, size_t open) {
        size_t k = open + 1;
        if (k < s.size() && s[k] == '^') ++k;
        bool first = true;
        for (; k < s.size(); ++k) {
            const char c = s[k];
            if (c == '\\') {
                ++k;
                first = false;
                continue;
            }
            if (c == ']') {
                if (first) {
                    first = false;
                    continue;
                }
                return k;
            }
            if (c == '[' && k + 1 < s.size() &&
                (s[k + 1] == ':' || s[k + 1] == '.' || s[k + 1] == '=')) {
                const size_t close = s.find(']', k + 2);
                if (close == std::string_view::npos) return std::string_view::npos;
                k = close;
                first = false;
                continue;
            }
            first = false;
        }
        return std::string_view::npos;
    }

    /// Finds the ']' closing the class opened at `open` (which points at '[').
    size_t findClassEnd(size_t open) const {
        size_t k = open + 1;
        if (k < src_.size() && src_[k] == '^') ++k;
        bool first = true;  // a ']' directly after '[' or '[^' is a literal
        for (; k < src_.size(); ++k) {
            const char c = src_[k];
            if (c == '\\') {
                ++k;
                first = false;
                continue;
            }
            if (c == ']') {
                if (first) {
                    first = false;
                    continue;
                }
                return k;
            }
            if (c == '[') {
                const size_t inner = findClassEnd(k);
                if (inner == std::string_view::npos) return std::string_view::npos;
                k = inner;
                first = false;
                continue;
            }
            first = false;
        }
        return std::string_view::npos;
    }

    /// `[^...]` nested inside a positive class: expand to the complement of
    /// its range set. Oniguruma reads nested classes as set union; PCRE2 has
    /// no nesting, so the union has to be flattened here.
    void stepNestedNegatedClass() {
        const size_t close = findClassEnd(i_);
        if (close == std::string_view::npos) {
            fail("unterminated nested character class");
            i_ = src_.size();
            return;
        }
        if (classNegated_) {
            fail("negated nested character class inside a negated class");
            i_ = close + 1;
            return;
        }
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        const std::string_view body = src_.substr(i_ + 2, close - i_ - 2);
        if (body.find("&&") != std::string_view::npos) {
            // Oniguruma class intersection. Not evaluable here; refusing is
            // better than emitting a class that silently means something else.
            fail("set intersection inside a nested negated class");
            i_ = close + 1;
            return;
        }
        if (!parseSimpleClassBody(body, ranges)) {
            fail("unsupported nested negated character class");
            i_ = close + 1;
            return;
        }
        emitComplementRanges(std::move(ranges));
        i_ = close + 1;
    }

    void stepEscape() {
        if (i_ + 1 >= src_.size()) {
            out_.push_back('\\');  // trailing backslash: pcre2_compile refuses
            ++i_;
            return;
        }
        const char d = src_[i_ + 1];
        switch (d) {
            case 'h':
                // Oniguruma means hexadecimal, PCRE2 means horizontal space.
                out_ += inClass_ ? "0-9A-Fa-f" : "[0-9A-Fa-f]";
                i_ += 2;
                return;
            case 'H':
                // POSIX negation composes correctly inside an existing class.
                out_ += inClass_ ? "[:^xdigit:]" : "[^0-9A-Fa-f]";
                i_ += 2;
                return;
            case 'Q': {
                // Quoted text is opaque: never translate escapes or brackets
                // inside it. An unclosed quote extends to the pattern end.
                const size_t end = src_.find("\\E", i_ + 2);
                const size_t stop = end == std::string_view::npos ? src_.size() : end + 2;
                out_.append(src_.substr(i_, stop - i_));
                i_ = stop;
                return;
            }
            case 'g':
                stepG();
                return;
            case 'u':
                stepUnicodeEscape();
                return;
            case 'O':
                // Oniguruma \O: any character, newline included.
                out_ += inClass_ ? "\\s\\S" : "[\\s\\S]";
                i_ += 2;
                return;
            case 'x':
                stepHexEscape();
                return;
            case 'o':
                stepOctalEscape();
                return;
            case 'p':
            case 'P':
                stepProperty();
                return;
            default:
                // Everything else (assertions, classes, \Q..\E, POSIX and
                // \p{...} bodies, quantifier modifiers) is native PCRE2; copy
                // the escape itself and let the main loop copy the rest.
                out_.push_back('\\');
                out_.push_back(d);
                i_ += 2;
                return;
        }
    }

    /// A '{' after a quantifier is a NESTED quantifier in Oniguruma: `a+{0,1}`
    /// means `(a+){0,1}`. PCRE2 refuses that outright ("quantifier does not
    /// follow a repeatable item"), so rewrite it to the equivalent grouped
    /// form. Treating the brace as a literal (the old behaviour) compiles but
    /// silently changes the rule, which is how every C++ `template<...>` rule
    /// stopped matching. A brace at pattern start or right after '(' / '|' has
    /// nothing to repeat: escape that one so both engines read a literal.
    void stepBrace() {
        if (endsWithQuantifier(out_)) {
            wrapQuantifiedAtom();
            emitQuantifier();
            return;
        }
        const char prev = out_.empty() ? '\0' : out_.back();
        const bool quantifiable = !out_.empty() && prev != '(' && prev != '|';
        if (quantifiable) {
            emitQuantifier();
        } else {
            out_ += "\\{";
            ++i_;
        }
    }

    /// Emits the quantifier starting at src_[i_] == '{'. Oniguruma swaps an
    /// inverted range (`a{2,1}` behaves like `a{1,2}`); PCRE2 refuses to
    /// compile it. Anything that is not a valid quantifier is emitted with
    /// its brace open so the main loop copies it as literal text, which is
    /// exactly how both engines read `a{}` and `x{foo}`.
    void emitQuantifier() {
        const size_t open = i_;
        const size_t close = src_.find('}', open + 1);
        if (close != std::string_view::npos) {
            uint32_t lo = 0;
            uint32_t hi = 0;
            bool hasLo = false;
            bool hasHi = false;
            bool comma = false;
            bool valid = true;
            for (size_t k = open + 1; k < close; ++k) {
                const char c = src_[k];
                if (c >= '0' && c <= '9') {
                    const uint32_t digit = static_cast<uint32_t>(c - '0');
                    if (!comma) {
                        lo = lo * 10u + digit;
                        hasLo = true;
                    } else {
                        hi = hi * 10u + digit;
                        hasHi = true;
                    }
                    if (lo > 1000000u || hi > 1000000u) {
                        valid = false;  // absurd bound: leave the text alone
                        break;
                    }
                } else if (c == ',' && !comma) {
                    comma = true;
                } else {
                    valid = false;
                    break;
                }
            }
            if (valid && hasLo && comma && hasHi && lo > hi) {
                out_.push_back('{');
                out_.append(std::to_string(hi));
                out_.push_back(',');
                out_.append(std::to_string(lo));
                out_.push_back('}');
                i_ = close + 1;
                return;
            }
        }
        out_.push_back('{');
        ++i_;
    }

    /// True when `out` ends in a quantifier: `*`, `+`, `?`, or a complete
    /// `{m,n}`. Escapes such as `\x{41}` / `\p{L}` are not quantifiers.
    static bool endsWithQuantifier(std::string_view out) {
        if (out.empty()) return false;
        const char c = out.back();
        if (c == '*' || c == '+' || c == '?') return true;
        return isQuantifierClose(out);
    }

    /// `out` ends with a `{digits[,digits]}` quantifier (and not an escape).
    static bool isQuantifierClose(std::string_view out) {
        if (out.size() < 3u || out.back() != '}') return false;
        const size_t open = out.rfind('{');
        if (open == std::string_view::npos || open == 0u) return false;
        // \x{41} / \p{L} / \P{L} / \o{7} are escapes, not quantifiers.
        if (open >= 2u && out[open - 2u] == '\\' &&
            (out[open - 1u] == 'x' || out[open - 1u] == 'p' || out[open - 1u] == 'P' ||
             out[open - 1u] == 'o')) {
            return false;
        }
        bool digits = false;
        for (size_t k = open + 1u; k + 1u < out.size(); ++k) {
            const char c = out[k];
            if (c >= '0' && c <= '9') {
                digits = true;
            } else if (c != ',') {
                return false;
            }
        }
        return digits;
    }

    /// True when the byte at `pos` is preceded by an odd number of backslashes.
    static bool isEscaped(std::string_view out, size_t pos) {
        size_t n = 0;
        while (pos > 0u && out[pos - 1u] == '\\') {
            --pos;
            ++n;
        }
        return (n & 1u) != 0u;
    }

    /// Index just past the quantified atom, i.e. the end with its quantifier
    /// (and any lazy/possessive modifier) stripped: `(ab)+?` -> end of `)`.
    static size_t skipQuantifierSuffix(std::string_view out, size_t end) {
        while (end > 0u) {
            const char c = out[end - 1u];
            if (c == '*' || c == '+' || c == '?') {
                --end;
                continue;
            }
            if (c == '}' && isQuantifierClose(out.substr(0u, end))) {
                const size_t open = out.substr(0u, end).rfind('{');
                if (open == std::string_view::npos) break;
                end = open;
                continue;
            }
            break;
        }
        return end;
    }

    /// Start of the single atom ending at `end`: a character class, a group,
    /// an escape, or one literal byte.
    static size_t atomStart(std::string_view out, size_t end) {
        if (end == 0u) return 0u;
        const char c = out[end - 1u];
        if (c == ']') return classStartFor(out, end - 1u);
        if (c == ')') {
            size_t depth = 0;
            for (size_t k = end; k-- > 0u;) {
                if (isEscaped(out, k)) continue;
                if (out[k] == ')') {
                    ++depth;
                } else if (out[k] == '(') {
                    --depth;
                    if (depth == 0u) return k;
                }
            }
            return end - 1u;
        }
        if (c == '}') {
            const size_t open = out.substr(0u, end).rfind('{');
            if (open != std::string_view::npos && open >= 2u && out[open - 2u] == '\\') {
                return open - 2u;  // \x{41} style escape atom
            }
            return end - 1u;
        }
        if (end >= 2u && out[end - 2u] == '\\') return end - 2u;  // \w, \s, ...
        return end - 1u;
    }

    /// `ATOM Q` at the tail becomes `(?:ATOM Q)` so a following quantifier can
    /// apply to the whole quantified atom the way Oniguruma reads it.
    void wrapQuantifiedAtom() {
        const size_t atomEnd = skipQuantifierSuffix(out_, out_.size());
        const size_t start = atomStart(out_, atomEnd);
        out_.insert(start, "(?:");
        out_.push_back(')');
    }

    /// \p{...} / \P{...}: Oniguruma accepts the POSIX long names (alpha,
    /// alnum, word, print, ...) that PCRE2's Unicode property syntax does not
    /// know. Those are rewritten to POSIX bracket classes, which PCRE2_UCP
    /// makes Unicode-aware. Unicode property names (L, Lu, Greek, ...) are native PCRE2
    /// and copied verbatim. Onig's \p{^Name} negation flips the sense.
    void stepProperty() {
        const bool negated = (src_[i_ + 1] == 'P');
        const size_t j = i_ + 2;
        if (j >= src_.size() || src_[j] != '{') {
            out_.push_back('\\');
            out_.push_back(src_[i_ + 1]);  // \p not followed by '{': refuse below
            i_ += 2;
            return;
        }
        const size_t close = src_.find('}', j + 1);
        if (close == std::string_view::npos) {
            out_.push_back('\\');
            out_.push_back(src_[i_ + 1]);
            i_ += 2;
            return;
        }
        std::string_view name = src_.substr(j + 1, close - j - 1);
        bool negateName = false;
        if (!name.empty() && name.front() == '^') {
            negateName = true;
            name.remove_prefix(1);
        }
        const bool negative = negated != negateName;
        static constexpr std::string_view kPosix[] = {
            "alpha", "alnum", "blank", "cntrl", "digit", "graph", "lower",
            "print", "punct", "space", "upper", "word",  "xdigit", "ascii",
        };
        bool posix = false;
        for (std::string_view candidate : kPosix) {
            if (name == candidate) {
                posix = true;
                break;
            }
        }
        if (!posix) {
            out_.append(src_.substr(i_, close - i_ + 1));  // native: verbatim
            i_ = close + 1;
            return;
        }
        if (inClass_) {
            out_ += negative ? "[:^" : "[:";
            out_.append(name);
            out_ += ":]";
        } else {
            out_ += negative ? "[^[:" : "[[:";
            out_.append(name);
            out_ += ":]]";
        }
        i_ = close + 1;
    }

    /// \g<name> \g'name' \g<n> \g<0> : Oniguruma subroutine calls, which PCRE2
    /// spells (?&name) / (?n) / (?R). Left verbatim, PCRE2 would read \g<name>
    /// as a *backreference* -- silently wrong, not a refusal -- so this
    /// rewrite is correctness, not polish.
    void stepG() {
        const size_t j = i_ + 2;
        if (j >= src_.size() || (src_[j] != '<' && src_[j] != '\'')) {
            out_ += "\\g";  // malformed: let pcre2_compile refuse it
            i_ += 2;
            return;
        }
        const char close = (src_[j] == '<') ? '>' : '\'';
        const size_t e = src_.find(close, j + 1);
        if (e == std::string_view::npos) {
            out_ += "\\g";
            i_ += 2;
            return;
        }
        const std::string_view inner = src_.substr(j + 1, e - j - 1);
        bool numeric = !inner.empty();
        size_t s = 0;
        if (!inner.empty() && (inner[0] == '+' || inner[0] == '-')) s = 1;
        for (size_t k = s; k < inner.size(); ++k) {
            if (!isDigitChar(inner[k])) numeric = false;
        }
        if (inner == "0") {
            out_ += "(?R)";
        } else if (numeric) {
            out_ += "(?";
            out_.append(inner);
            out_ += ")";
        } else {
            out_ += "(?&";
            out_.append(inner);
            out_ += ")";
        }
        i_ = e + 1;
    }

    /// \uXXXX and \u{...}: PCRE2 has no \u escape.
    void stepUnicodeEscape() {
        const size_t j = i_ + 2;
        if (j < src_.size() && src_[j] == '{') {
            const size_t close = src_.find('}', j + 1);
            if (close != std::string_view::npos) {
                uint32_t cp = 0;
                if (parseHex(src_.substr(j + 1, close - j - 1), cp)) {
                    emitCodepoint(cp);
                    i_ = close + 1;
                    return;
                }
            }
        } else if (j + 4 <= src_.size()) {
            uint32_t cp = 0;
            bool hex = true;
            for (size_t k = 0; k < 4; ++k) {
                if (!isHexDigitChar(src_[j + k])) hex = false;
            }
            if (hex && parseHex(src_.substr(j, 4), cp)) {
                emitCodepoint(cp);
                i_ = j + 4;
                return;
            }
        }
        out_ += "\\u";  // malformed: pcre2_compile refuses
        i_ += 2;
    }

    /// \x{...}: native PCRE2 syntax in UTF mode, emitted verbatim.
    void stepHexEscape() {
        const size_t j = i_ + 2;
        if (j < src_.size() && src_[j] == '{') {
            const size_t close = src_.find('}', j + 1);
            if (close != std::string_view::npos) {
                uint32_t cp = 0;
                if (parseHex(src_.substr(j + 1, close - j - 1), cp)) {
                    out_.append(src_.substr(i_, close - i_ + 1));
                    i_ = close + 1;
                    return;
                }
            }
        }
        out_ += "\\x";  // plain \xHH: hex digits copied by the main loop
        i_ += 2;
    }

    /// \o{...}: Oniguruma octal escape; PCRE2 has none.
    void stepOctalEscape() {
        const size_t j = i_ + 2;
        if (j < src_.size() && src_[j] == '{') {
            const size_t close = src_.find('}', j + 1);
            if (close != std::string_view::npos) {
                uint32_t cp = 0;
                if (parseOctal(src_.substr(j + 1, close - j - 1), cp)) {
                    emitCodepoint(cp);
                    i_ = close + 1;
                    return;
                }
            }
        }
        out_ += "\\o";  // malformed: pcre2_compile refuses
        i_ += 2;
    }

    /// At '(' + '?': recognises inline flag groups ((?flags) / (?flags:...))
    /// and maps Oniguruma's 'm' (dot-all) to PCRE2's 's'. Everything else
    /// (named groups, lookarounds, conditionals, (?#, (?R)...) starts with a
    /// non-flag character and is copied verbatim.
    void stepGroupQuestion() {
        const size_t n = src_.size();
        size_t j = i_ + 2;
        bool any = false;
        while (j < n) {
            const char f = src_[j];
            if (f == '-' || f == 'i' || f == 'm' || f == 's' || f == 'x' || f == 'a' ||
                f == 'd' || f == 'u' || f == 'l' || f == 'n' || f == 'p' || f == 'J' ||
                f == 'U') {
                ++j;
                any = true;
                continue;
            }
            break;
        }
        if (any && j < n && (src_[j] == ':' || src_[j] == ')')) {
            out_ += "(?";
            for (size_t k = i_ + 2; k < j; ++k) {
                out_.push_back(src_[k] == 'm' ? 's' : src_[k]);
            }
            out_.push_back(src_[j]);
            i_ = j + 1;
            return;
        }
        out_ += "(?";
        i_ += 2;
    }

    std::string_view src_;
    size_t i_ = 0;
    std::string out_;
    std::string reason_;
    bool inClass_ = false;
    bool classNegated_ = false;
    bool classFirst_ = false;
    int nestedClassDepth_ = 0;
    bool ok_ = true;
};

/// One compiled PCRE2 pattern. Holds the pcre2_code (freed in the destructor)
/// and whether the JIT accepted it; search() allocates fresh match data per
/// call so the pattern stays re-entrant with respect to distinct texts.
class Pcre2Regex final : public IRegex {
public:
    Pcre2Regex(std::string source, pcre2_code* code, bool jit,
               std::shared_ptr<RegexEngineStats> stats)
        : source_(std::move(source)),
          code_(code),
          jit_(jit),
          stats_(std::move(stats)) {
        uint32_t ncaps = 0;
        pcre2_pattern_info(code_, PCRE2_INFO_CAPTURECOUNT, &ncaps);
        groupCount_ = static_cast<int>(ncaps);
    }

    ~Pcre2Regex() override { pcre2_code_free(code_); }

    Pcre2Regex(const Pcre2Regex&) = delete;
    Pcre2Regex& operator=(const Pcre2Regex&) = delete;

    std::optional<MatchResult> search(std::string_view text, size_t startPos) noexcept override {
        if (stats_) ++stats_->searchCalls;
        if (startPos > text.size()) return std::nullopt;
        // PCRE2 never reads past `length`, but an empty subject may carry a
        // null data pointer; hand it a valid address instead.
        static const char kEmptySubject[1] = {'\0'};
        const char* base = text.empty() ? kEmptySubject : text.data();
        pcre2_match_data* md = pcre2_match_data_create_from_pattern(code_, nullptr);
        if (md == nullptr) {
            if (stats_) ++stats_->searchErrors;
            return std::nullopt;
        }
        int rc = jit_
                           ? pcre2_jit_match(code_,
                                             reinterpret_cast<PCRE2_SPTR>(base),
                                             static_cast<PCRE2_SIZE>(text.size()),
                                             static_cast<PCRE2_SIZE>(startPos), 0u, md, nullptr)
                           : pcre2_match(code_,
                                         reinterpret_cast<PCRE2_SPTR>(base),
                                         static_cast<PCRE2_SIZE>(text.size()),
                                         static_cast<PCRE2_SIZE>(startPos), 0u, md, nullptr);
        if (jit_ && (rc == PCRE2_ERROR_JIT_STACKLIMIT || rc == PCRE2_ERROR_JIT_BADOPTION)) {
            // JIT stack availability differs across Android ABIs. A runtime
            // JIT refusal must not silently disable an otherwise valid rule.
            rc = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(base),
                             static_cast<PCRE2_SIZE>(text.size()),
                             static_cast<PCRE2_SIZE>(startPos), PCRE2_NO_JIT, md, nullptr);
        }
        if (rc < 0) {
            // Includes PCRE2_ERROR_NOMATCH and any runtime failure (stack
            // limits, bad JIT state): never throw, report "no match".
            if (rc != PCRE2_ERROR_NOMATCH && stats_) ++stats_->searchErrors;
            pcre2_match_data_free(md);
            return std::nullopt;
        }
        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        MatchResult out;
        out.begin = static_cast<size_t>(ov[0]);
        out.end = static_cast<size_t>(ov[1]);
        const size_t groups = static_cast<size_t>(groupCount_) + 1u;
        out.captures.resize(groups);
        for (size_t g = 0; g < groups; ++g) {
            Capture& c = out.captures[g];
            c.index = static_cast<int>(g);
            const PCRE2_SIZE b = ov[2u * g];
            const PCRE2_SIZE e = ov[2u * g + 1u];
            if (b == kPcre2Unset || e == kPcre2Unset) continue;  // did not participate
            c.begin = static_cast<size_t>(b);
            c.end = static_cast<size_t>(e);
        }
        pcre2_match_data_free(md);
        return out;
    }

    [[nodiscard]] int groupCount() const noexcept override { return groupCount_; }
    [[nodiscard]] std::string_view pattern() const noexcept override { return source_; }

private:
    std::string source_;
    pcre2_code* code_;
    bool jit_;
    std::shared_ptr<RegexEngineStats> stats_;
    int groupCount_ = 0;
};

std::string pcre2ErrorMessage(int code, PCRE2_SIZE offset) {
    PCRE2_UCHAR buf[256];
    const int n = pcre2_get_error_message(code, buf, sizeof buf);
    std::string msg = "pcre2 compile error ";
    msg += std::to_string(code);
    msg += " at offset ";
    msg += std::to_string(static_cast<size_t>(offset));
    if (n > 0) {
        msg += ": ";
        msg.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    }
    return msg;
}

}  // namespace

bool translateOnigToPcre2(std::string_view pattern, std::string& out, std::string& reason) {
    Pcre2Translator tr(pattern);
    return tr.run(out, reason);
}

Pcre2RegexEngine::Pcre2RegexEngine() : stats_(std::make_shared<RegexEngineStats>()) {}

Pcre2RegexEngine::~Pcre2RegexEngine() = default;

std::shared_ptr<IRegex> Pcre2RegexEngine::compile(std::string_view pattern) noexcept {
    ++stats_->compileCalls;
    std::string key;
    std::string failureReason;
    bool failed = false;
    std::shared_ptr<IRegex> result;
    try {
        key.assign(pattern);
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            ++stats_->cacheHits;
            const auto reason = reasons_.find(key);
            lastError_ = (it->second || reason == reasons_.end()) ? std::string()
                                                                  : reason->second;
            return it->second;
        }
        std::string translated;
        std::string reason;
        if (!translateOnigToPcre2(pattern, translated, reason)) {
            failureReason = reason;
            failed = true;
        } else {
            int errorCode = 0;
            PCRE2_SIZE errorOffset = 0;
            // Oniguruma matches UTF-8 codepoints, not bytes, so PCRE2 must
            // run in UTF mode with Unicode properties to reproduce it.
            // MATCH_INVALID_UTF keeps hostile input safe: an invalid sequence
            // stops a match instead of failing the whole search.
            uint32_t compileOptions = PCRE2_UTF | PCRE2_UCP;
#ifdef PCRE2_MATCH_INVALID_UTF
            compileOptions |= PCRE2_MATCH_INVALID_UTF;
#endif
            pcre2_code* code =
                pcre2_compile(reinterpret_cast<PCRE2_SPTR>(translated.data()),
                              static_cast<PCRE2_SIZE>(translated.size()), compileOptions,
                              &errorCode, &errorOffset, nullptr);
            if (code == nullptr) {
                failureReason = pcre2ErrorMessage(errorCode, errorOffset);
                failed = true;
            } else {
                // JIT is an optimization only: a refusal (embedded, platform,
                // pattern-shape) must degrade to the interpreter, never fail.
                const int jitRc = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
                const bool jit = (jitRc == 0);
                auto re = std::make_shared<Pcre2Regex>(key, code, jit, stats_);
                cache_.emplace(key, re);
                ++stats_->compiled;
                lastError_.clear();
                result = std::move(re);
            }
        }
    } catch (const std::exception& e) {
        failureReason = std::string("compile failed: ") + e.what();
        failed = true;
    } catch (...) {
        failureReason = "compile failed: unknown error";
        failed = true;
    }
    if (failed) {
        try {
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

std::string Pcre2RegexEngine::errorFor(std::string_view pattern) const {
    const auto it = reasons_.find(std::string(pattern));
    return (it == reasons_.end()) ? std::string() : it->second;
}

RegexEngineCaps Pcre2RegexEngine::caps() const noexcept {
    RegexEngineCaps c;
    c.lookbehind = true;        // bounded lookbehind; unbounded refuses at compile
    c.anchorG = true;
    c.anchorAzZ = true;
    c.unicodeProperties = true; // PCRE2_UCP: \p{...} and POSIX classes are Unicode-aware
    c.posixClasses = true;
    c.namedGroups = true;
    c.possessive = true;
    c.atomicGroups = true;
    c.conditionals = true;
    c.subroutines = true;       // via the \g<> -> (?&name) rewrite
    c.keepOut = true;
    c.graphemeCluster = true;   // \X is native in UTF mode
    c.utf8Aware = true;         // PCRE2_UTF: classes/quantifiers span codepoints
    return c;
}

void Pcre2RegexEngine::clearCache() noexcept {
    try {
        cache_.clear();
        reasons_.clear();
        lastError_.clear();
    } catch (...) {
    }
}

}  // namespace ide::syntax

#endif  // COPE_HAS_PCRE2
