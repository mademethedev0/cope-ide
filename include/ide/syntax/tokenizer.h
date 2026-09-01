#pragma once

// Why this file exists
// -------------------
// The incremental line tokenizer: the heart of syntax highlighting. It consumes
// one line at a time plus the state the previous line ended in, and produces
// tokens plus a new state. Everything about it is shaped by two requirements:
//
//   * Incremental retokenization. Editing line 500 must not retokenize line
//     5,000. The caller keeps one State per line and stops as soon as a
//     recomputed end state equals the stored one, so State must be cheap to
//     copy, cheap to hash and *correctly* comparable.
//   * Survival. Real files contain 200KB minified lines, grammars contain
//     zero-width matches, cyclic includes and end patterns that std::regex
//     cannot compile. Every one of those has a named limit and a defined
//     degradation, never a hang and never a crash.
//
// Not implemented in this phase (deliberate, documented scope): grammar
// injections. Grammar::injections() is parsed and available, but the tokenizer
// does not apply injected patterns yet.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ide/syntax/grammar.h>
#include <ide/syntax/regex.h>
#include <ide/syntax/scope_stack.h>
#include <ide/syntax/trace.h>

namespace ide::syntax {

// --- hard safety limits -----------------------------------------------------
// Every one of these exists because a real file or a real grammar broke a naive
// implementation. They are constants, not options, so behaviour is reproducible.

/// Lines longer than this are not tokenized at all: they get one plain token
/// carrying the incoming scopes, and the state passes through unchanged.
inline constexpr size_t kMaxLineLength = 10000;

/// Maximum number of nested begin/end (or begin/while) rules. Beyond it, a
/// matching begin rule is treated as a plain match rule: its scopes still apply
/// to the matched text, but it is not entered.
inline constexpr size_t kMaxStackDepth = 100;

/// Maximum scan iterations for one line. Each iteration either advances the
/// position or is a zero-width match, so this is the absolute termination
/// guarantee even if every other guard is defeated.
inline constexpr size_t kMaxIterationsPerLine = 2 * kMaxLineLength + 1024;

/// How many consecutive zero-width matches are tolerated at one position before
/// the tokenizer force-advances by one UTF-8 character. Legitimate grammars use
/// zero-width begin patterns (e.g. `begin: "(^[\t ]+)?(?=#)"`) and then match
/// child rules at the same position, so this cannot be 1 - but it must be
/// finite, which is what guarantees progress.
inline constexpr size_t kMaxZeroWidthMatchesPerPosition = 16;

/// Depth limit for re-tokenizing captures that carry their own "patterns".
inline constexpr int kMaxCaptureRecursionDepth = 8;

/// Upper bound on the per-line (pattern, position) match cache. Past it, results
/// are computed without being cached rather than growing without bound.
inline constexpr size_t kMaxMatchCacheEntries = 1u << 16;

/// Upper bound on how many candidate patterns one rule list can expand to, and
/// how deep include expansion may recurse.
inline constexpr size_t kMaxPatternListSize = 4096;
inline constexpr int kMaxIncludeExpansionDepth = 64;

/// One token: byte offsets into the line plus an interned scope stack id.
/// Exactly three integers wide on purpose - see scope_stack.h.
struct TokenSpan {
    size_t begin = 0;
    size_t end = 0;
    ScopeStackId scopes = kRootScopeStack;

