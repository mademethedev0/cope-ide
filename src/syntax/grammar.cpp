#include <ide/syntax/grammar.h>

#include <utility>

namespace ide::syntax {
namespace {

/// Highest capture group index the loader will materialise. Grammars use group
/// numbers up to ~20; a bogus key like "99999999" must not allocate.
constexpr int kMaxCaptureIndex = 255;

const Rule& emptyRule() {
    static const Rule kEmpty;
    return kEmpty;
}

int parseCaptureIndex(std::string_view key) {
    if (key.empty() || key.size() > 3) return -1;
    int value = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

// Mutually recursive over the JSON tree. The recursion depth is bounded by
// kMaxRuleNestingDepth and, before that, by the JSON parser's own limit.
RuleId parseRule(const json::Value& v, Grammar& g, std::string debugName, int depth);

std::vector<RuleId> parsePatternList(const json::Value& v, Grammar& g,
                                     const std::string& debugBase, int depth) {
    std::vector<RuleId> out;
    if (!v.isArray()) return out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        std::string name = debugBase;
        name += '[';
        name += std::to_string(i);
        name += ']';
        const RuleId id = parseRule(v.at(i), g, std::move(name), depth + 1);
        if (id != kInvalidRuleId) out.push_back(id);
    }
    return out;
}

std::vector<CaptureSpec> parseCaptures(const json::Value& v, Grammar& g,
                                       const std::string& debugBase, int depth) {
    std::vector<CaptureSpec> out;
    if (!v.isObject()) return out;
    int maxIndex = -1;
    for (size_t k = 0; k < v.memberCount(); ++k) {
        const int index = parseCaptureIndex(v.keyAt(k));
        if (index > maxIndex && index <= kMaxCaptureIndex) maxIndex = index;
    }
    if (maxIndex < 0) return out;
    out.resize(static_cast<size_t>(maxIndex) + 1u);
    for (size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<int>(i);
    for (size_t k = 0; k < v.memberCount(); ++k) {
        const int index = parseCaptureIndex(v.keyAt(k));
        if (index < 0 || index > maxIndex) continue;
        const json::Value& spec = v.valueAt(k);
        CaptureSpec& c = out[static_cast<size_t>(index)];
        const json::Value* name = spec.find("name");
        if (name != nullptr && name->isString()) c.name = name->string();
        const json::Value* contentName = spec.find("contentName");
        if (contentName != nullptr && contentName->isString()) c.contentName = contentName->string();
        const json::Value& patterns = spec["patterns"];
        if (patterns.isArray() && patterns.size() > 0) {
            std::string childBase = debugBase;
            childBase += '.';
            childBase += v.keyAt(k);
            Rule container;
            container.kind = RuleKind::Container;
            container.debugName = childBase;
            container.patterns = parsePatternList(patterns, g, childBase, depth + 1);
            c.patternsRule = g.internRule(std::move(container));
        }
    }
    return out;
}

RuleId parseRule(const json::Value& v, Grammar& g, std::string debugName, int depth) {
    if (!v.isObject() || depth > kMaxRuleNestingDepth) return kInvalidRuleId;

    Rule r;
    r.debugName = std::move(debugName);

    const json::Value* include = v.find("include");
    if (include != nullptr && include->isString()) {
        r.kind = RuleKind::Include;
        r.include = parseIncludeRef(include->string());
        return g.internRule(std::move(r));
    }

    const json::Value* name = v.find("name");
    if (name != nullptr && name->isString()) r.name = name->string();
    const json::Value* contentName = v.find("contentName");
    if (contentName != nullptr && contentName->isString()) r.contentName = contentName->string();

    const json::Value* match = v.find("match");
    if (match != nullptr && match->isString()) {
        r.hasMatch = true;
        r.match = match->string();
    }
    const json::Value* begin = v.find("begin");
    if (begin != nullptr && begin->isString()) {
        r.hasBegin = true;
        r.begin = begin->string();
    }
    const json::Value* end = v.find("end");
    if (end != nullptr && end->isString()) {
        r.hasEnd = true;
        r.end = end->string();
    }
    const json::Value* whileValue = v.find("while");
    if (whileValue != nullptr && whileValue->isString()) {
        r.hasWhile = true;
        r.whilePattern = whileValue->string();
    }

    if (r.hasBegin) {
        r.kind = r.hasWhile ? RuleKind::BeginWhile : RuleKind::BeginEnd;
    } else if (r.hasMatch) {
        r.kind = RuleKind::Match;
    } else {
        r.kind = RuleKind::Container;
    }

    r.endHasBackrefs = r.hasEnd && patternHasBackrefs(r.end);
    r.whileHasBackrefs = r.hasWhile && patternHasBackrefs(r.whilePattern);

    const json::Value* applyLast = v.find("applyEndPatternLast");
    if (applyLast != nullptr) {
        r.applyEndPatternLast = applyLast->boolean(false) || applyLast->integer(0) != 0;
    }

    r.captures = parseCaptures(v["captures"], g, r.debugName + ".captures", depth);
    r.beginCaptures = parseCaptures(v["beginCaptures"], g, r.debugName + ".beginCaptures", depth);
    r.endCaptures = parseCaptures(v["endCaptures"], g, r.debugName + ".endCaptures", depth);
    r.whileCaptures = parseCaptures(v["whileCaptures"], g, r.debugName + ".whileCaptures", depth);
    // TextMate compatibility: a begin/end rule with only "captures" applies them
    // to both the begin and the end match.
    if (r.kind != RuleKind::Match && !r.captures.empty()) {
        if (r.beginCaptures.empty()) r.beginCaptures = r.captures;
        if (r.endCaptures.empty() && r.kind == RuleKind::BeginEnd) r.endCaptures = r.captures;
        if (r.whileCaptures.empty() && r.kind == RuleKind::BeginWhile) r.whileCaptures = r.captures;
    }

    r.patterns = parsePatternList(v["patterns"], g, r.debugName + ".patterns", depth);
    return g.internRule(std::move(r));
}

}  // namespace

const Rule& Grammar::rule(RuleId id) const noexcept {
    if (!validRule(id)) return emptyRule();
    return rules_[static_cast<size_t>(id)];
}

RuleId Grammar::repositoryRule(std::string_view name) const noexcept {
    const auto it = repository_.find(std::string(name));
    return (it == repository_.end()) ? kInvalidRuleId : it->second;
}

RuleId Grammar::internRule(Rule r) {
    const RuleId id = static_cast<RuleId>(rules_.size());
    r.id = id;
    rules_.push_back(std::move(r));
    return id;
}

Rule* Grammar::mutableRule(RuleId id) noexcept {
    if (!validRule(id)) return nullptr;
    return &rules_[static_cast<size_t>(id)];
}

void Grammar::noteExternalScope(std::string scopeName) {
    for (const std::string& existing : externalScopeRefs_) {
        if (existing == scopeName) return;
    }
    externalScopeRefs_.push_back(std::move(scopeName));
}

IncludeRef parseIncludeRef(std::string_view raw) {
    IncludeRef ref;
    ref.raw = std::string(raw);
    if (raw.empty()) {
        ref.kind = IncludeKind::Invalid;
        return ref;
    }
    if (raw == "$self") {
        ref.kind = IncludeKind::Self;
        return ref;
    }
    if (raw == "$base") {
        ref.kind = IncludeKind::Base;
        return ref;
    }
    if (raw.front() == '#') {
        const std::string_view name = raw.substr(1);
        if (name.empty()) {
            ref.kind = IncludeKind::Invalid;
            return ref;
        }
        ref.kind = IncludeKind::Repository;
        ref.ruleName = std::string(name);
        return ref;
    }
    const size_t hash = raw.find('#');
    if (hash == std::string_view::npos) {
        ref.kind = IncludeKind::ExternalGrammar;
        ref.scopeName = std::string(raw);
        return ref;
    }
    const std::string_view scope = raw.substr(0, hash);
    const std::string_view name = raw.substr(hash + 1);
    if (scope.empty() || name.empty()) {
        ref.kind = IncludeKind::Invalid;
        return ref;
    }
    if (scope == "$self" || scope == "$base") {
        // "$self#foo" means this grammar's repository entry.
        ref.kind = IncludeKind::Repository;
        ref.ruleName = std::string(name);
        return ref;
    }
    ref.kind = IncludeKind::ExternalRule;
    ref.scopeName = std::string(scope);
    ref.ruleName = std::string(name);
    return ref;
}

bool patternHasBackrefs(std::string_view pattern) {
    for (size_t i = 0; i + 1 < pattern.size(); ++i) {
        if (pattern[i] != '\\') continue;
        const char d = pattern[i + 1];
        if (d >= '0' && d <= '9') return true;
        ++i;  // consume the escaped character so "\\1" is not a backreference
    }
    return false;
}

bool loadGrammar(const json::Value& root, Grammar& out, std::string* error) {
    out = Grammar{};
    if (!root.isObject()) {
        if (error != nullptr) *error = "grammar root is not a JSON object";
        return false;
    }
    const json::Value* scope = root.find("scopeName");
    if (scope == nullptr || !scope->isString() || scope->string().empty()) {
        if (error != nullptr) *error = "grammar has no scopeName";
        return false;
    }
    out.setScopeName(scope->string());

    const json::Value* display = root.find("displayName");
    if (display != nullptr && display->isString()) {
        out.setDisplayName(display->string());
    } else {
        const json::Value* legacy = root.find("name");
        if (legacy != nullptr && legacy->isString()) out.setDisplayName(legacy->string());
    }
    const json::Value* firstLine = root.find("firstLineMatch");
    if (firstLine != nullptr && firstLine->isString()) out.setFirstLineMatch(firstLine->string());
    const json::Value* injectionSelector = root.find("injectionSelector");
    if (injectionSelector != nullptr && injectionSelector->isString()) {
        out.setInjectionSelector(injectionSelector->string());
    }
    const json::Value& fileTypes = root["fileTypes"];
    for (size_t i = 0; i < fileTypes.size(); ++i) {
        const json::Value& ft = fileTypes.at(i);
        if (ft.isString() && !ft.string().empty()) out.addFileType(ft.string());
    }

    // The root pattern list becomes a container rule, which is what "$self"
    // resolves to. Its name carries the grammar scope for diagnostics; scopes
    // are only pushed by rules that actually match, so this cannot double-push.
    Rule rootRule;
    rootRule.kind = RuleKind::Container;
    rootRule.name = out.scopeName();
    rootRule.debugName = "$self";
    rootRule.patterns = parsePatternList(root["patterns"], out, "$self.patterns", 0);
    out.setRootRule(out.internRule(std::move(rootRule)));

    const json::Value& repository = root["repository"];
    for (size_t k = 0; k < repository.memberCount(); ++k) {
        const std::string_view key = repository.keyAt(k);
        if (key.empty()) continue;
        std::string debugName = "repository.";
        debugName += key;
        const RuleId id = parseRule(repository.valueAt(k), out, std::move(debugName), 1);
        if (id != kInvalidRuleId) out.setRepositoryEntry(std::string(key), id);
    }

    const json::Value& injections = root["injections"];
    for (size_t k = 0; k < injections.memberCount(); ++k) {
        const std::string_view selector = injections.keyAt(k);
        if (selector.empty()) continue;
        std::string debugName = "injections.";
        debugName += selector;
        const RuleId id = parseRule(injections.valueAt(k), out, std::move(debugName), 1);
        if (id != kInvalidRuleId) out.addInjection(std::string(selector), id);
    }

    // Second pass: resolve every include we can resolve locally. External
    // references stay unresolved on purpose - GrammarRegistry fills them in when
    // (and if) the other grammar shows up.
    const RuleId ruleCount = static_cast<RuleId>(out.ruleCount());
    for (RuleId id = 0; id < ruleCount; ++id) {
        Rule* r = out.mutableRule(id);
        if (r == nullptr || r->kind != RuleKind::Include) continue;
        switch (r->include.kind) {
            case IncludeKind::Repository: {
                const RuleId target = out.repositoryRule(r->include.ruleName);
                if (target == kInvalidRuleId) {
                    out.noteBrokenInclude(r->include.raw);
                } else {
                    r->includeTarget = target;
                }
                break;
            }
            case IncludeKind::Self:
                r->includeTarget = out.rootRule();
                break;
            case IncludeKind::Base:
                // Local fallback; a registry with a base grammar overrides it.
                r->includeTarget = out.rootRule();
                break;
            case IncludeKind::ExternalGrammar:
            case IncludeKind::ExternalRule:
                out.noteExternalScope(r->include.scopeName);
                break;
            case IncludeKind::Invalid:
                out.noteBrokenInclude(r->include.raw);
                break;
            case IncludeKind::None:
                break;
        }
    }
    return true;
}

bool loadGrammarJson(std::string_view jsonText, Grammar& out, std::string* error) {
    json::ParseResult parsed = json::parse(jsonText);
    if (!parsed.ok) {
        if (error != nullptr) {
            *error = "json: " + parsed.error + " at offset " + std::to_string(parsed.errorOffset);
        }
        out = Grammar{};
        return false;
    }
    return loadGrammar(parsed.root, out, error);
}

GrammarId GrammarRegistry::addGrammar(const json::Value& root, std::string* error) {
    auto grammar = std::make_unique<Grammar>();
    if (!loadGrammar(root, *grammar, error)) return kInvalidGrammarId;
    const std::string scope = grammar->scopeName();
    const auto existing = byScope_.find(scope);
    if (existing != byScope_.end()) {
        // Keep the first registration and hand back its id: rule ids may already
        // be referenced by live tokenizer states, so replacing a grammar in
        // place is unsafe. Callers that care can compare ids.
        return existing->second;
    }
    const GrammarId id = static_cast<GrammarId>(grammars_.size());
    grammar->setId(id);
    // "inline.*" grammars are embedded fragments (CSS-in-JS template bodies
    // and the like) that a host grammar includes by scope reference. Some of
    // them ship fileTypes anyway (es-tag-css.json claims "js", "html",
    // "vue"...), which would let a fragment grammar hijack whole-file
    // extensions, so they are never allowed to claim one. This matters most
    // with lazy loading, where the hijack decides which file gets opened.
    if (scope.rfind("inline.", 0) != 0) {
        for (const std::string& extension : grammar->fileTypes()) {
            if (extension.empty()) continue;
            if (extensionToScope_.find(extension) == extensionToScope_.end()) {
                extensionToScope_[extension] = scope;
            }
        }
    }
    byScope_[scope] = id;
    loadAttempted_[scope] = true;
    grammars_.push_back(std::move(grammar));
    return id;
}

GrammarId GrammarRegistry::addGrammarJson(std::string_view jsonText, std::string* error) {
    json::ParseResult parsed = json::parse(jsonText);
    if (!parsed.ok) {
        if (error != nullptr) {
            *error = "json: " + parsed.error + " at offset " + std::to_string(parsed.errorOffset);
        }
        return kInvalidGrammarId;
    }
    return addGrammar(parsed.root, error);
}

void GrammarRegistry::mapExtension(std::string_view extension, std::string_view scopeName) {
    if (extension.empty()) return;
    std::string key(extension);
    if (key.front() == '.') key.erase(key.begin());
    if (key.empty()) return;
    extensionToScope_[std::move(key)] = std::string(scopeName);
}

std::string GrammarRegistry::scopeForExtension(std::string_view extension) const {
    if (extension.empty()) return {};
    std::string key(extension);
    if (key.front() == '.') key.erase(key.begin());
    const auto it = extensionToScope_.find(key);
    return (it == extensionToScope_.end()) ? std::string() : it->second;
}

const Grammar* GrammarRegistry::grammarById(GrammarId id) const noexcept {
    if (id < 0 || static_cast<size_t>(id) >= grammars_.size()) return nullptr;
    return grammars_[static_cast<size_t>(id)].get();
}

GrammarId GrammarRegistry::idForScope(std::string_view scopeName) const noexcept {
    const auto it = byScope_.find(std::string(scopeName));
    return (it == byScope_.end()) ? kInvalidGrammarId : it->second;
}

const Grammar* GrammarRegistry::grammarForScope(std::string_view scopeName) {
    if (scopeName.empty()) return nullptr;
    const std::string key(scopeName);
    const auto it = byScope_.find(key);
    if (it != byScope_.end()) return grammarById(it->second);

    const auto attempted = loadAttempted_.find(key);
    if (attempted != loadAttempted_.end()) return nullptr;  // already failed once
    loadAttempted_[key] = true;

    if (!loader_) {
        missingScopes_.push_back(key);
        return nullptr;
    }
    std::optional<std::string> text;
    try {
        text = loader_(scopeName);
    } catch (...) {
        text.reset();  // a misbehaving loader must not break tokenization
    }
    if (!text.has_value()) {
        missingScopes_.push_back(key);
        return nullptr;
    }
    std::string error;
    if (addGrammarJson(*text, &error) == kInvalidGrammarId) {
        missingScopes_.push_back(key);
        return nullptr;
    }
    const auto loaded = byScope_.find(key);
    if (loaded == byScope_.end()) {
        // The file declared a different scopeName than the one requested.
        missingScopes_.push_back(key);
        return nullptr;
    }
    return grammarById(loaded->second);
}

const Grammar* GrammarRegistry::grammarForExtension(std::string_view extension) {
    const std::string scope = scopeForExtension(extension);
    if (scope.empty()) return nullptr;
    return grammarForScope(scope);
}

RuleRef GrammarRegistry::resolveInclude(GrammarId owner, RuleId includeRuleId,
                                        GrammarId baseGrammar) {
    const Grammar* grammar = grammarById(owner);
    if (grammar == nullptr || !grammar->validRule(includeRuleId)) return RuleRef{};
    const Rule& r = grammar->rule(includeRuleId);
    if (r.kind != RuleKind::Include) return RuleRef{owner, includeRuleId};

    switch (r.include.kind) {
        case IncludeKind::Repository:
            if (r.includeTarget == kInvalidRuleId) return RuleRef{};
            return RuleRef{owner, r.includeTarget};
        case IncludeKind::Self:
            return RuleRef{owner, grammar->rootRule()};
        case IncludeKind::Base: {
            const Grammar* base = grammarById(baseGrammar);
            if (base != nullptr) return RuleRef{base->id(), base->rootRule()};
            return RuleRef{owner, grammar->rootRule()};
        }
        case IncludeKind::ExternalGrammar: {
            // Copy before the lazy load: loading appends to grammars_.
            const std::string scope = r.include.scopeName;
            const Grammar* target = grammarForScope(scope);
            if (target == nullptr) return RuleRef{};
            return RuleRef{target->id(), target->rootRule()};
        }
        case IncludeKind::ExternalRule: {
            const std::string scope = r.include.scopeName;
            const std::string name = r.include.ruleName;
            const Grammar* target = grammarForScope(scope);
            if (target == nullptr) return RuleRef{};
            const RuleId rule = target->repositoryRule(name);
            if (rule == kInvalidRuleId) return RuleRef{};
            return RuleRef{target->id(), rule};
        }
        case IncludeKind::Invalid:
        case IncludeKind::None:
            break;
    }
    return RuleRef{};
}

}  // namespace ide::syntax
