// PCRE2 adapter tests. The same file compiles in BOTH CI legs:
//  - COPE_USE_PCRE2=OFF: only the factory-selection test runs, asserting the
//    fallback to std::regex (the adapter is compiled out entirely).
//  - COPE_USE_PCRE2=ON:  the full adapter suite runs.
#include <ide/syntax/regex.h>
#include <ide/syntax/regex_factory.h>
#include <ide/syntax/std_regex_engine.h>
#ifdef COPE_HAS_PCRE2
#include <ide/syntax/regex_pcre2.h>
#endif

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using ide::syntax::IRegexEngine;
using ide::syntax::makeRegexEngine;

#ifdef COPE_HAS_PCRE2

using ide::syntax::Capture;
using ide::syntax::MatchResult;

/// Calls compile() and asserts it produced a pattern; failure output includes
/// the engine's reason so a refused pattern fails loudly, not silently.
std::shared_ptr<ide::syntax::IRegex> mustCompile(IRegexEngine& engine,
                                                 const std::string& pattern) {
    auto re = engine.compile(pattern);
    if (!re) {
        ADD_FAILURE() << "engine " << engine.name() << " refused [" << pattern << "]: "
                      << engine.lastError();
    }
    return re;
}

using ide::syntax::Pcre2RegexEngine;

TEST(RegexFactoryTest, SelectsPcre2WhenBuiltWithIt) {
    auto engine = makeRegexEngine();
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->name(), std::string_view("pcre2"));
}

TEST(Pcre2EngineTest, IdentityAndCaps) {
    Pcre2RegexEngine engine;
    EXPECT_EQ(engine.name(), std::string_view("pcre2"));
    const auto caps = engine.caps();
    EXPECT_TRUE(caps.lookbehind);
    EXPECT_TRUE(caps.anchorG);
    EXPECT_TRUE(caps.possessive);
    EXPECT_TRUE(caps.atomicGroups);
    EXPECT_TRUE(caps.utf8Aware);  // PCRE2_UTF: codepoints, not bytes
}

TEST(Pcre2EngineTest, CompileRefusalReportsReasonAndCachesIt) {
    Pcre2RegexEngine engine;
    // Unbounded lookbehind: PCRE2 refuses at compile time (bounded only).
    EXPECT_EQ(engine.compile("(?<=a*)x"), nullptr);
    EXPECT_FALSE(engine.lastError().empty());
    EXPECT_FALSE(engine.errorFor("(?<=a*)x").empty());
    // Malformed pattern must refuse, not crash.
    EXPECT_EQ(engine.compile("(["), nullptr);
    EXPECT_FALSE(engine.lastError().empty());
    // Negative results are cached: same refusal twice, one cache entry.
    const size_t sizeAfterFirst = engine.cacheSize();
    EXPECT_EQ(engine.compile("(?<=a*)x"), nullptr);
    EXPECT_EQ(engine.cacheSize(), sizeAfterFirst);
    EXPECT_EQ(engine.stats().rejected, size_t(2));  // two distinct bad patterns
}

TEST(Pcre2EngineTest, MatchOffsetsAreAbsoluteBytes) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, R"(\bworld\b)");
    auto m = re->search("hello world", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(6));
    EXPECT_EQ(m->end, size_t(11));
    // Starting past the match finds nothing.
    EXPECT_FALSE(re->search("hello world", 7).has_value());
    // Starting exactly at the match still finds it.
    auto m2 = re->search("hello world", 6);
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->begin, size_t(6));
}

