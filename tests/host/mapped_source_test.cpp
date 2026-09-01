// MappedSource: the owning bridge from host::MappedFile to text::ByteSource.
// This is what makes zero-copy open of a huge file real: Document pieces point
// straight into the mapping.

#include <gtest/gtest.h>

#include <string_view>

#include <ide/host/memory_host.h>
#include <ide/host/mapped_source.h>
#include <ide/text/byte_source.h>

namespace {

using ide::host::MappedSource;
using ide::host::MemoryHost;

TEST(MappedSourceTest, PresentsMappingBytesAsByteSource) {
    MemoryHost host;
    host.addFile("/doc.txt", "the quick brown fox");

    auto source = std::make_shared<MappedSource>(host.mapFile("/doc.txt"));
    ASSERT_TRUE(source->isMapped());
    EXPECT_EQ(source->size(), 19u);
    EXPECT_EQ(source->view(), "the quick brown fox");
}

TEST(MappedSourceTest, DefaultConstructedIsEmptyAndValid) {
    MappedSource source;
    EXPECT_FALSE(source.isMapped());
    EXPECT_EQ(source.size(), 0u);
    EXPECT_EQ(source.data(), nullptr);
    // view() must never wrap a null pointer.
    EXPECT_TRUE(source.view().empty());
}

TEST(MappedSourceTest, NullAdoptionIsEmptyAndValid) {
    MemoryHost host;  // "/missing" maps to nullptr
    MappedSource source(host.mapFile("/missing"));
    EXPECT_FALSE(source.isMapped());
    EXPECT_EQ(source.size(), 0u);
    EXPECT_TRUE(source.view().empty());
}

TEST(MappedSourceTest, EmptyFileMapsToValidEmptySource) {
    MemoryHost host;
    host.addFile("/empty.txt", "");
    MappedSource source(host.mapFile("/empty.txt"));
    ASSERT_TRUE(source.isMapped());
    EXPECT_EQ(source.size(), 0u);
    EXPECT_NE(source.data(), nullptr);  // valid pointer, per the contract
}

TEST(MappedSourceTest, BytesStayStableWhileSourceAlive) {
    MemoryHost host;
    host.addFile("/a.txt", "stable bytes");
    auto source = std::make_shared<MappedSource>(host.mapFile("/a.txt"));
    const char* pointer = source->data();
    host.addFile("/b.txt", "unrelated");
    EXPECT_EQ(source->data(), pointer);
    EXPECT_EQ(source->view(), "stable bytes");
}

TEST(MappedSourceTest, MakeFactoryProducesSharedByteSource) {
    // The factory exists for Document's shared_ptr<const ByteSource> parameter.
    MemoryHost host;
    host.addFile("/doc.txt", "x");
    std::shared_ptr<const ide::text::ByteSource> source =
        MappedSource::make(host.mapFile("/doc.txt"));
    EXPECT_EQ(source->view(), "x");
}

}  // namespace
