#pragma once

// Why this file exists
// -------------------
// This is the only highlighting API the UI will ever call, and the whole reason
// the module exists: it makes "the grammar refused 15% of its rules" invisible.
//
// THE CASCADE
// -----------
//   tier 1  TextMate grammar through ide::syntax::Tokenizer. Correct, and what
//           we want everywhere it works.
//   tier 2  the regex-free FallbackLexer. Coarser, but total.
//   tier 3  one plain span for the whole line. For lines and files past the
//           safety limits, where any tokenizer is a liability.
//
// Tier 2 is NOT merely an alternative to tier 1 - it is a REPAIR LAYER over it.
// After the grammar tokenizes a line, every maximal stretch of bytes the grammar
// left at the bare root scope is re-lexed by the fallback and its scopes are
// spliced in *underneath* the grammar's. The grammar therefore always wins where
// it produced something, and the holes it left - the words that used to render
// grey and made five previous attempts look broken - get a plausible colour.
//
// INVARIANTS (asserted by fuzz tests, relied on by the renderer)
//   * Output spans tile the line exactly: sorted, non-overlapping, gapless,
//     covering [0, line.size()). An empty line yields no spans.
//   * Adjacent spans with the same StyleId are merged. Phase 4 ships spans over
//     JNI as [begin, end, styleId] triples, so every merged span is bytes saved
//     on every frame.
//   * Spans are already theme-resolved: the UI never sees a scope string.
//
// Byte offsets are `size_t` into the line. Line indices are 0-based `int64_t`
// per docs/CONVENTIONS.md, but this header only ever sees a line at a time plus
// batches, so it needs no line numbers at all.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ide/highlight/fallback_lexer.h>
#include <ide/syntax/grammar.h>
#include <ide/syntax/regex.h>
#include <ide/syntax/scope_stack.h>
#include <ide/syntax/tokenizer.h>
#include <ide/theme/style.h>
#include <ide/theme/theme.h>

namespace ide::highlight {

/// Which machinery produced a line's spans. Reported, not guessed: the quality
/// report gates on it and the UI shows it in the status bar.
enum class Tier : uint8_t {
    kGrammar = 1,   ///< TextMate grammar (+ fallback repair of its holes)
    kFallback = 2,  ///< heuristic lexer only: no grammar, or a useless one
    kPlain = 3,     ///< one span per line: file or line past the safety limits
};

[[nodiscard]] std::string_view tierName(Tier tier) noexcept;

/// One painted range of a line. The only thing that crosses into the renderer.
struct StyledSpan {
    size_t begin = 0;
    size_t end = 0;
    ide::theme::StyleId style = ide::theme::kDefaultStyleId;

    friend bool operator==(const StyledSpan& a, const StyledSpan& b) noexcept {
        return a.begin == b.begin && a.end == b.end && a.style == b.style;
    }
    friend bool operator!=(const StyledSpan& a, const StyledSpan& b) noexcept { return !(a == b); }
};

/// One classified range of a line, before the theme is applied.
///
/// Kept as a separate, public step so a theme change costs a re-resolve instead
/// of a retokenization: hold on to the ScopedSpans, call setTheme(), then
/// styleSpans() again.
struct ScopedSpan {
    size_t begin = 0;
    size_t end = 0;
    syntax::ScopeStackId scopes = syntax::kRootScopeStack;

    friend bool operator==(const ScopedSpan& a, const ScopedSpan& b) noexcept {
        return a.begin == b.begin && a.end == b.end && a.scopes == b.scopes;
    }
    friend bool operator!=(const ScopedSpan& a, const ScopedSpan& b) noexcept { return !(a == b); }
};

/// Everything one line needs to know about the previous one: the TextMate rule
/// stack plus the fallback lexer's carry state.
///
/// Both halves are cheap to copy and correctly comparable, which is the whole
/// point: incremental relayout stores one LineState per line and stops as soon
/// as a recomputed end state equals the stored one.
struct LineState {
    syntax::State tm;
    FallbackState fallback;