    friend bool operator==(const TokenSpan& a, const TokenSpan& b) noexcept {
        return a.begin == b.begin && a.end == b.end && a.scopes == b.scopes;
    }
    friend bool operator!=(const TokenSpan& a, const TokenSpan& b) noexcept { return !(a == b); }
};

/// The tokenizer state at a line boundary: an immutable, persistent stack of
/// active rules.
///
/// Copying a State copies one shared_ptr and one bool. Pushing shares the whole
/// tail with the previous state (structural sharing), so storing one State per
/// line costs O(edits), not O(lines * depth). Each node caches a hash of the
/// entire chain below it, which makes inequality almost always a single compare;
/// operator== still walks and compares every frame, because "the hashes match"
/// is not the same as "the states are equal" and incremental retokenization must
/// not stop early on a collision.
class State {
public:
    /// One active rule.
    struct Frame {
        RuleRef rule{};
        /// Scopes including the rule's own `name`.
        ScopeStackId nameScopes = kRootScopeStack;
        /// Scopes including `name` and `contentName`; what tokens inside the
        /// rule's body get.
        ScopeStackId contentScopes = kRootScopeStack;
        /// The rule's end pattern *after* \1..\9 substitution from the begin
        /// match. Absent means "use the rule's own end source verbatim", which
        /// is distinct from present-but-empty.
        std::optional<std::string> endPattern;
        /// Same, for a begin/while rule's while pattern.
        std::optional<std::string> whilePattern;
        /// True when the begin match ended exactly at the end of its line. The
        /// next line then starts with its \G anchor at offset 0.
        bool beginCapturedEol = false;

        friend bool operator==(const Frame& a, const Frame& b) {
            return a.rule == b.rule && a.nameScopes == b.nameScopes &&
                   a.contentScopes == b.contentScopes && a.beginCapturedEol == b.beginCapturedEol &&
                   a.endPattern == b.endPattern && a.whilePattern == b.whilePattern;
        }
        friend bool operator!=(const Frame& a, const Frame& b) { return !(a == b); }
    };

    State() = default;

    /// An empty stack. `isFirstLine` tells the tokenizer whether \A may match;
    /// it is true for the first line of a document and false afterwards, and it
    /// participates in equality because it changes tokenization.
    [[nodiscard]] static State initial(bool isFirstLine = true);

    [[nodiscard]] bool empty() const noexcept { return top_ == nullptr; }
    [[nodiscard]] size_t depth() const noexcept { return top_ == nullptr ? 0u : top_->depth; }
    [[nodiscard]] uint64_t hash() const noexcept;
    [[nodiscard]] bool isFirstLine() const noexcept { return isFirstLine_; }

    /// Scopes of the innermost frame (name applied but not contentName).
    [[nodiscard]] ScopeStackId scopes() const noexcept;
    /// Scopes tokens get inside the innermost frame.
    [[nodiscard]] ScopeStackId contentScopes() const noexcept;
    /// The innermost rule, or an invalid ref for an empty stack.
    [[nodiscard]] RuleRef rule() const noexcept;

    [[nodiscard]] const Frame* top() const noexcept;
    /// Frame by index, 0 == outermost. nullptr when out of range.
    [[nodiscard]] const Frame* frameAt(size_t index) const noexcept;

    [[nodiscard]] State push(const Frame& frame) const;
    [[nodiscard]] State pop() const;
    /// The state with exactly `depth` frames, sharing nodes with this one.
    [[nodiscard]] State ancestor(size_t depth) const;
    /// Replaces the innermost frame (used to attach contentName scopes and the
    /// resolved end pattern after the begin captures have been handled).
    [[nodiscard]] State withTop(const Frame& frame) const;
    [[nodiscard]] State withContentScopes(ScopeStackId scopes) const;
    [[nodiscard]] State withFirstLine(bool isFirstLine) const;

    bool operator==(const State& other) const;
    bool operator!=(const State& other) const { return !(*this == other); }

private:
    struct Node {
        std::shared_ptr<const Node> parent;
        Frame frame;
        uint32_t depth = 0;
        uint64_t hash = 0;
    };

    static uint64_t hashFrame(const Frame& frame, uint64_t parentHash);

