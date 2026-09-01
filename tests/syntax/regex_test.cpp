#include "syntax_test_util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <ide/syntax/regex.h>
#include <ide/syntax/std_regex_engine.h>

namespace {

using ide::syntax::Capture;
using ide::syntax::escapeRegexLiteral;
using ide::syntax::IRegex;
using ide::syntax::kNeverMatchAssertion;
using ide::syntax::kNoPosition;
using ide::syntax::LookbehindKind;
using ide::syntax::MatchResult;
using ide::syntax::PatternTranslation;
using ide::syntax::RegexEngineCaps;
using ide::syntax::StdRegexEngine;
using ide::syntax::translateOnigToEcma;
using ember_test::Lcg;

// ---------------------------------------------------------------------------
// escapeRegexLiteral
// ---------------------------------------------------------------------------

TEST(EscapeRegexLiteral, EscapesEveryDialectMetacharacter) {
    EXPECT_EQ(escapeRegexLiteral(""), "");
    EXPECT_EQ(escapeRegexLiteral("abc123_:;=<>@%!"), "abc123_:;=<>@%!");
    EXPECT_EQ(escapeRegexLiteral("a.b*c"), "a\\.b\\*c");
    EXPECT_EQ(escapeRegexLiteral("^$|?+()[]{}"), "\\^\\$\\|\\?\\+\\(\\)\\[\\]\\{\\}");
    EXPECT_EQ(escapeRegexLiteral("\\"), "\\\\");
    // Not ECMAScript metacharacters, but reserved in PCRE2/Oniguruma modes.
    EXPECT_EQ(escapeRegexLiteral("/-#&~,"), "\\/\\-\\#\\&\\~\\,");
}

TEST(EscapeRegexLiteral, ControlBytesBecomeHexEscapes) {
    EXPECT_EQ(escapeRegexLiteral("\n"), "\\n");
    EXPECT_EQ(escapeRegexLiteral("\r"), "\\r");
    EXPECT_EQ(escapeRegexLiteral("\t"), "\\t");
    EXPECT_EQ(escapeRegexLiteral(std::string_view("\0", 1)), "\\x00");
    EXPECT_EQ(escapeRegexLiteral("\x1B"), "\\x1B");
    EXPECT_EQ(escapeRegexLiteral("\x7F"), "\\x7F");
}

TEST(EscapeRegexLiteral, NonAsciiBytesPassThrough) {
    // Multi-byte UTF-8 stays raw: outside a character class the bytes match
    // themselves in every supported dialect.
    EXPECT_EQ(escapeRegexLiteral("\xC3\xA9"), "\xC3\xA9");
    EXPECT_EQ(escapeRegexLiteral("a\xE2\x82\xAC" "b"), "a\xE2\x82\xAC" "b");
}

// ---------------------------------------------------------------------------
// translateOnigToEcma
// ---------------------------------------------------------------------------

TEST(TranslateOnigToEcma, PassesThroughPlainPatterns) {
    const PatternTranslation t = translateOnigToEcma("a+b*c?");
    ASSERT_TRUE(t.ok) << t.reason;
    EXPECT_EQ(t.ecma, "a+b*c?");
    EXPECT_EQ(t.groupCount, 0);
    EXPECT_FALSE(t.hasG);
    EXPECT_FALSE(t.icase);
    EXPECT_FALSE(t.lossy());
    EXPECT_TRUE(t.reason.empty());
    EXPECT_EQ(t.lookbehind, LookbehindKind::None);
}

TEST(TranslateOnigToEcma, TranslatesExactly) {
    EXPECT_EQ(translateOnigToEcma("\\h").ecma, "[0-9a-fA-F]");
    EXPECT_EQ(translateOnigToEcma("[\\h]").ecma, "[0-9a-fA-F]");
    EXPECT_EQ(translateOnigToEcma("\\H").ecma, "[^0-9a-fA-F]");
    EXPECT_EQ(translateOnigToEcma("\\e").ecma, "\\x1B");
    EXPECT_EQ(translateOnigToEcma("\\a").ecma, "\\x07");
    EXPECT_EQ(translateOnigToEcma("\\N").ecma, "[^\\n]");
    EXPECT_EQ(translateOnigToEcma("\\O").ecma, "[\\s\\S]");
    EXPECT_EQ(translateOnigToEcma("\\R").ecma, "(?:\\r\\n|[\\r\\n])");
    EXPECT_EQ(translateOnigToEcma("[[:alpha:]]").ecma, "[A-Za-z]");
    EXPECT_EQ(translateOnigToEcma("[[:digit:]x]").ecma, "[0-9x]");
    EXPECT_EQ(translateOnigToEcma("(?#a comment)x").ecma, "x");
    EXPECT_EQ(translateOnigToEcma("\\x41").ecma, "\\x41");
    EXPECT_EQ(translateOnigToEcma("\\Q a.b \\E").ecma, " a\\.b ");
    // A bare ']' or '{' is Annex B only; both must be escaped.
    EXPECT_EQ(translateOnigToEcma("(])").ecma, "(\\])");
    EXPECT_EQ(translateOnigToEcma("a{").ecma, "a\\{");
    EXPECT_EQ(translateOnigToEcma("}").ecma, "\\}");
    EXPECT_EQ(translateOnigToEcma("a{2,3}").ecma, "a{2,3}");
    EXPECT_EQ(translateOnigToEcma("a{,3}").ecma, "a{0,3}");
    EXPECT_EQ(translateOnigToEcma("a{2,}").ecma, "a{2,}");
    // A leading ']' inside a class is a member, not a terminator.
    EXPECT_EQ(translateOnigToEcma("[]a]").ecma, "[\\]a]");
    EXPECT_EQ(translateOnigToEcma("[^]a]").ecma, "[^\\]a]");
}

TEST(TranslateOnigToEcma, EndOfLineAnchorsAreWidened) {
    EXPECT_EQ(translateOnigToEcma("a$").ecma, "a(?=\\n?$)");
    EXPECT_EQ(translateOnigToEcma("\\z").ecma, "$");
    EXPECT_EQ(translateOnigToEcma("\\Z").ecma, "(?=\\n?$)");
    EXPECT_EQ(translateOnigToEcma("^a").ecma, "^a");

    const PatternTranslation a = translateOnigToEcma("\\Aa");
    ASSERT_TRUE(a.ok) << a.reason;
    EXPECT_EQ(a.ecma, "^a");
    EXPECT_TRUE(a.lossy());  // "\A treated as start of line"
}

TEST(TranslateOnigToEcma, CodepointEscapesBecomeUtf8Bytes) {
    const PatternTranslation t = translateOnigToEcma("\\x{263A}");
    ASSERT_TRUE(t.ok) << t.reason;
    EXPECT_EQ(t.ecma, "(?:\xE2\x98\xBA)");
    EXPECT_TRUE(t.lossy());
    EXPECT_EQ(translateOnigToEcma("\\x{41}").ecma, "\\x41");
}

TEST(TranslateOnigToEcma, NamedGroupsBecomeNumberedGroups) {
    const PatternTranslation t = translateOnigToEcma("(?<word>ab)\\k<word>");
    ASSERT_TRUE(t.ok) << t.reason;
    EXPECT_EQ(t.ecma, "(ab)\\1");
    EXPECT_EQ(t.groupCount, 1);
    ASSERT_EQ(t.groupNames.size(), 1u);
    EXPECT_EQ(t.groupNames[0].first, "word");
    EXPECT_EQ(t.groupNames[0].second, 1);

    const PatternTranslation p = translateOnigToEcma("(?P<w>a)");
    ASSERT_TRUE(p.ok) << p.reason;
    EXPECT_EQ(p.ecma, "(a)");
    EXPECT_EQ(p.groupCount, 1);
}

TEST(TranslateOnigToEcma, GroupCountingAndAlternation) {
    EXPECT_EQ(translateOnigToEcma("(a)(?:b)(c)").groupCount, 2);
    EXPECT_TRUE(translateOnigToEcma("a|b").topLevelAlternation);
    EXPECT_FALSE(translateOnigToEcma("(a|b)").topLevelAlternation);
    EXPECT_EQ(translateOnigToEcma("(?=a)(?!b)").ecma, "(?=a)(?!b)");
}

TEST(TranslateOnigToEcma, DocumentedLossyRewrites) {
    const PatternTranslation possessive = translateOnigToEcma("a++b*+");
    ASSERT_TRUE(possessive.ok) << possessive.reason;
    EXPECT_EQ(possessive.ecma, "a+b*");
    EXPECT_TRUE(possessive.lossy());

    const PatternTranslation atomic = translateOnigToEcma("(?>ab)");
    ASSERT_TRUE(atomic.ok) << atomic.reason;
    EXPECT_EQ(atomic.ecma, "(?:ab)");
    EXPECT_TRUE(atomic.lossy());

    const PatternTranslation icase = translateOnigToEcma("(?i)abc");
    ASSERT_TRUE(icase.ok) << icase.reason;
    EXPECT_EQ(icase.ecma, "abc");
    EXPECT_TRUE(icase.icase);
    EXPECT_TRUE(icase.lossy());

    const PatternTranslation scoped = translateOnigToEcma("(?i:a)b");
    ASSERT_TRUE(scoped.ok) << scoped.reason;
    EXPECT_EQ(scoped.ecma, "(?:a)b");
    EXPECT_TRUE(scoped.icase);

    const PatternTranslation prop = translateOnigToEcma("\\p{L}+");
    ASSERT_TRUE(prop.ok) << prop.reason;
    EXPECT_EQ(prop.ecma, "[A-Za-z]+");
    EXPECT_TRUE(prop.lossy());
    EXPECT_EQ(translateOnigToEcma("\\P{L}").ecma, "[^A-Za-z]");
    EXPECT_EQ(translateOnigToEcma("\\p{IsAlpha}").ecma, "[A-Za-z]");
    // No ASCII character has this property, so the class can never match.
    EXPECT_EQ(translateOnigToEcma("\\p{Mn}").ecma, "[^\\s\\S]");
}

TEST(TranslateOnigToEcma, LeadingGProducesTwoVariants) {
    const PatternTranslation t = translateOnigToEcma("\\Gfoo");
    ASSERT_TRUE(t.ok) << t.reason;
    EXPECT_TRUE(t.hasG);
    EXPECT_TRUE(t.leadingG);
    EXPECT_EQ(t.ecma, "(?![^\\s\\S])foo");  // always-true assertion
    EXPECT_EQ(t.ecmaNoG, std::string(kNeverMatchAssertion) + "foo");

    const PatternTranslation mid = translateOnigToEcma("a\\Gb");
    ASSERT_TRUE(mid.ok) << mid.reason;
    EXPECT_TRUE(mid.hasG);
    EXPECT_FALSE(mid.leadingG);
}

TEST(TranslateOnigToEcma, LeadingSingleCharacterLookbehindIsStripped) {
    const PatternTranslation neg = translateOnigToEcma("(?<!\\w)foo");
    ASSERT_TRUE(neg.ok) << neg.reason;
    EXPECT_EQ(neg.lookbehind, LookbehindKind::Negative);
    EXPECT_EQ(neg.lookbehindTest, "\\w");
    EXPECT_EQ(neg.ecma, "foo");
    EXPECT_TRUE(neg.lossy());

    const PatternTranslation pos = translateOnigToEcma("(?<=[.])x");
    ASSERT_TRUE(pos.ok) << pos.reason;
    EXPECT_EQ(pos.lookbehind, LookbehindKind::Positive);
    EXPECT_EQ(pos.lookbehindTest, "[.]");
    EXPECT_EQ(pos.ecma, "x");

    const PatternTranslation lit = translateOnigToEcma("(?<=a)b");
    ASSERT_TRUE(lit.ok) << lit.reason;
    EXPECT_EQ(lit.lookbehind, LookbehindKind::Positive);
    EXPECT_EQ(lit.lookbehindTest, "a");
    EXPECT_EQ(lit.ecma, "b");
}

TEST(TranslateOnigToEcma, UnsupportedConstructsAreRejectedWithAReason) {
    const char* unsupported[] = {
        "(?<=abc)x",      // multi-character lookbehind
        "(?<=a|b)x",      // alternated lookbehind
        "x(?<=a)y",       // non-leading lookbehind
        "\\g<1>",         // subroutine call
        "\\K",            // match reset
        "\\X",            // grapheme cluster
        "\\o{101}",       // octal brace escape
        "(?(1)a|b)",      // conditional group
        "[a&&b]",         // class set intersection
        "[[:^alpha:]]",   // negated POSIX class
        "[[:nosuch:]]",   // unknown POSIX class
        "\\p{Nonesuch}",  // unknown unicode property
        "(?x)a b",        // extended mode
        "\\G*",           // quantified anchor
        "a\\",            // trailing backslash
        "(",              // unbalanced open
        ")",              // unbalanced close
        "[abc",           // unterminated class
        "(?#unterminated",
    };
    for (const char* pattern : unsupported) {
        const PatternTranslation t = translateOnigToEcma(pattern);
        EXPECT_FALSE(t.ok) << pattern;
        EXPECT_FALSE(t.reason.empty()) << pattern;
        EXPECT_TRUE(t.ecma.empty()) << pattern;
        EXPECT_TRUE(t.ecmaNoG.empty()) << pattern;
    }

    // Spot-check the reasons that callers surface to users.
    EXPECT_NE(translateOnigToEcma("(?<=abc)x").reason.find("lookbehind"), std::string::npos);
    EXPECT_EQ(translateOnigToEcma("\\K").reason, "\\K match reset");
    EXPECT_EQ(translateOnigToEcma("[abc").reason, "unterminated character class");
    EXPECT_EQ(translateOnigToEcma("(").reason, "unbalanced parenthesis");
    EXPECT_EQ(translateOnigToEcma(")").reason, "unbalanced ')'");
}

TEST(TranslateOnigToEcma, AcceptsTheNeverMatchAssertionItProduces) {
    // Every engine must accept what the tokenizer substitutes for a disabled
    // anchor, otherwise "not the first line" cannot be expressed.
    const PatternTranslation t = translateOnigToEcma(std::string(kNeverMatchAssertion) + "a");
    ASSERT_TRUE(t.ok) << t.reason;
    EXPECT_EQ(t.ecma, std::string(kNeverMatchAssertion) + "a");
}

// ---------------------------------------------------------------------------
// StdRegexEngine
// ---------------------------------------------------------------------------

TEST(StdRegexEngineTest, Identity) {
    StdRegexEngine engine;
    EXPECT_EQ(engine.name(), "std::regex");
    const RegexEngineCaps caps = engine.caps();
    EXPECT_TRUE(caps.anchorG);
    EXPECT_TRUE(caps.posixClasses);
    EXPECT_TRUE(caps.namedGroups);
    EXPECT_FALSE(caps.lookbehind);
    EXPECT_FALSE(caps.utf8Aware);
    EXPECT_FALSE(caps.subroutines);
    EXPECT_TRUE(engine.lastError().empty());
}

TEST(StdRegexEngineTest, CompileAndSearchWithAbsoluteOffsets) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile("b+");
    ASSERT_NE(re, nullptr) << engine.lastError();
    EXPECT_EQ(re->pattern(), "b+");
    EXPECT_EQ(re->groupCount(), 0);

