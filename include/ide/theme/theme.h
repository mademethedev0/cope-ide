#pragma once

// Why this file exists
// -------------------
// One theme file drives the whole application: token colours for the editor and
// the ~144 UI colours for everything around it. Loading it once into an
// immutable object with an interned style palette is what lets phase 4 ship a
// screen of tokens across JNI as [start, length, styleId] integer triples with
// the palette transferred a single time.
//
// The input format is VS Code's JSON theme (all 60 files in textmate/themes/ use
// it): { name, displayName, type, colors, tokenColors, semanticTokenColors }.
// Loading is *forgiving by design* — real themes contain null colours, empty
// fontStyle strings and entries with no settings. A malformed entry is skipped,
// never fatal; only a non-object root fails the load.
//
// Everything here is host-independent: no filesystem, no JNI. The caller hands
// over parsed JSON, so the same code path serves the CLI, the Android app and
// the tests.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ide/syntax/json_lite.h>
#include <ide/theme/scope_selector.h>
#include <ide/theme/style.h>

namespace ide {
/// The theme API is specified in terms of `ide::json`, while the parser this
/// project vendors lives in `ide::syntax::json` (it was written for grammars
/// first). One alias keeps both spellings valid and means the eventual swap to
/// nlohmann/json is a single-line change here.
namespace json = ::ide::syntax::json;
}  // namespace ide

namespace ide::theme {

/// One entry of the theme's tokenColors array: the selectors it fires on and the
/// partial style it contributes. Kept in file order — that order is the final
/// tiebreak when two rules match with identical specificity.
struct TokenColorRule {
    std::vector<ScopeSelector> selectors;
    StyleDelta delta;
    std::string scopeText;  ///< original scope text(s), joined by ", "; diagnostics only
};

/// One entry of semanticTokenColors. Parsed so that a later phase can implement
/// semantic highlighting without touching the loader; nothing applies these yet.
struct SemanticRule {
    std::string selector;  ///< e.g. "class.builtin:python", "*.defaultLibrary"
    StyleDelta delta;
};

/// One entry of the theme's `colors` map, i.e. an application UI colour.
struct UiColor {
    std::string key;
    Rgba color{};
};

/// UI colour keys phase 4 needs. Any of them can legitimately be absent — of the
/// 60 shipped themes only "editor.background" is present in all of them — so
/// uiColor() returns nullopt and the caller must have its own fallback.
inline constexpr std::string_view kUiEditorBackground = "editor.background";
inline constexpr std::string_view kUiEditorForeground = "editor.foreground";
inline constexpr std::string_view kUiLineNumberForeground = "editorLineNumber.foreground";
inline constexpr std::string_view kUiSelectionBackground = "editor.selectionBackground";
inline constexpr std::string_view kUiSideBarBackground = "sideBar.background";
inline constexpr std::string_view kUiStatusBarBackground = "statusBar.background";
inline constexpr std::string_view kUiStatusBarForeground = "statusBar.foreground";

/// A loaded, immutable theme plus its interned style palette.
///
/// resolve() is logically const but memoises into mutable state, so a Theme is
/// NOT safe to share across threads without external synchronisation. Copying a
/// Theme is cheap enough for the CLI and copies the memo tables along.
class Theme {
public:
    /// An empty theme: no rules, no UI colours, and a palette holding exactly one
    /// entry — an unspecified default style at kDefaultStyleId.
    Theme();

    /// Loads from parsed JSON. Fails (nullopt, *error filled) only when `root` is
    /// not a JSON object; every other defect is skipped. `error` may be null and
    /// is written only on failure.
    [[nodiscard]] static std::optional<Theme> fromJson(const ide::json::Value& root, std::string* error);

    /// Convenience wrapper: parse `jsonText` then fromJson(). Adds the JSON
    /// parser's message and byte offset to *error on a syntax error.
    [[nodiscard]] static std::optional<Theme> fromJsonText(std::string_view jsonText, std::string* error);

