#pragma once

// Why this file exists
// -------------------
// The in-memory model of a .tmLanguage JSON grammar, plus the loader and a
// registry for cross-grammar (embedded language) references.
//
// Three structural decisions make the real-world grammar corpus survivable:
//
//  1. Rules are *interned into a flat vector* and referenced by integer id.
//     No owning pointers, no shared_ptr cycles, no dangling references when a
//     grammar is copied. The include graph in textmate/grammars is genuinely
//     cyclic (a includes b includes a, and $self includes itself), so a graph
//     of ids plus a visited set is the only cheap way to stay safe.
//  2. Includes are *rules*, not edges resolved at parse time. An include keeps
//     its unresolved form, so an external "source.foo#bar" reference to a
//     grammar that is not loaded yet is a normal state, not an error.
//  3. The registry never touches the filesystem. JSON text arrives through a
//     caller-supplied loader callback, which keeps core/ IO-free per
//     docs/CONVENTIONS.md.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ide/syntax/json_lite.h>

namespace ide::syntax {

using RuleId = int32_t;
using GrammarId = int32_t;

inline constexpr RuleId kInvalidRuleId = -1;
inline constexpr GrammarId kInvalidGrammarId = -1;

/// Deepest JSON rule nesting the loader will walk. Guards the loader's
/// recursion independently of the JSON parser's own depth limit.
inline constexpr int kMaxRuleNestingDepth = 100;

/// A rule reference that may cross a grammar boundary, which is what embedded
/// languages (html in php, css in html, ...) require. Trivially copyable and
/// hashable so the tokenizer can key caches on it.
struct RuleRef {
    GrammarId grammar = kInvalidGrammarId;
    RuleId rule = kInvalidRuleId;

    [[nodiscard]] bool valid() const noexcept { return grammar >= 0 && rule >= 0; }
    /// Packed form for use as a hash key.
    [[nodiscard]] uint64_t key() const noexcept {
        return (static_cast<uint64_t>(static_cast<uint32_t>(grammar)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(rule));
    }
    friend bool operator==(RuleRef a, RuleRef b) noexcept {
        return a.grammar == b.grammar && a.rule == b.rule;
    }
    friend bool operator!=(RuleRef a, RuleRef b) noexcept { return !(a == b); }
};

/// What kind of TextMate rule this is. Mirrors the four shapes a .tmLanguage
/// pattern object can take, plus Include which is a pure indirection.
enum class RuleKind : uint8_t {
    Container,    ///< "patterns" only ("include-only" in vscode-textmate)
    Match,        ///< "match" (+ captures)
    BeginEnd,     ///< "begin" + "end"
    BeginWhile,   ///< "begin" + "while"
    Include,      ///< "include"
};

/// The five include spellings TextMate allows.
enum class IncludeKind : uint8_t {
    None,
    Repository,       ///< "#ruleName" inside this grammar's repository
    Self,             ///< "$self": this grammar's root pattern list
    Base,             ///< "$base": the root of the grammar being tokenized
    ExternalGrammar,  ///< "source.foo"
    ExternalRule,     ///< "source.foo#bar"
    Invalid,          ///< syntactically unusable, kept for diagnostics
};

/// Parsed "include" payload. `scopeName`/`ruleName` are empty when unused.
struct IncludeRef {
    IncludeKind kind = IncludeKind::None;
    std::string scopeName;
    std::string ruleName;
    std::string raw;  ///< the original string, for tracing
};

/// Scope (and optional sub-rules) attached to one capture group.
/// A CaptureSpec with nothing set means "group N has no decoration", which must
/// stay distinguishable from "group N is absent from the match".
struct CaptureSpec {
    int index = 0;
    std::string name;         ///< may contain $1..$9 substitutions
    std::string contentName;  ///< only meaningful together with patternsRule
    /// Synthetic container rule holding this capture's "patterns", so the
    /// tokenizer can re-tokenize the captured text with the ordinary machinery.
    /// kInvalidRuleId when the capture has no sub-patterns.
    RuleId patternsRule = kInvalidRuleId;

    [[nodiscard]] bool isNull() const noexcept {
        return name.empty() && contentName.empty() && patternsRule == kInvalidRuleId;
    }
};

/// One TextMate rule. Plain data: no behaviour, no ownership, copyable.
struct Rule {
    RuleId id = kInvalidRuleId;
    RuleKind kind = RuleKind::Container;

    std::string name;         ///< scope pushed while the rule is active
    std::string contentName;  ///< scope pushed for the *content* of begin/end

    std::string match;
    std::string begin;
    std::string end;
    std::string whilePattern;  ///< "while"; `while` is a keyword

