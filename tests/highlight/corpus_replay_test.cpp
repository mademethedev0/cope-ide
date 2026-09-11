#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ide/highlight/highlighter.h>
#include <ide/syntax/grammar.h>
#include <ide/syntax/json_lite.h>
#include <ide/syntax/regex_factory.h>
#include <ide/theme/theme.h>

#include "highlight_test_util.h"

// Real grammar + real source, with a deliberately small incremental consumer.
// This tests the LineState convergence contract, not the Android scheduler and
// not TextMate compatibility. Fresh scope strings are the oracle for cache
// correctness; interned IDs from different highlighters are never compared.
namespace {

using cope_highlight_test::FlatSpan;
using cope_highlight_test::Lcg;
using cope_highlight_test::tiles;
using ide::highlight::Highlighter;
using ide::highlight::LineState;
using ide::highlight::ScopedSpan;
using ide::highlight::StyledSpan;
using ide::syntax::RegexBackend;
namespace json = ide::syntax::json;

std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::nullopt;
    return text;
}

// Keep terminators attached, including CRLF. A terminal LF creates the empty
// final editor line. No views into mutable source survive an edit.
std::vector<std::string> splitLines(std::string_view source) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        const size_t newline = source.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.emplace_back(source.substr(start));
            return lines;
        }
        lines.emplace_back(source.substr(start, newline + 1u - start));
        start = newline + 1u;
    }
}

using GrammarSources = std::map<std::string, std::string>;

class Session {
public:
    Session(const GrammarSources& sources, RegexBackend backend, std::string_view scope,
            std::string_view extension, bool repair)
        : engine_(ide::syntax::makeRegexEngine(backend)) {
        registry_.setLoader([&sources](std::string_view requested) -> std::optional<std::string> {
            const auto found = sources.find(std::string(requested));
            if (found == sources.end()) return std::nullopt;
            return found->second;
        });
        if (!engine_) return;
        if (registry_.grammarForScope(scope) == nullptr) return;
        // Explicit ownership avoids incidental fileTypes and inline grammars
        // claiming an extension. External references still load lazily.
        registry_.mapExtension(extension, scope);
        theme_ = ide::theme::Theme::fromJsonText(cope_highlight_test::kTestThemeJson, nullptr);
        if (!theme_) return;
        const std::string name = "fixture." + std::string(extension);
        ide::highlight::FileInfo file;
        file.name = name;
        ide::highlight::HighlightLimits limits;
        limits.repairWithFallback = repair;
        highlighter_.emplace(registry_, *engine_, *theme_, file, limits);
    }

    bool ready() const {
        return highlighter_.has_value() && highlighter_->tier() == ide::highlight::Tier::kGrammar;
    }
    Highlighter& highlighter() { return *highlighter_; }

private:
    ide::syntax::GrammarRegistry registry_;
    std::unique_ptr<ide::syntax::IRegexEngine> engine_;
    std::optional<ide::theme::Theme> theme_;
    std::optional<Highlighter> highlighter_;
};

struct CachedLine {
    std::vector<FlatSpan> spans;
    LineState endState;
};

CachedLine classify(Highlighter& highlighter, const std::string& line, LineState& state) {
    std::vector<ScopedSpan> scoped;
    highlighter.scopeLine(line, state, scoped);
    EXPECT_TRUE(tiles(scoped, line.size()));
    std::vector<StyledSpan> styled;
    highlighter.styleSpans(scoped, styled);
    EXPECT_TRUE(tiles(styled, line.size()));
    for (size_t i = 1; i < styled.size(); ++i) {
        EXPECT_NE(styled[i - 1u].style, styled[i].style);
    }

    CachedLine result;
    result.endState = state;
    for (const ScopedSpan& span : scoped) {
        const std::string scopes = highlighter.scopeTable().flatten(span.scopes);
        // Normalize redundant boundaries, not just instance-local IDs.
        if (!result.spans.empty() && result.spans.back().end == span.begin &&
            result.spans.back().scopes == scopes) {
            result.spans.back().end = span.end;
        } else {
            result.spans.push_back(FlatSpan{span.begin, span.end, scopes});
        }
    }
    return result;
}

class ReplayCache {
public:
    explicit ReplayCache(Highlighter& highlighter) : highlighter_(highlighter) {}

