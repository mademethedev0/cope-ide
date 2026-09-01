#include <gtest/gtest.h>

#include <ide/text/utf8.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace utf8 = ide::text::utf8;

// Byte sequences written as escapes so the test does not depend on this source
// file's own encoding. Escapes are never followed by a hex digit character.
constexpr const char* kTwoByte = "\xc3\xa9";              // U+00E9 e-acute
constexpr const char* kThreeByte = "\xe2\x82\xac";        // U+20AC euro sign
constexpr const char* kFourByte = "\xf0\x9d\x84\x9e";     // U+1D11E musical G clef
constexpr const char* kCjk = "\xe6\x97\xa5";              // U+65E5 CJK "day"
constexpr const char* kCombiningAcute = "\xcc\x81";       // U+0301 combining acute

TEST(Utf8Test, WidthTablesAreSorted) {
  // Lookup is a binary search, so a mis-ordered table entry would silently
  // corrupt every column calculation.
  EXPECT_TRUE(utf8::detail::widthTablesAreSorted());
}

TEST(Utf8Test, IsContinuation) {
  EXPECT_FALSE(utf8::isContinuation('a'));
  EXPECT_FALSE(utf8::isContinuation('\0'));
  EXPECT_FALSE(utf8::isContinuation(static_cast<char>(0xC3)));
  EXPECT_FALSE(utf8::isContinuation(static_cast<char>(0xF0)));
  EXPECT_TRUE(utf8::isContinuation(static_cast<char>(0x80)));
  EXPECT_TRUE(utf8::isContinuation(static_cast<char>(0xA9)));
  EXPECT_TRUE(utf8::isContinuation(static_cast<char>(0xBF)));
  EXPECT_FALSE(utf8::isContinuation(static_cast<char>(0xC0)));
}

TEST(Utf8Test, SequenceLength) {
  EXPECT_EQ(utf8::sequenceLength('a'), 1u);
  EXPECT_EQ(utf8::sequenceLength('\0'), 1u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0x7F)), 1u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0x80)), 0u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xBF)), 0u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xC0)), 0u);  // always overlong
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xC1)), 0u);  // always overlong
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xC2)), 2u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xDF)), 2u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xE0)), 3u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xEF)), 3u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xF0)), 4u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xF4)), 4u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xF5)), 0u);  // beyond U+10FFFF
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xF8)), 0u);
  EXPECT_EQ(utf8::sequenceLength(static_cast<char>(0xFF)), 0u);
}

TEST(Utf8Test, DecodeAsciiAndEmpty) {
  const utf8::Decoded empty = utf8::decode(std::string_view{}, 0);
  EXPECT_FALSE(empty.valid);
  EXPECT_EQ(empty.length, 0u);

  const utf8::Decoded past = utf8::decode("ab", 5);
  EXPECT_FALSE(past.valid);
  EXPECT_EQ(past.length, 0u);

  const utf8::Decoded ascii = utf8::decode("ab", 1);
  EXPECT_TRUE(ascii.valid);
  EXPECT_EQ(ascii.codepoint, 'b');
  EXPECT_EQ(ascii.length, 1u);
}

TEST(Utf8Test, DecodeTwoThreeFourByteSequences) {
  const utf8::Decoded two = utf8::decode(kTwoByte, 0);
  EXPECT_TRUE(two.valid);
  EXPECT_EQ(two.codepoint, 0x00E9u);
  EXPECT_EQ(two.length, 2u);

  const utf8::Decoded three = utf8::decode(kThreeByte, 0);
  EXPECT_TRUE(three.valid);
  EXPECT_EQ(three.codepoint, 0x20ACu);
  EXPECT_EQ(three.length, 3u);

  const utf8::Decoded four = utf8::decode(kFourByte, 0);
  EXPECT_TRUE(four.valid);
  EXPECT_EQ(four.codepoint, 0x1D11Eu);
  EXPECT_EQ(four.length, 4u);

  const utf8::Decoded cjk = utf8::decode(kCjk, 0);
  EXPECT_TRUE(cjk.valid);
  EXPECT_EQ(cjk.codepoint, 0x65E5u);
  EXPECT_EQ(cjk.length, 3u);
}

