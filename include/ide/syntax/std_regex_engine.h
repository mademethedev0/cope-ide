#pragma once

// Why this file exists
// -------------------
// The bring-up regex engine: pure standard library, zero dependencies, so the
// grammar loader and tokenizer can be developed, tested and shipped before any
// third-party regex library exists in the build. It translates the Oniguruma
// dialect used by .tmLanguage files into ECMAScript as far as that is possible,
// and cleanly *refuses* the rest (see the feature spec in regex.h) instead of
// throwing, aborting, or silently producing wrong matches.
//
// Everything here is also the reference for how a future PCRE2/Oniguruma engine
// must behave: same interface, same absolute byte offsets, same noexcept
// contract, same "unsupported is normal" degradation.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ide/syntax/regex.h>

namespace ide::syntax {

/// Which flavour of leading lookbehind a pattern carries, if any.
/// Only a *single character wide, leading* lookbehind can be emulated on top of
/// std::regex; see PatternTranslation::lookbehindTest.
enum class LookbehindKind : uint8_t {
    None,
    Positive,  ///< (?<=X) - the byte before the match must match lookbehindTest
    Negative,  ///< (?<!X) - the byte before the match must not match it
};

/// The full outcome of translating one Oniguruma pattern to ECMAScript.
/// Exposed (rather than hidden in the .cpp) because pattern translation is the
/// part most likely to be wrong, and it must be unit-testable in isolation.
struct PatternTranslation {
    bool ok = false;         ///< false => pattern must be rejected
    std::string reason;      ///< human readable, non-empty iff !ok

    /// ECMAScript source with \G rewritten to an always-true assertion. This is
    /// the variant used for a match that starts exactly at the search position.
    std::string ecma;
    /// Same source with \G rewritten to a never-true assertion, used when
    /// scanning forward past the search position.
    std::string ecmaNoG;

    bool icase = false;                ///< pattern asked for (?i) somewhere
    bool hasG = false;                 ///< \G occurs, so two variants are needed
    bool leadingG = false;             ///< \G is the very first token
    bool topLevelAlternation = false;  ///< an unparenthesised | exists
    int groupCount = 0;                ///< capturing groups, excluding group 0

    LookbehindKind lookbehind = LookbehindKind::None;
    std::string lookbehindTest;  ///< ECMAScript source matching exactly one byte

    /// (?<name>...) mappings, in declaration order. Kept so \k<name> can be
    /// lowered to a numeric backreference and so tooling can report them.
    std::vector<std::pair<std::string, int>> groupNames;

    /// Documented semantic losses, e.g. "possessive quantifier -> greedy".
    std::vector<std::string> notes;

    [[nodiscard]] bool lossy() const noexcept { return !notes.empty(); }
};

/// Translates one Oniguruma pattern. Never throws. Total: every input either
/// produces ok == true with a compilable ECMAScript source, or ok == false with
/// a reason. (It does not *guarantee* std::regex accepts the output - the
/// engine still compiles inside a try/catch and reports failures as reasons.)
[[nodiscard]] PatternTranslation translateOnigToEcma(std::string_view pattern);

/// std::regex based IRegexEngine. Caches by pattern string, including negative
/// results, so a rejected pattern costs one translation attempt for the whole
/// process lifetime.
class StdRegexEngine final : public IRegexEngine {
public:
    StdRegexEngine();
    ~StdRegexEngine() override;

    StdRegexEngine(const StdRegexEngine&) = delete;
    StdRegexEngine& operator=(const StdRegexEngine&) = delete;

    std::shared_ptr<IRegex> compile(std::string_view pattern) noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override { return "std::regex"; }
    [[nodiscard]] std::string lastError() const override { return lastError_; }
    [[nodiscard]] std::string errorFor(std::string_view pattern) const override;
    [[nodiscard]] RegexEngineCaps caps() const noexcept override;
    [[nodiscard]] RegexEngineStats stats() const noexcept override { return *stats_; }

    /// Every pattern this engine refused, mapped to why. This is how grammar
    /// coverage gets measured instead of guessed.
    [[nodiscard]] const std::unordered_map<std::string, std::string>& rejections() const noexcept {
        return reasons_;
    }
    /// Patterns that compiled but lost semantics, mapped to the first note.
    [[nodiscard]] const std::unordered_map<std::string, std::string>& lossyPatterns() const noexcept {
        return lossy_;
    }

    /// Number of distinct patterns held in the compile cache (successes and
    /// failures both). Tests use it to prove caching works.
    [[nodiscard]] size_t cacheSize() const noexcept { return cache_.size(); }
    void clearCache() noexcept;

private:
    std::unordered_map<std::string, std::shared_ptr<IRegex>> cache_;  ///< nullptr => known bad
    std::unordered_map<std::string, std::string> reasons_;
    std::unordered_map<std::string, std::string> lossy_;
    std::string lastError_;
    std::shared_ptr<RegexEngineStats> stats_;
};

}  // namespace ide::syntax