    void update(std::string_view source) {
        const auto nextLines = splitLines(source);
        const size_t common = std::min(lines_.size(), nextLines.size());
        size_t prefix = 0;
        while (prefix < common && lines_[prefix] == nextLines[prefix]) ++prefix;
        size_t suffix = 0;
        while (suffix < common - prefix &&
               lines_[lines_.size() - 1u - suffix] == nextLines[nextLines.size() - 1u - suffix]) {
            ++suffix;
        }

        std::vector<CachedLine> next;
        next.reserve(nextLines.size());
        for (size_t i = 0; i < prefix; ++i) next.push_back(cached_[i]);
        LineState state = prefix == 0 ? highlighter_.initialState() : next.back().endState;
        recomputed = 0;
        reusedSuffix = 0;
        const size_t newSuffix = nextLines.size() - suffix;
        const size_t oldSuffix = lines_.size() - suffix;
        for (size_t i = prefix; i < nextLines.size(); ++i) {
            // Equality is meaningful only within this same live highlighter,
            // and only at an aligned, byte-identical suffix boundary.
            if (i >= newSuffix) {
                const size_t oldIndex = oldSuffix + (i - newSuffix);
                const LineState oldStart = oldIndex == 0 ? highlighter_.initialState()
                                                         : cached_[oldIndex - 1u].endState;
                if (state == oldStart) {
                    for (size_t j = oldIndex; j < cached_.size(); ++j) next.push_back(cached_[j]);
                    reusedSuffix = cached_.size() - oldIndex;
                    break;
                }
            }
            next.push_back(classify(highlighter_, nextLines[i], state));
            ++recomputed;
        }
        lines_ = nextLines;
        cached_ = std::move(next);
    }

    const std::vector<CachedLine>& lines() const { return cached_; }
    size_t recomputed = 0;
    size_t reusedSuffix = 0;

private:
    Highlighter& highlighter_;
    std::vector<std::string> lines_;
    std::vector<CachedLine> cached_;
};

class CorpusReplayTest : public ::testing::TestWithParam<RegexBackend> {
protected:
    void SetUp() override {
        // Discover all scopes once per test so nested grammars use the same
        // lazy-loading contract as production, without adding IO to core.
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(COPE_GRAMMARS_DIR)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        ASSERT_FALSE(paths.empty());
        for (const auto& path : paths) {
            const auto text = readText(path);
            ASSERT_TRUE(text.has_value()) << path.filename().string();
            const auto parsed = json::parse(*text);
            ASSERT_TRUE(parsed.ok) << path.filename().string();
            const std::string scope = parsed.root["scopeName"].string();
            ASSERT_FALSE(scope.empty()) << path.filename().string();
            sources_.try_emplace(scope, *text);
        }
        const auto manifest = readText(std::filesystem::path(COPE_HIGHLIGHT_FIXTURES_DIR) / "corpus.json");
        ASSERT_TRUE(manifest.has_value());
        const auto parsed = json::parse(*manifest);
        ASSERT_TRUE(parsed.ok) << parsed.error;
        cases_ = parsed.root["cases"];
        ASSERT_TRUE(cases_.isArray());
        ASSERT_GT(cases_.size(), 0u);
    }

    void compareFresh(ReplayCache& cache, const json::Value& item, const std::string& source,
                      bool repair) {
        cache.update(source);
        Session fresh(sources_, GetParam(), item["scope"].string(), item["extension"].string(), repair);
        ASSERT_TRUE(fresh.ready());
        const auto lines = splitLines(source);
        ASSERT_EQ(cache.lines().size(), lines.size());
        LineState state = fresh.highlighter().initialState();
        for (size_t i = 0; i < lines.size(); ++i) {
            SCOPED_TRACE("line " + std::to_string(i));
            const CachedLine expected = classify(fresh.highlighter(), lines[i], state);
            EXPECT_EQ(cache.lines()[i].spans, expected.spans);
        }
    }

    GrammarSources sources_;
    json::Value cases_;
};