    friend bool operator==(const LineState& a, const LineState& b) {
        return a.fallback == b.fallback && a.tm == b.tm;
    }
    friend bool operator!=(const LineState& a, const LineState& b) { return !(a == b); }
};

/// Named, reproducible degradation limits. Constants would be enough for
/// production; they are options so tests can force each tier deterministically.
struct HighlightLimits {
    /// Lines longer than this get one plain span. Clamped to
    /// ide::syntax::kMaxLineLength, because past that the tokenizer bails anyway
    /// and repairing its single root token would mean lexing a 200KB "hole".
    size_t maxLineLength = syntax::kMaxLineLength;
    /// Files bigger than this (bytes / lines) are opened at tier 3. 0 disables
    /// the check.
    size_t maxFileBytes = 16u * 1024u * 1024u;
    size_t maxFileLines = 400000;
    /// How many lines probe() samples before deciding a grammar is useless.
    size_t probeLines = 64;
    /// Below this fraction of non-whitespace bytes scoped by the grammar alone,
    /// probe() demotes the file to tier 2. A grammar that scopes almost nothing
    /// is worse than no grammar: it costs regex time and produces grey text.
    double minGrammarCoverage = 0.10;
    /// Master switch for the repair layer. Off means "pure tier 1", which is
    /// what the tiling fuzz test uses to check the grammar path in isolation.
    bool repairWithFallback = true;
    /// Treat a stretch whose only scopes beyond the root are `meta.*` as a hole.
    /// meta scopes are structural markers that themes almost never colour, so
    /// leaving them unrepaired reproduces exactly the grey-word failure this
    /// module exists to prevent. Turning this off makes tier 1 absolute.
    bool repairMetaOnlyRuns = true;
};

/// What the highlighter is told about the file it is about to open. No IO here:
/// sizes come from the host, the name is only ever parsed as text.
struct FileInfo {
    std::string_view name;   ///< file name or path; extension parsed out of it
    size_t byteSize = 0;     ///< 0 == unknown
    size_t lineCount = 0;    ///< 0 == unknown
};

/// Flat result of a batch call, shaped for the JNI boundary: one span array plus
/// per-line offsets, instead of a vector of vectors.
///
/// Spans of line i are `spans[lineOffsets[i] .. lineOffsets[i + 1])`, so
/// lineOffsets always has lines.size() + 1 entries. endStates[i] is the state
/// *after* line i, which is what an incremental relayout must store.
struct BatchResult {
    std::vector<StyledSpan> spans;
    std::vector<size_t> lineOffsets;
    std::vector<LineState> endStates;

    [[nodiscard]] size_t lineCount() const noexcept {
        return lineOffsets.empty() ? 0u : lineOffsets.size() - 1u;
    }
    /// Spans of one line; empty for an out-of-range index.
    [[nodiscard]] std::span<const StyledSpan> lineSpans(size_t line) const noexcept;
};

/// The cascade. One instance per open document.
///
/// `registry`, `engine` and `theme` must outlive the highlighter. Not thread
/// safe: it memoises into the tokenizer's scope table, its own style cache and
/// the theme's resolve cache.
class Highlighter {
public:
    Highlighter(syntax::GrammarRegistry& registry, syntax::IRegexEngine& engine,
                const ide::theme::Theme& theme, const FileInfo& file, HighlightLimits limits = {});

    Highlighter(const Highlighter&) = delete;
    Highlighter& operator=(const Highlighter&) = delete;

    // --- what was selected, and why ----------------------------------------
    [[nodiscard]] Tier tier() const noexcept { return tier_; }
    [[nodiscard]] bool hasGrammar() const noexcept {
        return grammarId_ != syntax::kInvalidGrammarId;
    }
    /// Scope name of the grammar in use, empty when there is none.
    [[nodiscard]] std::string_view grammarScope() const noexcept { return grammarScope_; }
    /// Fallback profile selected for the file. Never null, even at tier 1: the
    /// repair layer needs it.
    [[nodiscard]] const LanguageProfile& profile() const noexcept { return lexer_.profile(); }
    [[nodiscard]] const HighlightLimits& limits() const noexcept { return limits_; }
    /// The collaborators this highlighter was built on. Handed out because the
    /// quality report and the CLI need the engine's rejection counters and the
    /// registry's missing-scope list; both are borrowed, never owned.
    [[nodiscard]] syntax::GrammarRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] syntax::IRegexEngine& engine() const noexcept { return *engine_; }
    [[nodiscard]] const syntax::Tokenizer& tokenizer() const noexcept { return *tokenizer_; }

    /// Samples the head of the file and demotes a useless grammar to tier 2.
    /// Idempotent and cheap; call it once after construction, before rendering,
    /// with the first lines of the document. Returns the (possibly new) tier.
    Tier probe(std::span<const std::string_view> lines);

    /// User override ("syntax highlighting: off") and test hook. Demoting to
    /// kPlain always works; promoting to kGrammar without a grammar is ignored.
    void forceTier(Tier tier) noexcept;

    // --- state -------------------------------------------------------------
    /// State for line 0.
    [[nodiscard]] LineState initialState();

    // --- the actual work ---------------------------------------------------
    /// Classifies one line. `state` is read as the start state and overwritten
    /// with the end state. `out` is cleared and filled with an exact tiling of
    /// [0, line.size()).
    void scopeLine(std::string_view line, LineState& state, std::vector<ScopedSpan>& out);

    /// Resolves scopes to palette ids and merges adjacent identical styles.
    /// Cheap enough to re-run on a theme change instead of retokenizing.
    void styleSpans(std::span<const ScopedSpan> scoped, std::vector<StyledSpan>& out);

    /// scopeLine() + styleSpans(). The call the UI makes.
    void highlightLine(std::string_view line, LineState& state, std::vector<StyledSpan>& out);
    [[nodiscard]] std::vector<StyledSpan> highlightLine(std::string_view line, LineState& state);

