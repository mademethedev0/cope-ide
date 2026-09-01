#include <ide/highlight/fallback_lexer.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ide::highlight {
namespace {

// --- byte classification -----------------------------------------------------
// All ASCII, all branch-free enough. Bytes >= 0x80 are treated as identifier
// bytes so a span boundary can never land inside a UTF-8 sequence.

[[nodiscard]] constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool isHexDigit(char c) noexcept {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] constexpr bool isAlpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool isHighByte(char c) noexcept {
    return static_cast<unsigned char>(c) >= 0x80u;
}

[[nodiscard]] constexpr bool isAsciiWordByte(char c) noexcept {
    return isAlpha(c) || isDigit(c) || c == '_';
}

/// Bytes that form an operator run. A run is emitted as one span, so "->", "<<="
/// and "==" all stay single tokens without a table of operators.
constexpr std::string_view kOperatorChars = "+-*/%=<>!&|^~?:@#$\\`";
/// Bytes that are always emitted one at a time.
constexpr std::string_view kPunctuationChars = "()[]{},;.";

[[nodiscard]] bool isOperatorByte(char c) noexcept {
    return kOperatorChars.find(c) != std::string_view::npos;
}
[[nodiscard]] bool isPunctuationByte(char c) noexcept {
    return kPunctuationChars.find(c) != std::string_view::npos;
}

[[nodiscard]] bool isIdentifierStart(char c, const LanguageProfile& p) noexcept {
    return isAlpha(c) || c == '_' || isHighByte(c) || (p.dollarIdentifiers && c == '$');
}

[[nodiscard]] bool isIdentifierPart(char c, const LanguageProfile& p) noexcept {
    if (isAsciiWordByte(c) || isHighByte(c)) return true;
    if (p.dollarIdentifiers && c == '$') return true;
    if (p.hyphenIdentifiers && (c == '-' || c == '?' || c == '!' || c == '*')) return true;
    return false;
}

/// Markup names: tag and attribute names allow '-', ':' and '.'.
[[nodiscard]] bool isMarkupNameByte(char c) noexcept {
    return isAsciiWordByte(c) || isHighByte(c) || c == '-' || c == ':' || c == '.';
}

// --- small string helpers ----------------------------------------------------

[[nodiscard]] bool startsWith(std::string_view text, size_t pos, std::string_view prefix) noexcept {
    if (prefix.empty() || pos > text.size()) return false;
    if (text.size() - pos < prefix.size()) return false;
    return text.compare(pos, prefix.size(), prefix) == 0;
}

/// First offset >= `from` where `needle` occurs, or text.size() when absent.
[[nodiscard]] size_t findFrom(std::string_view text, size_t from, std::string_view needle) noexcept {
    if (needle.empty() || from > text.size()) return text.size();
    for (size_t i = from; i + needle.size() <= text.size(); ++i) {
        if (text.compare(i, needle.size(), needle) == 0) return i;
    }
    return text.size();
}

/// End of the UTF-8 sequence starting at `pos`; always > pos for pos < size.
/// A malformed sequence advances one byte, which keeps the lexer total.
[[nodiscard]] size_t utf8Advance(std::string_view text, size_t pos) noexcept {
    if (pos >= text.size()) return text.size();
    size_t next = pos + 1;
    while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0u) == 0x80u) ++next;
    return next;
}

/// Case-insensitive ASCII comparison, for extension lookup only.
[[nodiscard]] bool iequalsAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

/// Whole-word set membership. Linear with a length + first-byte pre-filter,
/// which rejects almost every candidate before the string compare.
[[nodiscard]] bool containsWord(std::span<const std::string_view> words,
                                std::string_view word) noexcept {
    if (word.empty()) return false;
    for (const std::string_view candidate : words) {
        if (candidate.size() != word.size()) continue;
        if (candidate.empty() || candidate[0] != word[0]) continue;
        if (candidate == word) return true;
    }
    return false;
}

// --- span emitter ------------------------------------------------------------

/// Guarantees the tiling invariant by construction: every span starts exactly
/// where the previous one ended, empty spans are dropped, and out-of-order
/// requests are ignored rather than corrupting the output. Adjacent spans with
/// the same scope are merged, which typically halves the span count.
struct Sink {
    std::vector<FallbackSpan>* out = nullptr;
    size_t last = 0;

    void produce(size_t end, std::string_view scope) {
        if (out == nullptr || end <= last) return;
        if (!out->empty() && out->back().scope == scope) {
            out->back().end = end;
        } else {
            out->push_back(FallbackSpan{last, end, scope});
        }
        last = end;
    }
};

// --- scanners ----------------------------------------------------------------

/// End of the escape sequence whose backslash is at `pos`. Handles \xHH, \uHHHH,
/// \u{HHHH}, \UHHHHHHHH, octal \NNN and "\<one codepoint>" (so an escaped
/// multibyte character is never split).
[[nodiscard]] size_t scanEscape(std::string_view text, size_t pos) noexcept {
    const size_t n = text.size();
    size_t i = pos + 1;
    if (i >= n) return n;  // trailing backslash
    const char c = text[i];
    if (c == 'x' || c == 'X') {
        ++i;
        size_t j = i;
        while (j < n && j - i < 8u && isHexDigit(text[j])) ++j;
        return (j > i) ? j : i;
    }
    if (c == 'u' || c == 'U') {
        ++i;
        if (i < n && text[i] == '{') {
            size_t j = i + 1;
            while (j < n && text[j] != '}') ++j;
            return (j < n) ? j + 1 : j;
        }
        const size_t want = (c == 'u') ? 4u : 8u;
        size_t j = i;
        while (j < n && j - i < want && isHexDigit(text[j])) ++j;
        return (j > i) ? j : i;
    }
    if (c >= '0' && c <= '7') {
        size_t j = i;
        while (j < n && j - i < 3u && text[j] >= '0' && text[j] <= '7') ++j;
        return j;
    }
    return utf8Advance(text, i);
}

