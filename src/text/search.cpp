#include <ide/text/search.h>

#include <algorithm>
#include <span>
#include <string>

#include <ide/text/document.h>

namespace ide::text {
namespace {

constexpr char lowerAscii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool isWordByte(char c) noexcept {
  const unsigned char u = static_cast<unsigned char>(c);
  return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_' ||
         u >= 0x80u;
}

/// Does `needle` sit at `at` inside `hay`? Bounds-checked, so callers may pass
/// any index.
bool equalsAt(std::string_view hay, size_t at, std::string_view needle, bool caseSensitive) {
  if (needle.size() > hay.size() || at > hay.size() - needle.size()) {
    return false;
  }
  if (caseSensitive) {
    return hay.compare(at, needle.size(), needle) == 0;
  }
  for (size_t i = 0; i < needle.size(); ++i) {
    if (lowerAscii(hay[at + i]) != lowerAscii(needle[i])) {
      return false;
    }
  }
  return true;
}

/// Word-boundary test against the *document*, not the window: the byte before a
/// match at the window start lies outside the buffer.
bool wordBounded(const Document& doc, size_t offset, size_t length) {
  if (offset > 0 && isWordByte(doc.byteAt(offset - 1))) {
    return false;
  }
  const size_t end = offset + length;
  if (end < doc.size() && isWordByte(doc.byteAt(end))) {
    return false;
  }
  return true;
}

}  // namespace

std::optional<Match> findNext(const Document& doc, std::string_view needle, size_t from,
                              const SearchOptions& options) {
  const size_t n = needle.size();
  const size_t size = doc.size();
  if (n == 0 || n > size) {
    return std::nullopt;
  }
  // Largest legal start. Computed only after n <= size, so it cannot wrap.
  const size_t maxStart = size - n;
  if (from > maxStart) {
    return std::nullopt;
  }

  const size_t overlap = n - 1;
  std::string window;
  size_t start = from;
  while (start <= maxStart) {
    const size_t want = std::min(kSearchWindowBytes, size - start);
    window.resize(want);
    const size_t got = doc.copyOut(start, std::span<char>(window.data(), window.size()));
    window.resize(got);
    if (got < n) {
      break;  // cannot hold a match, and copyOut will not return more later
    }
    for (size_t i = 0; i + n <= got; ++i) {
      if (!equalsAt(window, i, needle, options.caseSensitive)) {
        continue;
      }
      const size_t offset = start + i;
      if (options.wholeWord && !wordBounded(doc, offset, n)) {
        continue;
      }
      return Match{offset, n};
    }
    if (start + got >= size) {
      break;  // the window reached the end of the document
    }
    // got >= n >= overlap + 1, so this always advances by at least one byte.
    start += got - overlap;
  }
  return std::nullopt;
}

std::optional<Match> findPrev(const Document& doc, std::string_view needle, size_t from,
                              const SearchOptions& options) {
  const size_t n = needle.size();
  const size_t size = doc.size();
  if (n == 0 || n > size || from == 0) {
    return std::nullopt;
  }
  const size_t maxStart = size - n;
  // Highest start offset we are allowed to return.
  size_t hi = std::min(from - 1, maxStart);

  std::string window;
  while (true) {
    // Candidate starts live in [lo, hi]; the window must also hold n - 1 bytes
    // past hi so a match beginning at hi is complete.
    const size_t candidates = std::min(kSearchWindowBytes, hi + 1);
    const size_t lo = hi + 1 - candidates;
    const size_t want = std::min(size - lo, candidates + n - 1);
    window.resize(want);
    const size_t got = doc.copyOut(lo, std::span<char>(window.data(), window.size()));
    window.resize(got);

    if (got >= n) {
      const size_t maxIndex = std::min(hi - lo, got - n);
      for (size_t k = maxIndex + 1; k-- > 0;) {
        if (!equalsAt(window, k, needle, options.caseSensitive)) {
          continue;
        }
        const size_t offset = lo + k;
        if (options.wholeWord && !wordBounded(doc, offset, n)) {
          continue;
        }
        return Match{offset, n};
      }
    }
    if (lo == 0) {
      return std::nullopt;
    }
    hi = lo - 1;
  }
}

std::vector<Match> findAll(const Document& doc, std::string_view needle,
                           const SearchOptions& options, size_t maxMatches) {
  std::vector<Match> matches;
  const size_t n = needle.size();
  if (n == 0) {
    return matches;
  }
  size_t from = 0;
  while (maxMatches == 0 || matches.size() < maxMatches) {
    const std::optional<Match> hit = findNext(doc, needle, from, options);
    if (!hit.has_value()) {
      break;
    }
    matches.push_back(*hit);
    from = hit->offset + n;  // non-overlapping
  }
  return matches;
}

ReplaceResult replaceAll(Document& doc, std::string_view needle, std::string_view replacement,
                         const SearchOptions& options, size_t maxSpanBytes) {
  ReplaceResult result;
  const std::vector<Match> matches = findAll(doc, needle, options);
  if (matches.empty()) {
    return result;
  }

  const size_t spanBegin = matches.front().offset;
  const size_t spanEnd = matches.back().offset + matches.back().length;
  const size_t spanLength = spanEnd - spanBegin;
  if (maxSpanBytes != 0 && spanLength > maxSpanBytes) {
    result.tooLarge = true;
    return result;
  }

  // Rewrite the span once: copy the untouched stretches between matches and
  // substitute at each hit. One Document::replace => one undo group.
  std::string rewritten;
  rewritten.reserve(spanLength + matches.size() * replacement.size());
  size_t cursor = spanBegin;
  for (const Match& match : matches) {
    if (match.offset > cursor) {
      rewritten += doc.textRange(cursor, match.offset - cursor);
    }
    rewritten.append(replacement);
    cursor = match.offset + match.length;
  }
  // cursor == spanEnd here: the last match ends the span by construction.

  doc.replace(spanBegin, spanLength, rewritten);
  result.replacements = matches.size();
  return result;
}

}  // namespace ide::text