    /// A range of lines in one call, flat, for a whole viewport.
    [[nodiscard]] BatchResult highlightLines(std::span<const std::string_view> lines,
                                             const LineState& startState);

    // --- theme -------------------------------------------------------------
    /// Swaps the theme. Existing ScopedSpans stay valid, so restyling a screen
    /// is a re-resolve (styleSpans) and never a retokenization. Every previously
    /// returned StyleId is invalidated: the palette belongs to the theme.
    ///
    /// Theme types are spelled `ide::theme::` throughout this class on purpose:
    /// a member function named theme() lives here, and while qualified name
    /// lookup ignores functions, leaning on that subtlety in a header every
    /// other lane includes is not worth the review cost.
    void setTheme(const ide::theme::Theme& newTheme);
    [[nodiscard]] const ide::theme::Theme& theme() const noexcept { return *theme_; }
    /// Style for text no rule matched. The renderer needs it for the background
    /// and for anything it paints outside a span.
    [[nodiscard]] ide::theme::Style defaultStyle() const noexcept {
        return theme_->defaultStyle();
    }
    [[nodiscard]] static constexpr ide::theme::StyleId defaultStyleId() noexcept {
        return ide::theme::kDefaultStyleId;
    }
    /// Palette id for an interned scope stack, memoised per stack id.
    [[nodiscard]] ide::theme::StyleId styleFor(syntax::ScopeStackId scopes);

    // --- introspection (quality report, tests, diagnostics) ----------------
    [[nodiscard]] const syntax::ScopeStackTable& scopeTable() const noexcept;
    /// Number of scope names the root state contributes (1 for a normal grammar,
    /// 0 when there is none). Anything at or below this depth is "unscoped".
    [[nodiscard]] size_t rootScopeDepth() const noexcept { return rootScopeDepth_; }
    /// True when `scopes` carries nothing the theme can colour beyond the
    /// grammar's root scope - the definition of a hole the repair layer fills.
    [[nodiscard]] bool isRootOnly(syntax::ScopeStackId scopes) const noexcept;
    /// Patterns the regex engine refused while tokenizing this document, and
    /// begin/end rules skipped because their end pattern was unusable. Both are
    /// the direct measure of "how much of this grammar actually works".
    [[nodiscard]] size_t refusedPatternCount() const noexcept;
    [[nodiscard]] size_t unusableRuleCount() const noexcept;

    struct Stats {
        size_t lines = 0;
        size_t grammarLines = 0;
        size_t fallbackLines = 0;
        size_t plainLines = 0;
        size_t repairRegions = 0;
        size_t repairedBytes = 0;  ///< non-whitespace bytes the fallback claimed
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = Stats{}; }

private:
    struct Region {
        size_t begin = 0;
        size_t end = 0;
        size_t spanStart = 0;  ///< index into repairSpans_
        size_t spanCount = 0;
    };

    [[nodiscard]] syntax::ScopeStackTable& table() noexcept;
    void emitPlain(std::string_view line, LineState& state, std::vector<ScopedSpan>& out);
    void emitFallback(std::string_view line, LineState& state, std::vector<ScopedSpan>& out);
    void emitGrammar(std::string_view line, LineState& state, std::vector<ScopedSpan>& out);
    /// Finds the root-only stretches of `line`, lexes them and splices the
    /// result under the grammar's tokens.
    void repairInto(std::string_view line, const std::vector<syntax::TokenSpan>& tokens,
                    std::vector<ScopedSpan>& out);

    syntax::GrammarRegistry* registry_ = nullptr;
    syntax::IRegexEngine* engine_ = nullptr;
    const ide::theme::Theme* theme_ = nullptr;
    HighlightLimits limits_;

    syntax::GrammarId grammarId_ = syntax::kInvalidGrammarId;
    std::string grammarScope_;
    Tier tier_ = Tier::kFallback;
    size_t rootScopeDepth_ = 0;

    FallbackLexer lexer_;
    /// Constructed once and never re-emplaced, so scopeTable() has a stable
    /// address and ScopeStackIds stay valid for the highlighter's lifetime.
    std::optional<syntax::Tokenizer> tokenizer_;

    std::unordered_map<syntax::ScopeStackId, ide::theme::StyleId> styleCache_;
    std::vector<std::string_view> scopeScratch_;

    // Per-line scratch, reused to keep the hot path allocation free.
    std::vector<uint8_t> holeFlags_;
    std::vector<Region> regions_;
    std::vector<FallbackSpan> repairSpans_;
    std::vector<FallbackSpan> lexScratch_;
    std::vector<ScopedSpan> scopedScratch_;

    Stats stats_;
};

/// True when `spans` is a sorted, non-overlapping, gapless tiling of
/// [0, length). The single most important property of this module; the fuzz test
/// asserts it over hundreds of generated lines for all three tiers.
[[nodiscard]] bool spansTile(std::span<const StyledSpan> spans, size_t length) noexcept;
[[nodiscard]] bool spansTile(std::span<const ScopedSpan> spans, size_t length) noexcept;

}  // namespace ide::highlight
