#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ide/syntax/scope_stack.h>

namespace {

using ide::syntax::kRootScopeStack;
using ide::syntax::ScopeStackId;
using ide::syntax::ScopeStackTable;

TEST(ScopeStackTableTest, RootIsInternedFirstAndEmpty) {
    ScopeStackTable table;
    EXPECT_EQ(table.size(), 1u);
    EXPECT_TRUE(table.valid(kRootScopeStack));
    EXPECT_EQ(kRootScopeStack, 0);
    EXPECT_EQ(table.depth(kRootScopeStack), 0u);
    EXPECT_EQ(table.scopeAt(kRootScopeStack), "");
    EXPECT_EQ(table.parentOf(kRootScopeStack), kRootScopeStack);
    EXPECT_TRUE(table.resolve(kRootScopeStack).empty());
    EXPECT_EQ(table.flatten(kRootScopeStack), "");
}

TEST(ScopeStackTableTest, PushSingleScope) {
    ScopeStackTable table;
    const ScopeStackId id = table.push(kRootScopeStack, "source.c");
    EXPECT_NE(id, kRootScopeStack);
    EXPECT_EQ(table.size(), 2u);
    EXPECT_EQ(table.depth(id), 1u);
    EXPECT_EQ(table.scopeAt(id), "source.c");
    EXPECT_EQ(table.parentOf(id), kRootScopeStack);
    EXPECT_EQ(table.flatten(id), "source.c");

    const std::vector<std::string_view> parts = table.resolve(id);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0], "source.c");
}

TEST(ScopeStackTableTest, SpaceSeparatedNameBecomesSeveralEntries) {
    ScopeStackTable table;
    const ScopeStackId id = table.push(kRootScopeStack, "meta.a meta.b meta.c");
    EXPECT_EQ(table.depth(id), 3u);
    EXPECT_EQ(table.scopeAt(id), "meta.c");
    EXPECT_EQ(table.flatten(id), "meta.a meta.b meta.c");
    EXPECT_EQ(table.flatten(id, '|'), "meta.a|meta.b|meta.c");
    EXPECT_EQ(table.size(), 4u);  // root + three nodes

    // Repeated and leading/trailing separators collapse.
    const ScopeStackId same = table.push(kRootScopeStack, "  meta.a   meta.b meta.c ");
    EXPECT_EQ(same, id);
    EXPECT_EQ(table.size(), 4u);

    const std::vector<std::string_view> parts = table.resolve(id);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "meta.a");  // outermost first
    EXPECT_EQ(parts[1], "meta.b");
    EXPECT_EQ(parts[2], "meta.c");
}

TEST(ScopeStackTableTest, EmptyOrBlankNameReturnsTheParentUnchanged) {
    ScopeStackTable table;
    const ScopeStackId parent = table.push(kRootScopeStack, "source.x");
    EXPECT_EQ(table.push(parent, ""), parent);
    EXPECT_EQ(table.push(parent, " "), parent);
    EXPECT_EQ(table.push(parent, "     "), parent);
    EXPECT_EQ(table.push(kRootScopeStack, ""), kRootScopeStack);
    EXPECT_EQ(table.size(), 2u);  // nothing new interned
}

TEST(ScopeStackTableTest, IdenticalStacksAreInternedToTheSameId) {
    ScopeStackTable table;
    const ScopeStackId a1 = table.push(kRootScopeStack, "source.x");
    const ScopeStackId a2 = table.push(kRootScopeStack, "source.x");
    EXPECT_EQ(a1, a2);

    const ScopeStackId b1 = table.push(a1, "string.quoted");
    const ScopeStackId b2 = table.push(a2, "string.quoted");
    EXPECT_EQ(b1, b2);
    EXPECT_EQ(table.size(), 3u);

    // Same leaf name, different parent -> different id.
    const ScopeStackId other = table.push(kRootScopeStack, "other");
    const ScopeStackId c = table.push(other, "string.quoted");
    EXPECT_NE(c, b1);
    EXPECT_EQ(table.scopeAt(c), table.scopeAt(b1));
    EXPECT_EQ(table.flatten(c), "other string.quoted");
}

