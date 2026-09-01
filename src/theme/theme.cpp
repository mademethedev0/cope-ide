#include <ide/theme/theme.h>

#include <algorithm>
#include <utility>

namespace ide::theme {
namespace {

/// FNV-1a over the stack contents with a separator byte, so that
/// ["ab", "c"] and ["a", "bc"] hash differently. Only a bucket selector: a
/// collision costs a comparison, never a wrong colour.
[[nodiscard]] uint64_t hashScopeStack(std::span<const std::string_view> scopeStack) noexcept {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const std::string_view& scope : scopeStack) {
        for (const char c : scope) {
            hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
            hash *= 0x100000001B3ull;
        }
        hash ^= 0xFFull;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

[[nodiscard]] bool sameStack(const std::vector<std::string>& stored,
                             std::span<const std::string_view> probe) noexcept {
    if (stored.size() != probe.size()) return false;
    for (size_t i = 0; i < stored.size(); ++i) {
        if (std::string_view(stored[i]) != probe[i]) return false;
    }
    return true;
}

/// Reads a theme rule's `settings` block. Unparsable colours and non-string
/// values are dropped, which is how "settings": { "foreground": null } survives.
[[nodiscard]] StyleDelta parseSettings(const json::Value& settings) {
    StyleDelta delta;
    if (const json::Value* value = settings.find("foreground"); value != nullptr && value->isString()) {
        if (const std::optional<Rgba> colour = parseColor(value->string()); colour.has_value()) {
            delta.fg = *colour;
            delta.hasFg = true;
        }
    }
    if (const json::Value* value = settings.find("background"); value != nullptr && value->isString()) {
        if (const std::optional<Rgba> colour = parseColor(value->string()); colour.has_value()) {
            delta.bg = *colour;
            delta.hasBg = true;
        }
    }
    if (const json::Value* value = settings.find("fontStyle"); value != nullptr && value->isString()) {
        // Presence is what matters, not content: "" and "regular" both mean
        // "explicitly no decorations" and must be able to beat an inherited italic.
        delta.fontStyle = parseFontStyleMask(value->string());
        delta.hasFontStyle = true;
    }
    return delta;
}

[[nodiscard]] bool isBlank(std::string_view text) noexcept {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

void setError(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

/// Rec. 601 luma; only used to guess `type` when a theme omits it.
[[nodiscard]] bool looksDark(Rgba background) noexcept {
    const int luma = (299 * static_cast<int>(background.r) + 587 * static_cast<int>(background.g) +
                      114 * static_cast<int>(background.b)) /
                     1000;
    return luma < 128;
}

}  // namespace

Theme::Theme() {
    palette_.push_back(Style{});
    paletteIndex_.emplace(styleKey(Style{}), kDefaultStyleId);
}

void Theme::setDefaultStyle(const Style& style) {
    palette_.clear();
    paletteIndex_.clear();
    palette_.push_back(normalizeStyle(style));
    paletteIndex_.emplace(styleKey(palette_.front()), kDefaultStyleId);
    cache_.clear();
    cacheEntries_ = 0;
}

StyleId Theme::intern(const Style& style) const {
    const StyleKey key = styleKey(style);
    const auto it = paletteIndex_.find(key);
    if (it != paletteIndex_.end()) return it->second;
    const StyleId id = static_cast<StyleId>(palette_.size());
    palette_.push_back(normalizeStyle(style));
    paletteIndex_.emplace(key, id);
    return id;
}

std::optional<Theme> Theme::fromJson(const ide::json::Value& root, std::string* error) {
    if (!root.isObject()) {
        setError(error, "theme root must be a JSON object");
        return std::nullopt;
    }

    Theme theme;

    if (const json::Value* value = root.find("name"); value != nullptr && value->isString()) {
        theme.name_ = value->string();
    }
    if (const json::Value* value = root.find("displayName"); value != nullptr && value->isString()) {
        theme.displayName_ = value->string();
    }
    if (theme.name_.empty()) theme.name_ = theme.displayName_;
    if (theme.displayName_.empty()) theme.displayName_ = theme.name_;

    if (const json::Value* value = root.find("semanticHighlighting"); value != nullptr && value->isBool()) {
        theme.semanticHighlighting_ = value->boolean(false);
    }

    // --- colors -------------------------------------------------------------
    if (const json::Value* colors = root.find("colors"); colors != nullptr && colors->isObject()) {
        theme.uiColors_.reserve(colors->memberCount());
        for (size_t i = 0; i < colors->memberCount(); ++i) {
            const json::Value& value = colors->valueAt(i);
            if (!value.isString()) continue;  // real themes map some keys to null
            const std::optional<Rgba> colour = parseColor(value.string());
            if (!colour.has_value()) continue;
            theme.uiColors_.push_back(UiColor{std::string(colors->keyAt(i)), *colour});
        }
        std::stable_sort(theme.uiColors_.begin(), theme.uiColors_.end(),
                         [](const UiColor& lhs, const UiColor& rhs) {
                             return std::string_view(lhs.key) < std::string_view(rhs.key);
                         });
    }

    // --- type ---------------------------------------------------------------
    const std::optional<Rgba> editorBackground = theme.uiColor(kUiEditorBackground);
    bool dark = editorBackground.has_value() ? looksDark(*editorBackground) : true;
    if (const json::Value* value = root.find("type"); value != nullptr && value->isString()) {
        if (value->string() == "light") {
            dark = false;
        } else if (value->string() == "dark") {
            dark = true;
        }
    }
    theme.isDark_ = dark;

    // --- tokenColors --------------------------------------------------------
    StyleDelta defaultDelta;
    if (const json::Value* tokenColors = root.find("tokenColors");
        tokenColors != nullptr && tokenColors->isArray()) {
        theme.rules_.reserve(tokenColors->size());
        for (size_t i = 0; i < tokenColors->size(); ++i) {
            const json::Value& entry = tokenColors->at(i);
            if (!entry.isObject()) continue;

            const json::Value* settings = entry.find("settings");
            if (settings == nullptr || !settings->isObject()) continue;
            const StyleDelta delta = parseSettings(*settings);
            if (delta.empty()) continue;

            const json::Value* scope = entry.find("scope");
            std::vector<std::string_view> selectorTexts;
            bool malformedScope = false;
            if (scope == nullptr || scope->isNull()) {
                // no scope at all: this entry defines the default style
            } else if (scope->isString()) {
                if (!isBlank(scope->string())) selectorTexts.push_back(scope->string());
            } else if (scope->isArray()) {
                for (size_t j = 0; j < scope->size(); ++j) {
                    const json::Value& element = scope->at(j);
                    if (!element.isString()) continue;
                    if (!isBlank(element.string())) selectorTexts.push_back(element.string());
                }
            } else {
                malformedScope = true;  // number/bool/object: skip the entry entirely
            }
            if (malformedScope) continue;

            if (selectorTexts.empty()) {
                // Merge into the running default; later entries win per attribute.
                if (delta.hasFg) {
                    defaultDelta.fg = delta.fg;
                    defaultDelta.hasFg = true;
                }
                if (delta.hasBg) {
                    defaultDelta.bg = delta.bg;
                    defaultDelta.hasBg = true;
                }
                if (delta.hasFontStyle) {
                    defaultDelta.fontStyle = delta.fontStyle;
                    defaultDelta.hasFontStyle = true;
                }
                continue;
            }

            TokenColorRule rule;
            rule.delta = delta;
            rule.selectors.reserve(selectorTexts.size());
            for (const std::string_view text : selectorTexts) {
                ScopeSelector selector = ScopeSelector::parse(text);
                if (selector.empty()) continue;
                if (!rule.scopeText.empty()) rule.scopeText.append(", ");
                rule.scopeText.append(text);
                rule.selectors.push_back(std::move(selector));
            }
            if (rule.selectors.empty()) continue;
            theme.rules_.push_back(std::move(rule));
        }
    }

    // --- default style ------------------------------------------------------
    // editor.foreground / editor.background form the base, and the scope-less
    // tokenColors entry (if any) overrides them: VS Code treats that entry as the
    // editor's own token default.
    Style base;
    if (const std::optional<Rgba> colour = theme.uiColor(kUiEditorForeground); colour.has_value()) {
        base.fg = *colour;
        base.hasFg = true;
    }
    if (editorBackground.has_value()) {
        base.bg = *editorBackground;
        base.hasBg = true;
    }
    theme.setDefaultStyle(applyDelta(base, defaultDelta));

    // --- semanticTokenColors (parsed, not applied in this phase) -------------
    if (const json::Value* semantic = root.find("semanticTokenColors");
        semantic != nullptr && semantic->isObject()) {
        theme.semanticRules_.reserve(semantic->memberCount());
        for (size_t i = 0; i < semantic->memberCount(); ++i) {
            const json::Value& value = semantic->valueAt(i);
            StyleDelta delta;
            if (value.isString()) {
                const std::optional<Rgba> colour = parseColor(value.string());
                if (!colour.has_value()) continue;
                delta.fg = *colour;
                delta.hasFg = true;
            } else if (value.isObject()) {
                delta = parseSettings(value);
                if (delta.empty()) continue;
            } else {
                continue;
            }
            theme.semanticRules_.push_back(SemanticRule{std::string(semantic->keyAt(i)), delta});
        }
    }

    return theme;
}

std::optional<Theme> Theme::fromJsonText(std::string_view jsonText, std::string* error) {
    json::ParseResult parsed = json::parse(jsonText);
    if (!parsed.ok) {
        setError(error, "theme JSON parse error at byte " + std::to_string(parsed.errorOffset) + ": " + parsed.error);
        return std::nullopt;
    }
    return fromJson(parsed.root, error);
}

StyleId Theme::resolveUncached(std::span<const std::string_view> scopeStack) const {
    MatchKey fgKey = kNoMatch;
    MatchKey bgKey = kNoMatch;
    MatchKey fontKey = kNoMatch;
    const StyleDelta* fgWinner = nullptr;
    const StyleDelta* bgWinner = nullptr;
    const StyleDelta* fontWinner = nullptr;

    // Rules are visited in file order and a tie is accepted (>=), which is
    // exactly the "later rule wins" tiebreak. Each attribute has its own winner
    // so that a colour rule and a fontStyle-only rule can both apply, which is
    // how real themes are written.
    for (const TokenColorRule& rule : rules_) {
        MatchKey key = kNoMatch;
        for (const ScopeSelector& selector : rule.selectors) {
            const MatchKey candidate = selector.match(scopeStack);
            if (candidate > key) key = candidate;
        }
        if (key == kNoMatch) continue;

        if (rule.delta.hasFg && key >= fgKey) {
            fgKey = key;
            fgWinner = &rule.delta;
        }
        if (rule.delta.hasBg && key >= bgKey) {
            bgKey = key;
            bgWinner = &rule.delta;
        }
        if (rule.delta.hasFontStyle && key >= fontKey) {
            fontKey = key;
            fontWinner = &rule.delta;
        }
    }

    Style style = palette_.front();
    if (fgWinner != nullptr) {
        style.fg = fgWinner->fg;
        style.hasFg = true;
    }
    if (bgWinner != nullptr) {
        style.bg = bgWinner->bg;
        style.hasBg = true;
    }
    if (fontWinner != nullptr) style.fontStyle = fontWinner->fontStyle;
    return intern(style);
}

StyleId Theme::resolve(std::span<const std::string_view> scopeStack) const {
    const uint64_t hash = hashScopeStack(scopeStack);
    {
        const auto it = cache_.find(hash);
        if (it != cache_.end()) {
            for (const CacheEntry& entry : it->second) {
                if (sameStack(entry.stack, scopeStack)) return entry.id;
            }
        }
    }

    const StyleId id = resolveUncached(scopeStack);

    // Drop the whole memo table on overflow. Must happen before the insert below,
    // and note that no iterator or reference into cache_ is alive here.
    if (cacheEntries_ >= kMaxCacheEntries) {
        cache_.clear();
        cacheEntries_ = 0;
    }
    CacheEntry entry;
    entry.stack.reserve(scopeStack.size());
    for (const std::string_view& scope : scopeStack) entry.stack.emplace_back(scope);
    entry.id = id;
    cache_[hash].push_back(std::move(entry));
    ++cacheEntries_;
    return id;
}

const Style& Theme::styleAt(StyleId id) const noexcept {
    if (id < 0 || static_cast<size_t>(id) >= palette_.size()) return palette_.front();
    return palette_[static_cast<size_t>(id)];
}

std::optional<Rgba> Theme::uiColor(std::string_view key) const {
    const auto it = std::lower_bound(uiColors_.begin(), uiColors_.end(), key,
                                     [](const UiColor& entry, std::string_view probe) {
                                         return std::string_view(entry.key) < probe;
                                     });
    if (it == uiColors_.end() || std::string_view(it->key) != key) return std::nullopt;
    return it->color;
}

void Theme::clearCache() const {
    cache_.clear();
    cacheEntries_ = 0;
}

}  // namespace ide::theme