    const std::optional<MatchResult> m = re->search("abbbc", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, 1u);
    EXPECT_EQ(m->end, 4u);
    EXPECT_EQ(m->length(), 3u);
    ASSERT_EQ(m->captures.size(), 1u);
    EXPECT_EQ(m->captures[0].index, 0);
    EXPECT_EQ(m->captures[0].begin, 1u);
    EXPECT_EQ(m->captures[0].end, 4u);

    // Offsets stay absolute when the search starts late.
    const std::optional<MatchResult> later = re->search("abbbcbb", 4);
    ASSERT_TRUE(later.has_value());
    EXPECT_EQ(later->begin, 5u);
    EXPECT_EQ(later->end, 7u);

    EXPECT_FALSE(re->search("aaa", 0).has_value());
    EXPECT_FALSE(re->search("abbb", 4).has_value());  // start == size, no match
    EXPECT_FALSE(re->search("abbb", 9).has_value());  // start > size is not a crash
}

TEST(StdRegexEngineTest, ZeroWidthMatchAtEndOfTextIsLegal) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile("x*");
    ASSERT_NE(re, nullptr) << engine.lastError();

    const std::optional<MatchResult> atEnd = re->search("ab", 2);
    ASSERT_TRUE(atEnd.has_value());
    EXPECT_EQ(atEnd->begin, 2u);
    EXPECT_EQ(atEnd->end, 2u);
    EXPECT_EQ(atEnd->length(), 0u);

    // Empty text must not dereference a null data() pointer.
    const std::optional<MatchResult> empty = re->search(std::string_view{}, 0);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->begin, 0u);
    EXPECT_EQ(empty->end, 0u);
}

