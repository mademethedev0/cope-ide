#pragma once

// Why this file exists
// -------------------
// Five previous attempts at this engine died because nobody could see what the
// tokenizer was doing: a scope came out wrong somewhere in a 900-rule grammar
// and the only tool available was guessing. This is the --trace facility: a sink
// that receives, for every position, which rule was tried, whether it matched,
// which one won, and every push/pop/limit event.
//
// Zero cost when disabled: the tokenizer tests a bool member at each call site,
// so nothing is constructed (not even a string_view) unless a sink is installed.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include <ide/syntax/grammar.h>

namespace ide::syntax {

enum class TraceEventKind : uint8_t {
    LineStart,   ///< a line began; position is the start offset
    WhileCheck,  ///< a while pattern was evaluated for a stack frame
    RuleTried,   ///< a candidate pattern was searched at `position`
    RuleWon,     ///< the candidate that won at `position`
    NoMatch,     ///< nothing matched at `position`; the line is finished
    Push,        ///< a begin/end or begin/while rule was entered
    Pop,         ///< a rule was left (end matched, or while failed)
    Limit,       ///< a safety limit fired (line length, depth, iterations, ...)
    Notice,      ///< anything else worth reporting (compile failure, degradation)
};

/// One trace record. All views point at grammar-owned or engine-owned storage
/// and are valid only for the duration of the callback.
struct TraceEvent {
    TraceEventKind kind = TraceEventKind::Notice;
    size_t position = 0;
    RuleRef rule{};
    bool matched = false;
    size_t matchBegin = 0;
    size_t matchEnd = 0;
    /// Index of the candidate in the current pattern list; -1 when not
    /// applicable, and -2 for the enclosing rule's end pattern.
    int candidateIndex = -1;
    std::string_view pattern;  ///< regex source that was searched
    std::string_view detail;   ///< rule debug name, limit name or reason
};

/// Candidate index used for the enclosing rule's end/while pattern.
inline constexpr int kEndPatternCandidateIndex = -2;

using TraceSink = std::function<void(const TraceEvent&)>;

[[nodiscard]] inline std::string_view traceEventKindName(TraceEventKind kind) noexcept {
    switch (kind) {
        case TraceEventKind::LineStart: return "line-start";
        case TraceEventKind::WhileCheck: return "while-check";
        case TraceEventKind::RuleTried: return "tried";
        case TraceEventKind::RuleWon: return "won";
        case TraceEventKind::NoMatch: return "no-match";
        case TraceEventKind::Push: return "push";
        case TraceEventKind::Pop: return "pop";
        case TraceEventKind::Limit: return "limit";
        case TraceEventKind::Notice: return "notice";
    }
    return "?";
}

}  // namespace ide::syntax