TEST(Pcre2EngineTest, OnigurumaHexClassesAreNotHorizontalWhitespace) {
    Pcre2RegexEngine engine;
    for (const std::string pattern : {R"(^\h+$)", R"(^[\h]+$)"}) {
        SCOPED_TRACE(pattern);
        const auto re = mustCompile(engine, pattern);
        ASSERT_NE(re, nullptr);
        EXPECT_TRUE(re->search("09aAfF", 0).has_value());
        EXPECT_FALSE(re->search(" \t", 0).has_value());
        EXPECT_FALSE(re->search("G", 0).has_value());
    }
    for (const std::string pattern : {R"(^\H+$)", R"(^[\H]+$)"}) {
        SCOPED_TRACE(pattern);
        const auto re = mustCompile(engine, pattern);
        ASSERT_NE(re, nullptr);
        EXPECT_TRUE(re->search("G_ \t", 0).has_value());
        EXPECT_FALSE(re->search("aF09", 0).has_value());
    }
    const auto negative = mustCompile(engine, R"(^[^\H]+$)");
    ASSERT_NE(negative, nullptr);
    EXPECT_TRUE(negative->search("deadBEEF", 0).has_value());
    EXPECT_FALSE(negative->search("g ", 0).has_value());
    const auto unionClass = mustCompile(engine, R"(^[x\h]+$)");
    ASSERT_NE(unionClass, nullptr);
    EXPECT_TRUE(unionClass->search("xF0", 0).has_value());
}

TEST(Pcre2EngineTest, HexTranslationPreservesEscapedAndQuotedText) {
    Pcre2RegexEngine engine;
    const auto escaped = mustCompile(engine, R"(^\\h$)");
    ASSERT_NE(escaped, nullptr);
    EXPECT_TRUE(escaped->search(R"(\h)", 0).has_value());
    const auto quoted = mustCompile(engine, R"(^\Q[\h\H\E\h$)");
    ASSERT_NE(quoted, nullptr);
    EXPECT_TRUE(quoted->search(R"([\h\HF)", 0).has_value());
    EXPECT_FALSE(quoted->search(R"([\h\H )", 0).has_value());
    EXPECT_EQ(engine.compile(R"(\g<>)"), nullptr);
}

TEST(Pcre2EngineTest, LookbehindMultichar) {
    Pcre2RegexEngine engine;
    // Multi-character lookbehind: rejected outright by std::regex.
    auto re = mustCompile(engine, "(?<=ab)c");
    auto m = re->search("xabc", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(3));
    EXPECT_EQ(m->end, size_t(3) + 1);
    EXPECT_FALSE(re->search("xbc", 0).has_value());
}

TEST(Pcre2EngineTest, NegativeLookbehind) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "(?<!\\w)@");
    auto m = re->search("a @", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(2));
    EXPECT_FALSE(re->search("a@", 0).has_value());
}

TEST(Pcre2EngineTest, PossessiveQuantifierDoesNotBacktrack) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "a++a");
    // Possessive a++ consumes everything and never gives a back: no match.
    EXPECT_FALSE(re->search("aaa", 0).has_value());
    auto re2 = mustCompile(engine, "a++b");
    auto m = re2->search("aaab", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->end, size_t(4));
}

TEST(Pcre2EngineTest, AtomicGroupDoesNotBacktrackIntoAlternation) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "(?>a|ab)c");
    // The atomic group commits to 'a', so 'b' fails and there is no retry.
    EXPECT_FALSE(re->search("abc", 0).has_value());
    auto re2 = mustCompile(engine, "(?>ab|a)c");
    auto m = re2->search("abc", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->end, size_t(3));
}

TEST(Pcre2EngineTest, Backreferences) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, R"((a|b)\1)");
    auto m = re->search("xaay", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(1));
    EXPECT_EQ(m->end, size_t(3));
    EXPECT_FALSE(re->search("xaby", 0).has_value());
}

TEST(Pcre2EngineTest, NamedGroupAndNamedBackref) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, R"((?<w>\w+) \k<w>)");
    auto m = re->search("say hi hi now", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(4));
    EXPECT_EQ(m->end, size_t(9)); // "hi hi" occupies bytes 4..8
    EXPECT_EQ(re->groupCount(), 1);
}

TEST(Pcre2EngineTest, AnchorGOnlyMatchesAtTheSearchPosition) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, R"(\Gab)");
    EXPECT_FALSE(re->search("xxab", 0).has_value());
    auto m = re->search("xxab", 2);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(2));
    EXPECT_EQ(m->end, size_t(4));
}

