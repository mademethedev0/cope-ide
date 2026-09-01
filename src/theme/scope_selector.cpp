#include <ide/theme/scope_selector.h>

#include <utility>

namespace ide::theme {
namespace {

[[nodiscard]] constexpr bool isAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// Walks the dot-separated segments of a scope name or pattern without
/// allocating. "a.b" yields "a", "b"; "a." yields "a", ""; "" yields nothing.
class SegmentScan {
public:
    explicit SegmentScan(std::string_view text) noexcept : rest_(text), done_(text.empty()) {}

    [[nodiscard]] bool next(std::string_view& out) noexcept {
        if (done_) return false;
        const size_t dot = rest_.find('.');
        if (dot == std::string_view::npos) {
            out = rest_;
            rest_ = std::string_view();
            done_ = true;
        } else {
            out = rest_.substr(0, dot);
            rest_ = rest_.substr(dot + 1);
        }
        return true;
    }

private:
    std::string_view rest_;
    bool done_;
};

// --- selector tokenisation ---------------------------------------------------

enum class TokenKind : uint8_t { Word, Alternate, Exclude, GroupOpen, GroupClose };

struct Token {
    TokenKind kind = TokenKind::Word;
    std::string_view text;  ///< only meaningful for Word
};

/// `-` is an exclusion operator only when it opens a token, which is what keeps
/// the hyphen inside "comment.line.double-slash.js" part of the scope name while
/// still parsing "variable - meta.import" and "-comment" as exclusions.
[[nodiscard]] bool isDelimiter(char c) noexcept {
    return isAsciiSpace(c) || c == ',' || c == '|' || c == '(' || c == ')';
}

[[nodiscard]] std::vector<Token> tokenize(std::string_view text) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (isAsciiSpace(c)) {
            ++i;
            continue;
        }
        if (c == ',' || c == '|') {
            tokens.push_back(Token{TokenKind::Alternate, std::string_view()});
            ++i;
            continue;
        }
        if (c == '(') {
            tokens.push_back(Token{TokenKind::GroupOpen, std::string_view()});
            ++i;
            continue;
        }
        if (c == ')') {
            tokens.push_back(Token{TokenKind::GroupClose, std::string_view()});
            ++i;
            continue;
        }
        if (c == '-') {
            tokens.push_back(Token{TokenKind::Exclude, std::string_view()});
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < text.size() && !isDelimiter(text[i])) ++i;
        tokens.push_back(Token{TokenKind::Word, text.substr(start, i - start)});
    }
    return tokens;
}

// --- selector parsing -------------------------------------------------------

/// An alternative under construction; the owned form lives in ScopeSelector.
struct RawAlternative {
    std::vector<std::string> path;
    std::vector<std::vector<std::string>> exclusions;
};

std::vector<RawAlternative> parseList(const std::vector<Token>& tokens, size_t& index, int depth);