    std::shared_ptr<const Node> top_;
    bool isFirstLine_ = false;
};

/// Result of tokenizing one line. `tokens` is contiguous and covers
/// [0, line.size()) with no gaps and no overlaps; an empty line yields no
/// tokens. The flags report which safety limit (if any) changed the outcome.
struct TokenizeResult {
    std::vector<TokenSpan> tokens;
    State endState;
    bool bailedOnLineLength = false;
    bool hitIterationLimit = false;
    bool hitDepthLimit = false;
    /// Number of times a zero-width match streak forced a manual advance.
    size_t forcedAdvances = 0;
};

/// Substitutes \1..\9 (in fact \0..\99) in an end/while pattern with the
/// literal, regex-escaped text of the corresponding group of the begin match.
/// Absent groups substitute to nothing. "\\1" (an escaped backslash followed by
/// a literal 1) is *not* a backreference - getting that wrong is the classic
/// TextMate implementation bug.
[[nodiscard]] std::string substituteBackreferences(std::string_view pattern, std::string_view line,
                                                  const MatchResult& match);

/// Substitutes $1 / ${1} / ${1:/downcase} / ${1:/upcase} in a scope name with
/// the captured text. Unknown or absent groups substitute to nothing.
[[nodiscard]] std::string substituteScopeCaptures(std::string_view templateText,
                                                  std::string_view line, const MatchResult& match);

/// Rewrites the \A and \G anchors of an Oniguruma pattern so they can never
/// match, which is how "this is not the first line" and "this is not the anchor
/// position" are expressed to an engine that has no such search flags. The
/// substitution is a plain zero-width assertion, valid in every dialect.
/// `sawA`/`sawG` report whether the anchors occur at all (character classes are
/// skipped, where \A and \G are literal letters).
[[nodiscard]] std::string rewriteAnchors(std::string_view pattern, bool allowA, bool allowG,
                                         bool* sawA = nullptr, bool* sawG = nullptr);

/// The line tokenizer. One instance per (grammar, regex engine) pair; it owns
/// the pattern caches and the scope interning table, so keeping it alive across
/// lines and files is the intended usage.
class Tokenizer {
public:
    /// `registry` and `engine` must outlive the tokenizer. `rootGrammar` is the
    /// grammar of the document being tokenized; it also decides what "$base"
    /// resolves to.
    Tokenizer(GrammarRegistry& registry, IRegexEngine& engine, GrammarId rootGrammar);

    /// State for line 0: the root rule pushed with the grammar's scope name.
    [[nodiscard]] State initialState();

    /// Tokenizes one line. `line` is a raw byte view; callers that want
    /// Oniguruma-compatible `$`/`\n` behaviour should include the trailing
    /// newline in the view. Offsets in the result are byte offsets into `line`.
    TokenizeResult tokenizeLine(std::string_view line, const State& startState);

    [[nodiscard]] ScopeStackTable& scopeTable() noexcept { return scopeTable_; }
    [[nodiscard]] const ScopeStackTable& scopeTable() const noexcept { return scopeTable_; }

    /// Installs (or with an empty sink, removes) the trace hook.
    void setTrace(TraceSink sink);

    struct Stats {
        size_t linesTokenized = 0;
        size_t linesBailedOnLength = 0;
        size_t regexCompileFailures = 0;   ///< patterns the engine refused
        size_t rulesDisabledByEndPattern = 0;  ///< begin rules skipped: unusable end
        size_t matchCacheHits = 0;
        size_t matchCacheMisses = 0;
        size_t forcedAdvances = 0;
        size_t depthLimitHits = 0;
        size_t iterationLimitHits = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = Stats{}; }

private:
    // Compiled variants of one pattern source. Index bits: 1 = \A allowed,
    // 2 = \G allowed. Patterns without those anchors only ever use index 3.
    struct VariantSet {
        bool hasA = false;
        bool hasG = false;
        std::array<std::shared_ptr<IRegex>, 4> variants{};
        std::array<bool, 4> attempted{};
    };

    struct MatchCacheKey {
        const IRegex* regex = nullptr;
        size_t position = 0;
        size_t lineEnd = 0;
        friend bool operator==(const MatchCacheKey& a, const MatchCacheKey& b) noexcept {
            return a.regex == b.regex && a.position == b.position && a.lineEnd == b.lineEnd;
        }
    };
    struct MatchCacheKeyHash {
        size_t operator()(const MatchCacheKey& k) const noexcept {
            size_t h = std::hash<const void*>{}(static_cast<const void*>(k.regex));
            h ^= (k.position + 0x9E3779B97F4A7C15ull) * 0x100000001B3ull;
            h ^= (k.lineEnd + 0x165667B19E3779F9ull) * 0x27220A95u;
            return h;
        }
    };

