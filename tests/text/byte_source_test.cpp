#include <gtest/gtest.h>

#include <ide/text/byte_source.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using ide::text::ByteSource;
using ide::text::MappedByteSource;
using ide::text::OwnedByteSource;

TEST(OwnedByteSourceTest, DefaultIsEmptyAndSafeToView) {
  const OwnedByteSource source;
  EXPECT_EQ(source.size(), 0u);
  // Must not build a string_view over a null pointer.
  EXPECT_TRUE(source.view().empty());
  EXPECT_EQ(source.view().size(), 0u);
}

TEST(OwnedByteSourceTest, FromString) {
  const OwnedByteSource source(std::string("hello"));
  EXPECT_EQ(source.size(), 5u);
  EXPECT_EQ(source.view(), "hello");
}

TEST(OwnedByteSourceTest, FromVectorKeepsEmbeddedNulAndHighBytes) {
  const std::vector<char> bytes = {'a', '\0', 'b', static_cast<char>(0xFF)};
  const OwnedByteSource source(bytes);
  ASSERT_EQ(source.size(), 4u);
  EXPECT_EQ(source.view().size(), 4u);
  EXPECT_EQ(source.view()[0], 'a');
  EXPECT_EQ(source.view()[1], '\0');
  EXPECT_EQ(source.view()[2], 'b');
  EXPECT_EQ(static_cast<unsigned char>(source.view()[3]), 0xFFu);
}

TEST(OwnedByteSourceTest, MakeReturnsSharedByteSource) {
  const std::shared_ptr<const ByteSource> source = OwnedByteSource::make(std::string("abc"));
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->size(), 3u);
  EXPECT_EQ(source->view(), "abc");
}

TEST(OwnedByteSourceTest, DataPointerIsStableAcrossReads) {
  const OwnedByteSource source(std::string("stable"));
  const char* first = source.data();
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(source.data(), first);
  }
}

TEST(MappedByteSourceTest, DefaultConstructedIsEmptyAndNotMapped) {
  const MappedByteSource source;
  EXPECT_FALSE(source.isMapped());
  EXPECT_EQ(source.size(), 0u);
  EXPECT_EQ(source.data(), nullptr);
  EXPECT_TRUE(source.view().empty());
}

TEST(MappedByteSourceTest, AdoptsHostProvidedRange) {
  // Stands in for a mapping the Host will hand over in a later phase; this
  // class never maps anything itself.
  static const char kBytes[] = "mapped bytes";
  const MappedByteSource source(kBytes, sizeof(kBytes) - 1);
  EXPECT_TRUE(source.isMapped());
  EXPECT_EQ(source.size(), sizeof(kBytes) - 1);
  EXPECT_EQ(source.view(), "mapped bytes");
  EXPECT_EQ(source.data(), kBytes);
}

TEST(ByteSourceTest, PolymorphicUseThroughBase) {
  const std::shared_ptr<const ByteSource> owned = OwnedByteSource::make(std::string("xyz"));
  const ByteSource& base = *owned;
  EXPECT_EQ(base.size(), 3u);
  EXPECT_EQ(base.view(), "xyz");
}

}  // namespace
