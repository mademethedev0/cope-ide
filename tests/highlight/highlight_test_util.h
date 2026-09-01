#pragma once

// Shared helpers for the ide::highlight test suite.
//
// Header-only on purpose: tests/CMakeLists.txt globs "*_test.cpp" only, so a
// helper .cpp would never be compiled.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/highlight/fallback_lexer.h>
#include <ide/highlight/highlighter.h>
#include <ide/highlight/quality.h>
#include <ide/syntax/std_regex_engine.h>
#include <ide/theme/theme.h>

namespace ember_highlight_test {

using ide::highlight::FallbackSpan;
using ide::highlight::FallbackState;
using ide::highlight::Highlighter;
using ide::highlight::HighlightLimits;
using ide::highlight::LineState;
using ide::highlight::ScopedSpan;
using ide::highlight::StyledSpan;
using ide::highlight::Tier;

/// Every scope family the fallback lexer can emit gets its OWN colour, so two
/// different classifications can never collapse into one StyleId. That matters:
/// the palette interns by visual equality, so duplicate colours would silently
/// break the "distinct styles" assertions.
inline constexpr std::string_view kTestThemeJson = R"json({
  "name": "ember-test",
  "type": "dark",
  "colors": { "editor.foreground": "#C0C0C0", "editor.background": "#101010" },
  "tokenColors": [
    { "scope": "comment", "settings": { "foreground": "#6A9955" } },
    { "scope": "string", "settings": { "foreground": "#CE9178" } },
    { "scope": "constant.numeric", "settings": { "foreground": "#B5CEA8" } },
    { "scope": "constant.language", "settings": { "foreground": "#569CD6" } },
    { "scope": "constant.character.escape", "settings": { "foreground": "#D7BA7D" } },
    { "scope": "keyword.control", "settings": { "foreground": "#C586C0" } },
    { "scope": "keyword.operator", "settings": { "foreground": "#D4D4D4" } },
    { "scope": "storage.type", "settings": { "foreground": "#4EC9B0" } },
    { "scope": "entity.name.function", "settings": { "foreground": "#DCDCAA" } },
    { "scope": "entity.name.tag", "settings": { "foreground": "#4FC1FF" } },
    { "scope": "entity.other.attribute-name", "settings": { "foreground": "#9CDCFE" } },
    { "scope": "variable.other.property", "settings": { "foreground": "#C8C8C8" } },
    { "scope": "variable.other", "settings": { "foreground": "#9CDCF0" } },
    { "scope": "punctuation", "settings": { "foreground": "#808080" } },
    { "scope": "meta.preprocessor", "settings": { "foreground": "#C586A0" } }
  ]
})json";

/// A theme with exactly one rule: everything except comments lands on the
/// default style. Used to prove the quality report notices a theme that is
/// effectively not working.
inline constexpr std::string_view kPoorThemeJson = R"json({
  "name": "ember-test-poor",
  "type": "dark",
  "colors": { "editor.foreground": "#C0C0C0" },
  "tokenColors": [
    { "scope": "comment", "settings": { "foreground": "#6A9955" } }
  ]
})json";

/// Same scopes as kTestThemeJson, different colours: proves a theme swap changes
/// StyleIds without any retokenization.
inline constexpr std::string_view kAltThemeJson = R"json({
  "name": "ember-test-alt",
  "type": "light",
  "colors": { "editor.foreground": "#202020", "editor.background": "#FFFFFF" },
  "tokenColors": [
    { "scope": "comment", "settings": { "foreground": "#008000" } },
    { "scope": "string", "settings": { "foreground": "#A31515" } },
    { "scope": "keyword.control", "settings": { "foreground": "#0000FF" } },
    { "scope": "variable.other", "settings": { "foreground": "#001080" } }
  ]
})json";

/// A span with its interned scope stack resolved back to text, so a failure
/// reads `[3,6) "source.t keyword.control"` instead of an opaque integer id.
struct FlatSpan {
    size_t begin = 0;
    size_t end = 0;
    std::string scopes;
};

inline bool operator==(const FlatSpan& a, const FlatSpan& b) {
    return a.begin == b.begin && a.end == b.end && a.scopes == b.scopes;
}
inline bool operator!=(const FlatSpan& a, const FlatSpan& b) { return !(a == b); }

inline void PrintTo(const FlatSpan& span, std::ostream* os) {
    *os << '[' << span.begin << ',' << span.end << ") \"" << span.scopes << '"';
}

/// THE invariant: sorted, non-overlapping, gapless, exactly covering [0, length).
/// Works for FallbackSpan, ScopedSpan and StyledSpan alike.
template <typename SpanT>
[[nodiscard]] ::testing::AssertionResult tiles(const std::vector<SpanT>& spans, size_t length) {
    size_t expected = 0;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].begin != expected) {
            return ::testing::AssertionFailure() << "span " << i << " begins at " << spans[i].begin
                                                << ", expected " << expected;
        }
        if (spans[i].end <= spans[i].begin) {
            return ::testing::AssertionFailure() << "span " << i << " is empty or inverted: ["
                                                 << spans[i].begin << ',' << spans[i].end << ')';
        }
        expected = spans[i].end;
    }
    if (expected != length) {
        return ::testing::AssertionFailure() << "spans cover " << expected << " bytes, input is "
                                             << length;
    }
    return ::testing::AssertionSuccess();
}

// --- fallback span helpers ---------------------------------------------------

[[nodiscard]] inline std::string_view scopeAt(const std::vector<FallbackSpan>& spans,
                                              size_t offset) {
    for (const FallbackSpan& span : spans) {
        if (offset >= span.begin && offset < span.end) return span.scope;
    }
    return std::string_view{"<no span>"};
}

