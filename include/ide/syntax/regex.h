#pragma once

// Why this file exists
// -------------------
// TextMate grammars are written against Oniguruma. Oniguruma is a large C
// dependency that has repeatedly sunk this project. Everything above this
// header therefore talks to *an* interface, never to a concrete engine, so the
// tokenizer can be brought up and tested with nothing but <regex> from the
// standard library and later upgraded (PCRE2, Oniguruma, a hand written VM)
// without touching a single line of grammar or tokenizer code.
//
// Two hard contracts make that swap safe:
//   1. compile() NEVER throws and NEVER aborts. An unsupported pattern is a
//      normal, expected outcome: it returns nullptr plus a human readable
//      reason. Callers degrade (the rule simply never matches) instead of
//      failing the file.
//   2. search() NEVER throws. std::regex can throw error_complexity /
//      error_stack at *match* time, so implementations must catch internally
//      and report "no match".
// Both are enforced structurally with noexcept.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ide::syntax {

/// Sentinel used for "this capture group did not participate in the match".
/// Distinct from an empty capture, which has begin == end != kNoPosition.
inline constexpr size_t kNoPosition = static_cast<size_t>(-1);

/// One capture group of one match. Byte offsets, absolute into the searched
/// text (not relative to the search start), so callers never rebase.
struct Capture {
    int index = 0;              ///< group number; 0 is the whole match
    size_t begin = kNoPosition; ///< byte offset, kNoPosition if group absent
    size_t end = kNoPosition;   ///< byte offset, kNoPosition if group absent

    /// True when the group participated in the match. An empty-but-present
    /// group returns true (begin == end, both valid).
    [[nodiscard]] bool matched() const noexcept { return begin != kNoPosition; }
    [[nodiscard]] size_t length() const noexcept { return matched() ? end - begin : 0u; }
};

/// Result of a successful search. `captures` is dense: engines guarantee
/// captures.size() == groupCount() + 1 and captures[i].index == i, with absent
/// groups carrying kNoPosition. Consumers should still use capture() so a
/// future sparse engine cannot silently break them.
struct MatchResult {
    size_t begin = 0;
    size_t end = 0;
    std::vector<Capture> captures;

    [[nodiscard]] size_t length() const noexcept { return end - begin; }

    /// Returns nullptr when the group does not exist at all.
    [[nodiscard]] const Capture* capture(int index) const noexcept {
        const size_t i = static_cast<size_t>(index);
        if (index >= 0 && i < captures.size() && captures[i].index == index) {
            return &captures[i];
        }
        for (const Capture& c : captures) {
            if (c.index == index) return &c;
        }
        return nullptr;
    }
};

/// A compiled pattern. Immutable and safe to share; search() must be
/// re-entrant with respect to distinct texts because the tokenizer caches
/// compiled patterns globally.
class IRegex {
public:
    virtual ~IRegex() = default;

    /// Finds the leftmost match starting at or after `startPos`.
    /// `startPos` is a byte offset; startPos == text.size() is legal and can
    /// still match a zero width pattern. Returned offsets are absolute.
    /// Must not throw: return std::nullopt for "no match" and for any internal
    /// engine failure.
    virtual std::optional<MatchResult> search(std::string_view text, size_t startPos) noexcept = 0;

    /// Number of capture groups, excluding group 0.
    [[nodiscard]] virtual int groupCount() const noexcept = 0;

    /// The original (untranslated) pattern source, for diagnostics.
    [[nodiscard]] virtual std::string_view pattern() const noexcept = 0;
};

/// What an engine can actually do. Purely informational: the tokenizer must
/// work with every field false. Used by tooling to grade a new engine against
/// the Oniguruma feature list documented below.
struct RegexEngineCaps {
    bool lookbehind = false;         ///< (?<=...) (?<!...) of arbitrary width
    bool anchorG = false;            ///< \G at any position, exact semantics
    bool anchorAzZ = false;          ///< \A \z \Z distinct from ^ $
    bool unicodeProperties = false;  ///< \p{...} \P{...}
    bool posixClasses = false;       ///< [[:alpha:]] including [[:^alpha:]]
    bool namedGroups = false;        ///< (?<name>...) and \k<name>
    bool possessive = false;         ///< a++ a*+ a?+
    bool atomicGroups = false;       ///< (?>...)
    bool conditionals = false;       ///< (?(1)yes|no)
    bool subroutines = false;        ///< \g<name> (?R) (?&name)
    bool keepOut = false;            ///< \K
    bool graphemeCluster = false;    ///< \X
    bool utf8Aware = false;          ///< classes/quantifiers over codepoints, not bytes
};

