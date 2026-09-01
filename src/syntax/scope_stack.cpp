#include <ide/syntax/scope_stack.h>

#include <algorithm>

namespace ide::syntax {

ScopeStackTable::ScopeStackTable() {
    nodes_.push_back(Node{-1, 0, std::string()});  // kRootScopeStack
}

bool ScopeStackTable::valid(ScopeStackId id) const noexcept {
    return id >= 0 && static_cast<size_t>(id) < nodes_.size();
}

ScopeStackId ScopeStackTable::pushOne(ScopeStackId parent, std::string_view scope) {
    if (!valid(parent)) parent = kRootScopeStack;
    if (scope.empty()) return parent;
    Key key{parent, std::string(scope)};
    const auto it = index_.find(key);
    if (it != index_.end()) return it->second;
    const ScopeStackId id = static_cast<ScopeStackId>(nodes_.size());
    const uint32_t depth = nodes_[static_cast<size_t>(parent)].depth + 1u;
    nodes_.push_back(Node{parent, depth, std::string(scope)});
    index_.emplace(std::move(key), id);
    return id;
}

ScopeStackId ScopeStackTable::push(ScopeStackId parent, std::string_view scopes) {
    if (!valid(parent)) parent = kRootScopeStack;
    ScopeStackId current = parent;
    size_t i = 0;
    while (i < scopes.size()) {
        while (i < scopes.size() && scopes[i] == ' ') ++i;
        const size_t start = i;
        while (i < scopes.size() && scopes[i] != ' ') ++i;
        if (i > start) current = pushOne(current, scopes.substr(start, i - start));
    }
    return current;
}

ScopeStackId ScopeStackTable::parentOf(ScopeStackId id) const noexcept {
    if (!valid(id) || id == kRootScopeStack) return kRootScopeStack;
    const ScopeStackId parent = nodes_[static_cast<size_t>(id)].parent;
    return valid(parent) ? parent : kRootScopeStack;
}

std::string_view ScopeStackTable::scopeAt(ScopeStackId id) const noexcept {
    if (!valid(id)) return {};
    return nodes_[static_cast<size_t>(id)].scope;
}

size_t ScopeStackTable::depth(ScopeStackId id) const noexcept {
    if (!valid(id)) return 0;
    return nodes_[static_cast<size_t>(id)].depth;
}

void ScopeStackTable::resolve(ScopeStackId id, std::vector<std::string_view>& out) const {
    out.clear();
    if (!valid(id)) return;
    ScopeStackId current = id;
    while (current > 0 && valid(current)) {
        const Node& node = nodes_[static_cast<size_t>(current)];
        out.push_back(node.scope);
        current = node.parent;
    }
    std::reverse(out.begin(), out.end());
}

std::vector<std::string_view> ScopeStackTable::resolve(ScopeStackId id) const {
    std::vector<std::string_view> out;
    resolve(id, out);
    return out;
}

std::string ScopeStackTable::flatten(ScopeStackId id, char separator) const {
    std::vector<std::string_view> parts;
    resolve(id, parts);
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.push_back(separator);
        out.append(parts[i]);
    }
    return out;
}

}  // namespace ide::syntax
