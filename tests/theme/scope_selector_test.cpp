#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/theme/scope_selector.h>

namespace {

using ide::theme::decodeMatchKey;
using ide::theme::encodeMatchScore;
using ide::theme::kNoMatch;
using ide::theme::MatchKey;
using ide::theme::MatchScore;
using ide::theme::scopePatternMatches;
using ide::theme::ScopeSelector;
using ide::theme::scopeSegmentCount;

using Stack = std::vector<std::string_view>;

MatchKey matchOf(std::string_view selector, const Stack& stack) {
    return ScopeSelector::parse(selector).match(std::span<const std::string_view>(stack));
}

bool matches(std::string_view selector, const Stack& stack) {
    return matchOf(selector, stack) != kNoMatch;
}

MatchScore scoreOf(std::string_view selector, const Stack& stack) {
    return decodeMatchKey(matchOf(selector, stack));
}

// --- per-segment prefix rule --------------------------------------------------

TEST(ScopePatternTest, WholeSegmentPrefixOnly) {
    EXPECT_TRUE(scopePatternMatches("string", "string"));
    EXPECT_TRUE(scopePatternMatches("string", "string.quoted"));
    EXPECT_TRUE(scopePatternMatches("string.quoted", "string.quoted.double.js"));
    EXPECT_TRUE(scopePatternMatches("string.quoted.double.js", "string.quoted.double.js"));

    // A partial segment is never a match.
    EXPECT_FALSE(scopePatternMatches("stri", "string"));
    EXPECT_FALSE(scopePatternMatches("stri", "string.quoted"));
    EXPECT_FALSE(scopePatternMatches("str.quoted", "string.quoted"));
    EXPECT_FALSE(scopePatternMatches("string.quote", "string.quoted"));

    // The pattern may not be longer than the scope.
    EXPECT_FALSE(scopePatternMatches("string.quoted.double", "string.quoted"));
    EXPECT_FALSE(scopePatternMatches("string.quoted", "string"));

    // Anchored at the first segment: a suffix match is not a match.
    EXPECT_FALSE(scopePatternMatches("comment", "punctuation.definition.comment"));
    EXPECT_FALSE(scopePatternMatches("quoted", "string.quoted"));
}

TEST(ScopePatternTest, WildcardSegment) {
    EXPECT_TRUE(scopePatternMatches("*", "anything"));
    EXPECT_TRUE(scopePatternMatches("*", "a.b.c"));
    EXPECT_TRUE(scopePatternMatches("meta.*.js", "meta.function.js"));
    EXPECT_TRUE(scopePatternMatches("meta.*", "meta.function.js"));
    EXPECT_FALSE(scopePatternMatches("meta.*.js", "meta.js"));      // '*' needs a segment
    EXPECT_FALSE(scopePatternMatches("meta.*.js", "meta.a.ts"));
}

TEST(ScopePatternTest, EdgeCasesAreTotal) {
    EXPECT_TRUE(scopePatternMatches("", ""));
    EXPECT_TRUE(scopePatternMatches("", "anything"));  // empty pattern is unconstrained
    EXPECT_FALSE(scopePatternMatches("a", ""));
    EXPECT_TRUE(scopePatternMatches("a.", "a."));
    EXPECT_FALSE(scopePatternMatches("a.", "a"));  // "a." is two segments, "a" is one
}

TEST(ScopePatternTest, Utf8SegmentsCompareByBytes) {
    EXPECT_TRUE(scopePatternMatches("str\xC3\xA4ng", "str\xC3\xA4ng.quoted"));
    EXPECT_FALSE(scopePatternMatches("str\xC3\xA4ng", "strang.quoted"));
    EXPECT_FALSE(scopePatternMatches("str\xC3\xA4", "str\xC3\xA4ng"));  // truncated UTF-8 sequence
    EXPECT_TRUE(scopePatternMatches("\xE6\xBC\xA2.\xE5\xAD\x97", "\xE6\xBC\xA2.\xE5\xAD\x97.x"));
}

TEST(ScopePatternTest, SegmentCount) {
    EXPECT_EQ(scopeSegmentCount(""), 0u);
    EXPECT_EQ(scopeSegmentCount("a"), 1u);
    EXPECT_EQ(scopeSegmentCount("a.b"), 2u);
    EXPECT_EQ(scopeSegmentCount("comment.line.double-slash.js"), 4u);
    EXPECT_EQ(scopeSegmentCount("a."), 2u);
    EXPECT_EQ(scopeSegmentCount("."), 2u);
}

TEST(ScopePatternTest, HugeSegmentCountsDoNotOverflowOrCrash) {
    std::string pattern;
    std::string scope;
    for (int i = 0; i < 2000; ++i) {
        if (i != 0) pattern.push_back('.');
        pattern.append("seg");
    }
    scope = pattern + ".tail";
    EXPECT_EQ(scopeSegmentCount(pattern), 2000u);
    EXPECT_TRUE(scopePatternMatches(pattern, scope));
    EXPECT_FALSE(scopePatternMatches(scope, pattern));
}

// --- the selector x scope-stack table ----------------------------------------

TEST(ScopeSelectorTest, DescendantPaths) {
    const Stack js{"source.js", "meta.function.js", "meta.block.js", "entity.name.function.js"};

    EXPECT_TRUE(matches("source.js meta.function entity.name", js));  // gaps allowed
    EXPECT_TRUE(matches("meta.function entity.name", js));
    EXPECT_TRUE(matches("source entity", js));
    EXPECT_TRUE(matches("source.js", js));
    EXPECT_TRUE(matches("entity.name.function", js));

    EXPECT_FALSE(matches("entity.name source.js", js));  // order matters
    EXPECT_FALSE(matches("source.js.embedded", js));
    EXPECT_FALSE(matches("meta.function meta.function meta.function", js));  // only two meta.function
    EXPECT_TRUE(matches("meta.function meta.block", js));
}

TEST(ScopeSelectorTest, InnermostElementIsMatchedLast) {
    const Stack stack{"source.js", "comment.line.double-slash.js", "punctuation.definition.comment.js"};

    EXPECT_TRUE(matches("comment", stack));
    EXPECT_TRUE(matches("comment.line", stack));
    EXPECT_TRUE(matches("comment.line.double-slash.js", stack));
    EXPECT_TRUE(matches("punctuation.definition", stack));
    EXPECT_TRUE(matches("comment punctuation.definition.comment", stack));
    EXPECT_FALSE(matches("comment.block", stack));
    EXPECT_FALSE(matches("punctuation.definition.comment.js.extra", stack));

    // "comment" matches element 1, not the innermost element, so the depth is 2.
    EXPECT_EQ(scoreOf("comment", stack).matchDepth, 2u);
    EXPECT_EQ(scoreOf("punctuation", stack).matchDepth, 3u);
}

TEST(ScopeSelectorTest, HyphensInsideScopeNamesAreNotExclusions) {
    const Stack stack{"source.js", "comment.line.double-slash.js"};
    EXPECT_TRUE(matches("comment.line.double-slash.js", stack));
    EXPECT_TRUE(matches("comment.line.double-slash", stack));
    const ScopeSelector selector = ScopeSelector::parse("comment.line.double-slash.js");
    ASSERT_EQ(selector.alternativeCount(), 1u);
    ASSERT_EQ(selector.pathAt(0).size(), 1u);
    EXPECT_EQ(selector.pathAt(0)[0], "comment.line.double-slash.js");
    EXPECT_EQ(selector.exclusionCountAt(0), 0u);
}

TEST(ScopeSelectorTest, Exclusions) {
    const Stack commented{"source.js", "comment.line.double-slash.js"};
    const Stack plain{"source.js", "keyword.control.js"};

    EXPECT_FALSE(matches("source.js -comment", commented));
    EXPECT_TRUE(matches("source.js -comment", plain));
    EXPECT_TRUE(matches("source.js -string", commented));

    // Excluded scopes are searched over the whole stack, deeper elements included.
    const Stack deep{"source.js", "meta.block.js", "comment.line.js"};
    EXPECT_FALSE(matches("source.js -comment", deep));
    EXPECT_FALSE(matches("meta.block -comment", deep));

    // Both spellings that occur in the shipped themes.
    const Stack imported{"source.ts", "meta.import.ts", "variable.other.readwrite.ts"};
    const Stack notImported{"source.ts", "variable.other.readwrite.ts"};
    EXPECT_FALSE(matches("variable - meta.import", imported));
    EXPECT_TRUE(matches("variable - meta.import", notImported));
    EXPECT_TRUE(matches("variable", imported));

    const ScopeSelector selector = ScopeSelector::parse("source.js -comment -string");
    ASSERT_EQ(selector.alternativeCount(), 1u);
    EXPECT_EQ(selector.pathAt(0).size(), 1u);
    EXPECT_EQ(selector.exclusionCountAt(0), 2u);

    // An exclusion with no positive part matches nothing rather than everything.
    EXPECT_EQ(ScopeSelector::parse("-comment").alternativeCount(), 0u);
    EXPECT_FALSE(matches("-comment", plain));
    EXPECT_FALSE(matches("-comment", commented));
}

TEST(ScopeSelectorTest, CommaAndPipeAlternatives) {
    EXPECT_TRUE(matches("comment, string", Stack{"string.quoted.double"}));
    EXPECT_TRUE(matches("comment, string", Stack{"comment.line"}));
    EXPECT_FALSE(matches("comment, string", Stack{"keyword.control"}));
    EXPECT_TRUE(matches("block.scope.end,block.scope.begin", Stack{"block.scope.begin"}));

    const Stack stack{"source.js", "comment.line.double-slash.js", "punctuation.definition.comment.js"};
    EXPECT_TRUE(matches("comment punctuation.definition.comment, string.quoted.docstring", stack));
    EXPECT_FALSE(matches("string.quoted.docstring, keyword", stack));

    // TextMate's '|' is alternation too.
    EXPECT_TRUE(matches("markup.heading | markup.heading entity.name", Stack{"markup.heading"}));
    EXPECT_TRUE(matches("markup.heading | markup.heading entity.name",
                        Stack{"markup.heading", "entity.name.section"}));
    EXPECT_FALSE(matches("markup.heading | markup.heading entity.name", Stack{"markup.list"}));

    EXPECT_EQ(ScopeSelector::parse("a, b, c").alternativeCount(), 3u);
    EXPECT_EQ(ScopeSelector::parse("a,,b").alternativeCount(), 2u);  // empty branch dropped
    EXPECT_EQ(ScopeSelector::parse(",").alternativeCount(), 0u);
}

TEST(ScopeSelectorTest, GroupsExpandIntoAlternatives) {
    const ScopeSelector selector = ScopeSelector::parse("(string, comment) punctuation");
    ASSERT_EQ(selector.alternativeCount(), 2u);
    ASSERT_EQ(selector.pathAt(0).size(), 2u);
    EXPECT_EQ(selector.pathAt(0)[0], "string");
    EXPECT_EQ(selector.pathAt(0)[1], "punctuation");
    EXPECT_EQ(selector.pathAt(1)[0], "comment");
    EXPECT_EQ(selector.pathAt(1)[1], "punctuation");

    EXPECT_TRUE(matches("(string, comment) punctuation",
                        Stack{"source.js", "string.quoted.double.js", "punctuation.definition.string.begin.js"}));
    EXPECT_TRUE(matches("(string, comment) punctuation",
                        Stack{"source.js", "comment.block.js", "punctuation.definition.comment.js"}));
    EXPECT_FALSE(matches("(string, comment) punctuation", Stack{"source.js", "keyword", "punctuation.x"}));
    EXPECT_TRUE(matches("(string, comment)", Stack{"comment.line"}));
    EXPECT_TRUE(matches("meta (string, comment)", Stack{"meta.tag", "comment.line"}));
    EXPECT_FALSE(matches("meta (string, comment)", Stack{"comment.line", "meta.tag"}));
}

TEST(ScopeSelectorTest, MalformedInputNeverCrashes) {
    const Stack stack{"source.js", "comment.line"};
    for (const std::string_view text : {"", " ", "\t\n", ",", ",,,", "-", "- ", "--", "-,-", "(", ")", "()",
                                        "(()", "())", "(,)", "a (", ") a", "-(", "-()", "((((((((((((a))))))))))))",
                                        "a -", "|", "a |", "| a", "a - - b", ".", "..", "a..b"}) {
        const ScopeSelector selector = ScopeSelector::parse(text);
        (void)selector.match(std::span<const std::string_view>(stack));
        EXPECT_EQ(selector.sourceText(), text);
    }
    // A selector nobody can parse must match nothing, never everything.
    EXPECT_FALSE(matches("", stack));
    EXPECT_FALSE(matches("   ", stack));
    EXPECT_FALSE(matches(",", stack));
    EXPECT_FALSE(matches("()", stack));
    EXPECT_TRUE(matches("a..b", Stack{"a..b.c"}));  // empty middle segment still compares
}

TEST(ScopeSelectorTest, ColonsAreOrdinaryScopeCharacters) {
    // From textmate/themes/dark/aurora-x.json and friends.
    const Stack stack{"source.astro", "source.astro.meta.attribute.client:idle.html"};
    EXPECT_TRUE(matches("source.astro.meta.attribute.client:idle.html", stack));
    EXPECT_TRUE(matches("source.astro.meta.attribute", stack));
    EXPECT_FALSE(matches("source.astro.meta.attribute.client", stack));
}

TEST(ScopeSelectorTest, EmptyStackMatchesNothing) {
    const Stack empty;
    EXPECT_FALSE(matches("comment", empty));
    EXPECT_FALSE(matches("*", empty));
    EXPECT_FALSE(matches("a, b, c", empty));
}

TEST(ScopeSelectorTest, WhitespaceIsInsignificant) {
    EXPECT_TRUE(matches("  comment  ", Stack{"comment.line"}));
    EXPECT_TRUE(matches("source.js\tcomment", Stack{"source.js", "comment.line"}));
    EXPECT_TRUE(matches("comment ,\n string", Stack{"string.quoted"}));
}

TEST(ScopeSelectorTest, DeepStacksAndManyAlternativesStayCheap) {
    Stack deep;
    deep.reserve(500);
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> names;
        names.reserve(500);
        for (int i = 0; i < 500; ++i) names.push_back("meta.level" + std::to_string(i) + ".js");
        return names;
    }();
    for (const std::string& name : owned) deep.push_back(name);