/// Aggregate counters so grammar coverage can be *measured* instead of guessed.
struct RegexEngineStats {
    size_t compileCalls = 0;   ///< compile() invocations
    size_t cacheHits = 0;      ///< served from the pattern cache
    size_t compiled = 0;       ///< distinct patterns successfully compiled
    size_t rejected = 0;       ///< distinct patterns rejected (nullptr returned)
    size_t lossy = 0;          ///< compiled, but with documented semantic loss
    size_t searchCalls = 0;
    size_t searchErrors = 0;   ///< engine failures swallowed inside search()
};

/// Factory + cache for compiled patterns. One instance per tokenizer is the
/// intended usage; implementations are not required to be thread safe.
class IRegexEngine {
public:
    virtual ~IRegexEngine() = default;

    /// Compiles `pattern` (Oniguruma source, as it appears in the grammar).
    /// Returns nullptr when the pattern cannot be supported; the reason is
    /// then available from lastError() and errorFor(pattern).
    /// Compiling the same pattern twice must not recompile.
    virtual std::shared_ptr<IRegex> compile(std::string_view pattern) noexcept = 0;

    /// Stable identifier, e.g. "std::regex", "pcre2", "oniguruma".
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Reason for the most recent failed compile(). Empty if the last compile
    /// succeeded.
    [[nodiscard]] virtual std::string lastError() const { return {}; }

    /// Reason recorded for a specific pattern, empty if that pattern is fine
    /// or was never seen.
    [[nodiscard]] virtual std::string errorFor(std::string_view /*pattern*/) const { return {}; }

    [[nodiscard]] virtual RegexEngineCaps caps() const noexcept { return {}; }
    [[nodiscard]] virtual RegexEngineStats stats() const noexcept { return {}; }
};