TEST(Utf8Test, DecodeRejectsMalformed) {
  // Lone continuation byte.
  const utf8::Decoded lone = utf8::decode("\x80", 0);
  EXPECT_FALSE(lone.valid);
  EXPECT_EQ(lone.length, 1u);

  // Truncated 3-byte sequence.
  const utf8::Decoded truncated = utf8::decode("\xe2\x82", 0);
  EXPECT_FALSE(truncated.valid);
  EXPECT_EQ(truncated.length, 1u);

  // Second byte is not a continuation: maximal subpart is 1 byte.
  const utf8::Decoded broken = utf8::decode("\xe2" "Z" "Z", 0);
  EXPECT_FALSE(broken.valid);
  EXPECT_EQ(broken.length, 1u);

  // Third byte is not a continuation: maximal subpart is 2 bytes.
  const utf8::Decoded broken2 = utf8::decode("\xe2\x82" "Z", 0);
  EXPECT_FALSE(broken2.valid);
  EXPECT_EQ(broken2.length, 2u);

  // Overlong encoding of '/' (U+002F): 0xC0 can never start a sequence, so the
  // maximal subpart is one byte.
  const utf8::Decoded overlong = utf8::decode("\xc0\xaf", 0);
  EXPECT_FALSE(overlong.valid);
  EXPECT_EQ(overlong.length, 1u);

  // Overlong 3-byte encoding of U+0041.
  const utf8::Decoded overlong3 = utf8::decode("\xe0\x81\x81", 0);
  EXPECT_FALSE(overlong3.valid);

  // Surrogate U+D800 encoded as UTF-8.
  const utf8::Decoded surrogate = utf8::decode("\xed\xa0\x80", 0);
  EXPECT_FALSE(surrogate.valid);

  // Above U+10FFFF.
  const utf8::Decoded tooBig = utf8::decode("\xf5\x80\x80\x80", 0);
  EXPECT_FALSE(tooBig.valid);
  EXPECT_EQ(tooBig.length, 1u);  // 0xF5 is not a valid lead at all
}

TEST(Utf8Test, EncodeRoundTrip) {
  const uint32_t points[] = {0x00,   0x41,   0x7F,    0x80,    0x7FF,
                             0x800,  0x20AC, 0xFFFF,  0x10000, 0x1D11E,
                             0x10FFFF};
  for (uint32_t cp : points) {
    char buffer[4] = {0, 0, 0, 0};
    const size_t written = utf8::encode(cp, std::span<char>(buffer, 4));
    ASSERT_GT(written, 0u) << "cp=" << cp;
    const utf8::Decoded decoded = utf8::decode(std::string_view(buffer, written), 0);
    EXPECT_TRUE(decoded.valid) << "cp=" << cp;
    EXPECT_EQ(decoded.codepoint, cp);
    EXPECT_EQ(decoded.length, written);
  }
}

TEST(Utf8Test, EncodeRejectsInvalidAndTooSmallBuffers) {
  char buffer[4] = {0, 0, 0, 0};
  EXPECT_EQ(utf8::encode(0xD800u, std::span<char>(buffer, 4)), 0u);
  EXPECT_EQ(utf8::encode(0x110000u, std::span<char>(buffer, 4)), 0u);
  EXPECT_EQ(utf8::encode(0x20ACu, std::span<char>(buffer, 2)), 0u);
  EXPECT_EQ(utf8::encode(0x1D11Eu, std::span<char>(buffer, 3)), 0u);
  EXPECT_EQ(utf8::encode('A', std::span<char>(buffer, 1)), 1u);
}

TEST(Utf8Test, DisplayWidth) {
  EXPECT_EQ(utf8::displayWidth('a'), 1);
  EXPECT_EQ(utf8::displayWidth(0x00E9u), 1);   // e-acute
  EXPECT_EQ(utf8::displayWidth(0x0301u), 0);   // combining acute
  EXPECT_EQ(utf8::displayWidth(0x0300u), 0);   // combining grave (range start)
  EXPECT_EQ(utf8::displayWidth(0x036Fu), 0);   // range end
  EXPECT_EQ(utf8::displayWidth(0x0370u), 1);   // just past the range
  EXPECT_EQ(utf8::displayWidth(0x200Bu), 0);   // zero width space
  EXPECT_EQ(utf8::displayWidth(0x65E5u), 2);   // CJK
  EXPECT_EQ(utf8::displayWidth(0x4E00u), 2);
  EXPECT_EQ(utf8::displayWidth(0xFF21u), 2);   // fullwidth A
  EXPECT_EQ(utf8::displayWidth(0x1F600u), 2);  // grinning face
  EXPECT_EQ(utf8::displayWidth(0x20ACu), 1);   // euro sign is narrow
  EXPECT_EQ(utf8::displayWidth(0x1100u), 2);   // hangul choseong
}