TEST(Pcre2EngineTest, ZeroWidthMatchAndAdvanceContract) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "x*");
    // Zero-width match at an interior position: begin == end == startPos.
    auto m = re->search("abc", 2);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(2));
    EXPECT_EQ(m->end, size_t(2));
    // Zero-width match at end of text is legal.
    auto m2 = re->search("abc", 3);
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->begin, size_t(3));
    EXPECT_EQ(m2->end, size_t(3));
    // Empty pattern matches empty at any position, including the end.
    auto empty = mustCompile(engine, "");
    auto m3 = empty->search("ab", 2);
    ASSERT_TRUE(m3.has_value());
    EXPECT_EQ(m3->begin, size_t(2));
    EXPECT_EQ(m3->end, size_t(2));
}

TEST(Pcre2EngineTest, BoundsStartPosBeyondEndIsNoMatch) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "a*");
    EXPECT_FALSE(re->search("ab", 3).has_value());
    EXPECT_FALSE(re->search("", 1).has_value());
    auto m = re->search("ab", 2);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->length(), size_t(0));
}

TEST(Pcre2EngineTest, CaptureAbsentVersusEmpty) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "(a)|(b)");
    auto m = re->search("b", 0);
    ASSERT_TRUE(m.has_value());
    ASSERT_EQ(m->captures.size(), size_t(3));
    const Capture* g1 = m->capture(1);
    const Capture* g2 = m->capture(2);
    ASSERT_NE(g1, nullptr);
    ASSERT_NE(g2, nullptr);
    EXPECT_FALSE(g1->matched());  // did not participate
    EXPECT_EQ(g1->begin, ide::syntax::kNoPosition);
    ASSERT_TRUE(g2->matched());
    EXPECT_EQ(g2->begin, size_t(0));
    EXPECT_EQ(g2->end, size_t(1));
    // Empty-but-present group: matched() is true with begin == end.
    auto re2 = mustCompile(engine, "(a*)b");
    auto m2 = re2->search("b", 0);
    ASSERT_TRUE(m2.has_value());
    const Capture* g = m2->capture(1);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->matched());
    EXPECT_EQ(g->begin, size_t(0));
    EXPECT_EQ(g->end, size_t(0));
}

TEST(Pcre2EngineTest, NeverMatchAssertionAcceptedAndNeverMatches) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, std::string(ide::syntax::kNeverMatchAssertion));
    EXPECT_FALSE(re->search("anything", 0).has_value());
    EXPECT_FALSE(re->search("", 0).has_value());
}

TEST(Pcre2EngineTest, SubroutineCallTranslation) {
    Pcre2RegexEngine engine;
    // Oniguruma \g<1> = "run group 1 again". PCRE2 would read \g<1> as a plain
    // backreference; the adapter must rewrite it to a subroutine call (?1),
    // which matches "aa" here (a backreference would ALSO match "aa", but the
    // distinguishing case is fresh matching of the group).
    auto re = mustCompile(engine, R"((a)\g<1>)");
    auto m = re->search("aa", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->end, size_t(2));
    // (?1) re-runs the group, so "b" repeated via a class-based group works
    // where a backreference to a *different* value would not: here group 1 is
    // (\w), and the subroutine re-executes it on the second character.
    auto re2 = mustCompile(engine, R"((\w)\g<1>)");
    auto m2 = re2->search("xy", 0);
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->end, size_t(2));
    // Whole-pattern recursion \g<0> -> (?R): (\w\g<0>)?|\w matches words.
    auto re3 = mustCompile(engine, R"((?:(\w)\g<0>|\w))");
    auto m3 = re3->search("hello", 0);
    ASSERT_TRUE(m3.has_value());
    EXPECT_EQ(m3->end, size_t(5));
}

TEST(Pcre2EngineTest, OnigDotAllFlagMTranslatedToPcreS) {
    Pcre2RegexEngine engine;
    // Oniguruma (?m) means "dot matches newline" (PCRE2 spells that (?s)).
    auto re = mustCompile(engine, "(?m:a.b)");
    auto m = re->search("a\nb", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->end, size_t(3));
    // (?-m) restores the default: no match across the newline.
    auto re2 = mustCompile(engine, "(?m:(?-m:a.b))");
    EXPECT_FALSE(re2->search("a\nb", 0).has_value());
}