/// End of the number starting at `pos`, which is either a digit or a '.'
/// followed by a digit. Covers decimal, 0x, 0b, 0o, C-style octal, floats with
/// exponents, '_' (and optionally '\'') digit separators and trailing type
/// suffixes (ull, f, i32, j, ...).
[[nodiscard]] size_t scanNumber(std::string_view text, size_t pos,
                                const LanguageProfile& p) noexcept {
    const size_t n = text.size();
    const bool tick = p.apostropheDigitSeparator;
    const auto isSep = [tick](char c) noexcept { return c == '_' || (tick && c == '\''); };
    size_t i = pos;

    const bool radix = (text[i] == '0') && (i + 1u < n);
    const char marker = radix ? text[i + 1u] : '\0';
    if (radix && (marker == 'x' || marker == 'X')) {
        i += 2u;
        while (i < n && (isHexDigit(text[i]) || isSep(text[i]))) ++i;
    } else if (radix && (marker == 'b' || marker == 'B')) {
        i += 2u;
        while (i < n && (text[i] == '0' || text[i] == '1' || isSep(text[i]))) ++i;
    } else if (radix && (marker == 'o' || marker == 'O')) {
        i += 2u;
        while (i < n && ((text[i] >= '0' && text[i] <= '7') || isSep(text[i]))) ++i;
    } else {
        while (i < n && (isDigit(text[i]) || isSep(text[i]))) ++i;
        // A '.' belongs to the number unless an identifier follows it, so "1.5"
        // is one number while "1.max" is a number, a dot and a property.
        if (i < n && text[i] == '.') {
            const bool identFollows = (i + 1u < n) && (isAlpha(text[i + 1u]) || text[i + 1u] == '_');
            if (!identFollows) {
                ++i;
                while (i < n && (isDigit(text[i]) || isSep(text[i]))) ++i;
            }
        }
        if (i < n && (text[i] == 'e' || text[i] == 'E')) {
            size_t j = i + 1u;
            if (j < n && (text[j] == '+' || text[j] == '-')) ++j;
            if (j < n && isDigit(text[j])) {
                i = j;
                while (i < n && (isDigit(text[i]) || isSep(text[i]))) ++i;
            }
        }
    }
    // Type suffix: any ASCII word run. Deliberately greedy so 0xFFull, 1.5e-3f,
    // 1_000u8 and 3j are single numeric tokens instead of number+identifier.
    while (i < n && isAsciiWordByte(text[i])) ++i;
    return i;
}

struct StringOutcome {
    size_t end = 0;
    bool closed = false;
};

/// Emits the body of a string literal. The open delimiter (and any prefix
/// letters) must already have been produced by the caller; everything from the
/// sink's current position up to the close delimiter gets `scope`, with escape
/// sequences carved out as constant.character.escape and the close delimiter
/// itself as punctuation.definition.string.end.
[[nodiscard]] StringOutcome emitStringBody(std::string_view text, size_t bodyStart,
                                           std::string_view close, std::string_view scope,
                                           bool escapes, Sink& sink) {
    const size_t n = text.size();
    size_t i = bodyStart;
    while (i < n) {
        if (escapes && text[i] == '\\') {
            sink.produce(i, scope);
            const size_t end = scanEscape(text, i);
            sink.produce(end, scopes::kEscape);
            i = end;
            continue;
        }
        if (startsWith(text, i, close)) {
            const size_t end = i + close.size();
            sink.produce(i, scope);
            sink.produce(end, scopes::kStringEnd);
            return StringOutcome{end, true};
        }
        i = utf8Advance(text, i);
    }
    sink.produce(n, scope);
    return StringOutcome{n, false};
}

/// True when a comment starts at `pos`. Used to stop an operator run from
/// swallowing "//" or "/*" (as in "x =/*c*/1").
[[nodiscard]] bool commentStartsAt(std::string_view text, size_t pos,
                                   const LanguageProfile& p) noexcept {
    for (const std::string_view token : p.lineComments) {
        if (startsWith(text, pos, token)) return true;
    }
    for (const BlockCommentDelimiter& d : p.blockComments) {
        if (startsWith(text, pos, d.open)) return true;
    }
    return false;
}

/// Lexes one markup tag: "<name attr='v'/>", "</name>", "<!DOCTYPE x>",
/// "<?xml ... ?>". Returns the position after the tag, or the end of the line
/// when the tag does not close on it. Always advances by at least one byte.
[[nodiscard]] size_t lexMarkupTag(std::string_view text, size_t pos, Sink& sink) {
    const size_t n = text.size();
    size_t nameStart = pos + 1u;
    if (nameStart < n && (text[nameStart] == '/' || text[nameStart] == '!' ||
                          text[nameStart] == '?')) {
        ++nameStart;
    }
    size_t nameEnd = nameStart;
    while (nameEnd < n && isMarkupNameByte(text[nameEnd])) ++nameEnd;
    if (nameEnd == nameStart) {
        // "<" with no name behind it: a stray less-than in prose.
        sink.produce(pos + 1u, scopes::kPunctuationTag);
        return pos + 1u;
    }
    sink.produce(nameStart, scopes::kPunctuationTag);
    sink.produce(nameEnd, scopes::kTag);

    size_t i = nameEnd;
    while (i < n) {
        const char c = text[i];
        if (isWhitespaceByte(c)) {
            size_t j = i;
            while (j < n && isWhitespaceByte(text[j])) ++j;
            sink.produce(j, std::string_view{});
            i = j;
            continue;
        }
        if (c == '>') {
            sink.produce(i + 1u, scopes::kPunctuationTag);
            return i + 1u;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1u;
            while (j < n && text[j] != c) j = utf8Advance(text, j);
            if (j < n) ++j;
            sink.produce(j, (c == '"') ? scopes::kStringDouble : scopes::kStringSingle);
            i = j;
            continue;
        }
        if (isAlpha(c) || c == '_' || c == ':' || isHighByte(c)) {
            size_t j = i;
            while (j < n && isMarkupNameByte(text[j])) ++j;
            sink.produce(j, scopes::kAttributeName);
            i = j;
            continue;
        }
        const size_t next = utf8Advance(text, i);
        sink.produce(next, scopes::kPunctuationTag);
        i = next;
    }
    return n;
}