TEST(StdRegexEngineTest, CaptureIndicesAbsentVersusEmptyGroups) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile("(a)|(b)(c?)");
    ASSERT_NE(re, nullptr) << engine.lastError();
    EXPECT_EQ(re->groupCount(), 3);

    const std::optional<MatchResult> m = re->search("zbz", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, 1u);
    EXPECT_EQ(m->end, 2u);
    ASSERT_EQ(m->captures.size(), 4u);
    for (size_t i = 0; i < m->captures.size(); ++i) {
        EXPECT_EQ(m->captures[i].index, static_cast<int>(i));
    }

    // Group 1 did not participate at all.
    const Capture* g1 = m->capture(1);
    ASSERT_NE(g1, nullptr);
    EXPECT_FALSE(g1->matched());
    EXPECT_EQ(g1->begin, kNoPosition);
    EXPECT_EQ(g1->end, kNoPosition);
    EXPECT_EQ(g1->length(), 0u);

    // Group 2 matched one byte.
    const Capture* g2 = m->capture(2);
    ASSERT_NE(g2, nullptr);
    EXPECT_TRUE(g2->matched());
    EXPECT_EQ(g2->begin, 1u);
    EXPECT_EQ(g2->end, 2u);

    // Group 3 participated but is empty - distinct from absent.
    const Capture* g3 = m->capture(3);
    ASSERT_NE(g3, nullptr);
    EXPECT_TRUE(g3->matched());
    EXPECT_EQ(g3->begin, 2u);
    EXPECT_EQ(g3->end, 2u);
    EXPECT_EQ(g3->length(), 0u);

    // A group that does not exist at all.
    EXPECT_EQ(m->capture(4), nullptr);
    EXPECT_EQ(m->capture(-1), nullptr);
    ASSERT_NE(m->capture(0), nullptr);
    EXPECT_EQ(m->capture(0)->begin, m->begin);
}