TEST(Pcre2EngineTest, AnyCharEscapeO) {
    Pcre2RegexEngine engine;
    // \O (any character including newline) has no PCRE2 spelling; translated.
    auto re = mustCompile(engine, "a\\Ob");
    auto m = re->search("a\nb", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->end, size_t(3));
    // Plain `.` still excludes the newline.
    auto re2 = mustCompile(engine, "a.b");
    EXPECT_FALSE(re2->search("a\nb", 0).has_value());
}

TEST(Pcre2EngineTest, UnicodeEscapeMatchesCodepoint) {
    Pcre2RegexEngine engine;
    // \u263A is U+263A and occupies 3 UTF-8 bytes; UTF mode matches it whole.
    auto re = mustCompile(engine, "\\u263A");
    auto m = re->search("\xE2\x98\xBA", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(0));
    EXPECT_EQ(m->end, size_t(3));
    auto re2 = mustCompile(engine, "\\x{263A}");
    auto m2 = re2->search("\xE2\x98\xBA", 0);
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->end, size_t(3));
}

TEST(Pcre2EngineTest, UnicodePropertyClass) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "\\p{L}+");
    auto m = re->search("ab1", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(0));
    EXPECT_EQ(m->end, size_t(2));
    EXPECT_FALSE(re->search("12", 0).has_value());
}

TEST(Pcre2EngineTest, KeepOutAndGraphemeCompile) {
    Pcre2RegexEngine engine;
    EXPECT_NE(engine.compile("\\Kfoo"), nullptr);
    EXPECT_NE(engine.compile("a\\Xb"), nullptr);
}

TEST(Pcre2EngineTest, OnigPosixPropertyNamesCompileAndMatch) {
    // Oniguruma's POSIX long property names are unknown to PCRE2's \p{...}
    // syntax; the translator must rewrite them. Word = [[:word:]].
    Pcre2RegexEngine engine;
    auto word = mustCompile(engine, "\\b\\p{word}+");
    auto m = word->search("  ab12!", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(2));
    EXPECT_EQ(m->end, size_t(6));

    auto print = mustCompile(engine, "#\\p{print}*$");
    EXPECT_TRUE(print->search("#abc", 0).has_value());

    // Inside a class the rewrite must stay class-internal.
    auto inClass = mustCompile(engine, "[\\p{alnum}_]+");
    auto c = inClass->search(" x9_ ", 0);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->begin, size_t(1));
    EXPECT_EQ(c->end, size_t(4));

    // Negated forms: \P{...} and Onig's \p{^...}.
    auto neg = mustCompile(engine, "\\P{blank}+");
    auto nm = neg->search(" \ta", 0);
    ASSERT_TRUE(nm.has_value());
    EXPECT_EQ(nm->begin, size_t(2));
    EXPECT_EQ(nm->end, size_t(3));
    auto caretNeg = mustCompile(engine, "\\p{^blank}+");
    auto cm = caretNeg->search(" \ta", 0);
    ASSERT_TRUE(cm.has_value());
    EXPECT_EQ(cm->begin, size_t(2));
    EXPECT_EQ(cm->end, size_t(3));
}

TEST(Pcre2EngineTest, NestedQuantifierAppliesToQuantifiedAtom) {
    // Oniguruma reads `a+{0,1}` as (a+){0,1}; PCRE2 refuses a quantifier
    // after a quantifier, so the translator groups the atom instead of
    // escaping the brace (escaping silently changed the rule).
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "(a+{0,1})b");
    auto m = re->search("aaab", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->end, size_t(4));
    // Empty match before b is legal: the outer quantifier allows zero.
    auto zero = mustCompile(engine, "x(a+{0,1})b");
    auto zm = zero->search("xb", 0);
    ASSERT_TRUE(zm.has_value());
    EXPECT_EQ(zm->end, size_t(2));
    // A leading brace (nothing to repeat) is still a literal.
    auto lead = mustCompile(engine, "{2}b");
    auto lm = lead->search("a{2}b", 0);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->begin, size_t(1));
    // A valid quantifier position must NOT be rewritten.
    auto quant = mustCompile(engine, "ab{2}");
    EXPECT_TRUE(quant->search("abb", 0).has_value());
    EXPECT_FALSE(quant->search("ab", 0).has_value());
}

