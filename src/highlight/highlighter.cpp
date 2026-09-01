#include <ide/highlight/highlighter.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ide::highlight {
namespace {

/// Emitter for ScopedSpans: guarantees the tiling invariant by construction, the
/// same way the fallback lexer's Sink does, and merges adjacent identical scope
/// stacks so the styling pass has less to do.
struct ScopedSink {
    std::vector<ScopedSpan>* out = nullptr;
    size_t last = 0;

    void produce(size_t end, syntax::ScopeStackId scopes) {
        if (out == nullptr || end <= last) return;
        if (!out->empty() && out->back().scopes == scopes) {
            out->back().end = end;
        } else {
            out->push_back(ScopedSpan{last, end, scopes});
        }
        last = end;
    }
};

/// `meta.*` scopes mark structure, not appearance: TextMate grammars wrap whole
/// function signatures and statement bodies in them and virtually no theme
/// colours them. A stretch whose only scopes are meta scopes therefore renders
/// exactly like an unscoped one, which is why the repair layer may treat it as a
/// hole (HighlightLimits::repairMetaOnlyRuns).
[[nodiscard]] bool isMetaScope(std::string_view scope) noexcept {
    return scope == "meta" || scope.starts_with("meta.");
}

[[nodiscard]] size_t countNonWhitespace(std::string_view text, size_t begin, size_t end) noexcept {
    const size_t stop = std::min(end, text.size());
    size_t count = 0;
    for (size_t i = std::min(begin, stop); i < stop; ++i) {
        if (!isWhitespaceByte(text[i])) ++count;
    }
    return count;
}

[[nodiscard]] std::string_view baseName(std::string_view path) noexcept {
    size_t start = 0;
    for (size_t i = path.size(); i > 0; --i) {
        const char c = path[i - 1u];
        if (c == '/' || c == '\\') {
            start = i;
            break;
        }
    }
    return path.substr(start);
}

[[nodiscard]] std::string toLowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

std::string_view tierName(Tier tier) noexcept {
    switch (tier) {
        case Tier::kGrammar: return "grammar";
        case Tier::kFallback: return "fallback";
        case Tier::kPlain: return "plain";
    }
    return "unknown";
}

std::span<const StyledSpan> BatchResult::lineSpans(size_t line) const noexcept {
    if (lineOffsets.size() < 2u || line > lineOffsets.size() - 2u) return {};
    const size_t from = lineOffsets[line];
    const size_t to = lineOffsets[line + 1u];
    if (from > to || to > spans.size()) return {};
    return std::span<const StyledSpan>(spans.data() + from, to - from);
}

// --- construction and tier selection -----------------------------------------

Highlighter::Highlighter(syntax::GrammarRegistry& registry, syntax::IRegexEngine& engine,
                         const ide::theme::Theme& theme, const FileInfo& file,
                         HighlightLimits limits)
    : registry_(&registry),
      engine_(&engine),
      theme_(&theme),
      limits_(limits),
      lexer_(profileForFileName(file.name)) {
    // Past the tokenizer's own bail-out length the grammar emits one root token
    // for the whole line; repairing that would mean lexing a 200KB "hole", so the
    // two limits must coincide.
    if (limits_.maxLineLength > syntax::kMaxLineLength) {
        limits_.maxLineLength = syntax::kMaxLineLength;
    }

    const syntax::Grammar* grammar = nullptr;
    const std::string_view extension = extensionOfFileName(file.name);
    if (!extension.empty()) {
        grammar = registry.grammarForExtension(extension);
        if (grammar == nullptr) grammar = registry.grammarForExtension(toLowerAscii(extension));
    }
    if (grammar == nullptr) {
        // Grammars register extensionless names ("Makefile", "Dockerfile") as
        // file types too, so the whole base name is worth one lookup.
        const std::string_view base = baseName(file.name);
        if (!base.empty()) grammar = registry.grammarForExtension(base);
    }

    const bool usable = grammar != nullptr && grammar->validRule(grammar->rootRule());
    if (usable) {
        grammarId_ = grammar->id();
        grammarScope_ = grammar->scopeName();
    }
    // Always constructed, even without a grammar: it owns the scope interning
    // table that every tier's spans refer to. With kInvalidGrammarId it simply
    // emits one root token per line, which is exactly tier 3's shape.
    tokenizer_.emplace(registry, engine, grammarId_);
    // Two statements on purpose: initialState() interns the grammar's scope name
    // into the table, so its depth may only be asked for afterwards.
    const syntax::ScopeStackId rootScopes = tokenizer_->initialState().contentScopes();
    rootScopeDepth_ = tokenizer_->scopeTable().depth(rootScopes);

    const bool tooBig = (limits_.maxFileBytes > 0u && file.byteSize > limits_.maxFileBytes) ||
                        (limits_.maxFileLines > 0u && file.lineCount > limits_.maxFileLines);
    tier_ = tooBig ? Tier::kPlain : (usable ? Tier::kGrammar : Tier::kFallback);
}

Tier Highlighter::probe(std::span<const std::string_view> lines) {
    if (tier_ != Tier::kGrammar || lines.empty()) return tier_;
    const size_t count = std::min(lines.size(), limits_.probeLines);
    LineState state = initialState();
    size_t nonWhitespace = 0;
    size_t scoped = 0;
    for (size_t i = 0; i < count; ++i) {
        const std::string_view line = lines[i];
        if (line.empty() || line.size() > limits_.maxLineLength) continue;
        const syntax::TokenizeResult result = tokenizer_->tokenizeLine(line, state.tm);
        state.tm = result.endState;
        for (const syntax::TokenSpan& token : result.tokens) {
            const size_t bytes = countNonWhitespace(line, token.begin, token.end);
            nonWhitespace += bytes;
            if (!isRootOnly(token.scopes)) scoped += bytes;
        }
    }
    if (nonWhitespace > 0u) {
        const double coverage =
            static_cast<double>(scoped) / static_cast<double>(nonWhitespace);
        if (coverage < limits_.minGrammarCoverage) tier_ = Tier::kFallback;
    }
    return tier_;
}

void Highlighter::forceTier(Tier tier) noexcept {
    if (tier == Tier::kGrammar && grammarId_ == syntax::kInvalidGrammarId) return;
    tier_ = tier;
}

LineState Highlighter::initialState() {
    LineState state;
    state.tm = tokenizer_->initialState();
    return state;
}

// --- theme -------------------------------------------------------------------

void Highlighter::setTheme(const ide::theme::Theme& newTheme) {
    theme_ = &newTheme;
    styleCache_.clear();
}

ide::theme::StyleId Highlighter::styleFor(syntax::ScopeStackId scopes) {
    const auto found = styleCache_.find(scopes);
    if (found != styleCache_.end()) return found->second;
    scopeScratch_.clear();
    scopeTable().resolve(scopes, scopeScratch_);
    const ide::theme::StyleId id =
        theme_->resolve(std::span<const std::string_view>(scopeScratch_));
    styleCache_.emplace(scopes, id);
    return id;
}

// --- introspection -----------------------------------------------------------

syntax::ScopeStackTable& Highlighter::table() noexcept { return tokenizer_->scopeTable(); }

const syntax::ScopeStackTable& Highlighter::scopeTable() const noexcept {
    return tokenizer_->scopeTable();
}

bool Highlighter::isRootOnly(syntax::ScopeStackId scopes) const noexcept {
    const syntax::ScopeStackTable& scopeStacks = scopeTable();
    if (scopeStacks.depth(scopes) <= rootScopeDepth_) return true;
    if (!limits_.repairMetaOnlyRuns) return false;
    syntax::ScopeStackId current = scopes;
    while (scopeStacks.depth(current) > rootScopeDepth_) {
        if (!isMetaScope(scopeStacks.scopeAt(current))) return false;
        const syntax::ScopeStackId parent = scopeStacks.parentOf(current);
        if (parent == current) break;  // defensive: the root maps to itself
        current = parent;
    }
    return true;
}

size_t Highlighter::refusedPatternCount() const noexcept {
    return tokenizer_->stats().regexCompileFailures;
}

size_t Highlighter::unusableRuleCount() const noexcept {
    return tokenizer_->stats().rulesDisabledByEndPattern;
}

// --- the cascade -------------------------------------------------------------

void Highlighter::scopeLine(std::string_view line, LineState& state,
                            std::vector<ScopedSpan>& out) {
    out.clear();
    ++stats_.lines;
    if (tier_ == Tier::kPlain || line.size() > limits_.maxLineLength) {
        emitPlain(line, state, out);
        return;
    }
    if (tier_ == Tier::kFallback) {
        emitFallback(line, state, out);
        return;
    }
    emitGrammar(line, state, out);
}

void Highlighter::emitPlain(std::string_view line, LineState& state,
                            std::vector<ScopedSpan>& out) {
    ++stats_.plainLines;
    // The incoming scopes are kept, so a line inside a block comment that got
    // too long still paints as a comment instead of flashing to plain text.
    if (!line.empty()) out.push_back(ScopedSpan{0u, line.size(), state.tm.contentScopes()});
    state.tm = state.tm.withFirstLine(false);
    // state.fallback is deliberately untouched: this line was never read, so any
    // open block comment stays open.
}

void Highlighter::emitFallback(std::string_view line, LineState& state,
                               std::vector<ScopedSpan>& out) {
    ++stats_.fallbackLines;
    lexScratch_.clear();
    lexer_.lex(line, state.fallback, lexScratch_);
    const syntax::ScopeStackId base = state.tm.contentScopes();
    ScopedSink sink{&out, 0u};
    for (const FallbackSpan& fallbackSpan : lexScratch_) {
        const syntax::ScopeStackId scopes =
            fallbackSpan.scope.empty() ? base : table().push(base, fallbackSpan.scope);
        sink.produce(fallbackSpan.end, scopes);
    }
    sink.produce(line.size(), base);
    state.tm = state.tm.withFirstLine(false);
}

void Highlighter::emitGrammar(std::string_view line, LineState& state,
                              std::vector<ScopedSpan>& out) {
    ++stats_.grammarLines;
    const syntax::TokenizeResult result = tokenizer_->tokenizeLine(line, state.tm);
    state.tm = result.endState;
    if (limits_.repairWithFallback && !result.bailedOnLineLength) {
        repairInto(line, result.tokens, out);
        return;
    }
    ScopedSink sink{&out, 0u};
    for (const syntax::TokenSpan& token : result.tokens) sink.produce(token.end, token.scopes);
    sink.produce(line.size(), result.tokens.empty() ? syntax::kRootScopeStack
                                                    : result.tokens.back().scopes);
}

void Highlighter::repairInto(std::string_view line, const std::vector<syntax::TokenSpan>& tokens,
                             std::vector<ScopedSpan>& out) {
    const size_t length = line.size();

    holeFlags_.assign(tokens.size(), uint8_t{0});
    for (size_t i = 0; i < tokens.size(); ++i) {
        holeFlags_[i] = isRootOnly(tokens[i].scopes) ? uint8_t{1} : uint8_t{0};
    }

    // A repair region is a maximal stretch of consecutive root-only tokens,
    // trimmed to start and end on a non-whitespace byte. Trimming is what keeps
    // whitespace-only gaps (indentation, the space between two grammar tokens)
    // untouched; keeping the interior whitespace is what lets "// hi" be lexed
    // as one comment instead of two disconnected fragments.
    regions_.clear();
    {
        size_t i = 0;
        while (i < tokens.size()) {
            if (holeFlags_[i] == 0u) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < tokens.size() && holeFlags_[j] != 0u) ++j;
            size_t begin = std::min(tokens[i].begin, length);
            size_t end = std::min(tokens[j - 1u].end, length);
            while (begin < end && isWhitespaceByte(line[begin])) ++begin;
            while (end > begin && isWhitespaceByte(line[end - 1u])) --end;
            if (begin < end) regions_.push_back(Region{begin, end, 0u, 0u});
            i = j;
        }
    }

