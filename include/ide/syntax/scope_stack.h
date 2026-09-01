#pragma once

// Why this file exists
// -------------------
// A token's scope is a *stack* of scope names ("source.c" / "meta.function.c" /
// "string.quoted.double.c"). Storing that list per token would be ruinous: a
// 5,000 line file has ~200k tokens. Instead every distinct stack is interned
// once and referred to by a 32-bit id, so a TokenSpan stays three integers wide
// and a whole line of tokens can later be memcpy'd across JNI as a flat blob.
//
// Interning also gives cheap equality: two tokens have the same scopes iff their
// ids are equal, which is what the theme resolver in a later phase wants.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ide::syntax {

using ScopeStackId = int32_t;

/// The empty scope stack. Always interned as id 0.
inline constexpr ScopeStackId kRootScopeStack = 0;

/// Interning table for scope stacks, shaped as a trie: every node stores its
/// parent plus one scope name, so pushing is O(1) amortised and sharing is
/// automatic.
class ScopeStackTable {
public:
    ScopeStackTable();

    /// Pushes one or more scope names onto `parent`. TextMate allows a rule name
    /// to hold several space separated scopes ("meta.a meta.b"), and each one
    /// becomes its own stack entry. Returns `parent` unchanged when `scopes` is
    /// empty or only whitespace, which is what an absent name/contentName means.
    ScopeStackId push(ScopeStackId parent, std::string_view scopes);

    /// kRootScopeStack maps to itself; an invalid id maps to kRootScopeStack.
    [[nodiscard]] ScopeStackId parentOf(ScopeStackId id) const noexcept;
    /// The last (innermost) scope name; empty for the root.
    [[nodiscard]] std::string_view scopeAt(ScopeStackId id) const noexcept;
    /// Number of scope names in the stack.
    [[nodiscard]] size_t depth(ScopeStackId id) const noexcept;
    [[nodiscard]] bool valid(ScopeStackId id) const noexcept;
    /// Number of interned nodes, including the root.
    [[nodiscard]] size_t size() const noexcept { return nodes_.size(); }

    /// Outermost-first list of scope names. Views point into the table and stay
    /// valid for as long as the table exists (nodes are never removed).
    void resolve(ScopeStackId id, std::vector<std::string_view>& out) const;
    [[nodiscard]] std::vector<std::string_view> resolve(ScopeStackId id) const;
    /// Outermost-first, joined by `separator`. Debug and test helper.
    [[nodiscard]] std::string flatten(ScopeStackId id, char separator = ' ') const;

private:
    struct Node {
        ScopeStackId parent = -1;
        uint32_t depth = 0;
        std::string scope;
    };
    struct Key {
        ScopeStackId parent = -1;
        std::string scope;
        friend bool operator==(const Key& a, const Key& b) noexcept {
            return a.parent == b.parent && a.scope == b.scope;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            size_t h = std::hash<std::string>{}(k.scope);
            h ^= static_cast<size_t>(static_cast<uint32_t>(k.parent)) * 0x9E3779B9u;
            return h;
        }
    };

    ScopeStackId pushOne(ScopeStackId parent, std::string_view scope);

    std::vector<Node> nodes_;
    std::unordered_map<Key, ScopeStackId, KeyHash> index_;
};

}  // namespace ide::syntax