TEST(Pcre2EngineTest, UnicodeIdentifiersAreMatchedAsCodepoints) {
    // Oniguruma's \w is Unicode-aware; PCRE2 needs UTF|UCP for the same.
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "\\w+");
    auto m = re->search("caf\xC3\xA9", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(0));
    EXPECT_EQ(m->end, size_t(5));
    // Invalid UTF-8 must not fail the search (MATCH_INVALID_UTF).
    auto bad = re->search("caf\xFF", 0);
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(bad->end, size_t(3));
}

TEST(Pcre2EngineTest, NestedCharacterClassUnion) {
    // [a-z[0-9]] is a union in Oniguruma; PCRE2 has no nested classes.
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "^[a-z[0-9]]+$");
    EXPECT_TRUE(re->search("ab12z", 0).has_value());
    EXPECT_FALSE(re->search("AB", 0).has_value());
    // The non-ASCII idiom used by the CSS grammar: ASCII word chars plus any
    // codepoint above U+007F.
    auto ident = mustCompile(engine, "^[-A-Z_a-z[^\\x00-\\x7F]]+$");
    EXPECT_TRUE(ident->search("card-name", 0).has_value());
    EXPECT_TRUE(ident->search("caf\xC3\xA9", 0).has_value());
    EXPECT_FALSE(ident->search("a b", 0).has_value());
}

TEST(Pcre2EngineTest, CacheServesRepeatedCompiles) {
    Pcre2RegexEngine engine;
    auto a = engine.compile(R"(\d+)");
    auto b = engine.compile(R"(\d+)");
    EXPECT_EQ(a, b);  // same cached object
    EXPECT_EQ(engine.cacheSize(), size_t(1));
    EXPECT_EQ(engine.stats().compileCalls, size_t(2));
    EXPECT_EQ(engine.stats().cacheHits, size_t(1));
    EXPECT_EQ(engine.stats().compiled, size_t(1));
}

TEST(Pcre2EngineTest, SearchCountersMove) {
    Pcre2RegexEngine engine;
    auto re = mustCompile(engine, "z+");
    (void)re->search("zzz", 0);
    (void)re->search("zzz", 9);  // out of bounds -> swallowed, no error
    EXPECT_EQ(engine.stats().searchCalls, size_t(2));
    EXPECT_EQ(engine.stats().searchErrors, size_t(0));
}

TEST(Pcre2EngineTest, RegexObjectOutlivesEngine) {
    // The tokenizer caches IRegex globally; a pattern must stay usable after
    // the engine that built it is gone (it owns its pcre2_code).
    std::shared_ptr<ide::syntax::IRegex> re;
    {
        Pcre2RegexEngine engine;
        re = mustCompile(engine, R"(\w+)");
    }
    auto m = re->search("hi", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->end, size_t(2));
}

TEST(Pcre2TranslateTest, VerbatimPassthroughForNativeConstructs) {
    std::string out;
    std::string reason;
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("(?<=\\w)_a++b", out, reason));
    EXPECT_EQ(out, "(?<=\\w)_a++b");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("(?<name>x)|\\k'name'", out, reason));
    EXPECT_EQ(out, "(?<name>x)|\\k'name'");
}

TEST(Pcre2TranslateTest, FlagGroupRewrites) {
    std::string out;
    std::string reason;
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("(?im:x)", out, reason));
    EXPECT_EQ(out, "(?is:x)");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("(?-m)", out, reason));
    EXPECT_EQ(out, "(?-s)");
    // Not a flag group: copied verbatim.
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("(?<n>a)(?R)", out, reason));
    EXPECT_EQ(out, "(?<n>a)(?R)");
}