TEST(Utf8Test, NextBoundaryWalksWholeCodepoints) {
  const std::string text = std::string("a") + kTwoByte + kThreeByte + kFourByte + "b";
  ASSERT_EQ(text.size(), 1u + 2u + 3u + 4u + 1u);
  size_t at = 0;
  std::vector<size_t> visited;
  while (at < text.size()) {
    visited.push_back(at);
    const size_t next = utf8::nextBoundary(text, at);
    ASSERT_GT(next, at);
    at = next;
  }
  EXPECT_EQ(at, text.size());
  const std::vector<size_t> expected = {0, 1, 3, 6, 10};
  EXPECT_EQ(visited, expected);
  EXPECT_EQ(utf8::nextBoundary(text, text.size()), text.size());
}

TEST(Utf8Test, PrevBoundaryWalksBackwards) {
  const std::string text = std::string("a") + kTwoByte + kThreeByte + kFourByte + "b";
  size_t at = text.size();
  std::vector<size_t> visited;
  while (at > 0) {
    const size_t previous = utf8::prevBoundary(text, at);
    ASSERT_LT(previous, at);
    visited.push_back(previous);
    at = previous;
  }
  const std::vector<size_t> expected = {10, 6, 3, 1, 0};
  EXPECT_EQ(visited, expected);
  EXPECT_EQ(utf8::prevBoundary(text, 0), 0u);
}

TEST(Utf8Test, PrevBoundaryFromInsideSequenceSnapsToItsStart) {
  const std::string text = kFourByte;  // 4 bytes, one codepoint
  EXPECT_EQ(utf8::prevBoundary(text, 1), 0u);
  EXPECT_EQ(utf8::prevBoundary(text, 2), 0u);
  EXPECT_EQ(utf8::prevBoundary(text, 3), 0u);
  EXPECT_EQ(utf8::prevBoundary(text, 4), 0u);
}

TEST(Utf8Test, SnapToBoundary) {
  const std::string text = std::string("x") + kFourByte + "y";
  EXPECT_EQ(utf8::snapToBoundary(text, 0), 0u);
  EXPECT_EQ(utf8::snapToBoundary(text, 1), 1u);
  EXPECT_EQ(utf8::snapToBoundary(text, 2), 1u);
  EXPECT_EQ(utf8::snapToBoundary(text, 3), 1u);
  EXPECT_EQ(utf8::snapToBoundary(text, 4), 1u);
  EXPECT_EQ(utf8::snapToBoundary(text, 5), 5u);
  EXPECT_EQ(utf8::snapToBoundary(text, 6), 6u);
  EXPECT_EQ(utf8::snapToBoundary(text, 99), text.size());
}

TEST(Utf8Test, MovementTerminatesOnGarbage) {
  // Bytes that are not valid UTF-8 at all: movement must still make progress.
  const std::string garbage("\x80\x80\xff\xc3\xf0\x9d\x80", 7);
  size_t at = 0;
  int steps = 0;
  while (at < garbage.size() && steps < 100) {
    const size_t next = utf8::nextBoundary(garbage, at);
    ASSERT_GT(next, at);
    at = next;
    ++steps;
  }
  EXPECT_EQ(at, garbage.size());

  at = garbage.size();
  steps = 0;
  while (at > 0 && steps < 100) {
    const size_t previous = utf8::prevBoundary(garbage, at);
    ASSERT_LT(previous, at);
    at = previous;
    ++steps;
  }
  EXPECT_EQ(at, 0u);
}

TEST(Utf8Test, CombiningSequenceIsTwoCodepointsButOneCell) {
  const std::string text = std::string("e") + kCombiningAcute;
  ASSERT_EQ(text.size(), 3u);
  EXPECT_EQ(utf8::nextBoundary(text, 0), 1u);
  EXPECT_EQ(utf8::nextBoundary(text, 1), 3u);
  const utf8::Decoded base = utf8::decode(text, 0);
  const utf8::Decoded mark = utf8::decode(text, 1);
  ASSERT_TRUE(base.valid);
  ASSERT_TRUE(mark.valid);
  EXPECT_EQ(mark.codepoint, 0x0301u);
  EXPECT_EQ(utf8::displayWidth(base.codepoint) + utf8::displayWidth(mark.codepoint), 1);
}

}  // namespace
