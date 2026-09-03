#pragma once

// Why this file exists
// -------------------
// The find bar needs to search a Document, and a Document is a piece table: its
// bytes are not contiguous, so `memmem` over a pointer is not available. Doing
// this in the UI layer would mean materializing the whole file as one string --
// exactly what the piece table exists to avoid.
//
// So searching happens here, in the text lane, over a sliding window read
// through Document::copyOut. Window size is fixed and small, overlap is
// needle.size() - 1, so a match that straddles a window boundary is still found
// and memory never scales with the document.
//
// Scope, deliberately: LITERAL search only. Regex search belongs to whatever
// owns an IRegexEngine (the app layer runs it per line), because making the
// text lane depend on the syntax lane would invert the module graph for a
// feature that does not need it.
//
// Case folding is ASCII-only and documented as such. Full Unicode case folding
// needs a case table this project does not ship, and a half-correct one that
// folds Latin-1 but not Greek is worse than an honest ASCII fold: the UI says
// "Aa" and means "ASCII".

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace ide::text {

class Document;

/// How to compare. Both flags off is a plain byte-for-byte substring search.
struct SearchOptions {
  /// When false, ASCII A-Z folds to a-z on both sides. Bytes >= 0x80 are never
  /// folded, so UTF-8 sequences compare exactly.
  bool caseSensitive = true;
  /// A match must not be adjacent to a word byte on either side. Word bytes are
  /// [0-9A-Za-z_] plus everything >= 0x80 -- treating UTF-8 continuation and
  /// lead bytes as word content is what makes "the" not match inside "thé".
  bool wholeWord = false;
};

/// One hit. `length` always equals the needle length: literal search cannot
/// produce a variable-length match.
struct Match {
  size_t offset = 0;
  size_t length = 0;

  bool operator==(const Match&) const = default;
};

/// Result of replaceAll. `tooLarge` means the span between the first and last
/// match exceeded the budget and NOTHING was changed -- so the UI can state the
/// real reason instead of silently doing half the job.
struct ReplaceResult {
  size_t replacements = 0;
  bool tooLarge = false;

  bool operator==(const ReplaceResult&) const = default;
};

/// Window size for the sliding read. Public because the tests assert that a
/// match straddling a window boundary is still found.
inline constexpr size_t kSearchWindowBytes = 64u * 1024u;

/// Default cap on the byte span replaceAll may rewrite in one undo group.
inline constexpr size_t kMaxReplaceSpanBytes = 8u * 1024u * 1024u;

/// First match starting at or after `from`. nullopt for an empty needle, a
/// needle longer than the document, or no match.
[[nodiscard]] std::optional<Match> findNext(const Document& doc, std::string_view needle,
                                           size_t from, const SearchOptions& options = {});

/// Last match starting strictly before `from`. Scans backwards in windows, so
/// "find previous" in a large file does not cost a full forward pass.
[[nodiscard]] std::optional<Match> findPrev(const Document& doc, std::string_view needle,
                                           size_t from, const SearchOptions& options = {});

/// Every match, in ascending offset order, non-overlapping (the scan resumes at
/// the end of each hit). `maxMatches` == 0 means unlimited.
[[nodiscard]] std::vector<Match> findAll(const Document& doc, std::string_view needle,
                                         const SearchOptions& options = {},
                                         size_t maxMatches = 0);

/// Replaces every match as ONE undo group.
///
/// Implementation note that is really a UX decision: the naive loop of N
/// Document::replace calls would create N undo groups, so undoing a Replace All
/// would take N taps. Instead this rewrites the single span from the first match
/// to the end of the last one, which is one edit, one undo. The cost is holding
/// that span twice in memory, hence `maxSpanBytes` and the `tooLarge` flag.
[[nodiscard]] ReplaceResult replaceAll(Document& doc, std::string_view needle,
                                       std::string_view replacement,
                                       const SearchOptions& options = {},
                                       size_t maxSpanBytes = kMaxReplaceSpanBytes);

}  // namespace ide::text