// ---------------------------------------------------------------------------
// ONIGURUMA FEATURE SPEC  (this block is the grading sheet for future engines)
// ---------------------------------------------------------------------------
// Below is every Oniguruma construct that appears in the 244 grammars shipped
// in textmate/grammars, what std::regex (ECMAScript) can do about it, and what
// a "good" engine is expected to do. Counts are pattern occurrences measured
// over the 31,241 match/begin/end/while patterns in that corpus.
//
// A. NOT EXPRESSIBLE in std::regex/ECMAScript -- must be handled natively by a
//    real engine. StdRegexEngine rejects (returns nullptr) or emulates as noted.
//
//   \G  (2523)  "match must start where the search started".
//        ECMAScript has no such anchor. Emulated in StdRegexEngine by
//        compiling two variants -- \G := always-true and \G := never-true --
//        and matching the first with std::regex_constants::match_continuous at
//        startPos, then scanning the second from startPos+1. That is exact when
//        \G sits at a position that is reachable only at the match start
//        (offset 0, or behind only group-open/alternation/^ tokens), which is
//        96% of real occurrences; for \G buried after consuming atoms the
//        assertion is evaluated one position too early (documented loss).
//        Note the *tokenizer* additionally rewrites \G to a never-matching
//        assertion when the scan position is not the anchor position, which is
//        the other half of Oniguruma's semantics.
//
//   (?<=..) (?<!..)  (5484 patterns, 155 of 244 grammars)
//        No lookbehind of any kind in ECMAScript. StdRegexEngine emulates the
//        single most common shape -- a *leading*, single-character-wide
//        lookbehind, e.g. (?<!\w), (?<=\)), (?<![$_[:alnum:]]) -- by stripping
//        it, matching the remainder, and testing the byte before the match
//        start against the class, retrying at the next position on failure.
//        That is exact for ASCII class bodies and recovers ~29% of lookbehind
//        patterns. Everything else (multi-char, alternated, nested, non-leading
//        lookbehind) is REJECTED. This is the single biggest reason to ship a
//        real engine: it is 12.9% of all patterns.
//
//   \g<name> \g<0> (?R) (?&name)  (88)   Subroutine calls / recursion. REJECTED.
//   \K  (1)                              Reset match start. REJECTED.
//   \X  (1)                              Grapheme cluster. REJECTED.
//   (?(cond)a|b) (0 in corpus)           Conditional groups. REJECTED.
//   [a-z&&[^aeiou]]  (29 in classes)     Class set intersection. REJECTED.
//   [[:^alpha:]]  (1)                    Negated POSIX class. REJECTED.
//   \P{...} inside a class  (13)         Cannot negate inside a class. REJECTED.
//
// B. TRANSLATED EXACTLY by StdRegexEngine (a native engine should do nothing).
//
//   \h \H                -> [0-9a-fA-F] / [^0-9a-fA-F]   (904)
//   \e \a                -> \x1B / \x07                  (162)
//   \N \O                -> [^\n] / [\s\S]               (22)
//   \R                   -> (?:\r\n|[\r\n])
//   \Q...\E              -> literal, every metacharacter escaped (4)
//   \x{HHHH}             -> the UTF-8 bytes of the codepoint (133)
//   (?<name>x) (?'name'x)-> plain group + name->index map (126)
//   \k<name> \k'name'    -> numeric backreference (31)
//   [[:alpha:]] & co     -> explicit ASCII ranges (implementation-defined in
//                           std::regex, so we expand them ourselves)
//   (?>x)                -> (?:x)  (187) -- same language, different
//                           backtracking; only reachable difference is a
//                           pathological match, and search() is exception-safe.
//   a++ a*+ a?+ a{n,m}+  -> greedy (1134 candidates, possessiveness is a
//                           performance hint in these grammars)
//   (?#comment)          -> stripped
//   lone ] } {           -> escaped, because a bare ] outside a class and a
//                           bare { that is not a quantifier are only legal in
//                           ECMAScript via Annex B, which std::regex omits.
//                           Real grammars contain both (e.g. ini's "(])").
//
// C. TRANSLATED WITH DOCUMENTED SEMANTIC LOSS.
//
//   \A                   -> ^   and \z -> $, \Z -> (?=\n?$).
//        We never set std::regex_constants::multiline and we set
//        match_prev_avail whenever startPos > 0, so ^ asserts exactly
//        "offset 0 of the text handed to search()", i.e. line start. That is
//        what a per-line tokenizer wants. Whether the line is the *first* line
//        of the document is decided by the tokenizer, which rewrites \A to a
//        never-matching assertion on every line but the first -- the same
//        trick vscode-textmate uses.
//   $                    -> (?=\n?$)
//        Oniguruma (Ruby syntax) treats $ as end-of-line and vscode-textmate
//        feeds lines with their trailing newline attached. Non-multiline
//        ECMAScript $ means end-of-text, so we widen it to "at the end, or
//        just before a final newline". Lines containing interior newlines are
//        not modelled (a tokenizer works one line at a time).
//   ^                    -> ^ (offset 0 only; interior newlines not modelled).
//   \p{...} \P{...}      -> ASCII approximations (\p{L} -> [A-Za-z] etc, 732
//                           patterns). Non-ASCII letters stop matching.
//   (?i) (?i:..) (?-i)   -> icase applied to the WHOLE pattern; scoped or
//                           toggled-off case sensitivity over-matches (2971).
//   (?m) (?s) (?-m)      -> ignored; irrelevant when matching a single line.
//   non-ASCII literals   -> passed through as raw UTF-8 bytes. Correct outside
//                           character classes, WRONG inside them: [äö] becomes
//                           a set of bytes (473 patterns touch this).
//   Quantifiers/classes  -> byte oriented, not codepoint oriented. `.` matches
//                           one byte. A real engine must set utf8Aware.
//
// A conforming replacement engine sets every RegexEngineCaps flag it supports,
// keeps offsets absolute and byte-based, keeps compile()/search() noexcept, and
// must accept the never-matching assertion "(?=[^\s\S])" that the tokenizer
// substitutes for disabled \A/\G anchors -- that construct is valid in
// ECMAScript, PCRE2 and Oniguruma alike.
// ---------------------------------------------------------------------------

/// The zero-width assertion the tokenizer substitutes for a disabled anchor.
/// Kept here (not in tokenizer.cpp) because every engine must accept it.
inline constexpr std::string_view kNeverMatchAssertion = "(?=[^\\s\\S])";

/// Escapes `text` so that it matches literally in every supported dialect
/// (ECMAScript, PCRE2, Oniguruma). Used for \Q...\E and, more importantly, for
/// substituting a begin match's captures into an end pattern, where the captured
/// text must be treated as literal characters and not as regex syntax.
[[nodiscard]] inline std::string escapeRegexLiteral(std::string_view text) {
    static constexpr std::string_view kSpecials = "\\^$.|?*+()[]{}/-#&~,";
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() + 8u);
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20u || u == 0x7Fu) {
            out += "\\x";
            out.push_back(kHex[(u >> 4) & 0xFu]);
            out.push_back(kHex[u & 0xFu]);
        } else if (u < 0x80u && kSpecials.find(c) != std::string_view::npos) {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace ide::syntax