// --- language profiles (pure data) -------------------------------------------

constexpr std::string_view kSlashLineComments[] = {"//"};
constexpr std::string_view kHashLineComments[] = {"#"};
constexpr std::string_view kSemicolonLineComments[] = {";"};
constexpr std::string_view kGenericLineComments[] = {"//", "#"};

constexpr BlockCommentDelimiter kCBlockComments[] = {{"/*", "*/"}};
constexpr BlockCommentDelimiter kLispBlockComments[] = {{"#|", "|#"}};
constexpr BlockCommentDelimiter kMarkupBlockComments[] = {{"<!--", "-->"}};

// Longest opener wins, so the order inside these tables does not matter.
// The C family spells its literal prefixes out instead of using
// stringPrefixLetters, because a digit ('8' of u8") must never be mistaken for
// the start of a prefix run in front of a number.
constexpr StringDelimiter kCStrings[] = {
    {"R\"(", ")\"", scopes::kStringOther, false, true},
    {"u8\"", "\"", scopes::kStringDouble, true, false},
    {"L\"", "\"", scopes::kStringDouble, true, false},
    {"u\"", "\"", scopes::kStringDouble, true, false},
    {"U\"", "\"", scopes::kStringDouble, true, false},
    {"\"", "\"", scopes::kStringDouble, true, false},
    {"L'", "'", scopes::kStringSingle, true, false},
    {"'", "'", scopes::kStringSingle, true, false},
    {"`", "`", scopes::kStringOther, true, false},
};
constexpr StringDelimiter kPythonStrings[] = {
    {"\"\"\"", "\"\"\"", scopes::kStringOther, true, true},
    {"'''", "'''", scopes::kStringOther, true, true},
    {"\"", "\"", scopes::kStringDouble, true, false},
    {"'", "'", scopes::kStringSingle, true, false},
};
constexpr StringDelimiter kShellStrings[] = {
    {"\"", "\"", scopes::kStringDouble, true, false},
    {"'", "'", scopes::kStringSingle, false, false},
    {"`", "`", scopes::kStringOther, true, false},
};
constexpr StringDelimiter kLispStrings[] = {
    {"\"", "\"", scopes::kStringDouble, true, false},
};
constexpr StringDelimiter kGenericStrings[] = {
    {"\"", "\"", scopes::kStringDouble, true, false},
    {"'", "'", scopes::kStringSingle, true, false},
    {"`", "`", scopes::kStringOther, true, false},
};

// The C-family keyword set is the union of C, C++, Java, C#, JS/TS, Go, Rust,
// Swift and Kotlin. Words that are commonly used as plain identifiers in one of
// those languages ("type", "in", "out", "ref", "select", "event", "end") are
// deliberately excluded: a keyword colour on a variable name is more damaging
// than a missing keyword colour.
constexpr std::string_view kCKeywords[] = {
    "alignas", "alignof", "and", "as", "asm", "assert", "async", "await", "break", "case",
    "catch", "class", "co_await", "co_return", "co_yield", "concept", "const", "const_cast",
    "consteval", "constexpr", "constinit", "continue", "decltype", "default", "defer", "delete",
    "do", "dynamic_cast", "elif", "else", "enum", "explicit", "export", "extends", "extern",
    "fallthrough", "final", "finally", "fn", "for", "foreach", "friend", "func", "function",
    "goto", "if", "impl", "implements", "import", "inline", "instanceof", "interface", "internal",
    "let", "macro", "mod", "module", "move", "mut", "mutable", "namespace", "native", "new",
    "noexcept", "not", "operator", "or", "override", "package", "private", "protected", "pub",
    "public", "raise", "readonly", "register", "reinterpret_cast", "requires", "return", "sealed",
    "self", "sizeof", "stackalloc", "static", "static_assert", "static_cast", "struct", "super",
    "switch", "synchronized", "template", "this", "throw", "throws", "trait", "transient", "try",
    "typedef", "typeid", "typename", "typeof", "union", "unsafe", "use", "using", "var", "virtual",
    "volatile", "where", "while", "yield",
};
constexpr std::string_view kCTypes[] = {
    "any", "auto", "bool", "boolean", "byte", "char", "char16_t", "char32_t", "char8_t",
    "double", "f32", "f64", "float", "i128", "i16", "i32", "i64", "i8", "int", "int16_t",
    "int32_t", "int64_t", "int8_t", "intptr_t", "isize", "long", "never", "number", "ptrdiff_t",
    "rune", "short", "signed", "size_t", "ssize_t", "str", "string", "u128", "u16", "u32", "u64",
    "u8", "uint16_t", "uint32_t", "uint64_t", "uint8_t", "uintptr_t", "unknown", "unsigned",
    "usize", "void", "wchar_t", "Box", "Boolean", "Character", "Double", "Float", "Integer",
    "Long", "Object", "Option", "Result", "Short", "String", "Vec",
};
constexpr std::string_view kCConstants[] = {
    "false", "Infinity", "NaN", "nil", "NO", "null", "NULL", "nullptr", "true", "undefined",
    "YES",
};

