#pragma once

// Why this file exists
// -------------------
// Five previous attempts at this editor were abandoned for one reason: the
// output *looked* broken. The bring-up regex engine refuses roughly 15% of the
// patterns in the TextMate corpus (see the feature spec in ide/syntax/regex.h),
// so a grammar reliably produces scattered words with no scope at all. Those
// words render in the default colour, and a reader who sees random grey words
// inside otherwise coloured code concludes the whole engine is broken - even
// when 85% of it is right. Partial colour reads as *worse* than no colour.
//
// This lexer exists to make that impossible. It is a hand written, single pass,
// regex-free heuristic tokenizer: no grammar, no backtracking, no pattern
// compilation, nothing that can be "refused". It cannot be as correct as a
// TextMate grammar, but it is *total* - every byte of every line gets a
// classification - and it never fails to produce output.
//
// Three design rules follow from that job:
//
//   1. It emits REAL TextMate scope names (comment.line, string.quoted.double,
//      entity.name.function, ...). The theme resolver in ide/theme therefore
//      styles fallback output for free, with no theme changes and no second
//      colour table. This is the difference between a fallback that looks
//      native and one that looks bolted on.
//   2. Languages are DATA (LanguageProfile), not code. Adding a language is
//      adding a static table, never a new code path, so the number of ways the
//      lexer can be wrong does not grow with the number of languages.
//   3. Output is an exact tiling of the input: sorted, non-overlapping, gapless,
//      covering [0, text.size()) precisely. The highlighter splices this into
//      grammar output, and splicing is only safe if both sides tile.
//
// Carry-in state (FallbackState) is a 3-byte trivially copyable value with
// equality, so it can be stored per line exactly like the TextMate state and
// compared to stop incremental relayout early.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ide::highlight {

/// The complete set of scope names the fallback lexer can emit.
///
/// Every one of these is a real TextMate scope that the shipped themes already
/// carry rules for; nothing here is invented for this engine. Two of them are
/// deliberate extensions of the sanctioned minimum set:
///   * kDecorator is entity.name.function.decorator, which the theme resolver
///     matches with any "entity.name.function" rule (per-dot-segment prefix
///     matching), so decorators get the function colour like they do in VS Code.
///   * kAttributeName is entity.other.attribute-name, the single most widely
///     themed markup scope; without it HTML attributes render as prose.
namespace scopes {
inline constexpr std::string_view kCommentLine = "comment.line";
inline constexpr std::string_view kCommentBlock = "comment.block";
inline constexpr std::string_view kStringDouble = "string.quoted.double";
inline constexpr std::string_view kStringSingle = "string.quoted.single";
inline constexpr std::string_view kStringOther = "string.quoted.other";
inline constexpr std::string_view kEscape = "constant.character.escape";
inline constexpr std::string_view kNumeric = "constant.numeric";
inline constexpr std::string_view kLanguageConstant = "constant.language";
inline constexpr std::string_view kKeyword = "keyword.control";
inline constexpr std::string_view kOperator = "keyword.operator";
inline constexpr std::string_view kStorageType = "storage.type";
inline constexpr std::string_view kFunction = "entity.name.function";
inline constexpr std::string_view kDecorator = "entity.name.function.decorator";
inline constexpr std::string_view kTag = "entity.name.tag";
inline constexpr std::string_view kAttributeName = "entity.other.attribute-name";
inline constexpr std::string_view kProperty = "variable.other.property";
inline constexpr std::string_view kVariable = "variable.other";
/// Punctuation is never emitted as bare "punctuation": theme matching is
/// per-segment prefix, so a deeper name still matches a theme's broad
/// "punctuation" rule while ALSO matching themes that only style the
/// specific kind. Measured against the 60 shipped themes: bare
/// "punctuation" is colored by 28; "punctuation.definition.string" by 49;
/// "punctuation.definition.string.begin" by 56.
inline constexpr std::string_view kPunctuationBracket =
    "punctuation.definition.brackets";
inline constexpr std::string_view kPunctuationComma = "punctuation.separator.comma";
inline constexpr std::string_view kPunctuationSemicolon = "punctuation.terminator";
inline constexpr std::string_view kPunctuationAccessor = "punctuation.accessor";
inline constexpr std::string_view kPunctuationTag = "punctuation.definition.tag";
inline constexpr std::string_view kPunctuationAnnotation =
    "punctuation.definition.annotation";
inline constexpr std::string_view kStringBegin = "punctuation.definition.string.begin";
inline constexpr std::string_view kStringEnd = "punctuation.definition.string.end";
inline constexpr std::string_view kPreprocessor = "meta.preprocessor";
}  // namespace scopes

