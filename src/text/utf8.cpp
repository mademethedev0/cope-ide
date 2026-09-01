#include <ide/text/utf8.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ide::text::utf8 {
namespace {

struct Range {
  uint32_t lo;
  uint32_t hi;
};

/// Combining marks and zero-width format characters. Sorted, non-overlapping.
constexpr Range kZeroWidth[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x0902},
    {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},
    {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC, 0x09BC},
    {0x09C1, 0x09C4}, {0x09CD, 0x09CD}, {0x0A01, 0x0A02}, {0x0A3C, 0x0A3C},
    {0x0A41, 0x0A42}, {0x0A47, 0x0A48}, {0x0A4B, 0x0A4D}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC},
    {0x0EC8, 0x0ECD}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
    {0x2028, 0x202E}, {0x2060, 0x2064}, {0x206A, 0x206F}, {0x20D0, 0x20F0},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

/// East Asian Wide/Fullwidth plus the emoji blocks that render double width.
/// Sorted, non-overlapping. Skin-tone modifiers (U+1F3FB..U+1F3FF) fall inside
/// {0x1F3F8, 0x1F43E} on purpose and must not also appear in kZeroWidth.
constexpr Range kWide[] = {
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},   {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},
    {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},
    {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},
    {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
    {0x17000, 0x187F7}, {0x18800, 0x18CD5}, {0x1B000, 0x1B152}, {0x1B164, 0x1B167},
    {0x1B170, 0x1B2FB}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C},
    {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A},
    {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

constexpr size_t kZeroWidthCount = sizeof(kZeroWidth) / sizeof(kZeroWidth[0]);
constexpr size_t kWideCount = sizeof(kWide) / sizeof(kWide[0]);

/// Binary search; correct only for sorted non-overlapping tables, which
/// detail::widthTablesAreSorted() checks in the unit tests.
bool inRanges(uint32_t cp, const Range* ranges, size_t count) noexcept {
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (cp < ranges[mid].lo) {
      hi = mid;
    } else if (cp > ranges[mid].hi) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

bool tableSorted(const Range* ranges, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    if (ranges[i].lo > ranges[i].hi) {
      return false;
    }
    if (i > 0 && ranges[i - 1].hi >= ranges[i].lo) {
      return false;
    }
  }
  return true;
}

/// Smallest codepoint legally encodable in a sequence of the given length;
/// used to reject overlong encodings.
constexpr uint32_t kMinForLength[5] = {0u, 0u, 0x80u, 0x800u, 0x10000u};

}  // namespace

size_t sequenceLength(char lead) noexcept {
  // Widened to unsigned int so every comparison below is unsigned-vs-unsigned.
  const unsigned int byte = static_cast<unsigned char>(lead);
  if (byte < 0x80u) {
    return 1;
  }
  if (byte < 0xC2u) {
    return 0;  // continuation byte, or 0xC0/0xC1 which are always overlong
  }
  if (byte < 0xE0u) {
    return 2;
  }
  if (byte < 0xF0u) {
    return 3;
  }
  if (byte < 0xF5u) {
    return 4;
  }
  return 0;  // 0xF5..0xFF would encode beyond U+10FFFF
}

Decoded decode(std::string_view bytes, size_t offset) noexcept {
  if (offset >= bytes.size()) {
    return Decoded{};
  }
  const unsigned char lead = static_cast<unsigned char>(bytes[offset]);
  const size_t length = sequenceLength(bytes[offset]);
  if (length == 0 || length > bytes.size() - offset) {
    return Decoded{0xFFFDu, 1, false};
  }

  uint32_t cp = 0;
  switch (length) {
    case 1:
      cp = lead;
      break;
    case 2:
      cp = lead & 0x1Fu;
      break;
    case 3:
      cp = lead & 0x0Fu;
      break;
    default:
      cp = lead & 0x07u;
      break;
  }
  for (size_t i = 1; i < length; ++i) {
    const unsigned char cont = static_cast<unsigned char>(bytes[offset + i]);
    if ((cont & 0xC0u) != 0x80u) {
      return Decoded{0xFFFDu, i, false};  // maximal subpart, always >= 1
    }
    cp = (cp << 6) | (cont & 0x3Fu);
  }
  if (cp < kMinForLength[length] || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
    return Decoded{0xFFFDu, length, false};
  }
  return Decoded{cp, length, true};
}

size_t encode(uint32_t cp, std::span<char> out) noexcept {
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
    return 0;
  }
  if (cp < 0x80u) {
    if (out.size() < 1) {
      return 0;
    }
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800u) {
    if (out.size() < 2) {
      return 0;
    }
    out[0] = static_cast<char>(0xC0u | (cp >> 6));
    out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp < 0x10000u) {
    if (out.size() < 3) {
      return 0;
    }
    out[0] = static_cast<char>(0xE0u | (cp >> 12));
    out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
    return 3;
  }
  if (out.size() < 4) {
    return 0;
  }
  out[0] = static_cast<char>(0xF0u | (cp >> 18));
  out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
  out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
  out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
  return 4;
}

int displayWidth(uint32_t cp) noexcept {
  if (inRanges(cp, kZeroWidth, kZeroWidthCount)) {
    return 0;
  }
  if (inRanges(cp, kWide, kWideCount)) {
    return 2;
  }
  // Control characters other than tab/newline are rendered as a single cell
  // placeholder by the view layer, so they count as 1 here.
  return 1;
}

size_t nextBoundary(std::string_view bytes, size_t offset) noexcept {
  const size_t total = bytes.size();
  if (offset >= total) {
    return total;
  }
  const size_t sequence = sequenceLength(bytes[offset]);
  if (sequence <= 1) {
    return offset + 1;
  }
  size_t cap = sequence;
  if (cap > total - offset) {
    cap = total - offset;  // truncated sequence at end of buffer
  }
  size_t length = 1;
  while (length < cap && isContinuation(bytes[offset + length])) {
    ++length;
  }
  return offset + length;
}

size_t snapToBoundary(std::string_view bytes, size_t offset) noexcept {
  const size_t total = bytes.size();
  if (offset >= total) {
    return total;
  }
  size_t steps = 0;
  while (offset > 0 && steps < 4 && isContinuation(bytes[offset])) {
    --offset;
    ++steps;
  }
  return offset;
}

size_t prevBoundary(std::string_view bytes, size_t offset) noexcept {
  const size_t total = bytes.size();
  if (offset > total) {
    offset = total;
  }
  if (offset == 0) {
    return 0;
  }
  const size_t snapped = snapToBoundary(bytes, offset);
  if (snapped < offset) {
    return snapped;  // `offset` was mid-sequence
  }
  size_t prev = offset - 1;
  size_t steps = 0;
  while (prev > 0 && steps < 3 && isContinuation(bytes[prev])) {
    --prev;
    ++steps;
  }
  return prev;
}

namespace detail {

bool widthTablesAreSorted() noexcept {
  return tableSorted(kZeroWidth, kZeroWidthCount) && tableSorted(kWide, kWideCount);
}

}  // namespace detail

}  // namespace ide::text::utf8