constexpr std::string_view kPythonKeywords[] = {
    "and", "as", "assert", "async", "await", "break", "case", "class", "continue", "def", "del",
    "do", "elif", "else", "elsif", "ensure", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "match", "module", "nonlocal", "not", "or", "pass", "raise",
    "require", "return", "then", "try", "unless", "while", "with", "yield",
};
constexpr std::string_view kPythonTypes[] = {
    "AssertionError", "AttributeError", "BaseException", "Exception", "ImportError", "IndexError",
    "IOError", "KeyError", "NotImplementedError", "OSError", "RuntimeError", "StopIteration",
    "TypeError", "ValueError", "ZeroDivisionError", "bool", "bytearray", "bytes", "complex",
    "dict", "float", "frozenset", "int", "list", "memoryview", "object", "set", "str", "tuple",
    "type",
};
constexpr std::string_view kPythonConstants[] = {
    "cls", "Ellipsis", "False", "None", "NotImplemented", "True", "false", "nil", "null",
    "self", "true",
};

constexpr std::string_view kShellKeywords[] = {
    "alias", "break", "case", "continue", "coproc", "declare", "do", "done", "elif", "else",
    "esac", "eval", "exec", "exit", "export", "fi", "for", "function", "if", "in", "let", "local",
    "readonly", "return", "select", "set", "shift", "source", "then", "time", "trap", "typeset",
    "unalias", "unset", "until", "wait", "while",
};
constexpr std::string_view kShellTypes[] = {
    "awk", "cat", "cd", "chmod", "cp", "curl", "echo", "find", "git", "grep", "head", "kill",
    "ls", "make", "mkdir", "mv", "printf", "pwd", "read", "rm", "sed", "sort", "tail", "test",
    "wc", "wget",
};
constexpr std::string_view kShellConstants[] = {"false", "true"};

constexpr std::string_view kLispKeywords[] = {
    "and", "begin", "case", "catch", "cond", "def", "defclass", "defconst", "defconstant",
    "defgeneric", "defmacro", "defmethod", "defn", "defpackage", "defparameter", "defprotocol",
    "defrecord", "defstruct", "deftype", "defun", "defvar", "do", "dolist", "doseq", "dotimes",
    "else", "finally", "flet", "fn", "for", "if", "if-let", "import", "labels", "lambda", "let",
    "let*", "letrec", "loop", "ns", "or", "progn", "quote", "recur", "require", "set!", "setf",
    "setq", "throw", "try", "unless", "use", "when", "when-let", "while",
};
constexpr std::string_view kLispTypes[] = {
    "append", "apply", "assoc", "atom", "car", "cdr", "concat", "concatenate", "cons", "consp",
    "equal", "eq", "eql", "filter", "format", "funcall", "gethash", "getf", "length", "list",
    "listp", "map", "mapcar", "member", "not", "nth", "null", "numberp", "prin1", "princ",
    "print", "reduce", "remove", "reverse", "stringp", "symbolp", "terpri", "vector", "zerop",
};
constexpr std::string_view kLispConstants[] = {"nil", "t", "false", "true"};

constexpr std::string_view kGenericKeywords[] = {
    "break", "case", "catch", "class", "const", "continue", "def", "default", "delete", "do",
    "elif", "else", "enum", "export", "extends", "finally", "for", "from", "func", "function",
    "if", "import", "include", "let", "module", "namespace", "new", "package", "private",
    "protected", "public", "require", "return", "static", "struct", "switch", "throw", "try",
    "use", "using", "var", "while",
};
constexpr std::string_view kGenericTypes[] = {
    "bool", "byte", "char", "double", "float", "int", "long", "short", "string", "void",
};
constexpr std::string_view kGenericConstants[] = {
    "False", "None", "no", "nil", "none", "null", "off", "on", "true", "false", "True",
    "undefined", "yes",
};

constexpr LanguageProfile kCFamilyProfile{
    .name = "c-family",
    .lineComments = kSlashLineComments,
    .blockComments = kCBlockComments,
    .strings = kCStrings,
    .keywords = kCKeywords,
    .types = kCTypes,
    .constants = kCConstants,
    .apostropheDigitSeparator = true,
    .preprocessorHash = true,
    .decoratorsAt = true,
    .bracketAnnotations = true,
};

constexpr LanguageProfile kPythonProfile{
    .name = "python-style",
    .lineComments = kHashLineComments,
    .blockComments = {},
    .strings = kPythonStrings,
    .keywords = kPythonKeywords,
    .types = kPythonTypes,
    .constants = kPythonConstants,
    .stringPrefixLetters = "rbfuRBFU",
    .decoratorsAt = true,
};

constexpr LanguageProfile kShellProfile{
    .name = "shell",
    .lineComments = kHashLineComments,
    .blockComments = {},
    .strings = kShellStrings,
    .keywords = kShellKeywords,
    .types = kShellTypes,
    .constants = kShellConstants,
    .dollarIdentifiers = true,
};