    /// Token emitter. Guarantees contiguous, non-empty, ordered spans.
    struct LineTokens {
        std::vector<TokenSpan>* out = nullptr;
        size_t lastEnd = 0;
        void produce(ScopeStackId scopes, size_t endIndex) {
            if (out == nullptr || endIndex <= lastEnd) return;
            out->push_back(TokenSpan{lastEnd, endIndex, scopes});
            lastEnd = endIndex;
        }
    };

    struct MatchOutcome {
        bool found = false;
        bool isEnd = false;
        RuleRef rule{};
        int candidateIndex = -1;
        MatchResult match;
    };

    struct WhileOutcome {
        State stack;
        size_t linePos = 0;
        size_t anchorPos = kNoPosition;
        bool isFirstLine = false;
    };

    // --- grammar helpers ---------------------------------------------------
    [[nodiscard]] const Rule* ruleFor(RuleRef ref) const;
    [[nodiscard]] RuleRef rootRuleRef() const;
    const std::vector<RuleRef>& flattenPatterns(RuleRef container);
    void expandInto(RuleRef ref, std::vector<RuleRef>& out, std::vector<uint64_t>& visited,
                    int depth);

    // --- regex helpers -----------------------------------------------------
    std::shared_ptr<IRegex> regexForSource(const std::string& source, bool allowA, bool allowG);
    std::shared_ptr<IRegex> regexForRule(RuleRef ref, const Rule& rule, bool allowA, bool allowG);
    std::shared_ptr<IRegex> variantFrom(VariantSet& set, const std::string& source, bool allowA,
                                        bool allowG);
    /// Pointer is valid until the next call. nullptr means "no match".
    const MatchResult* searchCached(IRegex* regex, std::string_view line, size_t position);

    // --- tokenization ------------------------------------------------------
    State tokenizeString(std::string_view line, size_t startPos, State stack, bool isFirstLine,
                         bool checkWhile, LineTokens& tokens, TokenizeResult& result,
                         int captureDepth);
    WhileOutcome checkWhileConditions(std::string_view line, State stack, LineTokens& tokens,
                                      TokenizeResult& result, bool isFirstLine);
    MatchOutcome matchRule(std::string_view line, size_t position, size_t anchorPos,
                           const State& stack, bool isFirstLine);
    void handleCaptures(std::string_view line, const State& stack, ScopeStackId baseScopes,
                        GrammarId owner, const std::vector<CaptureSpec>& captures,
                        const MatchResult& match, LineTokens& tokens, TokenizeResult& result,
                        bool isFirstLine, int captureDepth);

    // --- tracing -----------------------------------------------------------
    void emitTrace(const TraceEvent& event) const;
    void traceRule(TraceEventKind kind, size_t position, RuleRef ref, bool matched, size_t begin,
                   size_t end, int candidateIndex, std::string_view pattern) const;
    void traceNotice(TraceEventKind kind, size_t position, std::string_view detail) const;

    GrammarRegistry& registry_;
    IRegexEngine& engine_;
    GrammarId rootGrammar_ = kInvalidGrammarId;
    ScopeStackTable scopeTable_;
    std::unordered_map<std::string, std::shared_ptr<VariantSet>> patternCache_;
    std::unordered_map<uint64_t, std::shared_ptr<VariantSet>> ruleRegexCache_;
    std::unordered_map<uint64_t, std::vector<RuleRef>> flattenCache_;
    std::unordered_map<MatchCacheKey, std::optional<MatchResult>, MatchCacheKeyHash> matchCache_;
    /// Holds the result when the cache is full, so searchCached() can keep
    /// returning a pointer. Valid only until the next searchCached() call.
    std::optional<MatchResult> scratchMatch_;
    /// Per-line scratch: for each stack frame index, the anchor position that
    /// was current before that frame was pushed. kNoPosition means "not pushed
    /// on this line", exactly like vscode-textmate resetting _anchorPos at each
    /// line start.
    std::array<size_t, kMaxStackDepth + 2> frameAnchorPos_{};
    TraceSink trace_;
    bool tracing_ = false;
    Stats stats_;
};

}  // namespace ide::syntax
