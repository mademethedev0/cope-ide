#pragma once

// Why this file exists
// -------------------
// A theme rule's "scope" field is a TextMate *scope selector*, not a string
// comparison. Getting the matching and — much more importantly — the *ranking*
// wrong is the classic way a syntax-highlighting engine ends up subtly wrong on
// every file, so the selector language lives in its own translation unit with
// its own unit tests, independent of theme loading.
//
// THE RANKING RULE (read this before filing a highlighting bug)
// -----------------------------------------------------------
// A successful match produces a 64-bit `MatchKey`. Keys are compared as plain
// unsigned integers; 0 means "did not match". Fields, most significant first:
//
//   bits 63..48  pathLength    number of space-separated scope patterns in the
//                              selector alternative (all of which matched).
//                              "source.js comment" (2) beats "comment" (1).
//   bits 47..32  matchDepth    1-based index, counting from the OUTERMOST scope,
//                              of the stack element matched by the innermost
//                              pattern. Deeper beats shallower, so on
//                              ["source.js", "comment.line"] the rule "comment"
//                              (depth 2) beats "source.js" (depth 1).
//   bits 31..16  segmentCount  total dot-separated segments across all patterns
//                              of the alternative.
//                              "comment.line.double-slash.js" (4) beats
//                              "comment" (1) at equal path length and depth.
//   bits 15..0   zero          reserved.
//
// Each field saturates at 0xFFFF, which no real selector comes close to.
//
// The FOURTH and last tiebreak is *not* in the key: when two rules produce
// exactly equal keys, the rule appearing LATER in the theme's tokenColors array
// wins. Theme::resolve implements that by scanning rules in file order and
// accepting a new winner on `>=`. This mirrors CSS and matches VS Code, where a
// theme's later entries are understood as overrides.
//
// A comma-separated selector is a list of independent alternatives; the
// alternative with the highest key represents the whole selector.
//
// Supported selector syntax (a deliberate subset of TextMate's):
//   * descendant paths      "source.js meta.function entity.name" — the patterns
//     must appear in the scope stack in that order, innermost last, and need not
//     be adjacent.
//   * per-segment prefixing "string.quoted" matches "string.quoted.double.js";
//     "string.quoted.double" does NOT match "string.quoted"; and a partial
//     segment never matches — "stri" does not match "string".
//   * "*" as a whole segment wildcard: "meta.*.js" matches "meta.function.js".
//   * alternation with ',' and with TextMate's '|'.
//   * exclusion with '-': "source.js -comment" fails when "comment" matches
//     ANYWHERE in the stack. '-' is an operator only at the start of a token, so
//     "comment.line.double-slash.js" keeps its hyphen, while both "-comment" and
//     "variable - meta.import" (both shapes occur in the shipped themes) parse as
//     exclusions. The operand is one pattern or one parenthesised group.
//   * parenthesised groups, expanded at parse time into the cartesian product of
//     alternatives: "(string, comment) punctuation" becomes
//     "string punctuation" and "comment punctuation".
//
// Not supported, and deliberately: TextMate's L:/R:/B: side prefixes (':' is
// treated as an ordinary scope-name character because the shipped Astro theme
// contains "...client:idle.html") and nested exclusions inside an exclusion
// group (dropped). Anything unparsable yields zero alternatives, i.e. a selector
// that matches nothing, never a crash.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ide::theme {

/// True when `pattern` matches `scopeName` under the per-dot-segment prefix rule
/// documented above. An empty pattern matches everything (the selector parser
/// never produces one); an empty scope name is matched only by an empty pattern.
[[nodiscard]] bool scopePatternMatches(std::string_view pattern, std::string_view scopeName) noexcept;

/// Dot-separated segment count: 0 for "", 1 for "source", 3 for "a.b.c".
[[nodiscard]] uint32_t scopeSegmentCount(std::string_view scope) noexcept;

/// Packed specificity, see the header comment. 0 is "no match".
using MatchKey = uint64_t;
inline constexpr MatchKey kNoMatch = 0;

/// The decoded form of a MatchKey. Exists so tests and diagnostics can talk
/// about specificity in words instead of hex.
struct MatchScore {
    uint32_t pathLength = 0;
    uint32_t matchDepth = 0;
    uint32_t segmentCount = 0;

    [[nodiscard]] bool matched() const noexcept { return pathLength != 0; }
    friend bool operator==(const MatchScore& lhs, const MatchScore& rhs) = default;
};

[[nodiscard]] MatchKey encodeMatchScore(const MatchScore& score) noexcept;
[[nodiscard]] MatchScore decodeMatchKey(MatchKey key) noexcept;

/// One parsed theme scope selector, ready to be matched thousands of times.
///
/// Parsing normalises everything up front (groups expanded, patterns split,
/// segment counts precomputed) so that match() performs no allocation at all —
/// it is called once per rule per distinct scope stack.
class ScopeSelector {
public:
    /// An empty selector: matches nothing.
    ScopeSelector() = default;

    /// Never fails. Garbage in yields a selector with zero alternatives.
    [[nodiscard]] static ScopeSelector parse(std::string_view text);

    /// Highest-scoring alternative's key, or kNoMatch. `scopeStack` is
    /// OUTERMOST-FIRST, matching ide::syntax::ScopeStackTable::resolve().
    [[nodiscard]] MatchKey match(std::span<const std::string_view> scopeStack) const noexcept;
    [[nodiscard]] bool matches(std::span<const std::string_view> scopeStack) const noexcept;

    [[nodiscard]] size_t alternativeCount() const noexcept { return alternatives_.size(); }
    [[nodiscard]] bool empty() const noexcept { return alternatives_.empty(); }
    /// The selector text this was parsed from, for error messages.
    [[nodiscard]] std::string_view sourceText() const noexcept { return source_; }

    /// Patterns of one alternative, outermost first. Diagnostics and tests.
    [[nodiscard]] std::span<const std::string> pathAt(size_t alternative) const noexcept;
    /// Number of exclusion clauses on one alternative. Diagnostics and tests.
    [[nodiscard]] size_t exclusionCountAt(size_t alternative) const noexcept;

private:
    /// One comma-separated branch: a descendant path plus its exclusions.
    struct Alternative {
        std::vector<std::string> path;                     ///< outermost .. innermost
        std::vector<std::vector<std::string>> exclusions;  ///< any match rejects
        uint32_t segmentCount = 0;                         ///< sum over `path`
    };

    std::string source_;
    std::vector<Alternative> alternatives_;
};

/// Safety valves for pathological selector text. Real themes use at most a
/// handful of alternatives and no nesting at all.
inline constexpr size_t kMaxSelectorAlternatives = 64;
inline constexpr int kMaxSelectorGroupDepth = 8;

}  // namespace ide::theme