/// Scope of the first byte of the first occurrence of `needle`.
[[nodiscard]] inline std::string_view scopeOf(const std::vector<FallbackSpan>& spans,
                                              std::string_view text, std::string_view needle) {
    const size_t at = text.find(needle);
    if (at == std::string_view::npos) return std::string_view{"<needle not in text>"};
    return scopeAt(spans, at);
}

/// `needle` occurs in `text` and is covered by EXACTLY one span carrying `scope`.
/// This is the strong form: it catches a heuristic that classifies the right
/// bytes with the wrong boundaries.
[[nodiscard]] inline ::testing::AssertionResult hasExactSpan(
    const std::vector<FallbackSpan>& spans, std::string_view text, std::string_view needle,
    std::string_view scope) {
    const size_t at = text.find(needle);
    if (at == std::string_view::npos) {
        return ::testing::AssertionFailure() << '"' << needle << "\" is not in \"" << text << '"';
    }
    for (const FallbackSpan& span : spans) {
        if (span.begin != at) continue;
        if (span.end != at + needle.size()) {
            return ::testing::AssertionFailure()
                   << "span at " << at << " ends at " << span.end << ", expected "
                   << (at + needle.size()) << " (scope \"" << span.scope << "\")";
        }
        if (span.scope != scope) {
            return ::testing::AssertionFailure() << "span [" << span.begin << ',' << span.end
                                                 << ") has scope \"" << span.scope
                                                 << "\", expected \"" << scope << '"';
        }
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "no span begins at " << at << " (for \"" << needle
                                         << "\"); scope there is \"" << scopeAt(spans, at) << '"';
}

// --- deterministic PRNG ------------------------------------------------------

/// Knuth's LCG. Seeded explicitly so every fuzz failure reproduces byte for byte
/// on every runner.
class Lcg {
public:
    explicit Lcg(uint64_t seed) noexcept : state_(seed) {}

    uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return state_ >> 33;
    }
    size_t below(size_t bound) noexcept {
        return (bound == 0u) ? 0u : static_cast<size_t>(next() % bound);
    }
    bool chance(size_t oneIn) noexcept { return below(oneIn) == 0u; }

private:
    uint64_t state_ = 0;
};

// --- highlighter harness ----------------------------------------------------

struct Config {
    /// Empty means "no grammar is registered", which is how tier 2 is reached.
    std::string_view grammarJson;
    std::string_view fileName = "test.unknown";
    std::string_view themeJson = kTestThemeJson;
    HighlightLimits limits{};
    size_t byteSize = 0;
    size_t lineCount = 0;
};

/// Registry + engine + theme + highlighter, wired the way production code wires
/// them. Non-copyable: the highlighter holds references to the other three,
/// which therefore must be declared before it.
class Harness {
public:
    explicit Harness(const Config& config) {
        if (!config.grammarJson.empty()) {
            grammarId_ = registry_.addGrammarJson(config.grammarJson, &error_);
            EXPECT_NE(grammarId_, ide::syntax::kInvalidGrammarId) << "grammar load failed: "
                                                                  << error_;
        }
        std::string themeError;
        std::optional<ide::theme::Theme> loaded =
            ide::theme::Theme::fromJsonText(config.themeJson, &themeError);
        EXPECT_TRUE(loaded.has_value()) << "theme load failed: " << themeError;
        if (loaded.has_value()) theme_ = std::move(*loaded);

        ide::highlight::FileInfo info;
        info.name = config.fileName;
        info.byteSize = config.byteSize;
        info.lineCount = config.lineCount;
        highlighter_.emplace(registry_, engine_, theme_, info, config.limits);
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    [[nodiscard]] Highlighter& h() { return *highlighter_; }
    [[nodiscard]] ide::syntax::GrammarId grammarId() const noexcept { return grammarId_; }
    [[nodiscard]] ide::theme::Theme& theme() noexcept { return theme_; }
    [[nodiscard]] ide::syntax::GrammarRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] ide::syntax::StdRegexEngine& engine() noexcept { return engine_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Scoped spans of one line, starting from the document's initial state.
    [[nodiscard]] std::vector<ScopedSpan> scopeFirstLine(std::string_view line) {
        LineState state = highlighter_->initialState();
        std::vector<ScopedSpan> out;
        highlighter_->scopeLine(line, state, out);
        return out;
    }

    /// Same, with the scope stacks resolved to text.
    [[nodiscard]] std::vector<FlatSpan> flatten(const std::vector<ScopedSpan>& spans) const {
        std::vector<FlatSpan> out;
        out.reserve(spans.size());
        for (const ScopedSpan& span : spans) {
            out.push_back(FlatSpan{span.begin, span.end,
                                   highlighter_->scopeTable().flatten(span.scopes)});
        }
        return out;
    }

    /// Flattened scope stack covering the first byte of `needle`.
    [[nodiscard]] std::string scopesOf(const std::vector<ScopedSpan>& spans, std::string_view line,
                                       std::string_view needle) const {
        const size_t at = line.find(needle);
        if (at == std::string_view::npos) return "<needle not in line>";
        for (const ScopedSpan& span : spans) {
            if (at >= span.begin && at < span.end) {
                return highlighter_->scopeTable().flatten(span.scopes);
            }
        }
        return "<no span>";
    }

private:
    ide::syntax::GrammarRegistry registry_;
    ide::syntax::StdRegexEngine engine_;
    ide::theme::Theme theme_;
    ide::syntax::GrammarId grammarId_ = ide::syntax::kInvalidGrammarId;
    std::string error_;
    std::optional<Highlighter> highlighter_;
};

}  // namespace ember_highlight_test
