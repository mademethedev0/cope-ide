#pragma once

// Why this file exists
// --------------------
// PCRE2-backed implementation of IRegexEngine: the phase-5 upgrade that makes
// the ~15% of TextMate patterns std::regex refuses (lookbehind, \G, possessive
// quantifiers, atomic groups, backrefs into named groups, \K, \X, subroutine
// recursion) compile natively instead of being dropped.
//
// Everything here is optional. The whole header is guarded by COPE_HAS_PCRE2,
// which is defined only when the build links cope::regex_pcre2 (root CMake
// option COPE_USE_PCRE2, default OFF). With the option off, this header is an
// empty file and std::regex remains the engine -- never a hard dependency.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <ide/syntax/regex.h>

#ifdef COPE_HAS_PCRE2

namespace ide::syntax {

/// Translates one Oniguruma-dialect pattern into PCRE2 source. PCRE2 already
/// speaks almost all of Oniguruma natively (lookbehind, \G, possessive and
/// atomic groups, named groups, \K, \X, POSIX classes, \p{...}); only these
/// need rewriting:
///   \g<name> \g'name' \g<0> \g<n>  subroutine calls -> (?&name) (?R) (?n)
///   \uXXXX / \u{...}               -> \x{...} (PCRE2 has no \u escape)
///   \o{...}                        -> \x{...}
///   \O (any char incl newline)     -> [\s\S]
///   (?m) (?-m) (Onig: dot-all)     -> (?s) (?-s) (PCRE2: dot-all)
///   \x{cp} with cp > U+00FF        -> the UTF-8 bytes of cp, wrapped
///                                     (we compile in 8-bit non-UTF mode so
///                                      arbitrary file bytes never fail a match)
/// Returns false with a reason when the pattern cannot be expressed; such
/// patterns must be refused, never guessed. Never throws.
[[nodiscard]] bool translateOnigToPcre2(std::string_view pattern, std::string& out,
                                        std::string& reason);

/// PCRE2 based IRegexEngine. Caches by pattern string (including negative
/// results, like StdRegexEngine) so a refused pattern costs one compile for
/// the process lifetime. Matching uses the JIT when pcre2_jit_compile
/// succeeded for the pattern and falls back to the interpreting
/// pcre2_match otherwise -- a missing or declining JIT never fails a match.
class Pcre2RegexEngine final : public IRegexEngine {
public:
    Pcre2RegexEngine();
    ~Pcre2RegexEngine() override;

    Pcre2RegexEngine(const Pcre2RegexEngine&) = delete;
    Pcre2RegexEngine& operator=(const Pcre2RegexEngine&) = delete;

    std::shared_ptr<IRegex> compile(std::string_view pattern) noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override { return "pcre2"; }
    [[nodiscard]] std::string lastError() const override { return lastError_; }
    [[nodiscard]] std::string errorFor(std::string_view pattern) const override;
    [[nodiscard]] RegexEngineCaps caps() const noexcept override;
    [[nodiscard]] RegexEngineStats stats() const noexcept override { return *stats_; }

    /// Every pattern this engine refused, mapped to why (same shape as
    /// StdRegexEngine::rejections, so slice-3 differential tooling can treat
    /// both engines uniformly).
    [[nodiscard]] const std::unordered_map<std::string, std::string>& rejections()
        const noexcept {
        return reasons_;
    }

    /// Distinct patterns held in the compile cache (successes and failures).
    [[nodiscard]] size_t cacheSize() const noexcept { return cache_.size(); }
    void clearCache() noexcept;

private:
    std::unordered_map<std::string, std::shared_ptr<IRegex>> cache_;  ///< nullptr => known bad
    std::unordered_map<std::string, std::string> reasons_;
    std::string lastError_;
    std::shared_ptr<RegexEngineStats> stats_;
};

}  // namespace ide::syntax

#endif  // COPE_HAS_PCRE2