/// The whitespace definition used by every tier of the cascade: only these bytes
/// are "invisible" and therefore excluded from coverage metrics and from repair
/// region boundaries. Shared so the lexer, the highlighter and the quality
/// report can never disagree about what an unstyled byte is.
[[nodiscard]] inline constexpr bool isWhitespaceByte(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

/// One classified byte range of one line.
///
/// `scope` is empty for "no classification" (whitespace, and bytes no heuristic
/// claims). It always points at one of the static constants in `scopes` above,
/// so it is non-owning, cheap to copy and valid for the process lifetime.
struct FallbackSpan {
    size_t begin = 0;
    size_t end = 0;
    std::string_view scope;

    friend bool operator==(const FallbackSpan& a, const FallbackSpan& b) noexcept {
        return a.begin == b.begin && a.end == b.end && a.scope == b.scope;
    }
    friend bool operator!=(const FallbackSpan& a, const FallbackSpan& b) noexcept {
        return !(a == b);
    }
};

/// Which multi-line construct is open at a line boundary.
enum class CarryMode : uint8_t {
    kNone = 0,
    kBlockComment = 1,
    kString = 2,  ///< a delimiter declared `multiline` (""" ... """, R"( ... )")
};

/// Carry-in / carry-out state of the lexer: everything one line needs to know
/// about the previous one.
///
/// Trivially copyable and comparable on purpose - it is stored per line next to
/// the TextMate state, and incremental relayout stops as soon as a recomputed
/// end state equals the stored one.
struct FallbackState {
    CarryMode mode = CarryMode::kNone;
    /// Index into the profile's blockComments (kBlockComment) or strings
    /// (kString) table. Meaningless when mode == kNone.
    uint8_t delimiter = 0;
    /// The open string suppresses backslash escapes (python r"""..., C++ R"(...).
    /// Cannot be derived from the profile alone: the same delimiter may be raw
    /// or cooked depending on the literal's prefix letters.
    bool rawString = false;

    [[nodiscard]] bool clean() const noexcept { return mode == CarryMode::kNone; }

    friend bool operator==(const FallbackState& a, const FallbackState& b) noexcept {
        return a.mode == b.mode && a.delimiter == b.delimiter && a.rawString == b.rawString;
    }
    friend bool operator!=(const FallbackState& a, const FallbackState& b) noexcept {
        return !(a == b);
    }
};

static_assert(std::is_trivially_copyable_v<FallbackState>);

/// A block comment delimiter pair. `close` must never be empty: a construct that
/// cannot close would stay open for the rest of the file.
struct BlockCommentDelimiter {
    std::string_view open;
    std::string_view close;
};

/// A string-ish delimiter pair. Character literals are strings with a different
/// scope, which is exactly how TextMate grammars model them too.
struct StringDelimiter {
    std::string_view open;
    std::string_view close;
    std::string_view scope = scopes::kStringDouble;
    bool escapes = true;     ///< backslash escapes apply inside the body
    bool multiline = false;  ///< may stay open at end of line (carried in state)
};

/// Everything the lexer knows about one language family. Pure data: profiles are
/// static tables, so a new language costs a table entry and no new code path.
///
/// Spans point at static arrays with static storage duration; a LanguageProfile
/// is therefore a non-owning view and must not outlive them (all shipped
/// profiles are namespace-scope constants, so this is free).
struct LanguageProfile {
    std::string_view name;

    std::span<const std::string_view> lineComments;
    std::span<const BlockCommentDelimiter> blockComments;
    std::span<const StringDelimiter> strings;

    /// Word sets, looked up on the whole identifier so "iffy" is never "if".
    /// Scanned linearly with a length + first-byte pre-filter; real sets are
    /// under 200 entries and this runs once per identifier.
    std::span<const std::string_view> keywords;  ///< -> keyword.control
    std::span<const std::string_view> types;     ///< -> storage.type
    std::span<const std::string_view> constants; ///< -> constant.language

    /// Letters that may prefix a string literal (python "rbfuRBFU"). A run of at
    /// most three of them directly before a string opener is folded into the
    /// string, and an r/R in that run disables escapes. Languages whose prefixes
    /// contain digits (C++ u8") spell the openers out in `strings` instead, so a
    /// digit can never be mistaken for the start of a prefix run.
    std::string_view stringPrefixLetters;

    /// Scope for an identifier no heuristic claims. Empty leaves prose
    /// unclassified, which is what markup wants: colouring English text is the
    /// one thing that looks worse than not colouring it.
    std::string_view identifierScope = scopes::kVariable;

    bool dollarIdentifiers = false;   ///< '$' is an identifier byte (shell, php, js)
    bool hyphenIdentifiers = false;   ///< '-', '?', '!', '*' continue an identifier (lisp)
    bool apostropheDigitSeparator = false;  ///< 1'000 (C++)
    bool preprocessorHash = false;    ///< '#' as the first byte of a line is a directive
    bool decoratorsAt = false;        ///< '@name' is a decorator/annotation
    bool bracketAnnotations = false;  ///< '[Name]' at the start of a line is an annotation
    bool numbers = true;
    bool operators = true;            ///< classify operator and punctuation bytes
    bool functionCallHeuristic = true;   ///< identifier immediately before '(' is a call
    bool leadingParenCalls = false;      ///< identifier immediately after '(' is a call (lisp)
    bool propertyHeuristic = true;       ///< identifier immediately after '.' or '->' is a property
    bool markupTags = false;             ///< '<tag attr="v">' handling
    bool markupEntities = false;         ///< '&amp;' handling
};

// --- shipped profiles --------------------------------------------------------
// Six families cover the corpus well enough that the fallback never has to
// guess wildly; the generic one is a deliberately cautious middle ground.

/// C, C++, Java, C#, JS/TS, Go, Rust, Swift, Kotlin, PHP, ... : // and /* */,
/// double/single/backtick strings, u8"/L"/R"( prefixes, # preprocessor,
/// @annotations and [Attributes].
[[nodiscard]] const LanguageProfile& cFamilyProfile() noexcept;
/// Python, Ruby, Perl, YAML, TOML, CMake: # comments, triple quoted strings,
/// f/r/b string prefixes, @decorators.
[[nodiscard]] const LanguageProfile& pythonProfile() noexcept;
/// sh/bash/zsh/fish and Makefiles: # comments, $identifiers, literal '...'.
[[nodiscard]] const LanguageProfile& shellProfile() noexcept;
/// Lisp/Scheme/Clojure/Emacs Lisp: ; comments, #| |# blocks, hyphen identifiers,
/// and calls detected after '(' instead of before it.
[[nodiscard]] const LanguageProfile& lispProfile() noexcept;
/// HTML/XML/SVG/Vue/Markdown: tags, attributes, entities, <!-- -->. Numbers,
/// operators and bare identifiers are deliberately NOT classified, because
/// colouring prose is the failure mode this whole module exists to prevent.
[[nodiscard]] const LanguageProfile& markupProfile() noexcept;
/// Unknown extensions: // and # comments, /* */, the three quote forms, a tiny
/// near-universal keyword set. Never wrong in a spectacular way.
[[nodiscard]] const LanguageProfile& genericProfile() noexcept;

/// All shipped profiles, for tests and diagnostics.
[[nodiscard]] std::span<const LanguageProfile* const> allProfiles() noexcept;

/// The extension of a file name or path, without the dot and without any
/// directory part. Empty when the name has no extension ("Makefile", "LICENSE").
[[nodiscard]] std::string_view extensionOfFileName(std::string_view fileName) noexcept;

/// Profile for a bare extension (with or without a leading dot,
/// case-insensitive). TOTAL: an unknown or empty extension yields
/// genericProfile(), never a failure and never a null reference.
[[nodiscard]] const LanguageProfile& profileForExtension(std::string_view extension) noexcept;

/// Profile for a file name or path. Falls back to matching the whole name so
/// extensionless files ("Makefile", "CMakeLists.txt", ".bashrc") still land on a
/// sensible profile. TOTAL, like profileForExtension().
[[nodiscard]] const LanguageProfile& profileForFileName(std::string_view fileName) noexcept;

/// Per-call knobs. Kept out of LanguageProfile because they describe the *text*
/// being lexed, not the language.
struct FallbackOptions {
    /// True when `text` starts at the beginning of a logical line, i.e. only
    /// whitespace may precede it. Controls the two line-anchored heuristics
    /// (# directives and [Annotation]). The highlighter passes false when it
    /// lexes a repair region out of the middle of a line.
    bool atLineStart = true;
};

/// Hand written heuristic lexer. Holds nothing but a profile pointer, so copying
/// it is free and one instance per open document is the intended usage.
class FallbackLexer {
public:
    FallbackLexer() noexcept : profile_(&genericProfile()) {}
    explicit FallbackLexer(const LanguageProfile& profile) noexcept : profile_(&profile) {}

    [[nodiscard]] const LanguageProfile& profile() const noexcept { return *profile_; }
    void setProfile(const LanguageProfile& profile) noexcept { profile_ = &profile; }

    /// Lexes one line, or one fragment of one line, in a single forward pass.
    ///
    /// `state` is read as the carry-in state and overwritten with the carry-out
    /// state. `out` is cleared first and is guaranteed to be a sorted,
    /// non-overlapping, gapless tiling of [0, text.size()); an empty input
    /// yields no spans. Never throws, never allocates beyond `out`.
    void lex(std::string_view text, FallbackState& state, std::vector<FallbackSpan>& out,
             FallbackOptions options = {}) const;

    /// Allocating convenience wrapper for tests and one-off callers.
    [[nodiscard]] std::vector<FallbackSpan> lexLine(std::string_view text, FallbackState& state,
                                                    FallbackOptions options = {}) const;

private:
    const LanguageProfile* profile_;
};

/// True when `spans` is exactly a sorted, non-overlapping, gapless tiling of
/// [0, length): every span non-empty, spans[0].begin == 0, spans[i].end ==
/// spans[i+1].begin, spans.back().end == length. An empty span list is a valid
/// tiling of length 0 only. This is the invariant the whole cascade rests on.
[[nodiscard]] bool spansTile(std::span<const FallbackSpan> spans, size_t length) noexcept;

}  // namespace ide::highlight
