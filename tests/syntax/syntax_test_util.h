#pragma once

// Shared helpers for the ide::syntax test suite.
//
// Header-only on purpose: tests/CMakeLists.txt globs "*_test.cpp" only, so a
// helper .cpp would never be compiled. Everything here is `inline` or a class,
// so multiple test translation units may include it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <ide/syntax/grammar.h>
#include <ide/syntax/std_regex_engine.h>
#include <ide/syntax/tokenizer.h>

namespace cope_test {

/// A token with its interned scope stack resolved back to text, so a failure
/// reads `[3,6) "source.t meta.tag"` instead of an opaque integer id.
struct FlatToken {
    size_t begin = 0;
    size_t end = 0;
    std::string scopes;
};

inline bool operator==(const FlatToken& a, const FlatToken& b) {
    return a.begin == b.begin && a.end == b.end && a.scopes == b.scopes;
}
inline bool operator!=(const FlatToken& a, const FlatToken& b) { return !(a == b); }

/// Found by GoogleTest through ADL; keeps failure output readable.
inline void PrintTo(const FlatToken& token, std::ostream* os) {
    *os << '[' << token.begin << ',' << token.end << ") \"" << token.scopes << '"';
}

/// Registry + engine + tokenizer for one grammar, wired the way production code
/// wires them. Non-copyable because Tokenizer holds references to the registry
/// and the engine, which therefore must be declared before it.
class Harness {
public:
    explicit Harness(std::string_view grammarJson) {
        gid_ = registry_.addGrammarJson(grammarJson, &error_);
        tokenizer_.emplace(registry_, engine_, gid_);
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    [[nodiscard]] bool loaded() const { return gid_ != ide::syntax::kInvalidGrammarId; }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] ide::syntax::GrammarId gid() const { return gid_; }

    ide::syntax::GrammarRegistry& registry() { return registry_; }
    ide::syntax::StdRegexEngine& engine() { return engine_; }
    ide::syntax::Tokenizer& tokenizer() { return *tokenizer_; }
    [[nodiscard]] const ide::syntax::Grammar* grammar() const {
        return registry_.grammarById(gid_);
    }

    ide::syntax::State initialState() { return tokenizer_->initialState(); }

    /// Tokenizes one line and flattens the result. The raw TokenizeResult of the
    /// most recent call stays available through last().
    std::vector<FlatToken> tokenize(std::string_view line, const ide::syntax::State& start) {
        // Copy first: callers legitimately pass endState(), which aliases last_.
        const ide::syntax::State from = start;
        last_ = tokenizer_->tokenizeLine(line, from);
        return flatten(last_.tokens);
    }
    [[nodiscard]] const ide::syntax::TokenizeResult& last() const { return last_; }
    [[nodiscard]] const ide::syntax::State& endState() const { return last_.endState; }

    [[nodiscard]] std::vector<FlatToken> flatten(
        const std::vector<ide::syntax::TokenSpan>& tokens) const {
        std::vector<FlatToken> out;
        out.reserve(tokens.size());
        for (const ide::syntax::TokenSpan& span : tokens) {
            out.push_back(
                FlatToken{span.begin, span.end, tokenizer_->scopeTable().flatten(span.scopes)});
        }
        return out;
    }

private:
    ide::syntax::GrammarRegistry registry_;
    ide::syntax::StdRegexEngine engine_;
    ide::syntax::GrammarId gid_ = ide::syntax::kInvalidGrammarId;
    std::string error_;
    ide::syntax::TokenizeResult last_;
    std::optional<ide::syntax::Tokenizer> tokenizer_;
};

/// Deterministic PRNG so the fuzz-style tests reproduce byte for byte on every
/// runner. Knuth's LCG constants; only the high bits are used.
class Lcg {
public:
    explicit Lcg(uint64_t seed) noexcept : state_(seed) {}

    uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return state_ >> 33;
    }
    /// Uniform enough for tests: value in [0, bound), 0 when bound == 0.
    size_t below(size_t bound) noexcept {
        return (bound == 0u) ? 0u : static_cast<size_t>(next() % bound);
    }

private:
    uint64_t state_ = 0;
};

}  // namespace cope_test