constexpr LanguageProfile kLispProfile{
    .name = "lisp",
    .lineComments = kSemicolonLineComments,
    .blockComments = kLispBlockComments,
    .strings = kLispStrings,
    .keywords = kLispKeywords,
    .types = kLispTypes,
    .constants = kLispConstants,
    .hyphenIdentifiers = true,
    .functionCallHeuristic = false,
    .leadingParenCalls = true,
    .propertyHeuristic = false,
};

constexpr LanguageProfile kMarkupProfile{
    .name = "markup",
    .lineComments = {},
    .blockComments = kMarkupBlockComments,
    .strings = {},
    .keywords = {},
    .types = {},
    .constants = {},
    .identifierScope = {},
    .numbers = false,
    .operators = false,
    .functionCallHeuristic = false,
    .propertyHeuristic = false,
    .markupTags = true,
    .markupEntities = true,
};

constexpr LanguageProfile kGenericProfile{
    .name = "generic",
    .lineComments = kGenericLineComments,
    .blockComments = kCBlockComments,
    .strings = kGenericStrings,
    .keywords = kGenericKeywords,
    .types = kGenericTypes,
    .constants = kGenericConstants,
};

constexpr const LanguageProfile* kAllProfiles[] = {
    &kCFamilyProfile, &kPythonProfile, &kShellProfile,
    &kLispProfile,    &kMarkupProfile, &kGenericProfile,
};

enum class ProfileKind : uint8_t { kCFamily, kPython, kShell, kLisp, kMarkup, kGeneric };

struct ExtensionEntry {
    std::string_view name;
    ProfileKind kind;
};

/// Extension (and bare file name) table. Anything absent lands on the generic
/// profile, which is why this list may stay incomplete without breaking output.
constexpr ExtensionEntry kExtensionTable[] = {
    // C family
    {"c", ProfileKind::kCFamily},        {"h", ProfileKind::kCFamily},
    {"cc", ProfileKind::kCFamily},       {"cpp", ProfileKind::kCFamily},
    {"cxx", ProfileKind::kCFamily},      {"c++", ProfileKind::kCFamily},
    {"hpp", ProfileKind::kCFamily},      {"hh", ProfileKind::kCFamily},
    {"hxx", ProfileKind::kCFamily},      {"inl", ProfileKind::kCFamily},
    {"ino", ProfileKind::kCFamily},      {"m", ProfileKind::kCFamily},
    {"mm", ProfileKind::kCFamily},       {"java", ProfileKind::kCFamily},
    {"cs", ProfileKind::kCFamily},       {"js", ProfileKind::kCFamily},
    {"jsx", ProfileKind::kCFamily},      {"mjs", ProfileKind::kCFamily},
    {"cjs", ProfileKind::kCFamily},      {"ts", ProfileKind::kCFamily},
    {"tsx", ProfileKind::kCFamily},      {"go", ProfileKind::kCFamily},
    {"rs", ProfileKind::kCFamily},       {"swift", ProfileKind::kCFamily},
    {"kt", ProfileKind::kCFamily},       {"kts", ProfileKind::kCFamily},
    {"scala", ProfileKind::kCFamily},    {"dart", ProfileKind::kCFamily},
    {"php", ProfileKind::kCFamily},      {"groovy", ProfileKind::kCFamily},
    {"gradle", ProfileKind::kCFamily},   {"proto", ProfileKind::kCFamily},
    {"glsl", ProfileKind::kCFamily},     {"hlsl", ProfileKind::kCFamily},
    {"metal", ProfileKind::kCFamily},    {"zig", ProfileKind::kCFamily},
    {"d", ProfileKind::kCFamily},        {"css", ProfileKind::kCFamily},
    {"scss", ProfileKind::kCFamily},     {"less", ProfileKind::kCFamily},
    {"json", ProfileKind::kCFamily},     {"jsonc", ProfileKind::kCFamily},
    {"json5", ProfileKind::kCFamily},    {"rc", ProfileKind::kCFamily},
    // python-ish: '#' comments
    {"py", ProfileKind::kPython},        {"pyi", ProfileKind::kPython},
    {"pyw", ProfileKind::kPython},       {"rb", ProfileKind::kPython},
    {"gemspec", ProfileKind::kPython},   {"rake", ProfileKind::kPython},
    {"pl", ProfileKind::kPython},        {"pm", ProfileKind::kPython},
    {"r", ProfileKind::kPython},         {"jl", ProfileKind::kPython},
    {"yaml", ProfileKind::kPython},      {"yml", ProfileKind::kPython},
    {"toml", ProfileKind::kPython},      {"ini", ProfileKind::kPython},
    {"cfg", ProfileKind::kPython},       {"conf", ProfileKind::kPython},
    {"properties", ProfileKind::kPython},{"cmake", ProfileKind::kPython},
    {"nim", ProfileKind::kPython},       {"ex", ProfileKind::kPython},
    {"exs", ProfileKind::kPython},       {"cmakelists.txt", ProfileKind::kPython},
    {"dockerfile", ProfileKind::kPython},{"gitignore", ProfileKind::kPython},
    // shell
    {"sh", ProfileKind::kShell},         {"bash", ProfileKind::kShell},
    {"zsh", ProfileKind::kShell},        {"ksh", ProfileKind::kShell},
    {"fish", ProfileKind::kShell},       {"bashrc", ProfileKind::kShell},
    {"zshrc", ProfileKind::kShell},      {"profile", ProfileKind::kShell},
    {"makefile", ProfileKind::kShell},   {"mk", ProfileKind::kShell},
    {"env", ProfileKind::kShell},
    // lisp
    {"lisp", ProfileKind::kLisp},        {"lsp", ProfileKind::kLisp},
    {"cl", ProfileKind::kLisp},          {"el", ProfileKind::kLisp},
    {"scm", ProfileKind::kLisp},         {"ss", ProfileKind::kLisp},
    {"rkt", ProfileKind::kLisp},         {"clj", ProfileKind::kLisp},
    {"cljs", ProfileKind::kLisp},        {"cljc", ProfileKind::kLisp},
    {"edn", ProfileKind::kLisp},         {"fnl", ProfileKind::kLisp},
    // markup
    {"html", ProfileKind::kMarkup},      {"htm", ProfileKind::kMarkup},
    {"xhtml", ProfileKind::kMarkup},     {"xml", ProfileKind::kMarkup},
    {"svg", ProfileKind::kMarkup},       {"vue", ProfileKind::kMarkup},
    {"svelte", ProfileKind::kMarkup},    {"xaml", ProfileKind::kMarkup},
    {"plist", ProfileKind::kMarkup},     {"xsl", ProfileKind::kMarkup},
    {"xslt", ProfileKind::kMarkup},      {"rss", ProfileKind::kMarkup},
    {"atom", ProfileKind::kMarkup},      {"md", ProfileKind::kMarkup},
    {"markdown", ProfileKind::kMarkup},  {"rst", ProfileKind::kMarkup},
    {"txt", ProfileKind::kMarkup},
};

