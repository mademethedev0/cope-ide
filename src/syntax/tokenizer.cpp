#include <ide/syntax/tokenizer.h>

#include <algorithm>
#include <functional>
#include <utility>

namespace ide::syntax {
namespace {

/// Byte offset of the next UTF-8 character boundary at or after pos+1. Used only
/// by the forced-advance safety net, so a malformed sequence just advances one
/// byte instead of corrupting anything.
size_t utf8Next(std::string_view text, size_t pos) noexcept {
    if (pos >= text.size()) return text.size();
    size_t next = pos + 1;
    while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0u) == 0x80u) ++next;
    return next;
}

uint64_t mixHash(uint64_t h, uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

constexpr bool isDigitChar(char c) noexcept { return c >= '0' && c <= '9'; }

std::string toLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string toUpperAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

State State::initial(bool isFirstLine) {
    State s;
    s.isFirstLine_ = isFirstLine;
    return s;
}

uint64_t State::hashFrame(const Frame& frame, uint64_t parentHash) {
    uint64_t h = mixHash(parentHash, frame.rule.key());
    h = mixHash(h, static_cast<uint64_t>(static_cast<uint32_t>(frame.nameScopes)));
    h = mixHash(h, static_cast<uint64_t>(static_cast<uint32_t>(frame.contentScopes)));
    h = mixHash(h, frame.beginCapturedEol ? 0x51ull : 0x52ull);
    if (frame.endPattern.has_value()) {
        h = mixHash(h, static_cast<uint64_t>(std::hash<std::string>{}(*frame.endPattern)));
        h = mixHash(h, 0x61ull);
    } else {
        h = mixHash(h, 0x62ull);
    }
    if (frame.whilePattern.has_value()) {
        h = mixHash(h, static_cast<uint64_t>(std::hash<std::string>{}(*frame.whilePattern)));
        h = mixHash(h, 0x71ull);
    } else {
        h = mixHash(h, 0x72ull);
    }
    return h;
}

uint64_t State::hash() const noexcept {
    const uint64_t base = (top_ == nullptr) ? 0ull : top_->hash;
    return isFirstLine_ ? mixHash(base, 0xF1B51ull) : base;
}

ScopeStackId State::scopes() const noexcept {
    return (top_ == nullptr) ? kRootScopeStack : top_->frame.nameScopes;
}

ScopeStackId State::contentScopes() const noexcept {
    return (top_ == nullptr) ? kRootScopeStack : top_->frame.contentScopes;
}

RuleRef State::rule() const noexcept {
    return (top_ == nullptr) ? RuleRef{} : top_->frame.rule;
}

const State::Frame* State::top() const noexcept {
    return (top_ == nullptr) ? nullptr : &top_->frame;
}

const State::Frame* State::frameAt(size_t index) const noexcept {
    const Node* node = top_.get();
    if (node == nullptr || index >= node->depth) return nullptr;
    while (node != nullptr && static_cast<size_t>(node->depth) - 1u > index) node = node->parent.get();
    return (node == nullptr) ? nullptr : &node->frame;
}

State State::push(const Frame& frame) const {
    auto node = std::make_shared<Node>();
    node->parent = top_;
    node->frame = frame;
    node->depth = static_cast<uint32_t>(depth() + 1u);
    node->hash = hashFrame(frame, (top_ == nullptr) ? 0ull : top_->hash);
    State s;
    s.top_ = std::move(node);
    s.isFirstLine_ = isFirstLine_;
    return s;
}

State State::pop() const {
    State s;
    s.isFirstLine_ = isFirstLine_;
    if (top_ != nullptr) s.top_ = top_->parent;
    return s;
}

State State::ancestor(size_t depth) const {
    State s = *this;
    while (s.top_ != nullptr && static_cast<size_t>(s.top_->depth) > depth) {
        s.top_ = s.top_->parent;
    }
    return s;
}

State State::withTop(const Frame& frame) const {
    if (top_ == nullptr) return push(frame);
    return pop().push(frame);
}

State State::withContentScopes(ScopeStackId scopes) const {
    if (top_ == nullptr) return *this;
    Frame frame = top_->frame;
    frame.contentScopes = scopes;
    return withTop(frame);
}

State State::withFirstLine(bool isFirstLine) const {
    State s = *this;
    s.isFirstLine_ = isFirstLine;
    return s;
}

bool State::operator==(const State& other) const {
    if (isFirstLine_ != other.isFirstLine_) return false;
    const Node* a = top_.get();
    const Node* b = other.top_.get();
    for (;;) {
        if (a == b) return true;  // structural sharing: identical tails
        if (a == nullptr || b == nullptr) return false;
        if (a->depth != b->depth) return false;
        if (a->hash != b->hash) return false;  // fast reject only
        if (!(a->frame == b->frame)) return false;
        a = a->parent.get();
        b = b->parent.get();
    }
}

// ---------------------------------------------------------------------------
// free helpers
// ---------------------------------------------------------------------------

std::string substituteBackreferences(std::string_view pattern, std::string_view line,
                                     const MatchResult& match) {
    std::string out;
    out.reserve(pattern.size() + 16u);
    size_t i = 0;
    while (i < pattern.size()) {
        const char c = pattern[i];
        if (c != '\\') {
            out.push_back(c);
            ++i;
            continue;
        }
        if (i + 1 >= pattern.size()) {
            out.push_back(c);
            ++i;
            continue;
        }
        const char d = pattern[i + 1];
        if (!isDigitChar(d)) {
            // Keep the escape intact. This is what makes "\\1" (escaped
            // backslash, literal 1) different from "\1" (backreference).
            out.push_back(c);
            out.push_back(d);
            i += 2;
            continue;
        }
        size_t j = i + 1;
        int index = 0;
        while (j < pattern.size() && isDigitChar(pattern[j]) && index < 100) {
            index = index * 10 + (pattern[j] - '0');
            ++j;
        }
        const Capture* capture = match.capture(index);
        if (capture != nullptr && capture->matched() && capture->begin <= capture->end &&
            capture->end <= line.size()) {
            out += escapeRegexLiteral(line.substr(capture->begin, capture->end - capture->begin));
        }
        i = j;
    }
    return out;
}

std::string substituteScopeCaptures(std::string_view templateText, std::string_view line,
                                    const MatchResult& match) {
    if (templateText.find('$') == std::string_view::npos) return std::string(templateText);
    std::string out;
    out.reserve(templateText.size() + 16u);
    size_t i = 0;
    while (i < templateText.size()) {
        if (templateText[i] != '$') {
            out.push_back(templateText[i]);
            ++i;
            continue;
        }
        size_t j = i + 1;
        bool braced = false;
        if (j < templateText.size() && templateText[j] == '{') {
            braced = true;
            ++j;
        }
        const size_t digitsStart = j;
        int index = 0;
        while (j < templateText.size() && isDigitChar(templateText[j]) && index < 100) {
            index = index * 10 + (templateText[j] - '0');
            ++j;
        }
        if (j == digitsStart) {  // not a capture reference
            out.push_back('$');
            ++i;
            continue;
        }
        std::string transform;
        if (braced) {
            if (j < templateText.size() && templateText[j] == ':') {
                const size_t close = templateText.find('}', j);
                if (close == std::string_view::npos) {
                    out.push_back('$');
                    ++i;
                    continue;
                }
                transform = std::string(templateText.substr(j + 1, close - j - 1));
                j = close;
            }
            if (j >= templateText.size() || templateText[j] != '}') {
                out.push_back('$');
                ++i;
                continue;
            }
            ++j;
        }
        const Capture* capture = match.capture(index);
        if (capture != nullptr && capture->matched() && capture->begin <= capture->end &&
            capture->end <= line.size()) {
            std::string text(line.substr(capture->begin, capture->end - capture->begin));
            if (transform == "/downcase") {
                text = toLowerAscii(std::move(text));
            } else if (transform == "/upcase") {
                text = toUpperAscii(std::move(text));
            }
            out += text;
        }
        i = j;
    }
    return out;
}

std::string rewriteAnchors(std::string_view pattern, bool allowA, bool allowG, bool* sawA,
                           bool* sawG) {
    if (sawA != nullptr) *sawA = false;
    if (sawG != nullptr) *sawG = false;
    std::string out;
    out.reserve(pattern.size() + 16u);
    bool inClass = false;
    size_t i = 0;
    while (i < pattern.size()) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            const char d = pattern[i + 1];
            if (!inClass && d == 'A') {
                if (sawA != nullptr) *sawA = true;
                if (allowA) {
                    out += "\\A";
                } else {
                    out.append(kNeverMatchAssertion);
                }
                i += 2;
                continue;
            }
            if (!inClass && d == 'G') {
                if (sawG != nullptr) *sawG = true;
                if (allowG) {
                    out += "\\G";
                } else {
                    out.append(kNeverMatchAssertion);
                }
                i += 2;
                continue;
            }
            out.push_back(c);
            out.push_back(d);
            i += 2;
            continue;
        }
        if (!inClass && c == '[') {
            inClass = true;
            out.push_back(c);
            ++i;
            if (i < pattern.size() && pattern[i] == '^') {
                out.push_back(pattern[i]);
                ++i;
            }
            if (i < pattern.size() && pattern[i] == ']') {
                out.push_back(pattern[i]);
                ++i;
            }
            continue;
        }
        if (inClass && c == '[' && i + 1 < pattern.size() && pattern[i + 1] == ':') {
            const size_t close = pattern.find(":]", i + 2);
            if (close != std::string_view::npos) {
                out.append(pattern.substr(i, close + 2u - i));
                i = close + 2u;
                continue;
            }
        }
        if (inClass && c == ']') inClass = false;
        out.push_back(c);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

Tokenizer::Tokenizer(GrammarRegistry& registry, IRegexEngine& engine, GrammarId rootGrammar)
    : registry_(registry), engine_(engine), rootGrammar_(rootGrammar) {
    frameAnchorPos_.fill(kNoPosition);
}

void Tokenizer::setTrace(TraceSink sink) {
    trace_ = std::move(sink);
    tracing_ = static_cast<bool>(trace_);
}

const Rule* Tokenizer::ruleFor(RuleRef ref) const {
    const Grammar* grammar = registry_.grammarById(ref.grammar);
    if (grammar == nullptr || !grammar->validRule(ref.rule)) return nullptr;
    return &grammar->rule(ref.rule);
}

RuleRef Tokenizer::rootRuleRef() const {
    const Grammar* grammar = registry_.grammarById(rootGrammar_);
    if (grammar == nullptr) return RuleRef{};
    return RuleRef{rootGrammar_, grammar->rootRule()};
}

State Tokenizer::initialState() {
    State state = State::initial(true);
    const Grammar* grammar = registry_.grammarById(rootGrammar_);
    if (grammar == nullptr || !grammar->validRule(grammar->rootRule())) return state;
    State::Frame frame;
    frame.rule = RuleRef{rootGrammar_, grammar->rootRule()};
    frame.nameScopes = scopeTable_.push(kRootScopeStack, grammar->scopeName());
    frame.contentScopes = frame.nameScopes;
    return state.push(frame);
}

void Tokenizer::expandInto(RuleRef ref, std::vector<RuleRef>& out, std::vector<uint64_t>& visited,
                           int depth) {
    if (!ref.valid() || depth > kMaxIncludeExpansionDepth) return;
    if (out.size() >= kMaxPatternListSize) return;
    const Rule* rule = ruleFor(ref);
    if (rule == nullptr) return;
    switch (rule->kind) {
        case RuleKind::Include: {
            const uint64_t key = ref.key();
            for (uint64_t seen : visited) {
                if (seen == key) return;
            }
            visited.push_back(key);
            const RuleRef target = registry_.resolveInclude(ref.grammar, ref.rule, rootGrammar_);
            if (target.valid()) expandInto(target, out, visited, depth + 1);
            return;
        }
        case RuleKind::Container: {
            const uint64_t key = ref.key();
            for (uint64_t seen : visited) {
                if (seen == key) return;
            }
            visited.push_back(key);
            for (RuleId child : rule->patterns) {
                expandInto(RuleRef{ref.grammar, child}, out, visited, depth + 1);
            }
            return;
        }
        case RuleKind::Match:
        case RuleKind::BeginEnd:
        case RuleKind::BeginWhile:
            // Terminal candidates. Deduplicated so one position never scans the
            // same pattern twice, and so the earliest (highest priority)
            // occurrence keeps its place in the list.
            for (const RuleRef& existing : out) {
                if (existing == ref) return;
            }
            out.push_back(ref);
            return;
    }
}

const std::vector<RuleRef>& Tokenizer::flattenPatterns(RuleRef container) {
    static const std::vector<RuleRef> kEmpty;
    if (!container.valid()) return kEmpty;
    const uint64_t key = container.key();
    const auto cached = flattenCache_.find(key);
    if (cached != flattenCache_.end()) return cached->second;
    std::vector<RuleRef> out;
    const Rule* rule = ruleFor(container);
    if (rule != nullptr) {
        std::vector<uint64_t> visited;
        visited.push_back(key);  // the container itself must not be re-expanded
        for (RuleId child : rule->patterns) {
            expandInto(RuleRef{container.grammar, child}, out, visited, 1);
        }
    }
    return flattenCache_.emplace(key, std::move(out)).first->second;
}

std::shared_ptr<IRegex> Tokenizer::variantFrom(VariantSet& set, const std::string& source,
                                               bool allowA, bool allowG) {
    const bool a = set.hasA ? allowA : true;
    const bool g = set.hasG ? allowG : true;
    const size_t index = (a ? 1u : 0u) | (g ? 2u : 0u);
    if (set.attempted[index]) return set.variants[index];
    set.attempted[index] = true;
    std::shared_ptr<IRegex> regex;
    if (a && g) {
        regex = engine_.compile(source);
    } else {
        const std::string rewritten = rewriteAnchors(source, a, g);
        regex = engine_.compile(rewritten);
    }
    if (regex == nullptr) {
        ++stats_.regexCompileFailures;
        if (tracing_) {
            const std::string reason = engine_.lastError();
            traceNotice(TraceEventKind::Notice, 0, reason);
        }
    }
    set.variants[index] = regex;
    return regex;
}

std::shared_ptr<IRegex> Tokenizer::regexForSource(const std::string& source, bool allowA,
                                                  bool allowG) {
    if (source.empty()) return nullptr;  // an absent pattern can never match
    auto it = patternCache_.find(source);
    if (it == patternCache_.end()) {
        auto set = std::make_shared<VariantSet>();
        (void)rewriteAnchors(source, true, true, &set->hasA, &set->hasG);
        it = patternCache_.emplace(source, std::move(set)).first;
    }
    return variantFrom(*it->second, source, allowA, allowG);
}

std::shared_ptr<IRegex> Tokenizer::regexForRule(RuleRef ref, const Rule& rule, bool allowA,
                                                bool allowG) {
    const std::string& source = rule.hasMatch ? rule.match : rule.begin;
    if (source.empty()) return nullptr;
    const uint64_t key = ref.key();
    auto it = ruleRegexCache_.find(key);
    if (it == ruleRegexCache_.end()) {
        auto set = std::make_shared<VariantSet>();
        (void)rewriteAnchors(source, true, true, &set->hasA, &set->hasG);
        it = ruleRegexCache_.emplace(key, std::move(set)).first;
    }
    return variantFrom(*it->second, source, allowA, allowG);
}

const MatchResult* Tokenizer::searchCached(IRegex* regex, std::string_view line, size_t position) {
    if (regex == nullptr) return nullptr;
    const MatchCacheKey key{regex, position, line.size()};
    const auto it = matchCache_.find(key);
    if (it != matchCache_.end()) {
        ++stats_.matchCacheHits;
        return it->second.has_value() ? &it->second.value() : nullptr;
    }
    ++stats_.matchCacheMisses;
    std::optional<MatchResult> result = regex->search(line, position);
    if (matchCache_.size() < kMaxMatchCacheEntries) {
        const auto inserted = matchCache_.emplace(key, std::move(result));
        return inserted.first->second.has_value() ? &inserted.first->second.value() : nullptr;
    }
    scratchMatch_ = std::move(result);
    return scratchMatch_.has_value() ? &scratchMatch_.value() : nullptr;
}

Tokenizer::MatchOutcome Tokenizer::matchRule(std::string_view line, size_t position,
                                             size_t anchorPos, const State& stack,
                                             bool isFirstLine) {
    MatchOutcome best;
    const bool allowA = isFirstLine;
    const bool allowG = (position == anchorPos);

    RuleRef containerRef = stack.rule();
    if (!containerRef.valid()) containerRef = rootRuleRef();
    const Rule* container = ruleFor(containerRef);
    if (container == nullptr) return best;

    // The enclosing begin/end rule's end pattern competes with its sub-patterns.
    std::shared_ptr<IRegex> endRegex;
    bool endFirst = false;
    if (container->kind == RuleKind::BeginEnd && stack.top() != nullptr) {
        const State::Frame& frame = *stack.top();
        const std::string& endSource =
            frame.endPattern.has_value() ? *frame.endPattern : container->end;
        endRegex = regexForSource(endSource, allowA, allowG);
        endFirst = !container->applyEndPatternLast;
        if (endRegex == nullptr) {
            // The rule can never close. Closing it here loses highlighting for
            // this construct; *not* closing it would swallow the rest of the
            // file, which is how previous attempts produced "everything is a
            // string" bugs. Close it, count it, and keep going.
            ++stats_.rulesDisabledByEndPattern;
            if (tracing_) {
                traceNotice(TraceEventKind::Notice, position,
                            "end pattern unusable; closing the rule at this position");
            }
            best.found = true;
            best.isEnd = true;
            best.rule = containerRef;
            best.candidateIndex = kEndPatternCandidateIndex;
            best.match.begin = position;
            best.match.end = position;
            best.match.captures.push_back(Capture{0, position, position});
            return best;
        }
    }

    const auto tryEndPattern = [&]() {
        const MatchResult* m = searchCached(endRegex.get(), line, position);
        if (tracing_) {
            traceRule(TraceEventKind::RuleTried, position, containerRef, m != nullptr,
                      (m != nullptr) ? m->begin : 0u, (m != nullptr) ? m->end : 0u,
                      kEndPatternCandidateIndex, endRegex->pattern());
        }
        if (m == nullptr) return;
        if (!best.found || m->begin < best.match.begin) {
            best.found = true;
            best.isEnd = true;
            best.rule = containerRef;
            best.candidateIndex = kEndPatternCandidateIndex;
            best.match = *m;
        }
    };

    if (endRegex != nullptr && endFirst) {
        tryEndPattern();
        // A match exactly at the scan position cannot be beaten by anything.
        if (best.found && best.match.begin <= position) return best;
    }

    const std::vector<RuleRef>& candidates = flattenPatterns(containerRef);
    for (size_t i = 0; i < candidates.size(); ++i) {
        const RuleRef ref = candidates[i];
        const Rule* rule = ruleFor(ref);
        if (rule == nullptr) continue;
        const std::shared_ptr<IRegex> regex = regexForRule(ref, *rule, allowA, allowG);
        if (regex == nullptr) continue;
        const MatchResult* m = searchCached(regex.get(), line, position);
        if (tracing_) {
            traceRule(TraceEventKind::RuleTried, position, ref, m != nullptr,
                      (m != nullptr) ? m->begin : 0u, (m != nullptr) ? m->end : 0u,
                      static_cast<int>(i), regex->pattern());
        }
        if (m == nullptr) continue;
        if (!best.found || m->begin < best.match.begin) {
            best.found = true;
            best.isEnd = false;
            best.rule = ref;
            best.candidateIndex = static_cast<int>(i);
            best.match = *m;
            if (best.match.begin <= position) break;  // earliest possible
        }
    }

    if (endRegex != nullptr && !endFirst && !(best.found && best.match.begin <= position)) {
        tryEndPattern();
    }
    return best;
}

void Tokenizer::handleCaptures(std::string_view line, const State& stack, ScopeStackId baseScopes,
                               GrammarId owner, const std::vector<CaptureSpec>& captures,
                               const MatchResult& match, LineTokens& tokens,
                               TokenizeResult& result, bool isFirstLine, int captureDepth) {
    if (captures.empty()) return;
    const size_t count = std::min(captures.size(), match.captures.size());
    if (count == 0) return;

    struct LocalScope {
        ScopeStackId scopes = kRootScopeStack;
        size_t endPos = 0;
    };
    std::vector<LocalScope> localStack;
    const size_t maxEnd = match.end;

    for (size_t i = 0; i < count; ++i) {
        const CaptureSpec& spec = captures[i];
        if (spec.isNull()) continue;
        const Capture& capture = match.captures[i];
        if (!capture.matched()) continue;
        if (capture.end <= capture.begin) continue;  // empty captures decorate nothing
        if (capture.begin > maxEnd) break;           // outside the match: done

        while (!localStack.empty() && localStack.back().endPos <= capture.begin) {
            tokens.produce(localStack.back().scopes, localStack.back().endPos);
            localStack.pop_back();
        }
        const ScopeStackId base = localStack.empty() ? baseScopes : localStack.back().scopes;
        tokens.produce(base, capture.begin);

        if (spec.patternsRule != kInvalidRuleId && captureDepth < kMaxCaptureRecursionDepth) {
            // Re-tokenize the captured text with the capture's own patterns.
            const std::string name = substituteScopeCaptures(spec.name, line, match);
            const ScopeStackId nameScopes = scopeTable_.push(base, name);
            const std::string contentName = substituteScopeCaptures(spec.contentName, line, match);
            State::Frame frame;
            frame.rule = RuleRef{owner, spec.patternsRule};
            frame.nameScopes = nameScopes;
            frame.contentScopes = scopeTable_.push(nameScopes, contentName);
            const State sub = stack.push(frame);
            tokenizeString(line.substr(0, capture.end), capture.begin, sub,
                           isFirstLine && capture.begin == 0, false, tokens, result,
                           captureDepth + 1);
            continue;
        }
        if (!spec.name.empty()) {
            const std::string name = substituteScopeCaptures(spec.name, line, match);
            localStack.push_back(LocalScope{scopeTable_.push(base, name), capture.end});
        }
    }
    while (!localStack.empty()) {
        tokens.produce(localStack.back().scopes, localStack.back().endPos);
        localStack.pop_back();
    }
}

Tokenizer::WhileOutcome Tokenizer::checkWhileConditions(std::string_view line, State stack,
                                                        LineTokens& tokens, TokenizeResult& result,
                                                        bool isFirstLine) {
    WhileOutcome out;
    out.linePos = 0;
    out.isFirstLine = isFirstLine;
    const State::Frame* topFrame = stack.top();
    out.anchorPos = (topFrame != nullptr && topFrame->beginCapturedEol) ? 0u : kNoPosition;

    // Bottom-up, exactly like vscode-textmate: the outermost while rule decides
    // first, and the first failure pops everything above it.
    std::vector<size_t> whileFrames;
    const size_t depth = stack.depth();
    for (size_t i = 0; i < depth; ++i) {
        const State::Frame* frame = stack.frameAt(i);
        if (frame == nullptr) continue;
        const Rule* rule = ruleFor(frame->rule);
        if (rule != nullptr && rule->kind == RuleKind::BeginWhile) whileFrames.push_back(i);
    }

    for (const size_t index : whileFrames) {
        const State::Frame* frame = stack.frameAt(index);
        if (frame == nullptr) break;
        const Rule* rule = ruleFor(frame->rule);
        if (rule == nullptr) break;
        const std::string& source =
            frame->whilePattern.has_value() ? *frame->whilePattern : rule->whilePattern;
        const std::shared_ptr<IRegex> regex =
            regexForSource(source, out.isFirstLine, out.linePos == out.anchorPos);
        const MatchResult* found =
            (regex == nullptr) ? nullptr : searchCached(regex.get(), line, out.linePos);
        if (tracing_) {
            traceRule(TraceEventKind::WhileCheck, out.linePos, frame->rule, found != nullptr,
                      (found != nullptr) ? found->begin : 0u, (found != nullptr) ? found->end : 0u,
                      kEndPatternCandidateIndex,
                      (regex == nullptr) ? std::string_view{} : regex->pattern());
        }
        if (found == nullptr) {
            stack = stack.ancestor(index);  // pop this frame and everything above it
            if (tracing_) {
                traceNotice(TraceEventKind::Pop, out.linePos, "while condition failed");
            }
            break;
        }
        const MatchResult matched = *found;  // copy: further searches may reuse the scratch slot
        const State frameStack = stack.ancestor(index + 1u);
        tokens.produce(frameStack.contentScopes(), matched.begin);
        handleCaptures(line, frameStack, frameStack.contentScopes(), frame->rule.grammar,
                       rule->whileCaptures, matched, tokens, result, out.isFirstLine, 0);
        tokens.produce(frameStack.contentScopes(), matched.end);
        out.anchorPos = matched.end;
        if (matched.end > out.linePos) {
            out.linePos = matched.end;
            out.isFirstLine = false;
        }
    }
    out.stack = std::move(stack);
    return out;
}

State Tokenizer::tokenizeString(std::string_view line, size_t startPos, State stack,
                                bool isFirstLine, bool checkWhile, LineTokens& tokens,
                                TokenizeResult& result, int captureDepth) {
    size_t linePos = startPos;
    size_t anchorPos = kNoPosition;

    if (checkWhile && !stack.empty()) {
        WhileOutcome outcome = checkWhileConditions(line, std::move(stack), tokens, result,
                                                    isFirstLine);
        stack = std::move(outcome.stack);
        linePos = outcome.linePos;
        anchorPos = outcome.anchorPos;
        isFirstLine = outcome.isFirstLine;
    }

    size_t iterations = 0;
    size_t zeroWidthStreak = 0;
    for (;;) {
        if (++iterations > kMaxIterationsPerLine) {
            result.hitIterationLimit = true;
            ++stats_.iterationLimitHits;
            if (tracing_) traceNotice(TraceEventKind::Limit, linePos, "kMaxIterationsPerLine");
            break;
        }
        const size_t posBefore = linePos;
        const MatchOutcome outcome = matchRule(line, linePos, anchorPos, stack, isFirstLine);
        if (!outcome.found) {
            if (tracing_) traceNotice(TraceEventKind::NoMatch, linePos, "no rule matched");
            break;
        }
        if (tracing_) {
            traceRule(TraceEventKind::RuleWon, linePos, outcome.rule, true, outcome.match.begin,
                      outcome.match.end, outcome.candidateIndex, {});
        }

        if (outcome.isEnd) {
            const Rule* popped = ruleFor(outcome.rule);
            tokens.produce(stack.contentScopes(), outcome.match.begin);
            // The end match itself is outside the rule's content, so it carries
            // the rule's own scopes and not the contentName scopes.
            const ScopeStackId endScopes = stack.scopes();
            if (popped != nullptr) {
                handleCaptures(line, stack, endScopes, outcome.rule.grammar, popped->endCaptures,
                               outcome.match, tokens, result, isFirstLine, captureDepth);
            }
            tokens.produce(endScopes, outcome.match.end);
            const size_t frameIndex = (stack.depth() > 0u) ? stack.depth() - 1u : 0u;
            stack = stack.pop();
            anchorPos = (frameIndex < frameAnchorPos_.size()) ? frameAnchorPos_[frameIndex]
                                                             : kNoPosition;
            if (tracing_) {
                traceRule(TraceEventKind::Pop, outcome.match.end, outcome.rule, true,
                          outcome.match.begin, outcome.match.end, outcome.candidateIndex, {});
            }
            linePos = outcome.match.end;
        } else {
            const Rule* rule = ruleFor(outcome.rule);
            if (rule == nullptr) break;  // cannot happen: matchRule resolved it
            tokens.produce(stack.contentScopes(), outcome.match.begin);

            const std::string scopeName = substituteScopeCaptures(rule->name, line, outcome.match);
            const ScopeStackId nameScopes = scopeTable_.push(stack.contentScopes(), scopeName);

            const bool wantsPush =
                (rule->kind == RuleKind::BeginEnd || rule->kind == RuleKind::BeginWhile);
            bool depthLimited = false;
            if (wantsPush && stack.depth() >= kMaxStackDepth) {
                depthLimited = true;
                result.hitDepthLimit = true;
                ++stats_.depthLimitHits;
                if (tracing_) traceNotice(TraceEventKind::Limit, linePos, "kMaxStackDepth");
            }

            // Resolve (and validate) the end/while pattern *before* entering the
            // rule: a rule that can never close must not be entered at all.
            std::optional<std::string> endOverride;
            std::optional<std::string> whileOverride;
            bool patternUsable = true;
            if (wantsPush && !depthLimited) {
                if (rule->kind == RuleKind::BeginEnd) {
                    std::string endSource =
                        rule->endHasBackrefs
                            ? substituteBackreferences(rule->end, line, outcome.match)
                            : rule->end;
                    if (!rule->hasEnd || regexForSource(endSource, true, true) == nullptr) {
                        patternUsable = false;
                    } else if (rule->endHasBackrefs) {
                        endOverride = std::move(endSource);
                    }
                } else {
                    std::string whileSource =
                        rule->whileHasBackrefs
                            ? substituteBackreferences(rule->whilePattern, line, outcome.match)
                            : rule->whilePattern;
                    if (!rule->hasWhile || regexForSource(whileSource, true, true) == nullptr) {
                        patternUsable = false;
                    } else if (rule->whileHasBackrefs) {
                        whileOverride = std::move(whileSource);
                    }
                }
                if (!patternUsable) {
                    ++stats_.rulesDisabledByEndPattern;
                    if (tracing_) {
                        traceNotice(TraceEventKind::Notice, linePos,
                                    "rule not entered: end/while pattern unusable");
                    }
                }
            }
            const bool enter = wantsPush && !depthLimited && patternUsable;

            if (!enter) {
                // Behaves exactly like a match rule: the scopes cover the matched
                // text only and nothing is pushed onto the stack.
                const std::vector<CaptureSpec>& caps =
                    (rule->kind == RuleKind::Match) ? rule->captures : rule->beginCaptures;
                handleCaptures(line, stack, nameScopes, outcome.rule.grammar, caps, outcome.match,
                               tokens, result, isFirstLine, captureDepth);
                tokens.produce(nameScopes, outcome.match.end);
            } else {
                State::Frame frame;
                frame.rule = outcome.rule;
                frame.nameScopes = nameScopes;
                frame.contentScopes = nameScopes;
                frame.beginCapturedEol = (outcome.match.end == line.size());
                const State pushed = stack.push(frame);
                if (tracing_) {
                    traceRule(TraceEventKind::Push, outcome.match.begin, outcome.rule, true,
                              outcome.match.begin, outcome.match.end, outcome.candidateIndex, {});
                }
                handleCaptures(line, pushed, nameScopes, outcome.rule.grammar,
                               rule->beginCaptures, outcome.match, tokens, result, isFirstLine,
                               captureDepth);
                tokens.produce(nameScopes, outcome.match.end);
                const size_t frameIndex = pushed.depth() - 1u;
                if (frameIndex < frameAnchorPos_.size()) frameAnchorPos_[frameIndex] = anchorPos;
                anchorPos = outcome.match.end;
                State::Frame updated = frame;
                const std::string contentName =
                    substituteScopeCaptures(rule->contentName, line, outcome.match);
                updated.contentScopes = scopeTable_.push(nameScopes, contentName);
                updated.endPattern = std::move(endOverride);
                updated.whilePattern = std::move(whileOverride);
                stack = pushed.withTop(updated);
            }
            linePos = outcome.match.end;
        }

        if (linePos > line.size()) linePos = line.size();
        if (linePos == posBefore) {
            ++zeroWidthStreak;
            if (zeroWidthStreak >= kMaxZeroWidthMatchesPerPosition) {
                const size_t next = utf8Next(line, linePos);
                if (next > linePos) {
                    linePos = next;
                    ++result.forcedAdvances;
                    ++stats_.forcedAdvances;
                    zeroWidthStreak = 0;
                    if (tracing_) {
                        traceNotice(TraceEventKind::Limit, linePos,
                                    "kMaxZeroWidthMatchesPerPosition: forced advance");
                    }
                } else {
                    if (tracing_) {
                        traceNotice(TraceEventKind::Limit, linePos,
                                    "zero-width match loop at end of line");
                    }
                    break;
                }
            }
        } else {
            zeroWidthStreak = 0;
        }
    }

    tokens.produce(stack.contentScopes(), line.size());
    return stack;
}

TokenizeResult Tokenizer::tokenizeLine(std::string_view line, const State& startState) {
    TokenizeResult result;
    ++stats_.linesTokenized;

    if (line.size() > kMaxLineLength) {
        ++stats_.linesBailedOnLength;
        result.bailedOnLineLength = true;
        result.tokens.push_back(TokenSpan{0u, line.size(), startState.contentScopes()});
        result.endState = startState.withFirstLine(false);
        if (tracing_) traceNotice(TraceEventKind::Limit, 0, "kMaxLineLength");
        return result;
    }

    matchCache_.clear();
    scratchMatch_.reset();
    frameAnchorPos_.fill(kNoPosition);

    LineTokens tokens;
    tokens.out = &result.tokens;
    tokens.lastEnd = 0;

    if (tracing_) traceNotice(TraceEventKind::LineStart, 0, "line start");

    const State endState =
        tokenizeString(line, 0, startState, startState.isFirstLine(), true, tokens, result, 0);
    result.endState = endState.withFirstLine(false);
    return result;
}

void Tokenizer::emitTrace(const TraceEvent& event) const {
    if (trace_) trace_(event);
}

void Tokenizer::traceRule(TraceEventKind kind, size_t position, RuleRef ref, bool matched,
                          size_t begin, size_t end, int candidateIndex,
                          std::string_view pattern) const {
    TraceEvent event;
    event.kind = kind;
    event.position = position;
    event.rule = ref;
    event.matched = matched;
    event.matchBegin = begin;
    event.matchEnd = end;
    event.candidateIndex = candidateIndex;
    event.pattern = pattern;
    const Rule* rule = ruleFor(ref);
    if (rule != nullptr) event.detail = rule->debugName;
    emitTrace(event);
}

void Tokenizer::traceNotice(TraceEventKind kind, size_t position, std::string_view detail) const {
    TraceEvent event;
    event.kind = kind;
    event.position = position;
    event.detail = detail;
    emitTrace(event);
}

}  // namespace ide::syntax