    if (regions_.empty()) {
        ScopedSink sink{&out, 0u};
        for (const syntax::TokenSpan& token : tokens) sink.produce(token.end, token.scopes);
        sink.produce(length,
                     tokens.empty() ? syntax::kRootScopeStack : tokens.back().scopes);
        return;
    }
    stats_.repairRegions += regions_.size();

    size_t firstNonWhitespace = 0;
    while (firstNonWhitespace < length && isWhitespaceByte(line[firstNonWhitespace])) {
        ++firstNonWhitespace;
    }

    repairSpans_.clear();
    for (Region& region : regions_) {
        // Fragments never carry state: a multi-line construct inside a hole is
        // the grammar's business, and a fresh state cannot leak an open string
        // from one region into the next.
        FallbackState fresh;
        FallbackOptions options;
        options.atLineStart = region.begin <= firstNonWhitespace;
        lexScratch_.clear();
        lexer_.lex(line.substr(region.begin, region.end - region.begin), fresh, lexScratch_,
                   options);
        region.spanStart = repairSpans_.size();
        repairSpans_.insert(repairSpans_.end(), lexScratch_.begin(), lexScratch_.end());
        region.spanCount = repairSpans_.size() - region.spanStart;
    }

    // Splice. Walking the grammar's tokens (not the regions) is what makes
    // "the grammar always wins" structural: a region is only ever allowed to
    // subdivide a token it fully or partially covers, and the token's own scope
    // stack stays the base of every span the fallback contributes.
    ScopedSink sink{&out, 0u};
    size_t next = 0;
    for (const syntax::TokenSpan& token : tokens) {
        const size_t tokenEnd = std::min(token.end, length);
        size_t cursor = std::min(token.begin, tokenEnd);
        while (next < regions_.size() && regions_[next].end <= cursor) ++next;
        while (cursor < tokenEnd) {
            if (next >= regions_.size() || regions_[next].begin >= tokenEnd) {
                sink.produce(tokenEnd, token.scopes);
                cursor = tokenEnd;
                break;
            }
            const Region& region = regions_[next];
            if (region.begin > cursor) {
                sink.produce(region.begin, token.scopes);
                cursor = region.begin;
            }
            const size_t stop = std::min(region.end, tokenEnd);
            for (size_t s = 0; s < region.spanCount; ++s) {
                const FallbackSpan& fallbackSpan = repairSpans_[region.spanStart + s];
                const size_t from = region.begin + fallbackSpan.begin;
                const size_t to = region.begin + fallbackSpan.end;
                if (to <= cursor) continue;
                if (from >= stop) break;
                const size_t clipped = std::min(to, stop);
                const size_t emitFrom = sink.last;
                if (fallbackSpan.scope.empty()) {
                    sink.produce(clipped, token.scopes);
                } else {
                    sink.produce(clipped, table().push(token.scopes, fallbackSpan.scope));
                    stats_.repairedBytes += countNonWhitespace(line, emitFrom, clipped);
                }
            }
            cursor = stop;
            if (region.end <= cursor) ++next;
        }
    }
    sink.produce(length, tokens.empty() ? syntax::kRootScopeStack : tokens.back().scopes);
}