TEST_P(CorpusReplayTest, RealSourceEditsAndUndoMatchFreshTokenization) {
    for (size_t index = 0; index < cases_.size(); ++index) {
        const auto& item = cases_.at(index);
        SCOPED_TRACE(item["name"].string());
        const std::string original = item["source"].string();
        ASSERT_FALSE(original.empty());
        ASSERT_FALSE(item["scope"].string().empty());
        ASSERT_FALSE(item["extension"].string().empty());
        for (const bool repair : {false, true}) {
            SCOPED_TRACE(repair ? "repaired" : "grammar-only");
            Session incremental(sources_, GetParam(), item["scope"].string(),
                                item["extension"].string(), repair);
            ASSERT_TRUE(incremental.ready());
            ReplayCache cache(incremental.highlighter());
            compareFresh(cache, item, original, repair);
            ASSERT_FALSE(HasFatalFailure());

            const auto& mutations = item["mutations"];
            ASSERT_TRUE(mutations.isArray());
            ASSERT_GT(mutations.size(), 0u);
            for (size_t m = 0; m < mutations.size(); ++m) {
                SCOPED_TRACE("mutation " + std::to_string(m));
                const auto& mutation = mutations.at(m);
                const std::string needle = mutation["find"].string();
                ASSERT_FALSE(needle.empty());
                const size_t position = original.find(needle);
                ASSERT_NE(position, std::string::npos);
                std::string changed = original;
                changed.replace(position, needle.size(), mutation["replace"].string());
                compareFresh(cache, item, changed, repair);
                ASSERT_FALSE(HasFatalFailure());
                compareFresh(cache, item, original, repair);
                ASSERT_FALSE(HasFatalFailure());
            }

            // Byte-level edits intentionally include malformed UTF-8, NUL,
            // newline splits/joins and edits at the first/last byte. Each edit
            // is undone, checking that cached pattern/state history is harmless.
            const std::vector<std::string> inserts = {
                "\n", "\r\n", "/*", "*/", "\"", "`", "é", "\xC3", std::string(1, '\0'), ""};
            Lcg rng(0xC0FEu + index);
            for (size_t edit = 0; edit < inserts.size(); ++edit) {
                SCOPED_TRACE("byte edit " + std::to_string(edit));
                const size_t position = edit == 0 ? 0u :
                    (edit == 1 ? original.size() : rng.below(original.size() + 1u));
                const size_t erase = std::min(rng.below(3u), original.size() - position);
                std::string changed = original;
                changed.replace(position, erase, inserts[edit]);
                compareFresh(cache, item, changed, repair);
                ASSERT_FALSE(HasFatalFailure());
                compareFresh(cache, item, original, repair);
                ASSERT_FALSE(HasFatalFailure());
            }
            // Deterministic line join (not dependent on a random byte landing
            // on LF), followed by restoring the exact original terminator.
            const size_t newline = original.find('\n');
            ASSERT_NE(newline, std::string::npos);
            std::string joined = original;
            const bool crlf = newline > 0u && original[newline - 1u] == '\r';
            joined.erase(crlf ? newline - 1u : newline, crlf ? 2u : 1u);
            compareFresh(cache, item, joined, repair);
            ASSERT_FALSE(HasFatalFailure());
            compareFresh(cache, item, original, repair);
            ASSERT_FALSE(HasFatalFailure());

            // Explicit whole-document replacement and recovery.
            compareFresh(cache, item, "", repair);
            ASSERT_FALSE(HasFatalFailure());
            compareFresh(cache, item, original, repair);
            ASSERT_FALSE(HasFatalFailure());
        }
    }
}

TEST_P(CorpusReplayTest, ConvergenceActuallyReusesAlignedSuffix) {
    // A tiny controlled grammar makes exact work-count expectations independent
    // of changes in the bundled C++ grammar. Real assets are exercised above.
    sources_["source.replay"] = R"json({
      "scopeName":"source.replay",
      "patterns":[
        {"name":"constant.numeric.replay","match":"[0-9]+"},
        {"name":"comment.block.replay","begin":"/\\*","end":"\\*/"}
      ]
    })json";
    const auto parsed = json::parse(R"json({"scope":"source.replay","extension":"cpp"})json");
    ASSERT_TRUE(parsed.ok);
    const auto& item = parsed.root;
    Session incremental(sources_, GetParam(), "source.replay", "cpp", false);
    ASSERT_TRUE(incremental.ready());
    ReplayCache cache(incremental.highlighter());
    const std::string original = "int value = 1;\nint tail = 2;\nint end = 3;\n";
    compareFresh(cache, item, original, false);
    ASSERT_FALSE(HasFatalFailure());
    compareFresh(cache, item, original, false);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(cache.recomputed, 0u);

    compareFresh(cache, item, "int value = 9;\nint tail = 2;\nint end = 3;\n", false);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(cache.recomputed, 1u);
    EXPECT_GT(cache.reusedSuffix, 0u);

    compareFresh(cache, item, "int value = 9;\n\nint tail = 2;\nint end = 3;\n", false);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(cache.recomputed, 1u);
    EXPECT_GT(cache.reusedSuffix, 0u);

    compareFresh(cache, item, "int value = 9;\nint tail = 2;\nint end = 3;\n", false);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(cache.recomputed, 0u);
    EXPECT_GT(cache.reusedSuffix, 0u);

    // An unmatched opener must invalidate every following cached line; an
    // implementation that reuses the suffix just because bytes match fails.
    compareFresh(cache, item, "/*\nint tail = 2;\nint end = 3;\n", false);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(cache.recomputed, 4u);
    EXPECT_EQ(cache.reusedSuffix, 0u);
    compareFresh(cache, item, original, false);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(cache.recomputed, 4u);
    EXPECT_EQ(cache.reusedSuffix, 0u);
}

TEST(CorpusLineSplitTest, PreservesTerminatorsAndEmptyFinalLine) {
    EXPECT_EQ(splitLines(""), (std::vector<std::string>{""}));
    EXPECT_EQ(splitLines("a\r\nb\n"), (std::vector<std::string>{"a\r\n", "b\n", ""}));
    EXPECT_EQ(splitLines("a\nb"), (std::vector<std::string>{"a\n", "b"}));
    EXPECT_EQ(splitLines("\n\n"), (std::vector<std::string>{"\n", "\n", ""}));
}

INSTANTIATE_TEST_SUITE_P(Std, CorpusReplayTest, ::testing::Values(RegexBackend::kStd));
#ifdef COPE_HAS_PCRE2
INSTANTIATE_TEST_SUITE_P(Pcre2, CorpusReplayTest, ::testing::Values(RegexBackend::kPcre2));
#endif

}  // namespace