TEST(StdRegexEngineTest, LeadingGOnlyMatchesAtTheSearchPosition) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile("\\G[a-z]+");
    ASSERT_NE(re, nullptr) << engine.lastError();

    // "xfoo": nothing matches at 0 because \G forbids scanning forward.
    EXPECT_FALSE(re->search("Xfoo", 0).has_value());

    const std::optional<MatchResult> at1 = re->search("Xfoo", 1);
    ASSERT_TRUE(at1.has_value());
    EXPECT_EQ(at1->begin, 1u);
    EXPECT_EQ(at1->end, 4u);

    // Without \G the same source scans forward.
    const std::shared_ptr<IRegex> plain = engine.compile("[a-z]+");
    ASSERT_NE(plain, nullptr) << engine.lastError();
    const std::optional<MatchResult> scanned = plain->search("Xfoo", 0);
    ASSERT_TRUE(scanned.has_value());
    EXPECT_EQ(scanned->begin, 1u);
    EXPECT_EQ(scanned->end, 4u);
}

TEST(StdRegexEngineTest, EmulatedLookbehindRetriesAtLaterPositions) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile("(?<!\\w)foo");
    ASSERT_NE(re, nullptr) << engine.lastError();

    // "xfoo foo": the first candidate is preceded by 'x' and must be rejected,
    // the second is preceded by a space and wins.
    const std::optional<MatchResult> m = re->search("xfoo foo", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, 5u);
    EXPECT_EQ(m->end, 8u);

    // At offset 0 there is no preceding byte, so a negative lookbehind holds.
    const std::optional<MatchResult> start = re->search("foo", 0);
    ASSERT_TRUE(start.has_value());
    EXPECT_EQ(start->begin, 0u);

    // Nothing satisfies the assertion here.
    EXPECT_FALSE(re->search("xfoo", 0).has_value());
}