[[nodiscard]] const LanguageProfile& profileFor(ProfileKind kind) noexcept {
    switch (kind) {
        case ProfileKind::kCFamily: return kCFamilyProfile;
        case ProfileKind::kPython: return kPythonProfile;
        case ProfileKind::kShell: return kShellProfile;
        case ProfileKind::kLisp: return kLispProfile;
        case ProfileKind::kMarkup: return kMarkupProfile;
        case ProfileKind::kGeneric: break;
    }
    return kGenericProfile;
}

}  // namespace

// --- profile accessors -------------------------------------------------------

const LanguageProfile& cFamilyProfile() noexcept { return kCFamilyProfile; }
const LanguageProfile& pythonProfile() noexcept { return kPythonProfile; }
const LanguageProfile& shellProfile() noexcept { return kShellProfile; }
const LanguageProfile& lispProfile() noexcept { return kLispProfile; }
const LanguageProfile& markupProfile() noexcept { return kMarkupProfile; }
const LanguageProfile& genericProfile() noexcept { return kGenericProfile; }

std::span<const LanguageProfile* const> allProfiles() noexcept {
    return std::span<const LanguageProfile* const>(kAllProfiles);
}

std::string_view extensionOfFileName(std::string_view fileName) noexcept {
    size_t nameStart = 0;
    for (size_t i = fileName.size(); i > 0; --i) {
        const char c = fileName[i - 1u];
        if (c == '/' || c == '\\') {
            nameStart = i;
            break;
        }
    }
    const std::string_view name = fileName.substr(nameStart);
    // A leading dot is part of the name (".bashrc"), not an extension separator.
    for (size_t i = name.size(); i > 1u; --i) {
        if (name[i - 1u] == '.') return name.substr(i);
    }
    return std::string_view{};
}

const LanguageProfile& profileForExtension(std::string_view extension) noexcept {
    std::string_view ext = extension;
    while (!ext.empty() && ext.front() == '.') ext.remove_prefix(1);
    if (ext.empty()) return kGenericProfile;
    for (const ExtensionEntry& entry : kExtensionTable) {
        if (iequalsAscii(entry.name, ext)) return profileFor(entry.kind);
    }
    return kGenericProfile;
}

const LanguageProfile& profileForFileName(std::string_view fileName) noexcept {
    // The whole base name is tried FIRST, so "CMakeLists.txt" is a build script
    // and not a text file, and "Makefile" / ".bashrc" resolve at all.
    size_t nameStart = 0;
    for (size_t i = fileName.size(); i > 0; --i) {
        const char c = fileName[i - 1u];
        if (c == '/' || c == '\\') {
            nameStart = i;
            break;
        }
    }
    std::string_view name = fileName.substr(nameStart);
    while (!name.empty() && name.front() == '.') name.remove_prefix(1);
    if (!name.empty()) {
        for (const ExtensionEntry& entry : kExtensionTable) {
            if (iequalsAscii(entry.name, name)) return profileFor(entry.kind);
        }
    }
    const std::string_view ext = extensionOfFileName(fileName);
    if (!ext.empty()) {
        for (const ExtensionEntry& entry : kExtensionTable) {
            if (iequalsAscii(entry.name, ext)) return profileFor(entry.kind);
        }
    }
    return kGenericProfile;
}

// --- the lexer ---------------------------------------------------------------

