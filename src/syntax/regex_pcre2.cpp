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
                } else {
                    if (c == ']') inClass_ = false;
                    out_.push_back(c);
                    ++i_;
                }
                continue;
            }
            if (c == '\\') {
                stepEscape();
            } else if (c == '[') {
                inClass_ = true;
                out_.push_back(c);
                ++i_;
            } else if (c == '(' && i_ + 1 < n && src_[i_ + 1] == '?') {
                stepGroupQuestion();
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

    /// Emits a codepoint above U+00FF as its UTF-8 bytes wrapped in a
    /// non-capturing group, because we compile in 8-bit non-UTF mode (so
    /// arbitrary file bytes can never make a match fail) and PCRE2 refuses
    /// \x{cp} with cp > 0xFF in that mode. Byte-oriented, like StdRegex.
    void emitCodepoint(uint32_t cp) {
        if (inClass_) {
            fail("codepoint above U+00FF inside a character class");
            return;
        }
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
        } else {
            buf[0] = static_cast<char>(0xF0u | (cp >> 18));
            buf[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            buf[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            buf[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
            len = 4;
        }
        out_ += "(?:";
        out_.append(buf, static_cast<size_t>(len));
        out_ += ")";
    }

    void emitSmallCodepoint(uint32_t cp) {
        static const char kHex[] = "0123456789abcdef";
        out_ += "\\x{";
        // At most two digits: callers guarantee cp <= 0xFF.
        if (cp >= 0x10u) out_.push_back(kHex[(cp >> 4) & 0xFu]);
        out_.push_back(kHex[cp & 0xFu]);
        out_ += "}";
    }

    void stepEscape() {
        if (i_ + 1 >= src_.size()) {
            out_.push_back('\\');  // trailing backslash: pcre2_compile refuses
            ++i_;
            return;
        }
        const char d = src_[i_ + 1];
        switch (d) {
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
        if (inner[0] == '+' || inner[0] == '-') s = 1;
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
                    if (cp > 0xFFu) {
                        emitCodepoint(cp);
                    } else {
                        emitSmallCodepoint(cp);
                    }
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
                if (cp > 0xFFu) {
                    emitCodepoint(cp);
                } else {
                    emitSmallCodepoint(cp);
                }
                i_ = j + 4;
                return;
            }
        }
        out_ += "\\u";  // malformed: pcre2_compile refuses
        i_ += 2;
    }

    /// \x{...}: verbatim for cp <= 0xFF (native), UTF-8 emission above that.
    void stepHexEscape() {
        const size_t j = i_ + 2;
        if (j < src_.size() && src_[j] == '{') {
            const size_t close = src_.find('}', j + 1);
            if (close != std::string_view::npos) {
                uint32_t cp = 0;
                if (parseHex(src_.substr(j + 1, close - j - 1), cp)) {
                    if (cp > 0xFFu) {
                        emitCodepoint(cp);
                    } else {
                        out_ += src_.substr(i_, close - i_ + 1);  // verbatim
                    }
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
                    if (cp > 0xFFu) {
                        emitCodepoint(cp);
                    } else {
                        emitSmallCodepoint(cp);
                    }
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
        groupCount_ = static_cast<int>(pcre2_get_capture_count(code_));
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
        const int rc = jit_
                           ? pcre2_jit_match(code_,
                                             reinterpret_cast<PCRE2_SPTR>(base),
                                             static_cast<PCRE2_SIZE>(text.size()),
                                             static_cast<PCRE2_SIZE>(startPos), 0u, md, nullptr)
                           : pcre2_match(code_,
                                         reinterpret_cast<PCRE2_SPTR>(base),
                                         static_cast<PCRE2_SIZE>(text.size()),
                                         static_cast<PCRE2_SIZE>(startPos), 0u, md, nullptr);
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
            pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(translated.data()),
                                             static_cast<PCRE2_SIZE>(translated.size()), 0u,
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
    c.unicodeProperties = true; // byte mode: only ASCII bytes can participate
    c.posixClasses = true;
    c.namedGroups = true;
    c.possessive = true;
    c.atomicGroups = true;
    c.conditionals = true;
    c.subroutines = true;       // via the \g<> -> (?&name) rewrite
    c.keepOut = true;
    c.graphemeCluster = true;   // byte-mode approximation of \X
    c.utf8Aware = false;        // deliberate: 8-bit non-UTF mode, byte offsets
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