    /// The theme's `name` field, falling back to `displayName` when absent.
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    /// The theme's `displayName`, falling back to `name` when absent.
    [[nodiscard]] std::string_view displayName() const noexcept { return displayName_; }
    /// From `type`: "light" is light, "dark" is dark. When `type` is missing or
    /// unrecognised, inferred from the luminance of editor.background, and when
    /// that is missing too, dark (the majority of shipped themes).
    [[nodiscard]] bool isDark() const noexcept { return isDark_; }
    /// The theme's `semanticHighlighting` flag; false when absent. Advisory only.
    [[nodiscard]] bool semanticHighlightingEnabled() const noexcept { return semanticHighlighting_; }

    /// Style for a token whose scope stack is `scopeStack`, OUTERMOST FIRST
    /// (index 0 the root "source.x", last element the innermost scope) — the
    /// order ide::syntax::ScopeStackTable::resolve() produces.
    ///
    /// The returned id indexes the palette; equal styles always yield the same
    /// id. Repeated calls with the same stack contents hit a memo table and do
    /// not allocate.
    [[nodiscard]] StyleId resolve(std::span<const std::string_view> scopeStack) const;

    /// Palette lookup. An out-of-range id yields the default style rather than
    /// undefined behaviour: bad ids arrive from the Java side eventually.
    ///
    /// The returned reference, and the span returned by palette(), are
    /// invalidated by the next resolve() call, which may grow the palette. Copy
    /// the Style if you need to keep it; it is a handful of bytes.
    [[nodiscard]] const Style& styleAt(StyleId id) const noexcept;
    [[nodiscard]] size_t paletteSize() const noexcept { return palette_.size(); }
    /// The whole palette in StyleId order; palette()[0] is the default style.
    [[nodiscard]] std::span<const Style> palette() const noexcept { return palette_; }
    /// Base style for unmatched tokens: editor.foreground/editor.background,
    /// overlaid with the tokenColors entry that has no scope, if any.
    [[nodiscard]] Style defaultStyle() const noexcept { return palette_.front(); }

    /// An application UI colour from the theme's `colors` map. nullopt when the
    /// key is absent or its value was not a parsable colour (both happen: many
    /// themes map keys to JSON null).
    [[nodiscard]] std::optional<Rgba> uiColor(std::string_view key) const;
    /// All UI colours, sorted by key. Lets the CLI dump a theme.
    [[nodiscard]] std::span<const UiColor> uiColors() const noexcept { return uiColors_; }

    /// tokenColors rules in file order, and semanticTokenColors entries in file
    /// order. Diagnostics, and the hook a later semantic-highlighting phase uses.
    [[nodiscard]] std::span<const TokenColorRule> rules() const noexcept { return rules_; }
    [[nodiscard]] std::span<const SemanticRule> semanticRules() const noexcept { return semanticRules_; }

    /// Number of distinct scope stacks currently memoised. Tests only.
    [[nodiscard]] size_t cacheEntryCount() const noexcept { return cacheEntries_; }
    /// Drops the memo table. The palette is untouched, so existing StyleIds stay
    /// valid. Only useful to measure cold-path costs.
    void clearCache() const;

    /// Memo table cap. On overflow the table is dropped whole: a bounded, O(1)
    /// eviction that cannot invalidate a StyleId, which matters because Java may
    /// still be holding ids from a previous frame.
    static constexpr size_t kMaxCacheEntries = 8192;

private:
    /// A memo entry keyed by the stack contents, kept in a hash bucket so that a
    /// 64-bit hash collision degrades into a comparison, not a wrong colour.
    struct CacheEntry {
        std::vector<std::string> stack;
        StyleId id = kDefaultStyleId;
    };

    void setDefaultStyle(const Style& style);
    [[nodiscard]] StyleId intern(const Style& style) const;
    [[nodiscard]] StyleId resolveUncached(std::span<const std::string_view> scopeStack) const;

    std::string name_;
    std::string displayName_;
    bool isDark_ = true;
    bool semanticHighlighting_ = false;

    std::vector<UiColor> uiColors_;  ///< sorted by key, binary searched
    std::vector<TokenColorRule> rules_;
    std::vector<SemanticRule> semanticRules_;

    mutable std::vector<Style> palette_;                                      ///< index == StyleId
    mutable std::unordered_map<StyleKey, StyleId, StyleKeyHash> paletteIndex_;
    mutable std::unordered_map<uint64_t, std::vector<CacheEntry>> cache_;
    mutable size_t cacheEntries_ = 0;
};

}  // namespace ide::theme