TEST(Pcre2TranslateTest, SubroutineRewrites) {
    std::string out;
    std::string reason;
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\g<name>", out, reason));
    EXPECT_EQ(out, "(?&name)");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\g'name'", out, reason));
    EXPECT_EQ(out, "(?&name)");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\g<2>", out, reason));
    EXPECT_EQ(out, "(?2)");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\g<0>", out, reason));
    EXPECT_EQ(out, "(?R)");
}

TEST(Pcre2TranslateTest, EscapeRewrites) {
    std::string out;
    std::string reason;
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\O", out, reason));
    EXPECT_EQ(out, "[\\s\\S]");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("[\\O]", out, reason));
    EXPECT_EQ(out, "[\\s\\S]");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\u0041", out, reason));
    EXPECT_EQ(out, "\\x{41}");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\x{41}", out, reason));
    EXPECT_EQ(out, "\\x{41}");
    // UTF mode: codepoints are native PCRE2 escapes, no byte re-encoding.
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\x{263A}", out, reason));
    EXPECT_EQ(out, "\\x{263A}");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\o{101}", out, reason));
    EXPECT_EQ(out, "\\x{41}");
}

TEST(Pcre2TranslateTest, NestedCharacterClassesFlatten) {
    std::string out;
    std::string reason;
    // Positive nested class: brackets disappear, contents union.
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("[a-z[0-9]]", out, reason));
    EXPECT_EQ(out, "[a-z0-9]");
    // Negated nested class: expanded to the complement over Unicode.
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("[A[^\\x00-\\x7F]]", out, reason));
    EXPECT_EQ(out, "[A\\x{80}-\\x{10ffff}]");
    // A high codepoint inside a class is native now, not a refusal.
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("[\\x{263A}]", out, reason));
    EXPECT_EQ(out, "[\\x{263A}]");
    // POSIX classes keep their own closing bracket (not treated as nesting).
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("[[:alpha:]_]", out, reason));
    EXPECT_EQ(out, "[[:alpha:]_]");
}

TEST(Pcre2TranslateTest, NestedQuantifierGroupsTheAtom) {
    std::string out;
    std::string reason;
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("a+{0,1}", out, reason));
    EXPECT_EQ(out, "(?:a+){0,1}");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("(ab)+?{2,3}", out, reason));
    EXPECT_EQ(out, "(?:(ab)+?){2,3}");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\s+{0,1}<x>", out, reason));
    EXPECT_EQ(out, "(?:\\s+){0,1}<x>");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("[a-c]+{0,1}", out, reason));
    EXPECT_EQ(out, "(?:[a-c]+){0,1}");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\w+{0,1}", out, reason));
    EXPECT_EQ(out, "(?:\\w+){0,1}");
}

TEST(Pcre2TranslateTest, InvertedQuantifierRangeIsSwapped) {
    // Oniguruma reads {min,max} as {1,2} when min > max; PCRE2 refuses.
    std::string out;
    std::string reason;
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("a{2,1}", out, reason));
    EXPECT_EQ(out, "a{1,2}");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("\\s+{1,0}/x", out, reason));
    EXPECT_EQ(out, "(?:\\s+){0,1}/x");
    // Valid quantifiers and literal braces are untouched.
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("a{2,3}", out, reason));
    EXPECT_EQ(out, "a{2,3}");
    EXPECT_TRUE(ide::syntax::translateOnigToPcre2("a{}", out, reason));
    EXPECT_EQ(out, "a{}");
}



}  // namespace

#else  // !COPE_HAS_PCRE2

TEST(RegexFactoryTest, FallsBackToStdRegexWhenPcre2Absent) {
    auto engine = makeRegexEngine();
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->name(), std::string_view("std::regex"));
    // And the fallback engine still works end to end.
    auto re = engine->compile(R"(\d+)");
    ASSERT_NE(re, nullptr);
    auto m = re->search("ab123", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, size_t(2));
    EXPECT_EQ(m->end, size_t(5));
}

}  // namespace

#endif  // COPE_HAS_PCRE2
