#include <ide/text/document.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <ide/text/byte_source.h>
#include <ide/text/piece_table.h>
#include <ide/text/undo.h>
#include <ide/text/utf8.h>

namespace ide::text {

Document::Document() : pieces_() {}

Document::Document(std::string bytes) : pieces_(OwnedByteSource::make(std::move(bytes))) {}

Document::Document(std::shared_ptr<const ByteSource> original) : pieces_(std::move(original)) {}

int64_t Document::nowMs() noexcept {
  const std::chrono::steady_clock::duration since =
      std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
}

// --- reading --------------------------------------------------------------

size_t Document::copyOut(size_t offset, std::span<char> dest) const {
  return pieces_.copyOut(offset, dest);
}

std::optional<std::string_view> Document::contiguousText(size_t offset, size_t length) const {
  return pieces_.contiguous(offset, length);
}

std::string Document::textRange(size_t offset, size_t length) const {
  const size_t total = pieces_.size();
  if (offset >= total || length == 0) {
    return std::string{};
  }
  if (length > total - offset) {
    length = total - offset;
  }
  std::string out(length, '\0');
  pieces_.copyOut(offset, std::span<char>(out.data(), out.size()));
  return out;
}

std::string Document::text() const { return textRange(0, pieces_.size()); }

// --- lines ----------------------------------------------------------------

LineRange Document::lineAt(int64_t line) const {
  const int64_t count = lineCount();  // always >= 1
  if (line < 0) {
    line = 0;
  }
  if (line >= count) {
    line = count - 1;
  }
  LineRange range;
  range.start = pieces_.lineStartOffset(line);
  if (line == count - 1) {
    // Last line: no terminator, ends at end of document.
    range.end = pieces_.size();
    range.terminatorLength = 0;
    return range;
  }
  const size_t newline = pieces_.offsetOfNewline(line);
  size_t terminator = 1;
  // CRLF counts as one line break, but the bytes are left exactly as they are.
  if (newline > range.start && pieces_.byteAt(newline - 1) == '\r') {
    terminator = 2;
  }
  range.end = newline + 1 - terminator;
  range.terminatorLength = terminator;
  return range;
}

Position Document::lineColumnOf(size_t offset) const {
  const size_t total = pieces_.size();
  if (offset > total) {
    offset = total;
  }
  Position position;
  position.line = pieces_.newlinesBefore(offset);
  const size_t start = pieces_.lineStartOffset(position.line);
  position.column = static_cast<int64_t>(offset - start);
  return position;
}

size_t Document::offsetOf(int64_t line, int64_t byteColumn) const {
  const LineRange range = lineAt(line);
  if (byteColumn <= 0) {
    return range.start;
  }
  // Compare in the column domain, not the offset domain, so a huge column can
  // never overflow range.start + byteColumn.
  const size_t maxColumn = range.length();
  size_t wanted = range.end;  // a caret can never sit inside a CRLF
  if (static_cast<uint64_t>(byteColumn) < static_cast<uint64_t>(maxColumn)) {
    wanted = range.start + static_cast<size_t>(byteColumn);
  }
  const size_t snapped = snapToCodepointStart(wanted);
  return snapped < range.start ? range.start : snapped;
}

std::string Document::lineContent(const LineRange& range) const {
  return textRange(range.start, range.length());
}

// --- UTF-8 aware movement -------------------------------------------------

size_t Document::snapToCodepointStart(size_t offset) const noexcept {
  const size_t total = pieces_.size();
  if (offset >= total) {
    return total;
  }
  size_t steps = 0;
  while (offset > 0 && steps < 4 && utf8::isContinuation(pieces_.byteAt(offset))) {
    --offset;
    ++steps;
  }
  return offset;
}

size_t Document::nextCodepoint(size_t offset) const noexcept {
  const size_t total = pieces_.size();
  if (offset >= total) {
    return total;
  }
  char buffer[4] = {0, 0, 0, 0};
  const size_t available = pieces_.copyOut(offset, std::span<char>(buffer, 4));
  // available >= 1 because offset < total.
  const size_t sequence = utf8::sequenceLength(buffer[0]);
  if (sequence <= 1) {
    return offset + 1;  // ASCII, or a byte that cannot start a sequence
  }
  size_t cap = sequence;
  if (cap > available) {
    cap = available;  // truncated sequence at end of document
  }
  size_t length = 1;
  while (length < cap && utf8::isContinuation(buffer[length])) {
    ++length;
  }
  return offset + length;  // maximal subpart, always >= 1
}

size_t Document::prevCodepoint(size_t offset) const noexcept {
  const size_t total = pieces_.size();
  if (offset > total) {
    offset = total;
  }
  if (offset == 0) {
    return 0;
  }
  const size_t snapped = snapToCodepointStart(offset);
  if (snapped < offset) {
    return snapped;  // `offset` was inside a sequence
  }
  size_t previous = offset - 1;
  size_t steps = 0;
  while (previous > 0 && steps < 3 && utf8::isContinuation(pieces_.byteAt(previous))) {
    --previous;
    ++steps;
  }
  return previous;
}

int64_t Document::displayColumnOf(size_t offset, int tabWidth) const {
  if (tabWidth < 1) {
    tabWidth = 1;
  }
  const size_t total = pieces_.size();
  if (offset > total) {
    offset = total;
  }
  const Position position = lineColumnOf(offset);
  const LineRange range = lineAt(position.line);
  size_t stop = offset < range.end ? offset : range.end;
  if (stop < range.start) {
    stop = range.start;
  }
  const std::string content = textRange(range.start, stop - range.start);

  const int64_t width = tabWidth;
  int64_t column = 0;
  size_t at = 0;
  while (at < content.size()) {
    if (content[at] == '\t') {
      column = ((column / width) + 1) * width;
      ++at;
      continue;
    }
    const utf8::Decoded decoded = utf8::decode(content, at);
    const size_t step = decoded.length == 0 ? 1 : decoded.length;
    column += decoded.valid ? utf8::displayWidth(decoded.codepoint) : 1;
    at += step;
  }
  return column;
}

size_t Document::offsetOfDisplayColumn(int64_t line, int64_t displayColumn, int tabWidth) const {
  if (tabWidth < 1) {
    tabWidth = 1;
  }
  const LineRange range = lineAt(line);
  if (displayColumn <= 0) {
    return range.start;
  }
  const std::string content = lineContent(range);

  const int64_t width = tabWidth;
  int64_t column = 0;
  size_t at = 0;
  while (at < content.size()) {
    if (content[at] == '\t') {
      const int64_t next = ((column / width) + 1) * width;
      if (next > displayColumn) {
        break;  // the target column falls inside the tab: stop before it
      }
      column = next;
      ++at;
      continue;
    }
    const utf8::Decoded decoded = utf8::decode(content, at);
    const size_t step = decoded.length == 0 ? 1 : decoded.length;
    const int64_t cells = decoded.valid ? utf8::displayWidth(decoded.codepoint) : 1;
    if (column + cells > displayColumn) {
      break;  // the target column falls inside this character
    }
    column += cells;
    at += step;
  }
  return range.start + at;
}

// --- mutation -------------------------------------------------------------

bool Document::isTypingUnit(std::string_view bytes) noexcept {
  if (bytes.empty() || bytes.size() > 4) {
    return false;
  }
  if (utf8::sequenceLength(bytes[0]) != bytes.size()) {
    return false;  // not exactly one codepoint
  }
  return bytes.find('\n') == std::string_view::npos;
}

void Document::recordEdit(EditRecord record, const CursorState& before, const CursorState& after,
                          const EditOptions& options, bool coalescable) {
  const int64_t timestamp = options.timestampMs >= 0 ? options.timestampMs : nowMs();
  if (options.coalesce && coalescable &&
      undo_.tryCoalesce(record, before, after, timestamp, coalesceWindowMs_)) {
    return;
  }
  EditGroup group;
  group.edits.push_back(std::move(record));
  group.before = before;
  group.after = after;
  group.timestampMs = timestamp;
  group.coalescable = coalescable;
  undo_.push(std::move(group));
}

void Document::insert(size_t offset, std::string_view text, const EditOptions& options) {
  if (text.empty()) {
    return;
  }
  const size_t total = pieces_.size();
  if (offset > total) {
    offset = total;
  }
  const CursorState before = cursor_;

  EditRecord record;
  record.offset = offset;
  record.inserted.assign(text);
  // `text` may alias the add buffer and dangle once it grows, so everything
  // below reads our own owned copy instead.
  const bool coalescable = isTypingUnit(record.inserted);
  pieces_.insert(offset, record.inserted);
  ++version_;

  const size_t caret = offset + record.inserted.size();
  cursor_ = CursorState{caret, caret};
  recordEdit(std::move(record), before, cursor_, options, coalescable);
}

void Document::erase(size_t offset, size_t length, const EditOptions& options) {
  const size_t total = pieces_.size();
  if (length == 0 || offset >= total) {
    return;
  }
  if (length > total - offset) {
    length = total - offset;
  }
  const CursorState before = cursor_;

  EditRecord record;
  record.offset = offset;
  record.removed = textRange(offset, length);
  pieces_.erase(offset, length);
  ++version_;

  cursor_ = CursorState{offset, offset};
  const bool coalescable = isTypingUnit(record.removed);
  recordEdit(std::move(record), before, cursor_, options, coalescable);
}

void Document::replace(size_t offset, size_t length, std::string_view text,
                       const EditOptions& options) {
  const size_t total = pieces_.size();
  if (offset > total) {
    offset = total;
  }
  if (length > total - offset) {
    length = total - offset;
  }
  if (length == 0 && text.empty()) {
    return;
  }
  const CursorState before = cursor_;

  EditRecord record;
  record.offset = offset;
  record.removed = textRange(offset, length);
  record.inserted.assign(text);
  if (length > 0) {
    pieces_.erase(offset, length);
  }
  if (!record.inserted.empty()) {
    pieces_.insert(offset, record.inserted);
  }
  ++version_;

  const size_t caret = offset + record.inserted.size();
  cursor_ = CursorState{caret, caret};
  // A replacement is never coalescable: it is one deliberate operation.
  recordEdit(std::move(record), before, cursor_, options, false);
}

// --- history --------------------------------------------------------------

bool Document::undo() {
  const EditGroup* group = undo_.undoGroup();
  if (group == nullptr) {
    return false;
  }
  // Invert in reverse order: undo the last record first.
  for (size_t i = group->edits.size(); i-- > 0;) {
    const EditRecord& record = group->edits[i];
    if (!record.inserted.empty()) {
      pieces_.erase(record.offset, record.inserted.size());
    }
    if (!record.removed.empty()) {
      pieces_.insert(record.offset, record.removed);
    }
  }
  cursor_ = group->before;
  undo_.moveToParent();
  ++version_;
  return true;
}

bool Document::redo(size_t branchIndex) {
  const size_t branches = undo_.redoBranchCount();
  if (branches == 0) {
    return false;
  }
  const size_t branch = branchIndex == kNewestBranch ? undo_.newestRedoBranch() : branchIndex;
  const EditGroup* group = undo_.redoGroup(branch);
  if (group == nullptr) {
    return false;
  }
  for (const EditRecord& record : group->edits) {
    if (!record.removed.empty()) {
      pieces_.erase(record.offset, record.removed.size());
    }
    if (!record.inserted.empty()) {
      pieces_.insert(record.offset, record.inserted);
    }
  }
  cursor_ = group->after;
  undo_.moveToRedo(branch);
  ++version_;
  return true;
}

}  // namespace ide::text