    EXPECT_TRUE(matches("meta.level0 meta.level250 meta.level499", deep));
    EXPECT_FALSE(matches("meta.level499 meta.level0", deep));
    EXPECT_EQ(scoreOf("meta.level0 meta.level250 meta.level499", deep).matchDepth, 500u);
    EXPECT_EQ(scoreOf("meta", deep).matchDepth, 500u);  // deepest possible placement wins
}

// --- specificity --------------------------------------------------------------

TEST(ScopeSelectorSpecificityTest, ScoreFieldsAreAsDocumented) {
    const Stack stack{"source.js", "comment.line.double-slash.js"};

    EXPECT_EQ(scoreOf("comment", stack), (MatchScore{1, 2, 1}));
    EXPECT_EQ(scoreOf("comment.line.double-slash.js", stack), (MatchScore{1, 2, 4}));
    EXPECT_EQ(scoreOf("source.js", stack), (MatchScore{1, 1, 2}));
    EXPECT_EQ(scoreOf("source.js comment", stack), (MatchScore{2, 2, 3}));
    EXPECT_FALSE(scoreOf("keyword", stack).matched());
}

TEST(ScopeSelectorSpecificityTest, RankingOrder) {
    const Stack stack{"source.js", "comment.line.double-slash.js"};

    const MatchKey path = matchOf("source.js comment", stack);
    const MatchKey specific = matchOf("comment.line.double-slash.js", stack);
    const MatchKey shallow = matchOf("source.js", stack);
    const MatchKey generic = matchOf("comment", stack);

    // 1. more path elements beats everything else
    EXPECT_GT(path, specific);
    EXPECT_GT(path, generic);
    // 2. deeper in the stack beats more segments
    EXPECT_GT(generic, shallow);
    // 3. at equal depth, more segments wins
    EXPECT_GT(specific, generic);
    EXPECT_NE(generic, kNoMatch);
}

