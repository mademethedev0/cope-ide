// MemoryHost: the in-memory Host used by tests (and later a web build).
// Every Host method is round-tripped here, plus the edge cases the handoff
// calls out: missing file, empty file, directory-passed-as-file, readDir
// excluding dot entries. No real filesystem is ever touched.

#include <gtest/gtest.h>

#include <ide/host/host.h>
#include <ide/host/memory_host.h>

namespace {

using ide::host::FileInfo;
using ide::host::Host;
using ide::host::MemoryHost;

TEST(MemoryHostTest, ReadFileRoundTrip) {
    MemoryHost host;
    host.addFile("/src/main.cpp", "int main() { return 0; }\n");
    const auto bytes = host.readFile("/src/main.cpp");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(*bytes, "int main() { return 0; }\n");
}

TEST(MemoryHostTest, WriteFileThenReadBack) {
    MemoryHost host;
    ASSERT_TRUE(host.writeFile("/tmp/scratch.txt", "hello"));
    const auto bytes = host.readFile("/tmp/scratch.txt");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(*bytes, "hello");
    // Overwrite replaces, not appends.
    ASSERT_TRUE(host.writeFile("/tmp/scratch.txt", "bye"));
    const auto again = host.readFile("/tmp/scratch.txt");
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, "bye");
}

TEST(MemoryHostTest, ReadFileMissingReturnsNullopt) {
    MemoryHost host;
    EXPECT_FALSE(host.readFile("/no/such/file").has_value());
}

TEST(MemoryHostTest, ReadFileOnDirectoryReturnsNullopt) {
    MemoryHost host;
    host.addDir("/src");
    EXPECT_FALSE(host.readFile("/src").has_value());
}

TEST(MemoryHostTest, EmptyFileRoundTrips) {
    MemoryHost host;
    host.addFile("/empty.txt", "");
    const auto bytes = host.readFile("/empty.txt");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_TRUE(bytes->empty());

    const auto info = host.stat("/empty.txt");
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->isDirectory);
    EXPECT_EQ(info->size, 0u);

    auto mapped = host.mapFile("/empty.txt");
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped->size(), 0u);
}

TEST(MemoryHostTest, StatReportsFileAndDirectory) {
    MemoryHost host;
    host.addFile("/src/a.txt", "12345");
    host.addDir("/src");

    const auto file = host.stat("/src/a.txt");
    ASSERT_TRUE(file.has_value());
    EXPECT_FALSE(file->isDirectory);
    EXPECT_EQ(file->size, 5u);

    const auto dir = host.stat("/src");
    ASSERT_TRUE(dir.has_value());
    EXPECT_TRUE(dir->isDirectory);

    EXPECT_FALSE(host.stat("/missing").has_value());
}

TEST(MemoryHostTest, TrailingSlashIsTheSamePath) {
    MemoryHost host;
    host.addFile("/src/a.txt", "x");
    const auto viaSlash = host.stat("/src/a.txt/");
    ASSERT_TRUE(viaSlash.has_value());
    EXPECT_EQ(viaSlash->size, 1u);
}

TEST(MemoryHostTest, ReadDirListsDirectChildrenOnly) {
    MemoryHost host;
    host.addDir("/proj");
    host.addFile("/proj/main.cpp", "x");
    host.addFile("/proj/README.md", "x");
    host.addFile("/proj/src/engine.cpp", "x");   // grandchild, must not appear
    host.addFile("/proj/.hidden", "x");          // dot entry, must not appear

    const auto names = host.readDir("/proj");
    ASSERT_EQ(names.size(), 2u);
    // std::map iterates in key order, so the order is deterministic here.
    EXPECT_EQ(names[0], "README.md");
    EXPECT_EQ(names[1], "main.cpp");
}

TEST(MemoryHostTest, ReadDirOfMissingOrFileIsEmpty) {
    MemoryHost host;
    host.addFile("/a.txt", "x");
    EXPECT_TRUE(host.readDir("/nope").empty());
    EXPECT_TRUE(host.readDir("/a.txt").empty());
}

TEST(MemoryHostTest, MapFileRoundTripAndFailureModes) {
    MemoryHost host;
    host.addFile("/big.dat", "0123456789");
    host.addDir("/dir");

    auto mapped = host.mapFile("/big.dat");
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped->size(), 10u);
    EXPECT_EQ(std::string_view(mapped->data(), mapped->size()), "0123456789");

    EXPECT_TRUE(host.mapFile("/missing.dat") == nullptr);
    EXPECT_TRUE(host.mapFile("/dir") == nullptr);
}

TEST(MemoryHostTest, MappedBytesStayStableAcrossLaterWrites) {
    MemoryHost host;
    host.addFile("/a.txt", "original");
    auto mapped = host.mapFile("/a.txt");
    ASSERT_NE(mapped, nullptr);
    // Stability contract: a mapping stays valid while its path is not
    // mutated. Adding and writing OTHER entries (new map nodes) must not move
    // or invalidate the bytes we are holding.
    host.addFile("/b.txt", "other");
    host.writeFile("/c.txt", "more");
    EXPECT_EQ(std::string_view(mapped->data(), mapped->size()), "original");
}

TEST(MemoryHostTest, HostIsHost) {
    // Compile-time check that MemoryHost satisfies the injected interface.
    MemoryHost host;
    Host& asHost = host;
    (void)asHost;
    SUCCEED();
}

}  // namespace