    bool hasMatch = false;
    bool hasBegin = false;
    bool hasEnd = false;
    bool hasWhile = false;

    /// When true the end pattern is tried *after* the sub-patterns instead of
    /// before them (JSON "applyEndPatternLast").
    bool applyEndPatternLast = false;
    /// True when the end/while source contains \1..\9, which must be
    /// substituted from the begin match before compiling.
    bool endHasBackrefs = false;
    bool whileHasBackrefs = false;

    std::vector<RuleId> patterns;
    std::vector<CaptureSpec> captures;       ///< for "match"
    std::vector<CaptureSpec> beginCaptures;
    std::vector<CaptureSpec> endCaptures;
    std::vector<CaptureSpec> whileCaptures;

    IncludeRef include;
    /// Same-grammar resolution of `include`, filled at load time for
    /// "#name"/"$self"/"$base". kInvalidRuleId for external references.
    RuleId includeTarget = kInvalidRuleId;

    /// Human readable path such as "repository.strings.patterns[2]". Only used
    /// by the tracing facility and error messages.
    std::string debugName;
};

/// One loaded grammar: metadata plus the interned rule table.
class Grammar {
public:
    Grammar() = default;

    // --- metadata -----------------------------------------------------------
    [[nodiscard]] const std::string& scopeName() const noexcept { return scopeName_; }
    [[nodiscard]] const std::string& displayName() const noexcept { return displayName_; }
    [[nodiscard]] const std::string& firstLineMatch() const noexcept { return firstLineMatch_; }
    [[nodiscard]] const std::vector<std::string>& fileTypes() const noexcept { return fileTypes_; }
    /// Selector this grammar wants to be injected into ("injectionSelector").
    [[nodiscard]] const std::string& injectionSelector() const noexcept {
        return injectionSelector_;
    }

    /// "injections": selector -> container rule. Parsed and exposed, but the
    /// phase-2 tokenizer does not apply injections yet (see tokenizer.h).
    struct Injection {
        std::string selector;
        RuleId rule = kInvalidRuleId;
    };
    [[nodiscard]] const std::vector<Injection>& injections() const noexcept { return injections_; }

    [[nodiscard]] GrammarId id() const noexcept { return id_; }

    // --- rule table ---------------------------------------------------------
    [[nodiscard]] bool validRule(RuleId id) const noexcept {
        return id >= 0 && static_cast<size_t>(id) < rules_.size();
    }
    /// Always returns a usable object; an invalid id yields a shared empty rule
    /// so callers never need a null check.
    [[nodiscard]] const Rule& rule(RuleId id) const noexcept;
    [[nodiscard]] size_t ruleCount() const noexcept { return rules_.size(); }
    [[nodiscard]] RuleId rootRule() const noexcept { return rootRule_; }
    /// kInvalidRuleId when the repository has no such entry.
    [[nodiscard]] RuleId repositoryRule(std::string_view name) const noexcept;
    [[nodiscard]] const std::unordered_map<std::string, RuleId>& repository() const noexcept {
        return repository_;
    }

    /// Scope names this grammar references but does not contain, deduplicated.
    /// The registry uses this to know what to load for embedded languages.
    [[nodiscard]] const std::vector<std::string>& externalScopeRefs() const noexcept {
        return externalScopeRefs_;
    }
    /// Includes that could not be resolved inside this grammar ("#missing").
    [[nodiscard]] const std::vector<std::string>& brokenIncludes() const noexcept {
        return brokenIncludes_;
    }

