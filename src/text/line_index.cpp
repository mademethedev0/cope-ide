#include <ide/text/line_index.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ide::text {

int64_t countNewlines(std::string_view bytes) noexcept {
  int64_t found = 0;
  const char* cursor = bytes.data();
  size_t remaining = bytes.size();
  while (remaining > 0) {
    const void* hit = std::memchr(cursor, '\n', remaining);
    if (hit == nullptr) {
      break;
    }
    const char* at = static_cast<const char*>(hit);
    const size_t consumed = static_cast<size_t>(at - cursor) + 1;
    ++found;
    remaining -= consumed;
    cursor = at + 1;
  }
  return found;
}

void SourceLineTable::clear() {
  prefix_.clear();
  prefix_.push_back(0);
  covered_ = 0;
}

void SourceLineTable::extendTo(std::string_view bytes) {
  if (bytes.size() < covered_) {
    // Sources are append-only; a shrink means the caller swapped the buffer.
    clear();
  }

  // Entries at block boundaries that lie inside the old coverage are exact and
  // survive. The last entry may describe a partial block, so drop it.
  const size_t keep = covered_ / kBlockBytes + 1;
  if (keep < prefix_.size()) {
    prefix_.resize(keep);
  }
  covered_ = bytes.size();

  size_t pos = (prefix_.size() - 1) * kBlockBytes;
  int64_t running = prefix_.back();
  while (pos < covered_) {
    size_t blockEnd = pos + kBlockBytes;
    if (blockEnd > covered_) {
      blockEnd = covered_;
    }
    running += countNewlines(bytes.substr(pos, blockEnd - pos));
    prefix_.push_back(running);
    pos = blockEnd;
  }
}

int64_t SourceLineTable::newlinesBefore(std::string_view bytes, size_t pos) const noexcept {
  if (pos > covered_) {
    pos = covered_;
  }
  if (pos > bytes.size()) {
    pos = bytes.size();
  }
  const size_t block = pos / kBlockBytes;
  // block <= floor(covered_/B) <= prefix_.size() - 1, so this index is safe.
  const size_t base = block * kBlockBytes;
  return prefix_[block] + countNewlines(bytes.substr(base, pos - base));
}

int64_t SourceLineTable::newlinesIn(std::string_view bytes, size_t from,
                                    size_t to) const noexcept {
  if (from >= to) {
    return 0;
  }
  return newlinesBefore(bytes, to) - newlinesBefore(bytes, from);
}

size_t SourceLineTable::kthNewline(std::string_view bytes, size_t from, size_t to,
                                   int64_t k) const noexcept {
  if (k < 0 || from >= to) {
    return to;
  }
  // Rank of the wanted newline counted from the start of the whole source.
  const int64_t target = newlinesBefore(bytes, from) + k;

  // prefix_[block] <= target < prefix_[block + 1] locates the block holding it.
  // prefix_[0] == 0 <= target, so upper_bound never returns begin().
  const size_t upper = static_cast<size_t>(
      std::upper_bound(prefix_.begin(), prefix_.end(), target) - prefix_.begin());
  const size_t block = upper - 1;
  int64_t skip = target - prefix_[block];

  size_t pos = block * kBlockBytes;
  if (from > pos) {
    // Starting at `from` and skipping k newlines is equivalent, and bounds the
    // scan by the caller's range instead of by the whole block.
    pos = from;
    skip = k;
  }
  const size_t limit = covered_ <= bytes.size() ? covered_ : bytes.size();
  const char* base = bytes.data();
  while (pos < limit) {
    const void* hit = std::memchr(base + pos, '\n', limit - pos);
    if (hit == nullptr) {
      break;
    }
    const size_t at = static_cast<size_t>(static_cast<const char*>(hit) - base);
    if (skip == 0) {
      return at;
    }
    --skip;
    pos = at + 1;
  }
  // Precondition violated (k >= newlinesIn(from, to)).
  return to;
}

}  // namespace ide::text