TEST(ScopeStackTableTest, DeepChainResolvesOutermostFirst) {
    ScopeStackTable table;
    ScopeStackId id = kRootScopeStack;
    const size_t kDepth = 64u;
    for (size_t i = 0; i < kDepth; ++i) {
        id = table.push(id, "s" + std::to_string(i));
    }
    EXPECT_EQ(table.depth(id), kDepth);
    EXPECT_EQ(table.scopeAt(id), "s63");

    const std::vector<std::string_view> parts = table.resolve(id);
    ASSERT_EQ(parts.size(), kDepth);
    for (size_t i = 0; i < kDepth; ++i) {
        EXPECT_EQ(parts[i], "s" + std::to_string(i));
    }

    // parentOf walks back down one level at a time.
    ScopeStackId walk = id;
    for (size_t i = 0; i < kDepth; ++i) {
        EXPECT_EQ(table.depth(walk), kDepth - i);
        walk = table.parentOf(walk);
    }
    EXPECT_EQ(walk, kRootScopeStack);
}

TEST(ScopeStackTableTest, InvalidIdsDegradeToRoot) {
    ScopeStackTable table;
    const ScopeStackId valid = table.push(kRootScopeStack, "a");
    EXPECT_FALSE(table.valid(-1));
    EXPECT_FALSE(table.valid(999));
    EXPECT_EQ(table.depth(-1), 0u);
    EXPECT_EQ(table.depth(999), 0u);
    EXPECT_EQ(table.scopeAt(-1), "");
    EXPECT_EQ(table.scopeAt(999), "");
    EXPECT_EQ(table.parentOf(-1), kRootScopeStack);
    EXPECT_EQ(table.parentOf(999), kRootScopeStack);
    EXPECT_TRUE(table.resolve(-1).empty());
    EXPECT_EQ(table.flatten(999), "");

    // Pushing onto an invalid parent behaves like pushing onto the root.
    EXPECT_EQ(table.push(-5, "a"), valid);
    EXPECT_EQ(table.push(999, "a"), valid);
}

TEST(ScopeStackTableTest, ScopeNamesAreByteExactIncludingUtf8) {
    ScopeStackTable table;
    const std::string name = "meta.\xC3\xA9\xE2\x82\xAC";  // e-acute + euro sign
    const ScopeStackId id = table.push(kRootScopeStack, name);
    EXPECT_EQ(table.scopeAt(id), name);
    EXPECT_EQ(table.flatten(id), name);
    EXPECT_EQ(table.depth(id), 1u);

    // Multi-byte bytes are never treated as separators.
    const ScopeStackId two = table.push(kRootScopeStack, name + " b");
    EXPECT_EQ(table.depth(two), 2u);
    EXPECT_EQ(table.flatten(two), name + " b");
}

TEST(ScopeStackTableTest, ResolveIntoCallerBufferClearsIt) {
    ScopeStackTable table;
    const ScopeStackId id = table.push(kRootScopeStack, "a b");
    std::vector<std::string_view> buffer{"stale", "entries", "here"};
    table.resolve(id, buffer);
    ASSERT_EQ(buffer.size(), 2u);
    EXPECT_EQ(buffer[0], "a");
    EXPECT_EQ(buffer[1], "b");

    table.resolve(kRootScopeStack, buffer);
    EXPECT_TRUE(buffer.empty());
}

TEST(ScopeStackTableTest, ManyDistinctStacksStayCorrect) {
    // Interning must not confuse stacks that share prefixes.
    ScopeStackTable table;
    std::vector<ScopeStackId> ids;
    ids.reserve(200u);
    for (size_t i = 0; i < 200u; ++i) {
        const ScopeStackId base = table.push(kRootScopeStack, "base" + std::to_string(i % 7u));
        ids.push_back(table.push(base, "leaf" + std::to_string(i)));
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(table.flatten(ids[i]),
                  "base" + std::to_string(i % 7u) + " leaf" + std::to_string(i));
        EXPECT_EQ(table.depth(ids[i]), 2u);
    }
    // 7 shared bases + 200 leaves + root.
    EXPECT_EQ(table.size(), 208u);
}

}  // namespace