    // --- loader facing ------------------------------------------------------
    // These are used by loadGrammar() and GrammarRegistry only. They are public
    // because the loader is a free function (keeps Grammar itself dumb), not
    // because callers should mutate a live grammar.
    RuleId internRule(Rule r);
    /// Reference is invalidated by the next internRule(); never hold it.
    Rule* mutableRule(RuleId id) noexcept;
    void setId(GrammarId id) noexcept { id_ = id; }
    void setScopeName(std::string v) { scopeName_ = std::move(v); }
    void setDisplayName(std::string v) { displayName_ = std::move(v); }
    void setFirstLineMatch(std::string v) { firstLineMatch_ = std::move(v); }
    void setInjectionSelector(std::string v) { injectionSelector_ = std::move(v); }
    void addFileType(std::string v) { fileTypes_.push_back(std::move(v)); }
    void addInjection(std::string selector, RuleId rule) {
        injections_.push_back(Injection{std::move(selector), rule});
    }
    void setRootRule(RuleId id) noexcept { rootRule_ = id; }
    void setRepositoryEntry(std::string name, RuleId id) {
        repository_[std::move(name)] = id;
    }
    void noteExternalScope(std::string scopeName);
    void noteBrokenInclude(std::string raw) { brokenIncludes_.push_back(std::move(raw)); }

private:
    std::string scopeName_;
    std::string displayName_;
    std::string firstLineMatch_;
    std::string injectionSelector_;
    std::vector<std::string> fileTypes_;
    std::vector<Injection> injections_;
    std::vector<Rule> rules_;
    std::unordered_map<std::string, RuleId> repository_;
    std::vector<std::string> externalScopeRefs_;
    std::vector<std::string> brokenIncludes_;
    RuleId rootRule_ = kInvalidRuleId;
    GrammarId id_ = kInvalidGrammarId;
};

/// Splits an "include" string into its parsed form. Total function.
[[nodiscard]] IncludeRef parseIncludeRef(std::string_view raw);

/// True when `pattern` contains a \1..\99 style backreference that has to be
/// substituted from a begin match. Understands \\ so that "\\\\1" (an escaped
/// backslash followed by a literal 1) is not mistaken for a backreference.
[[nodiscard]] bool patternHasBackrefs(std::string_view pattern);

/// Builds `out` from an already parsed JSON document. Never throws; returns
/// false and fills `error` for structurally unusable input (e.g. not an
/// object). Missing optional fields are simply absent, not errors.
[[nodiscard]] bool loadGrammar(const json::Value& root, Grammar& out, std::string* error);

/// Convenience: parse JSON text, then loadGrammar().
[[nodiscard]] bool loadGrammarJson(std::string_view jsonText, Grammar& out, std::string* error);

/// scopeName -> Grammar, extension -> scopeName, plus lazy loading and
/// cross-grammar include resolution.
///
/// The registry owns grammars through unique_ptr so that pointers handed out by
/// grammarForScope() stay valid when lazy loading appends more grammars.
class GrammarRegistry {
public:
    /// Returns the JSON text for a scope name, or nullopt if unavailable.
    /// The *caller* does the IO; core stays filesystem-free.
    using SourceLoader = std::function<std::optional<std::string>(std::string_view scopeName)>;

    GrammarRegistry() = default;
    explicit GrammarRegistry(SourceLoader loader) : loader_(std::move(loader)) {}

    void setLoader(SourceLoader loader) { loader_ = std::move(loader); }

    /// Parses and interns a grammar, registering its scope name and file types.
    /// Returns kInvalidGrammarId on failure.
    GrammarId addGrammar(const json::Value& root, std::string* error = nullptr);
    GrammarId addGrammarJson(std::string_view jsonText, std::string* error = nullptr);

    /// Manual extension mapping, e.g. mapExtension("cc", "source.cpp").
    /// Extensions are stored without a leading dot and matched case sensitively.
    void mapExtension(std::string_view extension, std::string_view scopeName);
    [[nodiscard]] std::string scopeForExtension(std::string_view extension) const;

    /// Lazily loads through the SourceLoader on a miss. nullptr when unknown.
    const Grammar* grammarForScope(std::string_view scopeName);
    const Grammar* grammarForExtension(std::string_view extension);
    [[nodiscard]] const Grammar* grammarById(GrammarId id) const noexcept;
    [[nodiscard]] GrammarId idForScope(std::string_view scopeName) const noexcept;
    [[nodiscard]] size_t grammarCount() const noexcept { return grammars_.size(); }

    /// Resolves one include rule to a concrete rule, loading the target grammar
    /// if needed. `baseGrammar` is the grammar at the root of the current
    /// tokenization and decides what "$base" means; pass kInvalidGrammarId to
    /// make "$base" behave like "$self".
    ///
    /// Does not recurse: an include that points at another include resolves to
    /// that include rule, and the caller (the tokenizer's pattern flattener)
    /// walks the chain with a visited set. That is what keeps cyclic grammars
    /// from becoming infinite recursion.
    RuleRef resolveInclude(GrammarId owner, RuleId includeRuleId, GrammarId baseGrammar);

    /// Scope names that were requested but could not be loaded, deduplicated.
    [[nodiscard]] const std::vector<std::string>& missingScopes() const noexcept {
        return missingScopes_;
    }

private:
    std::vector<std::unique_ptr<Grammar>> grammars_;
    std::unordered_map<std::string, GrammarId> byScope_;
    std::unordered_map<std::string, std::string> extensionToScope_;
    std::unordered_map<std::string, bool> loadAttempted_;
    std::vector<std::string> missingScopes_;
    SourceLoader loader_;
};

}  // namespace ide::syntax