/// One comma-free branch: a sequence of patterns, groups and exclusions. Groups
/// multiply the alternatives under construction, which is how
/// "(a, b) c" becomes "a c" and "b c".
std::vector<RawAlternative> parseTerm(const std::vector<Token>& tokens, size_t& index, int depth) {
    std::vector<RawAlternative> current;
    current.emplace_back();

    while (index < tokens.size()) {
        const TokenKind kind = tokens[index].kind;
        if (kind == TokenKind::Alternate || kind == TokenKind::GroupClose) break;

        if (kind == TokenKind::Word) {
            const std::string_view word = tokens[index].text;
            ++index;
            for (RawAlternative& alternative : current) alternative.path.emplace_back(word);
            continue;
        }

        if (kind == TokenKind::GroupOpen) {
            ++index;
            std::vector<RawAlternative> inner;
            if (depth < kMaxSelectorGroupDepth) {
                inner = parseList(tokens, index, depth + 1);
            } else {
                // Too deep: consume the group and contribute nothing, which makes
                // the enclosing alternative match nothing rather than everything.
                int nesting = 1;
                while (index < tokens.size() && nesting > 0) {
                    if (tokens[index].kind == TokenKind::GroupOpen) ++nesting;
                    if (tokens[index].kind == TokenKind::GroupClose) --nesting;
                    ++index;
                }
                current.clear();
                continue;
            }
            if (index < tokens.size() && tokens[index].kind == TokenKind::GroupClose) ++index;

            std::vector<RawAlternative> product;
            for (const RawAlternative& prefix : current) {
                for (const RawAlternative& suffix : inner) {
                    if (product.size() >= kMaxSelectorAlternatives) break;
                    RawAlternative combined = prefix;
                    combined.path.insert(combined.path.end(), suffix.path.begin(), suffix.path.end());
                    combined.exclusions.insert(combined.exclusions.end(), suffix.exclusions.begin(),
                                               suffix.exclusions.end());
                    product.push_back(std::move(combined));
                }
            }
            current = std::move(product);
            continue;
        }

        // TokenKind::Exclude: the operand is one pattern or one group.
        ++index;
        std::vector<RawAlternative> excluded;
        if (index < tokens.size() && tokens[index].kind == TokenKind::Word) {
            RawAlternative one;
            one.path.emplace_back(tokens[index].text);
            excluded.push_back(std::move(one));
            ++index;
        } else if (index < tokens.size() && tokens[index].kind == TokenKind::GroupOpen) {
            ++index;
            if (depth < kMaxSelectorGroupDepth) excluded = parseList(tokens, index, depth + 1);
            if (index < tokens.size() && tokens[index].kind == TokenKind::GroupClose) ++index;
        } else {
            continue;  // dangling '-', ignored
        }
        for (RawAlternative& alternative : current) {
            for (const RawAlternative& clause : excluded) {
                if (!clause.path.empty()) alternative.exclusions.push_back(clause.path);
            }
        }
    }
    return current;
}

std::vector<RawAlternative> parseList(const std::vector<Token>& tokens, size_t& index, int depth) {
    std::vector<RawAlternative> out;
    while (true) {
        std::vector<RawAlternative> term = parseTerm(tokens, index, depth);
        for (RawAlternative& alternative : term) {
            // An alternative with no positive pattern would match everything,
            // which is never what a theme means. Drop it.
            if (alternative.path.empty()) continue;
            if (out.size() >= kMaxSelectorAlternatives) break;
            out.push_back(std::move(alternative));
        }
        if (index < tokens.size() && tokens[index].kind == TokenKind::Alternate) {
            ++index;
            continue;
        }
        break;
    }
    return out;
}

/// Deepest stack index the LAST pattern of `path` can occupy in a complete,
/// order-preserving placement, or -1 when the path does not match.
///
/// Scanning from the innermost end and taking the deepest candidate for each
/// pattern is both complete and depth-maximal: if any valid placement exists this
/// finds one, and it uses the largest possible index for the innermost pattern
/// (exchange argument — every greedy choice is at least as deep as the
/// corresponding index of any other valid placement).
[[nodiscard]] int64_t matchPathDeepest(const std::vector<std::string>& path,
                                       std::span<const std::string_view> scopeStack) noexcept {
    if (path.empty()) return -1;
    size_t remaining = path.size();
    size_t i = scopeStack.size();
    int64_t innermost = -1;
    while (remaining > 0 && i > 0) {
        --i;
        if (scopePatternMatches(path[remaining - 1], scopeStack[i])) {
            if (remaining == path.size()) innermost = static_cast<int64_t>(i);
            --remaining;
        }
    }
    return (remaining == 0) ? innermost : -1;
}

[[nodiscard]] uint32_t clampField(size_t value) noexcept {
    return (value > 0xFFFFu) ? 0xFFFFu : static_cast<uint32_t>(value);
}

}  // namespace