TEST(StdRegexEngineTest, IcaseAppliesToTheWholePattern) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile("(?i)abc");
    ASSERT_NE(re, nullptr) << engine.lastError();
    const std::optional<MatchResult> m = re->search("xxABC", 0);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->begin, 2u);
    EXPECT_EQ(m->end, 5u);
}

TEST(StdRegexEngineTest, NeverMatchAssertionCompilesAndNeverMatches) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile(std::string(kNeverMatchAssertion) + "a");
    ASSERT_NE(re, nullptr) << engine.lastError();
    EXPECT_FALSE(re->search("aaaa", 0).has_value());
    EXPECT_FALSE(re->search("", 0).has_value());
}

TEST(StdRegexEngineTest, UnsupportedPatternReturnsNullptrWithAReason) {
    StdRegexEngine engine;
    EXPECT_EQ(engine.compile("(?<=abc)x"), nullptr);
    EXPECT_FALSE(engine.lastError().empty());
    EXPECT_NE(engine.lastError().find("lookbehind"), std::string::npos);
    EXPECT_EQ(engine.errorFor("(?<=abc)x"), engine.lastError());
    EXPECT_EQ(engine.errorFor("never seen"), "");
    EXPECT_EQ(engine.stats().rejected, 1u);
    ASSERT_EQ(engine.rejections().size(), 1u);
    EXPECT_EQ(engine.rejections().begin()->first, "(?<=abc)x");

    // A successful compile clears lastError().
    ASSERT_NE(engine.compile("ok"), nullptr);
    EXPECT_TRUE(engine.lastError().empty());
}