// --- styling -----------------------------------------------------------------

void Highlighter::styleSpans(std::span<const ScopedSpan> scoped, std::vector<StyledSpan>& out) {
    out.clear();
    out.reserve(scoped.size());
    for (const ScopedSpan& item : scoped) {
        if (item.end <= item.begin) continue;
        const ide::theme::StyleId id = styleFor(item.scopes);
        if (!out.empty() && out.back().style == id && out.back().end == item.begin) {
            out.back().end = item.end;
            continue;
        }
        out.push_back(StyledSpan{item.begin, item.end, id});
    }
}

void Highlighter::highlightLine(std::string_view line, LineState& state,
                                std::vector<StyledSpan>& out) {
    scopeLine(line, state, scopedScratch_);
    styleSpans(scopedScratch_, out);
}

std::vector<StyledSpan> Highlighter::highlightLine(std::string_view line, LineState& state) {
    std::vector<StyledSpan> out;
    highlightLine(line, state, out);
    return out;
}

BatchResult Highlighter::highlightLines(std::span<const std::string_view> lines,
                                        const LineState& startState) {
    BatchResult result;
    result.lineOffsets.reserve(lines.size() + 1u);
    result.endStates.reserve(lines.size());
    LineState state = startState;
    std::vector<StyledSpan> lineSpans;
    for (const std::string_view line : lines) {
        result.lineOffsets.push_back(result.spans.size());
        highlightLine(line, state, lineSpans);
        result.spans.insert(result.spans.end(), lineSpans.begin(), lineSpans.end());
        result.endStates.push_back(state);
    }
    result.lineOffsets.push_back(result.spans.size());
    return result;
}

// --- invariant checks --------------------------------------------------------

bool spansTile(std::span<const StyledSpan> spans, size_t length) noexcept {
    size_t expected = 0;
    for (const StyledSpan& item : spans) {
        if (item.begin != expected || item.end <= item.begin) return false;
        expected = item.end;
    }
    return expected == length;
}

bool spansTile(std::span<const ScopedSpan> spans, size_t length) noexcept {
    size_t expected = 0;
    for (const ScopedSpan& item : spans) {
        if (item.begin != expected || item.end <= item.begin) return false;
        expected = item.end;
    }
    return expected == length;
}

}  // namespace ide::highlight