bool scopePatternMatches(std::string_view pattern, std::string_view scopeName) noexcept {
    SegmentScan patternScan(pattern);
    SegmentScan scopeScan(scopeName);
    std::string_view patternSegment;
    std::string_view scopeSegment;
    while (patternScan.next(patternSegment)) {
        // The pattern may not be longer than the scope: "string.quoted.double"
        // must not match "string.quoted".
        if (!scopeScan.next(scopeSegment)) return false;
        // Segments compare whole, never by prefix: "stri" must not match "string".
        if (patternSegment != "*" && patternSegment != scopeSegment) return false;
    }
    // Leftover scope segments are fine: "string.quoted" matches
    // "string.quoted.double.js".
    return true;
}

uint32_t scopeSegmentCount(std::string_view scope) noexcept {
    SegmentScan scan(scope);
    std::string_view segment;
    uint32_t count = 0;
    while (scan.next(segment)) ++count;
    return count;
}

MatchKey encodeMatchScore(const MatchScore& score) noexcept {
    if (score.pathLength == 0) return kNoMatch;
    const MatchKey path = static_cast<MatchKey>(score.pathLength > 0xFFFFu ? 0xFFFFu : score.pathLength);
    const MatchKey depth = static_cast<MatchKey>(score.matchDepth > 0xFFFFu ? 0xFFFFu : score.matchDepth);
    const MatchKey segments = static_cast<MatchKey>(score.segmentCount > 0xFFFFu ? 0xFFFFu : score.segmentCount);
    return (path << 48) | (depth << 32) | (segments << 16);
}

MatchScore decodeMatchKey(MatchKey key) noexcept {
    MatchScore score;
    score.pathLength = static_cast<uint32_t>((key >> 48) & 0xFFFFu);
    score.matchDepth = static_cast<uint32_t>((key >> 32) & 0xFFFFu);
    score.segmentCount = static_cast<uint32_t>((key >> 16) & 0xFFFFu);
    return score;
}

ScopeSelector ScopeSelector::parse(std::string_view text) {
    ScopeSelector selector;
    selector.source_.assign(text);

    const std::vector<Token> tokens = tokenize(text);
    size_t index = 0;
    std::vector<RawAlternative> raw = parseList(tokens, index, 0);

    selector.alternatives_.reserve(raw.size());
    for (RawAlternative& alternative : raw) {
        Alternative out;
        out.segmentCount = 0;
        for (const std::string& pattern : alternative.path) {
            out.segmentCount += scopeSegmentCount(pattern);
        }
        out.path = std::move(alternative.path);
        out.exclusions = std::move(alternative.exclusions);
        selector.alternatives_.push_back(std::move(out));
    }
    return selector;
}

MatchKey ScopeSelector::match(std::span<const std::string_view> scopeStack) const noexcept {
    MatchKey best = kNoMatch;
    for (const Alternative& alternative : alternatives_) {
        const int64_t depth = matchPathDeepest(alternative.path, scopeStack);
        if (depth < 0) continue;

        bool rejected = false;
        for (const std::vector<std::string>& clause : alternative.exclusions) {
            if (matchPathDeepest(clause, scopeStack) >= 0) {
                rejected = true;
                break;
            }
        }
        if (rejected) continue;

        MatchScore score;
        score.pathLength = clampField(alternative.path.size());
        score.matchDepth = clampField(static_cast<size_t>(depth) + 1u);
        score.segmentCount = alternative.segmentCount;
        const MatchKey key = encodeMatchScore(score);
        if (key > best) best = key;
    }
    return best;
}

bool ScopeSelector::matches(std::span<const std::string_view> scopeStack) const noexcept {
    return match(scopeStack) != kNoMatch;
}

std::span<const std::string> ScopeSelector::pathAt(size_t alternative) const noexcept {
    if (alternative >= alternatives_.size()) return {};
    return alternatives_[alternative].path;
}

size_t ScopeSelector::exclusionCountAt(size_t alternative) const noexcept {
    if (alternative >= alternatives_.size()) return 0;
    return alternatives_[alternative].exclusions.size();
}

}  // namespace ide::theme