TEST(StdRegexEngineTest, PatternsRefusedByStdRegexAreReportedNotThrown) {
    StdRegexEngine engine;
    // Translates cleanly, then std::regex refuses it (nothing to repeat).
    const PatternTranslation t = translateOnigToEcma("*");
    ASSERT_TRUE(t.ok) << t.reason;
    EXPECT_EQ(engine.compile("*"), nullptr);
    // The exact wording comes from the standard library, so only its presence
    // and its association with the pattern are asserted.
    EXPECT_FALSE(engine.lastError().empty());
    EXPECT_EQ(engine.errorFor("*"), engine.lastError());
    EXPECT_EQ(engine.stats().rejected, 1u);
    EXPECT_EQ(engine.cacheSize(), 1u);
    // Cached negative result: no second translation attempt.
    EXPECT_EQ(engine.compile("*"), nullptr);
    EXPECT_EQ(engine.stats().rejected, 1u);
    EXPECT_EQ(engine.stats().cacheHits, 1u);
}

TEST(StdRegexEngineTest, CompileCacheServesRepeatedPatterns) {
    StdRegexEngine engine;
    EXPECT_EQ(engine.cacheSize(), 0u);

    const std::shared_ptr<IRegex> first = engine.compile("x[0-9]+");
    ASSERT_NE(first, nullptr) << engine.lastError();
    const std::shared_ptr<IRegex> second = engine.compile("x[0-9]+");
    EXPECT_EQ(first.get(), second.get());  // same object, not recompiled
    EXPECT_EQ(engine.cacheSize(), 1u);
    EXPECT_EQ(engine.stats().compileCalls, 2u);
    EXPECT_EQ(engine.stats().cacheHits, 1u);
    EXPECT_EQ(engine.stats().compiled, 1u);

    ASSERT_NE(engine.compile("other"), nullptr);
    EXPECT_EQ(engine.cacheSize(), 2u);
    EXPECT_EQ(engine.stats().compiled, 2u);
    EXPECT_EQ(engine.stats().cacheHits, 1u);

    engine.clearCache();
    EXPECT_EQ(engine.cacheSize(), 0u);
    EXPECT_TRUE(engine.lastError().empty());
    const std::shared_ptr<IRegex> third = engine.compile("x[0-9]+");
    ASSERT_NE(third, nullptr) << engine.lastError();
    EXPECT_NE(third.get(), first.get());  // recompiled after the cache was cleared
}

