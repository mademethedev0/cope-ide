#include <ide/text/piece_table.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <ide/text/byte_source.h>
#include <ide/text/line_index.h>

namespace ide::text {

PieceTable::PieceTable() : PieceTable(std::shared_ptr<const ByteSource>{}) {}

PieceTable::PieceTable(std::shared_ptr<const ByteSource> original)
    : original_(std::move(original)) {
  // nodes_[kNil] is a permanently zeroed sentinel: aggregate reads of a missing
  // child then need no branch, and update() on a real node is unconditional.
  nodes_.emplace_back();
  if (!original_) {
    original_ = std::make_shared<const OwnedByteSource>();
  }
  const std::string_view view = original_->view();
  // The single linear pass over the file: one memchr sweep counting newlines
  // per 4 KiB block. No document byte is copied, then or ever.
  originalLines_.extendTo(view);
  addLines_.extendTo(std::string_view{});
  if (!view.empty()) {
    Piece whole;
    whole.source = Source::Original;
    whole.offset = 0;
    whole.length = view.size();
    root_ = allocNode(whole, originalLines_.totalNewlines());
  }
}

// --- sizes ----------------------------------------------------------------

size_t PieceTable::size() const noexcept { return bytesOf(root_); }

int64_t PieceTable::newlineCount() const noexcept { return newlinesOf(root_); }

size_t PieceTable::pieceCount() const noexcept {
  return nodes_.size() - 1 - free_.size();  // minus the sentinel
}

size_t PieceTable::addBufferSize() const noexcept { return add_.size(); }

// --- arena ----------------------------------------------------------------

uint32_t PieceTable::nextPriority() noexcept {
  // splitmix64 from a fixed seed: random enough for treap balance, fully
  // deterministic so a failing test always reproduces.
  uint64_t z = (rng_ += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z = z ^ (z >> 31);
  return static_cast<uint32_t>(z >> 32);
}

uint32_t PieceTable::allocNode(const Piece& piece, int64_t newlines) {
  uint32_t id = kNil;
  if (!free_.empty()) {
    id = free_.back();
    free_.pop_back();
  } else {
    id = static_cast<uint32_t>(nodes_.size());
    nodes_.emplace_back();
  }
  Node& node = nodes_[id];
  node.piece = piece;
  node.pieceNewlines = newlines;
  node.left = kNil;
  node.right = kNil;
  node.priority = nextPriority();
  node.subtreeBytes = piece.length;
  node.subtreeNewlines = newlines;
  return id;
}

void PieceTable::freeSubtree(uint32_t node) {
  if (node == kNil) {
    return;
  }
  const uint32_t left = nodes_[node].left;
  const uint32_t right = nodes_[node].right;
  freeSubtree(left);
  freeSubtree(right);
  nodes_[node].left = kNil;
  nodes_[node].right = kNil;
  nodes_[node].piece.length = 0;
  nodes_[node].pieceNewlines = 0;
  nodes_[node].subtreeBytes = 0;
  nodes_[node].subtreeNewlines = 0;
  free_.push_back(node);
}

void PieceTable::update(uint32_t node) noexcept {
  if (node == kNil) {
    return;  // never corrupt the sentinel
  }
  Node& self = nodes_[node];
  self.subtreeBytes =
      nodes_[self.left].subtreeBytes + self.piece.length + nodes_[self.right].subtreeBytes;
  self.subtreeNewlines =
      nodes_[self.left].subtreeNewlines + self.pieceNewlines + nodes_[self.right].subtreeNewlines;
}

// --- treap primitives -----------------------------------------------------

uint32_t PieceTable::join(uint32_t left, uint32_t right) {
  if (left == kNil) {
    return right;
  }
  if (right == kNil) {
    return left;
  }
  if (nodes_[left].priority > nodes_[right].priority) {
    nodes_[left].right = join(nodes_[left].right, right);
    update(left);
    return left;
  }
  nodes_[right].left = join(left, nodes_[right].left);
  update(right);
  return right;
}

void PieceTable::split(uint32_t node, size_t at, uint32_t& outLeft, uint32_t& outRight) {
  if (node == kNil) {
    outLeft = kNil;
    outRight = kNil;
    return;
  }
  const size_t leftBytes = bytesOf(nodes_[node].left);
  const size_t pieceLength = nodes_[node].piece.length;

  if (at <= leftBytes) {
    uint32_t innerLeft = kNil;
    uint32_t innerRight = kNil;
    split(nodes_[node].left, at, innerLeft, innerRight);
    nodes_[node].left = innerRight;
    update(node);
    outLeft = innerLeft;
    outRight = node;
    return;
  }
  if (at >= leftBytes + pieceLength) {
    uint32_t innerLeft = kNil;
    uint32_t innerRight = kNil;
    split(nodes_[node].right, at - leftBytes - pieceLength, innerLeft, innerRight);
    nodes_[node].right = innerLeft;
    update(node);
    outLeft = node;
    outRight = innerRight;
    return;
  }

  // `at` falls strictly inside this node's piece, so the piece itself is cut.
  // Only the descriptor is cut -- the bytes are untouched, which is the whole
  // point of a piece table.
  const size_t within = at - leftBytes;
  const Piece head = nodes_[node].piece;
  const uint32_t oldRight = nodes_[node].right;

  Piece tail;
  tail.source = head.source;
  tail.offset = head.offset + within;
  tail.length = head.length - within;

  const int64_t tailNewlines =
      newlinesInSource(tail.source, tail.offset, tail.offset + tail.length);
  const int64_t headNewlines = newlinesInSource(head.source, head.offset, head.offset + within);

  const uint32_t tailNode = allocNode(tail, tailNewlines);  // may reallocate nodes_
  nodes_[node].piece.length = within;
  nodes_[node].pieceNewlines = headNewlines;
  nodes_[node].right = kNil;
  update(node);

  outLeft = node;
  // tailNode carries a fresh random priority, so the old right subtree must be
  // re-attached with join() rather than hung underneath it directly; otherwise
  // the heap property (and with it the balance guarantee) would rot.
  outRight = join(tailNode, oldRight);
}

std::optional<PieceTable::Located> PieceTable::locate(size_t offset) const noexcept {
  uint32_t node = root_;
  size_t base = 0;
  while (node != kNil) {
    const size_t leftBytes = bytesOf(nodes_[node].left);
    if (offset < leftBytes) {
      node = nodes_[node].left;
      continue;
    }
    offset -= leftBytes;
    base += leftBytes;
    const size_t pieceLength = nodes_[node].piece.length;
    if (offset < pieceLength) {
      Located found;
      found.node = node;
      found.nodeStart = base;
      found.within = offset;
      return found;
    }
    offset -= pieceLength;
    base += pieceLength;
    node = nodes_[node].right;
  }
  return std::nullopt;
}

// --- mutation -------------------------------------------------------------

bool PieceTable::extendAt(uint32_t node, size_t offset, std::string_view text) {
  // INVARIANT: offset > 0 on entry, so `offset <= leftBytes` implies a non-nil
  // left child.
  if (node == kNil) {
    return false;
  }
  const size_t leftBytes = bytesOf(nodes_[node].left);
  const size_t pieceLength = nodes_[node].piece.length;

  if (offset <= leftBytes) {
    if (!extendAt(nodes_[node].left, offset, text)) {
      return false;
    }
  } else if (offset > leftBytes + pieceLength) {
    if (!extendAt(nodes_[node].right, offset - leftBytes - pieceLength, text)) {
      return false;
    }
  } else if (offset == leftBytes + pieceLength) {
    const Piece piece = nodes_[node].piece;
    if (piece.source != Source::Add) {
      return false;
    }
    if (piece.offset + piece.length != add_.size()) {
      return false;  // not the live tail of the add buffer
    }
    // Measure before appending: `text` may point into add_ and dangle after.
    const size_t length = text.size();
    const int64_t newlines = countNewlines(text);
    appendAdd(text);
    nodes_[node].piece.length = piece.length + length;
    nodes_[node].pieceNewlines += newlines;
  } else {
    return false;  // strictly inside this piece: the general path must split it
  }
  update(node);
  return true;
}

void PieceTable::insert(size_t offset, std::string_view text) {
  if (text.empty()) {
    return;
  }
  const size_t total = size();
  if (offset > total) {
    offset = total;
  }

  // Typing fast path: grow the piece that already ends here instead of adding
  // one piece per keystroke.
  if (offset > 0 && extendAt(root_, offset, text)) {
    return;
  }

  // Measure before appending: `text` may point into add_ and dangle after.
  const size_t length = text.size();
  const int64_t newlines = countNewlines(text);
  const size_t addOffset = add_.size();
  appendAdd(text);

  Piece piece;
  piece.source = Source::Add;
  piece.offset = addOffset;
  piece.length = length;
  const uint32_t node = allocNode(piece, newlines);

  uint32_t left = kNil;
  uint32_t right = kNil;
  split(root_, offset, left, right);
  root_ = join(join(left, node), right);
}

void PieceTable::erase(size_t offset, size_t length) {
  const size_t total = size();
  if (length == 0 || offset >= total) {
    return;
  }
  if (length > total - offset) {
    length = total - offset;
  }
  uint32_t left = kNil;
  uint32_t rest = kNil;
  split(root_, offset, left, rest);
  uint32_t middle = kNil;
  uint32_t right = kNil;
  split(rest, length, middle, right);
  freeSubtree(middle);
  root_ = join(left, right);
}

// --- reading --------------------------------------------------------------

void PieceTable::copyRange(uint32_t node, size_t from, size_t to, char*& out) const {
  if (node == kNil || from >= to) {
    return;
  }
  const size_t leftBytes = bytesOf(nodes_[node].left);
  const size_t pieceEnd = leftBytes + nodes_[node].piece.length;

  if (from < leftBytes) {
    copyRange(nodes_[node].left, from, to < leftBytes ? to : leftBytes, out);
  }
  if (from < pieceEnd && to > leftBytes) {
    const size_t begin = (from > leftBytes ? from : leftBytes) - leftBytes;
    const size_t end = (to < pieceEnd ? to : pieceEnd) - leftBytes;
    if (end > begin) {
      const Piece piece = nodes_[node].piece;
      const char* source = sourceView(piece.source).data() + piece.offset + begin;
      std::memcpy(out, source, end - begin);
      out += end - begin;
    }
  }
  if (to > pieceEnd) {
    copyRange(nodes_[node].right, (from > pieceEnd ? from : pieceEnd) - pieceEnd, to - pieceEnd,
              out);
  }
}

size_t PieceTable::copyOut(size_t offset, std::span<char> dest) const {
  const size_t total = size();
  if (offset >= total || dest.empty()) {
    return 0;
  }
  size_t count = dest.size();
  if (count > total - offset) {
    count = total - offset;
  }
  char* out = dest.data();
  copyRange(root_, offset, offset + count, out);
  return count;
}

std::optional<std::string_view> PieceTable::contiguous(size_t offset, size_t length) const {
  if (length == 0) {
    return std::string_view{};
  }
  const size_t total = size();
  if (offset >= total || length > total - offset) {
    return std::nullopt;
  }
  const std::optional<Located> found = locate(offset);
  if (!found.has_value()) {
    return std::nullopt;
  }
  const Piece piece = nodes_[found->node].piece;
  if (found->within + length > piece.length) {
    return std::nullopt;  // spans a piece boundary
  }
  return sourceView(piece.source).substr(piece.offset + found->within, length);
}

char PieceTable::byteAt(size_t offset) const noexcept {
  const std::optional<Located> found = locate(offset);
  if (!found.has_value()) {
    return '\0';
  }
  const Piece piece = nodes_[found->node].piece;
  const std::string_view view = sourceView(piece.source);
  const size_t at = piece.offset + found->within;
  return at < view.size() ? view[at] : '\0';
}

// --- line queries ---------------------------------------------------------

int64_t PieceTable::newlinesBefore(size_t offset) const noexcept {
  const size_t total = size();
  if (offset > total) {
    offset = total;
  }
  int64_t count = 0;
  uint32_t node = root_;
  while (node != kNil) {
    const size_t leftBytes = bytesOf(nodes_[node].left);
    if (offset <= leftBytes) {
      node = nodes_[node].left;
      continue;
    }
    count += newlinesOf(nodes_[node].left);
    offset -= leftBytes;
    const Piece piece = nodes_[node].piece;
    if (offset < piece.length) {
      count += newlinesInSource(piece.source, piece.offset, piece.offset + offset);
      return count;
    }
    count += nodes_[node].pieceNewlines;
    offset -= piece.length;
    node = nodes_[node].right;
  }
  return count;
}

size_t PieceTable::offsetOfNewline(int64_t rank) const noexcept {
  if (rank < 0 || rank >= newlineCount()) {
    return size();
  }
  size_t base = 0;
  uint32_t node = root_;
  while (node != kNil) {
    const int64_t leftNewlines = newlinesOf(nodes_[node].left);
    if (rank < leftNewlines) {
      node = nodes_[node].left;
      continue;
    }
    rank -= leftNewlines;
    base += bytesOf(nodes_[node].left);
    const Piece piece = nodes_[node].piece;
    const int64_t pieceNewlines = nodes_[node].pieceNewlines;
    if (rank < pieceNewlines) {
      const size_t inSource =
          kthNewlineInSource(piece.source, piece.offset, piece.offset + piece.length, rank);
      return base + (inSource - piece.offset);
    }
    rank -= pieceNewlines;
    base += piece.length;
    node = nodes_[node].right;
  }
  return size();
}

size_t PieceTable::lineStartOffset(int64_t line) const noexcept {
  if (line <= 0) {
    return 0;
  }
  const int64_t total = newlineCount();
  if (line > total) {
    return size();  // past the last line
  }
  return offsetOfNewline(line - 1) + 1;
}

// --- diagnostics ----------------------------------------------------------

void PieceTable::collect(uint32_t node, std::vector<Piece>& out) const {
  if (node == kNil) {
    return;
  }
  collect(nodes_[node].left, out);
  out.push_back(nodes_[node].piece);
  collect(nodes_[node].right, out);
}

std::vector<Piece> PieceTable::pieceList() const {
  std::vector<Piece> out;
  out.reserve(pieceCount());
  collect(root_, out);
  return out;
}

int64_t PieceTable::heightOf(uint32_t node) const {
  if (node == kNil) {
    return 0;
  }
  const int64_t left = heightOf(nodes_[node].left);
  const int64_t right = heightOf(nodes_[node].right);
  return 1 + (left > right ? left : right);
}

int64_t PieceTable::treeHeight() const { return heightOf(root_); }

// --- sources --------------------------------------------------------------

std::string_view PieceTable::sourceView(Source source) const noexcept {
  if (source == Source::Original) {
    return original_->view();
  }
  return add_.empty() ? std::string_view{} : std::string_view{add_.data(), add_.size()};
}

const SourceLineTable& PieceTable::tableFor(Source source) const noexcept {
  return source == Source::Original ? originalLines_ : addLines_;
}

int64_t PieceTable::newlinesInSource(Source source, size_t from, size_t to) const noexcept {
  return tableFor(source).newlinesIn(sourceView(source), from, to);
}

size_t PieceTable::kthNewlineInSource(Source source, size_t from, size_t to,
                                      int64_t k) const noexcept {
  return tableFor(source).kthNewline(sourceView(source), from, to, k);
}

bool PieceTable::aliasesAdd(std::string_view text) const noexcept {
  if (add_.empty() || text.empty()) {
    return false;
  }
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(add_.data());
  const std::uintptr_t limit = base + add_.capacity();
  const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(text.data());
  return at >= base && at < limit;
}

void PieceTable::appendAdd(std::string_view text) {
  if (aliasesAdd(text)) {
    // A caller may re-insert text obtained from contiguous(); growing add_
    // while reading from it would read freed memory.
    const std::vector<char> copy(text.begin(), text.end());
    add_.insert(add_.end(), copy.begin(), copy.end());
  } else {
    add_.insert(add_.end(), text.begin(), text.end());
  }
  // The add buffer is append-only, so this only scans the new bytes.
  addLines_.extendTo(sourceView(Source::Add));
}

}  // namespace ide::text