void FallbackLexer::lex(std::string_view text, FallbackState& state,
                        std::vector<FallbackSpan>& out, FallbackOptions options) const {
    out.clear();
    const LanguageProfile& p = *profile_;
    const size_t n = text.size();
    if (n == 0) return;

    Sink sink{&out, 0};
    size_t i = 0;

    // --- finish whatever the previous line left open ------------------------
    if (state.mode == CarryMode::kBlockComment) {
        const size_t index = static_cast<size_t>(state.delimiter);
        if (index >= p.blockComments.size() || p.blockComments[index].close.empty()) {
            state = FallbackState{};  // profile changed under us: recover, never hang
        } else {
            const std::string_view close = p.blockComments[index].close;
            const size_t at = findFrom(text, 0, close);
            if (at >= n) {
                sink.produce(n, scopes::kCommentBlock);
                return;  // still open, state unchanged
            }
            sink.produce(at + close.size(), scopes::kCommentBlock);
            i = at + close.size();
            state = FallbackState{};
            options.atLineStart = false;
        }
    } else if (state.mode == CarryMode::kString) {
        const size_t index = static_cast<size_t>(state.delimiter);
        if (index >= p.strings.size() || p.strings[index].close.empty()) {
            state = FallbackState{};
        } else {
            const StringDelimiter& d = p.strings[index];
            const StringOutcome outcome =
                emitStringBody(text, 0, d.close, d.scope, d.escapes && !state.rawString, sink);
            if (!outcome.closed) return;  // still open, state unchanged
            i = outcome.end;
            state = FallbackState{};
            options.atLineStart = false;
        }
    }

    bool lineStart = options.atLineStart;

    while (i < n) {
        const char c = text[i];

        // 1. whitespace runs carry no scope at all.
        if (isWhitespaceByte(c)) {
            size_t j = i;
            while (j < n && isWhitespaceByte(text[j])) ++j;
            sink.produce(j, std::string_view{});
            i = j;
            continue;
        }

        // 2. line comment: everything to the end of the line.
        {
            bool isLineComment = false;
            for (const std::string_view token : p.lineComments) {
                if (startsWith(text, i, token)) {
                    isLineComment = true;
                    break;
                }
            }
            if (isLineComment) {
                sink.produce(n, scopes::kCommentLine);
                i = n;
                break;
            }
        }

        // 3. block comment, possibly spanning lines.
        {
            size_t best = p.blockComments.size();
            size_t bestLength = 0;
            for (size_t k = 0; k < p.blockComments.size(); ++k) {
                const std::string_view open = p.blockComments[k].open;
                if (open.size() > bestLength && startsWith(text, i, open)) {
                    best = k;
                    bestLength = open.size();
                }
            }
            if (best < p.blockComments.size()) {
                const BlockCommentDelimiter& d = p.blockComments[best];
                const size_t from = i + d.open.size();
                const size_t at = d.close.empty() ? n : findFrom(text, from, d.close);
                if (at >= n) {
                    sink.produce(n, scopes::kCommentBlock);
                    if (!d.close.empty()) {
                        state.mode = CarryMode::kBlockComment;
                        state.delimiter = static_cast<uint8_t>(best);
                        state.rawString = false;
                    }
                    i = n;
                    break;
                }
                sink.produce(at + d.close.size(), scopes::kCommentBlock);
                i = at + d.close.size();
                lineStart = false;
                continue;
            }
        }

        // 4. string literal, with up to three prefix letters folded in.
        {
            size_t prefix = 0;
            if (!p.stringPrefixLetters.empty()) {
                while (prefix < 3u && i + prefix < n &&
                       p.stringPrefixLetters.find(text[i + prefix]) != std::string_view::npos) {
                    ++prefix;
                }
            }
            size_t best = p.strings.size();
            size_t bestLength = 0;
            size_t bestAt = i;
            for (size_t k = 0; k <= prefix; ++k) {
                const size_t at = i + k;
                if (at >= n) break;
                for (size_t s = 0; s < p.strings.size(); ++s) {
                    const std::string_view open = p.strings[s].open;
                    if (open.size() > bestLength && startsWith(text, at, open)) {
                        best = s;
                        bestLength = open.size();
                        bestAt = at;
                    }
                }
                if (best < p.strings.size()) break;
            }
            if (best < p.strings.size()) {
                const StringDelimiter& d = p.strings[best];
                bool escapes = d.escapes;
                for (size_t k = i; k < bestAt; ++k) {
                    if (text[k] == 'r' || text[k] == 'R') escapes = false;
                }
                // Prefix letters keep the string scope; the open delimiter is
                // punctuation.definition.string.begin, so themes that give
                // quotes their own colour (the majority) render them.
                sink.produce(bestAt, d.scope);
                sink.produce(bestAt + d.open.size(), scopes::kStringBegin);
                const StringOutcome outcome =
                    emitStringBody(text, bestAt + d.open.size(), d.close, d.scope, escapes, sink);
                if (!outcome.closed) {
                    if (d.multiline) {
                        state.mode = CarryMode::kString;
                        state.delimiter = static_cast<uint8_t>(best);
                        state.rawString = !escapes;
                    }
                    i = n;
                    break;
                }
                i = outcome.end;
                lineStart = false;
                continue;
            }
        }

        // 5. preprocessor directive: '#' as the first non-blank byte of a line.
        if (p.preprocessorHash && lineStart && c == '#') {
            size_t nameStart = i + 1u;
            while (nameStart < n && isWhitespaceByte(text[nameStart])) ++nameStart;
            size_t nameEnd = nameStart;
            while (nameEnd < n && (isAlpha(text[nameEnd]) || text[nameEnd] == '_')) ++nameEnd;
            sink.produce(nameEnd, scopes::kPreprocessor);
            const std::string_view directive = text.substr(nameStart, nameEnd - nameStart);
            i = nameEnd;
            if (directive == "include" || directive == "import" || directive == "include_next") {
                size_t at = i;
                while (at < n && isWhitespaceByte(text[at])) ++at;
                if (at < n && text[at] == '<') {
                    size_t end = at + 1u;
                    while (end < n && text[end] != '>') ++end;
                    if (end < n) ++end;
                    sink.produce(at, std::string_view{});
                    sink.produce(end, scopes::kStringOther);
                    i = end;
                }
            }
            lineStart = false;
            continue;
        }

        // 6. decorator / annotation: @name, @name.attr.
        if (p.decoratorsAt && c == '@' && i + 1u < n && isIdentifierStart(text[i + 1u], p)) {
            size_t j = i + 1u;
            while (j < n && (isIdentifierPart(text[j], p) || text[j] == '.')) ++j;
            sink.produce(j, scopes::kDecorator);
            i = j;
            lineStart = false;
            continue;
        }

        // 7. numbers.
        if (p.numbers && (isDigit(c) || (c == '.' && i + 1u < n && isDigit(text[i + 1u])))) {
            const size_t end = scanNumber(text, i, p);
            sink.produce(end, scopes::kNumeric);
            i = (end > i) ? end : i + 1u;
            lineStart = false;
            continue;
        }

        // 8. markup tags and entities.
        if (p.markupTags && c == '<') {
            const size_t end = lexMarkupTag(text, i, sink);
            i = (end > i) ? end : i + 1u;
            lineStart = false;
            continue;
        }
        if (p.markupEntities && c == '&') {
            size_t j = i + 1u;
            if (j < n && text[j] == '#') ++j;
            size_t k = j;
            while (k < n && (isAlpha(text[k]) || isDigit(text[k]))) ++k;
            if (k > j && k < n && text[k] == ';') {
                sink.produce(k + 1u, scopes::kEscape);
                i = k + 1u;
                lineStart = false;
                continue;
            }
        }

        // 9. bracket annotation: [Name] as the first token of a line.
        if (p.bracketAnnotations && lineStart && c == '[') {
            size_t j = i + 1u;
            while (j < n && (isAsciiWordByte(text[j]) || text[j] == '.')) ++j;
            if (j > i + 1u && j < n && text[j] == ']') {
                sink.produce(i + 1u, scopes::kPunctuationAnnotation);
                sink.produce(j, scopes::kTag);
                sink.produce(j + 1u, scopes::kPunctuationAnnotation);
                i = j + 1u;
                lineStart = false;
                continue;
            }
        }

        // 10. identifiers, keywords, types, constants and the two heuristics
        //     that buy the most perceived quality.
        if (isIdentifierStart(c, p)) {
            size_t j = i + 1u;
            while (j < n && isIdentifierPart(text[j], p)) ++j;
            const std::string_view word = text.substr(i, j - i);
            std::string_view scope = p.identifierScope;
            const bool afterDot =
                p.propertyHeuristic && i > 0 &&
                (text[i - 1u] == '.' || (i >= 2u && text[i - 2u] == '-' && text[i - 1u] == '>'));
            const bool beforeParen = p.functionCallHeuristic && j < n && text[j] == '(';
            const bool afterParen = p.leadingParenCalls && i > 0 && text[i - 1u] == '(';
            // Order matters and is not arbitrary:
            //   * after a '.' a word is never a keyword ('promise.catch(' is a
            //     method), so the dot suppresses the keyword lookup;
            //   * a call beats a property, so 'obj.run()' colours 'run' as a
            //     function like every real grammar does, while 'obj.field'
            //     stays a property;
            //   * a call beats a type, so python's 'int(x)' is a call and a bare
            //     'int' is still a type.
            if (p.dollarIdentifiers && word.front() == '$') {
                scope = scopes::kVariable;
            } else if (afterDot) {
                scope = beforeParen ? scopes::kFunction : scopes::kProperty;
            } else if (containsWord(p.keywords, word)) {
                scope = scopes::kKeyword;
            } else if (beforeParen || afterParen) {
                scope = scopes::kFunction;
            } else if (containsWord(p.types, word)) {
                scope = scopes::kStorageType;
            } else if (containsWord(p.constants, word)) {
                scope = scopes::kLanguageConstant;
            }
            sink.produce(j, scope);
            i = j;
            lineStart = false;
            continue;
        }

        // 11. operators and punctuation.
        if (p.operators) {
            if (isPunctuationByte(c)) {
                // Class-specific scopes: brackets, separators, terminators and
                // accessors each carry a deeper scope name than bare
                // "punctuation", which strictly widens theme coverage (prefix
                // matching) without changing what a broad-rule theme sees.
                std::string_view scope = scopes::kPunctuationBracket;
                if (c == ',') {
                    scope = scopes::kPunctuationComma;
                } else if (c == ';') {
                    scope = scopes::kPunctuationSemicolon;
                } else if (c == '.') {
                    scope = scopes::kPunctuationAccessor;
                }
                sink.produce(i + 1u, scope);
                i += 1u;
                lineStart = false;
                continue;
            }
            if (isOperatorByte(c)) {
                size_t j = i;
                while (j < n && isOperatorByte(text[j]) && !commentStartsAt(text, j, p)) ++j;
                if (j <= i) j = i + 1u;  // unreachable: comments are matched first
                sink.produce(j, scopes::kOperator);
                i = j;
                lineStart = false;
                continue;
            }
        }

        // 12. anything else: one codepoint, unclassified.
        const size_t next = utf8Advance(text, i);
        sink.produce(next, std::string_view{});
        i = (next > i) ? next : i + 1u;
        lineStart = false;
    }

    // Structurally unreachable when the loop above is correct; keeps the tiling
    // invariant true even if it is not.
    sink.produce(n, std::string_view{});
}

std::vector<FallbackSpan> FallbackLexer::lexLine(std::string_view text, FallbackState& state,
                                                 FallbackOptions options) const {
    std::vector<FallbackSpan> out;
    lex(text, state, out, options);
    return out;
}

bool spansTile(std::span<const FallbackSpan> spans, size_t length) noexcept {
    size_t expected = 0;
    for (const FallbackSpan& span : spans) {
        if (span.begin != expected) return false;
        if (span.end <= span.begin) return false;
        expected = span.end;
    }
    return expected == length;
}

}  // namespace ide::highlight