TEST(StdRegexEngineTest, LossyPatternsAreRecorded) {
    StdRegexEngine engine;
    ASSERT_NE(engine.compile("a++"), nullptr) << engine.lastError();
    EXPECT_EQ(engine.stats().lossy, 1u);
    ASSERT_EQ(engine.lossyPatterns().count("a++"), 1u);
    EXPECT_EQ(engine.lossyPatterns().at("a++"), "possessive quantifier relaxed to greedy");

    ASSERT_NE(engine.compile("plain"), nullptr) << engine.lastError();
    EXPECT_EQ(engine.stats().lossy, 1u);
    EXPECT_EQ(engine.lossyPatterns().count("plain"), 0u);
}

TEST(StdRegexEngineTest, SearchCountersMove) {
    StdRegexEngine engine;
    const std::shared_ptr<IRegex> re = engine.compile("a");
    ASSERT_NE(re, nullptr) << engine.lastError();
    EXPECT_EQ(engine.stats().searchCalls, 0u);
    (void)re->search("bab", 0);
    (void)re->search("bab", 2);
    EXPECT_EQ(engine.stats().searchCalls, 2u);
    EXPECT_EQ(engine.stats().searchErrors, 0u);
}

// --- differential fuzz: escaped literals behave like std::string::find -------

TEST(StdRegexEngineTest, EscapedLiteralsMatchNaiveFind) {
    // escapeRegexLiteral() plus the engine must behave exactly like a byte-wise
    // substring search. The alphabet is deliberately full of metacharacters,
    // control bytes and multi-byte UTF-8.
    static const char kAlphabet[] = {'a',  'b',  '.',  '*',  '+',    '?',  '(',
                                     ')',  '[',  ']',  '{',  '}',    '|',  '^',
                                     '$',  '\\', '/',  '-',  '#',    '&',  '~',
                                     ',',  '\n', '\t', ' ',  '\x01', '\xC3', '\xA9'};
    constexpr size_t kAlphabetSize = sizeof(kAlphabet) / sizeof(kAlphabet[0]);

    StdRegexEngine engine;
    Lcg rng(0xABCDEFu);
    for (int trial = 0; trial < 250; ++trial) {
        std::string text;
        const size_t textLen = rng.below(20u) + 1u;
        for (size_t i = 0; i < textLen; ++i) text.push_back(kAlphabet[rng.below(kAlphabetSize)]);

        std::string needle;
        const size_t needleLen = rng.below(3u) + 1u;
        for (size_t i = 0; i < needleLen; ++i) needle.push_back(kAlphabet[rng.below(kAlphabetSize)]);

        const std::string pattern = escapeRegexLiteral(needle);
        const std::shared_ptr<IRegex> re = engine.compile(pattern);
        ASSERT_NE(re, nullptr) << "pattern: " << pattern << " reason: " << engine.lastError();

        for (size_t start = 0; start <= text.size(); ++start) {
            const size_t expected = text.find(needle, start);
            const std::optional<MatchResult> m = re->search(text, start);
            if (expected == std::string::npos) {
                EXPECT_FALSE(m.has_value())
                    << "trial " << trial << " start " << start << " pattern " << pattern;
            } else {
                ASSERT_TRUE(m.has_value())
                    << "trial " << trial << " start " << start << " pattern " << pattern;
                EXPECT_EQ(m->begin, expected) << "trial " << trial << " start " << start;
                EXPECT_EQ(m->end, expected + needle.size())
                    << "trial " << trial << " start " << start;
            }
        }
    }
    EXPECT_EQ(engine.stats().searchErrors, 0u);
}

}  // namespace