TEST(ScopeSelectorSpecificityTest, BestAlternativeRepresentsTheSelector) {
    const Stack stack{"source.js", "comment.line.double-slash.js"};
    // The second alternative is the more specific one, and it is what the
    // selector as a whole scores.
    EXPECT_EQ(matchOf("source.js, comment.line.double-slash.js", stack),
              matchOf("comment.line.double-slash.js", stack));
    EXPECT_EQ(matchOf("comment.line.double-slash.js, source.js", stack),
              matchOf("comment.line.double-slash.js", stack));
}

TEST(ScopeSelectorSpecificityTest, EncodeDecodeRoundTrip) {
    const MatchScore score{3, 7, 11};
    const MatchKey key = encodeMatchScore(score);
    EXPECT_EQ(decodeMatchKey(key), score);
    EXPECT_NE(key, kNoMatch);

    EXPECT_EQ(encodeMatchScore(MatchScore{0, 5, 5}), kNoMatch);  // pathLength 0 is "no match"
    EXPECT_EQ(decodeMatchKey(kNoMatch), (MatchScore{0, 0, 0}));

    const MatchScore saturated{70000, 70000, 70000};
    EXPECT_EQ(decodeMatchKey(encodeMatchScore(saturated)), (MatchScore{0xFFFF, 0xFFFF, 0xFFFF}));

    // Field order: path dominates depth dominates segments.
    EXPECT_GT(encodeMatchScore(MatchScore{2, 1, 1}), encodeMatchScore(MatchScore{1, 0xFFFF, 0xFFFF}));
    EXPECT_GT(encodeMatchScore(MatchScore{1, 2, 1}), encodeMatchScore(MatchScore{1, 1, 0xFFFF}));
    EXPECT_GT(encodeMatchScore(MatchScore{1, 1, 2}), encodeMatchScore(MatchScore{1, 1, 1}));
}

TEST(ScopeSelectorTest, CopyingKeepsBehaviour) {
    const ScopeSelector original = ScopeSelector::parse("source.js comment, string -meta");
    const ScopeSelector copy = original;
    const Stack stack{"source.js", "comment.line"};
    EXPECT_EQ(copy.alternativeCount(), original.alternativeCount());
    EXPECT_EQ(copy.match(std::span<const std::string_view>(stack)),
              original.match(std::span<const std::string_view>(stack)));
    EXPECT_EQ(copy.sourceText(), "source.js comment, string -meta");
}

}  // namespace
