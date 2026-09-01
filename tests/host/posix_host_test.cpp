// The ONE test allowed to touch the real filesystem. It only ever reads and
// writes fixed filenames in the test's working directory (the build dir):
//   ember_posix_host_test.tmp      round-trip target
//   ember_posix_host_test.empty    the zero-length mmap special case
// Everything else (missing file, directory-as-file, readDir dot entries) is
// probed against the same two files and the working directory itself.

#include <posix_host.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace {

const char* const kTempFile = "ember_posix_host_test.tmp";
const char* const kEmptyFile = "ember_posix_host_test.empty";

}  // namespace

TEST(PosixHostTest, RealFilesystemRoundTrip) {
    ide::host::PosixHost host;

    // Leftovers from a previous run must not confuse this one.
    std::remove(kTempFile);
    std::remove(kEmptyFile);

    // --- writeFile / readFile / stat round-trip ------------------------------
    const std::string payload = "int main() { return 0; }\n// second line\n";
    ASSERT_TRUE(host.writeFile(kTempFile, payload));

    const auto read = host.readFile(kTempFile);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, payload);

    const auto info = host.stat(kTempFile);
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->isDirectory);
    EXPECT_EQ(info->size, payload.size());

    // --- empty file: mmap(0) fails on Linux, this must not -------------------
    ASSERT_TRUE(host.writeFile(kEmptyFile, ""));
    const auto emptyRead = host.readFile(kEmptyFile);
    ASSERT_TRUE(emptyRead.has_value());
    EXPECT_TRUE(emptyRead->empty());

    auto mapped = host.mapFile(kEmptyFile);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped->size(), 0u);
    // data() may be null exactly when size() == 0; if it is non-null it must
    // still never be dereferenced, which size() == 0 already guarantees.

    // --- mapFile on the non-empty file ---------------------------------------
    auto big = host.mapFile(kTempFile);
    ASSERT_NE(big, nullptr);
    ASSERT_EQ(big->size(), payload.size());
    EXPECT_EQ(std::string(big->data(), big->size()), payload);

    // --- overwrite: the rename path must replace content atomically ----------
    const std::string second = "short";
    ASSERT_TRUE(host.writeFile(kTempFile, second));
    const auto secondRead = host.readFile(kTempFile);
    ASSERT_TRUE(secondRead.has_value());
    EXPECT_EQ(*secondRead, second);

    // --- missing file ---------------------------------------------------------
    EXPECT_FALSE(host.readFile("ember_posix_host_test.does-not-exist").has_value());
    EXPECT_EQ(host.mapFile("ember_posix_host_test.does-not-exist"), nullptr);
    EXPECT_FALSE(host.stat("ember_posix_host_test.does-not-exist").has_value());

    // --- directory passed where a file is expected ----------------------------
    const auto dirInfo = host.stat(".");
    ASSERT_TRUE(dirInfo.has_value());
    EXPECT_TRUE(dirInfo->isDirectory);
    EXPECT_FALSE(host.readFile(".").has_value());
    EXPECT_EQ(host.mapFile("."), nullptr);

    // --- readDir: names only, no dot entries ----------------------------------
    const auto entries = host.readDir(".");
    ASSERT_FALSE(entries.empty());
    for (const std::string& name : entries) {
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name.front(), '.') << "dot entries must be excluded";
    }
    bool sawTemp = false;
    bool sawEmpty = false;
    for (const std::string& name : entries) {
        sawTemp = sawTemp || name == kTempFile;
        sawEmpty = sawEmpty || name == kEmptyFile;
    }
    EXPECT_TRUE(sawTemp);
    EXPECT_TRUE(sawEmpty);

    // --- readDir on a missing directory: empty, not a crash -------------------
    EXPECT_TRUE(host.readDir("ember_posix_host_test.no-such-dir").empty());

    // Cleanup; failures here are cosmetic, not test failures.
    std::remove(kTempFile);
    std::remove(kEmptyFile);
    std::remove((std::string(kTempFile) + ".tmp-ember").c_str());
    std::remove((std::string(kEmptyFile) + ".tmp-ember").c_str());
}
