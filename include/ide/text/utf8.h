#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

/// UTF-8 primitives over *contiguous* memory.
///
/// Why a separate namespace: Document has to do the same decoding over the
/// piece tree, where bytes are not contiguous. Keeping the byte-level rules in
/// one place means the tree walker and the contiguous walker cannot disagree
/// about what a codepoint boundary is.
namespace ide::text::utf8 {

/// A UTF-8 continuation byte is 10xxxxxx. Codepoint boundaries are exactly the
/// offsets whose byte is *not* a continuation byte.
inline bool isContinuation(char c) noexcept {
  return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
}

/// Bytes in the sequence introduced by `lead`: 1, 2, 3 or 4.
/// Returns 0 when `lead` cannot start a *well-formed* sequence: continuation
/// bytes (0x80..0xBF), 0xC0/0xC1 (always overlong) and 0xF5..0xFF (always past
/// U+10FFFF). Valid leads are therefore 0x00..0x7F, 0xC2..0xF4.
size_t sequenceLength(char lead) noexcept;

/// Result of decoding one sequence.
/// `length` is always >= 1 for a non-empty input so that decoding loops always
/// make progress, even on garbage. On failure `codepoint` is U+FFFD and
/// `length` is the "maximal subpart" length prescribed by Unicode 3.9.
struct Decoded {
  uint32_t codepoint = 0;
  size_t length = 0;
  bool valid = false;
};

/// Decodes the sequence starting at `offset`. Rejects overlong encodings,
/// surrogates (U+D800..U+DFFF) and anything above U+10FFFF.
Decoded decode(std::string_view bytes, size_t offset) noexcept;

/// Encodes `cp` into `out` (which must have room for 4 bytes) and returns the
/// number of bytes written, or 0 if `cp` is not a scalar value or `out` is too
/// small. Used by tests and by callers that build text programmatically.
size_t encode(uint32_t cp, std::span<char> out) noexcept;

/// Terminal cells occupied by `cp`: 0 for combining marks and zero-width
/// format characters, 2 for East Asian wide/fullwidth and emoji, else 1.
///
/// Tabs are *not* handled here: a tab's width depends on the current column,
/// so it is resolved by Document's display-column mapping.
///
/// The range tables are a curated approximation, not the full Unicode
/// character database: a generated table is a data-file concern for a later
/// phase. Everything in Latin/Greek/Cyrillic/CJK/emoji that a code editor
/// realistically meets is covered.
int displayWidth(uint32_t cp) noexcept;

/// Offset of the next codepoint boundary at or after `offset + 1`, clamped to
/// bytes.size(). Always strictly greater than `offset` while offset <
/// bytes.size(), so movement loops terminate.
size_t nextBoundary(std::string_view bytes, size_t offset) noexcept;

/// Offset of the previous codepoint boundary. If `offset` is *inside* a
/// sequence, returns the start of that sequence; if `offset` is already a
/// boundary, returns the start of the preceding codepoint. Always < `offset`
/// while offset > 0.
size_t prevBoundary(std::string_view bytes, size_t offset) noexcept;

/// Moves `offset` back to the start of the sequence containing it. A no-op on
/// a boundary. Never returns a mid-codepoint offset for well-formed input.
size_t snapToBoundary(std::string_view bytes, size_t offset) noexcept;

namespace detail {
/// Self-check for the displayWidth range tables: they must be sorted and
/// non-overlapping because lookup is a binary search. Asserted by unit tests
/// so a mis-typed table entry cannot silently corrupt column arithmetic.
bool widthTablesAreSorted() noexcept;
}  // namespace detail

}  // namespace ide::text::utf8
