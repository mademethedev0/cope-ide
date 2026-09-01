#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <ide/syntax/regex.h>
#include <ide/syntax/tokenizer.h>

namespace {

using ide::syntax::Capture;
using ide::syntax::kNoPosition;
using ide::syntax::kNeverMatchAssertion;
using ide::syntax::kRootScopeStack;
using ide::syntax::MatchResult;
using ide::syntax::rewriteAnchors;
using ide::syntax::RuleRef;
using ide::syntax::ScopeStackId;
using ide::syntax::State;
using ide::syntax::substituteBackreferences;
using ide::syntax::substituteScopeCaptures;
using ide::syntax::TokenSpan;

Capture cap(int index, size_t begin, size_t end) { return Capture{index, begin, end}; }
Capture absentCap(int index) { return Capture{index, kNoPosition, kNoPosition}; }

State::Frame frame(RuleRef rule, ScopeStackId name, ScopeStackId content) {
    State::Frame f;
    f.rule = rule;
    f.nameScopes = name;
    f.contentScopes = content;
    return f;
}

// ---------------------------------------------------------------------------
// TokenSpan
// ---------------------------------------------------------------------------

TEST(TokenSpanTest, EqualityComparesAllThreeFields) {
    const TokenSpan a{1u, 4u, 7};
    EXPECT_TRUE(a == (TokenSpan{1u, 4u, 7}));
    EXPECT_TRUE(a != (TokenSpan{0u, 4u, 7}));
    EXPECT_TRUE(a != (TokenSpan{1u, 5u, 7}));
    EXPECT_TRUE(a != (TokenSpan{1u, 4u, 8}));
    const TokenSpan def;
    EXPECT_EQ(def.begin, 0u);
    EXPECT_EQ(def.end, 0u);
    EXPECT_EQ(def.scopes, kRootScopeStack);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

TEST(StateTest, InitialStateIsEmpty) {
    const State first = State::initial(true);
    EXPECT_TRUE(first.empty());
    EXPECT_EQ(first.depth(), 0u);
    EXPECT_EQ(first.scopes(), kRootScopeStack);
    EXPECT_EQ(first.contentScopes(), kRootScopeStack);
    EXPECT_FALSE(first.rule().valid());
    EXPECT_EQ(first.top(), nullptr);
    EXPECT_EQ(first.frameAt(0), nullptr);
    EXPECT_TRUE(first.isFirstLine());

    const State later = State::initial(false);
    EXPECT_FALSE(later.isFirstLine());
    const State defaulted;
    EXPECT_TRUE(defaulted.empty());
    EXPECT_FALSE(defaulted.isFirstLine());
    EXPECT_TRUE(defaulted == later);
}

TEST(StateTest, PushPopAndFrameAccess) {
    const State base = State::initial(true);
    const State::Frame outer = frame(RuleRef{1, 2}, 3, 4);
    const State::Frame inner = frame(RuleRef{1, 5}, 6, 7);

    const State one = base.push(outer);
    const State two = one.push(inner);

    EXPECT_EQ(one.depth(), 1u);
    EXPECT_EQ(two.depth(), 2u);
    EXPECT_TRUE(two.isFirstLine());  // push preserves the flag

    ASSERT_NE(two.top(), nullptr);
    EXPECT_TRUE(*two.top() == inner);
    EXPECT_EQ(two.scopes(), 6);
    EXPECT_EQ(two.contentScopes(), 7);
    EXPECT_TRUE(two.rule() == (RuleRef{1, 5}));

    ASSERT_NE(two.frameAt(0), nullptr);
    EXPECT_TRUE(*two.frameAt(0) == outer);  // index 0 is the outermost frame
    ASSERT_NE(two.frameAt(1), nullptr);
    EXPECT_TRUE(*two.frameAt(1) == inner);
    EXPECT_EQ(two.frameAt(2), nullptr);
    EXPECT_EQ(two.frameAt(99), nullptr);

    // Popping shares the tail with the state it came from.
    const State popped = two.pop();
    EXPECT_EQ(popped.depth(), 1u);
    EXPECT_TRUE(popped == one);
    EXPECT_EQ(popped.top(), one.top());  // structural sharing, same node
    EXPECT_TRUE(popped.pop().empty());
    EXPECT_TRUE(popped.pop().pop().empty());  // popping an empty state is safe
}

TEST(StateTest, AncestorTrimsToTheRequestedDepth) {
    State stack = State::initial(true);
    for (int i = 0; i < 5; ++i) {
        stack = stack.push(frame(RuleRef{1, i}, i, i));
    }
    EXPECT_EQ(stack.depth(), 5u);
    EXPECT_EQ(stack.ancestor(5).depth(), 5u);
    EXPECT_EQ(stack.ancestor(99).depth(), 5u);  // never grows
    EXPECT_EQ(stack.ancestor(3).depth(), 3u);
    ASSERT_NE(stack.ancestor(3).top(), nullptr);
    EXPECT_TRUE(stack.ancestor(3).rule() == (RuleRef{1, 2}));
    EXPECT_TRUE(stack.ancestor(0).empty());
    EXPECT_TRUE(stack.ancestor(0).isFirstLine());
}

TEST(StateTest, WithTopWithContentScopesWithFirstLine) {
    const State base = State::initial(true);
    const State one = base.push(frame(RuleRef{1, 2}, 3, 3));

    const State replaced = one.withTop(frame(RuleRef{1, 9}, 8, 8));
    EXPECT_EQ(replaced.depth(), 1u);
    EXPECT_TRUE(replaced.rule() == (RuleRef{1, 9}));
    EXPECT_TRUE(one.rule() == (RuleRef{1, 2}));  // the original is untouched

    const State recontented = one.withContentScopes(42);
    EXPECT_EQ(recontented.depth(), 1u);
    EXPECT_EQ(recontented.scopes(), 3);
    EXPECT_EQ(recontented.contentScopes(), 42);
    EXPECT_FALSE(recontented == one);

    // withContentScopes on an empty stack is a no-op, not a push.
    EXPECT_TRUE(base.withContentScopes(42).empty());
    // withTop on an empty stack pushes.
    EXPECT_EQ(base.withTop(frame(RuleRef{1, 1}, 1, 1)).depth(), 1u);

    const State notFirst = one.withFirstLine(false);
    EXPECT_FALSE(notFirst.isFirstLine());
    EXPECT_TRUE(one.isFirstLine());
    EXPECT_FALSE(notFirst == one);
    EXPECT_EQ(notFirst.depth(), one.depth());
}

TEST(StateTest, IdenticalStacksAreEqualWithoutSharingNodes) {
    // Two stacks built independently: no shared nodes at all, so equality must
    // come from comparing frames, not from pointer identity.
    State a = State::initial(true);
    State b = State::initial(true);
    for (int i = 0; i < 4; ++i) {
        const State::Frame f = frame(RuleRef{2, i}, i + 1, i + 10);
        a = a.push(f);
        b = b.push(f);
    }
    ASSERT_NE(a.top(), b.top());
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_EQ(a.hash(), b.hash());
    EXPECT_EQ(a.depth(), b.depth());

    // Equality is symmetric and reflexive.
    EXPECT_TRUE(b == a);
    EXPECT_TRUE(a == a);
}

TEST(StateTest, DifferentStacksAreUnequal) {
    const State base = State::initial(true);
    const State::Frame f0 = frame(RuleRef{1, 1}, 1, 1);
    const State::Frame f1 = frame(RuleRef{1, 2}, 2, 2);

    const State ab = base.push(f0).push(f1);
    // Different depth.
    EXPECT_FALSE(ab == base.push(f0));
    EXPECT_FALSE(base.push(f0) == ab);
    EXPECT_FALSE(ab == base);
    EXPECT_FALSE(base == ab);
    // Different order of the same frames.
    EXPECT_FALSE(ab == base.push(f1).push(f0));
    // Different first-line flag with an identical stack.
    EXPECT_FALSE(ab == ab.withFirstLine(false));
    EXPECT_NE(ab.hash(), ab.withFirstLine(false).hash());
}

TEST(StateTest, EqualityComparesEveryFrameFieldNotJustTheHash) {
    // Frame::operator== is hash-free, and State::operator== is specified to use
    // it for every frame after the (cheap) hash reject. Each field below is
    // varied on its own, at the *bottom* of a three deep stack, so the walk has
    // to reach it. A genuine 64 bit hash collision cannot be constructed through
    // the public API, so that is the one thing this test cannot demonstrate.
    State::Frame reference;
    reference.rule = RuleRef{1, 1};
    reference.nameScopes = 5;
    reference.contentScopes = 6;
    reference.endPattern = std::string("end");
    reference.whilePattern = std::nullopt;
    reference.beginCapturedEol = false;

    State::Frame variants[6] = {reference, reference, reference, reference, reference, reference};
    variants[0].rule = RuleRef{1, 2};
    variants[1].nameScopes = 50;
    variants[2].contentScopes = 60;
    variants[3].endPattern = std::string("other");
    variants[4].whilePattern = std::string("while");
    variants[5].beginCapturedEol = true;

    const State::Frame filler1 = frame(RuleRef{3, 3}, 3, 3);
    const State::Frame filler2 = frame(RuleRef{4, 4}, 4, 4);
    const State reference3 =
        State::initial(true).push(reference).push(filler1).push(filler2);

    for (const State::Frame& variant : variants) {
        EXPECT_FALSE(variant == reference);
        EXPECT_TRUE(variant != reference);
        const State variant3 = State::initial(true).push(variant).push(filler1).push(filler2);
        EXPECT_FALSE(variant3 == reference3);
        EXPECT_TRUE(variant3 != reference3);
        // The identical top frames must not short-circuit the comparison.
        ASSERT_NE(variant3.top(), nullptr);
        EXPECT_TRUE(*variant3.top() == filler2);
    }

    // An absent pattern is not the same as a present empty pattern.
    State::Frame emptyPattern = reference;
    emptyPattern.endPattern = std::string();
    State::Frame absentPattern = reference;
    absentPattern.endPattern = std::nullopt;
    EXPECT_FALSE(emptyPattern == absentPattern);
    EXPECT_FALSE(State::initial(true).push(emptyPattern) ==
                 State::initial(true).push(absentPattern));

    // ...and an identical copy still compares equal.
    const State::Frame copy = reference;
    EXPECT_TRUE(copy == reference);
    EXPECT_TRUE(State::initial(true).push(copy) == State::initial(true).push(reference));
}

TEST(StateTest, HashIsStableForEqualStatesAndCheapToCopy) {
    State stack = State::initial(true);
    stack = stack.push(frame(RuleRef{1, 1}, 1, 1));
    stack = stack.push(frame(RuleRef{1, 2}, 2, 2));

    const State copy = stack;  // shares the whole chain
    EXPECT_TRUE(copy == stack);
    EXPECT_EQ(copy.hash(), stack.hash());
    EXPECT_EQ(copy.top(), stack.top());

    EXPECT_NE(stack.hash(), stack.pop().hash());
    EXPECT_NE(State::initial(true).hash(), State::initial(false).hash());
    EXPECT_EQ(State::initial(false).hash(), State().hash());
}

// ---------------------------------------------------------------------------
// substituteBackreferences
// ---------------------------------------------------------------------------

TEST(SubstituteBackreferences, InsertsEscapedCaptureText) {
    const std::string_view line = "a(b)c";
    MatchResult m;
    m.begin = 0;
    m.end = 5;
    m.captures.push_back(cap(0, 0, 5));
    m.captures.push_back(cap(1, 1, 3));  // "(b" - must be escaped
    m.captures.push_back(absentCap(2));

    EXPECT_EQ(substituteBackreferences("\\1", line, m), "\\(b");
    EXPECT_EQ(substituteBackreferences("^\\1$", line, m), "^\\(b$");
    EXPECT_EQ(substituteBackreferences("\\1\\1", line, m), "\\(b\\(b");
    EXPECT_EQ(substituteBackreferences("\\0", line, m), "a\\(b\\)c");
}

TEST(SubstituteBackreferences, AbsentGroupsSubstituteToNothing) {
    const std::string_view line = "abc";
    MatchResult m;
    m.begin = 0;
    m.end = 3;
    m.captures.push_back(cap(0, 0, 3));
    m.captures.push_back(absentCap(1));

    EXPECT_EQ(substituteBackreferences("x\\1y", line, m), "xy");
    EXPECT_EQ(substituteBackreferences("x\\7y", line, m), "xy");  // no such group
    EXPECT_EQ(substituteBackreferences("", line, m), "");
    EXPECT_EQ(substituteBackreferences("no refs", line, m), "no refs");
}

TEST(SubstituteBackreferences, EscapedBackslashIsNotABackreference) {
    const std::string_view line = "abc";
    MatchResult m;
    m.begin = 0;
    m.end = 3;
    m.captures.push_back(cap(0, 0, 3));
    m.captures.push_back(cap(1, 0, 1));  // "a"

    EXPECT_EQ(substituteBackreferences("\\1", line, m), "a");
    EXPECT_EQ(substituteBackreferences("\\\\1", line, m), "\\\\1");  // \\ then literal 1
    EXPECT_EQ(substituteBackreferences("\\\\\\1", line, m), "\\\\a");
    EXPECT_EQ(substituteBackreferences("\\d\\1", line, m), "\\da");
    EXPECT_EQ(substituteBackreferences("abc\\", line, m), "abc\\");  // trailing backslash kept
}

TEST(SubstituteBackreferences, MultiDigitGroupNumbers) {
    const std::string_view line = "0123456789X";
    MatchResult m;
    m.begin = 0;
    m.end = 11;
    for (int i = 0; i <= 9; ++i) {
        m.captures.push_back(cap(i, static_cast<size_t>(i), static_cast<size_t>(i) + 1u));
    }
    m.captures.push_back(cap(10, 10, 11));  // "X"

    EXPECT_EQ(substituteBackreferences("\\10", line, m), "X");
    EXPECT_EQ(substituteBackreferences("\\1", line, m), "1");
    EXPECT_EQ(substituteBackreferences("\\1 \\10", line, m), "1 X");
}

TEST(SubstituteBackreferences, ControlCharactersInCaptureTextAreEscaped) {
    const std::string_view line = "a\nb";
    MatchResult m;
    m.begin = 0;
    m.end = 3;
    m.captures.push_back(cap(0, 0, 3));
    m.captures.push_back(cap(1, 1, 2));  // the newline itself

    EXPECT_EQ(substituteBackreferences("\\1", line, m), "\\n");
}

TEST(SubstituteBackreferences, OutOfRangeCaptureOffsetsAreIgnored) {
    const std::string_view line = "ab";
    MatchResult m;
    m.begin = 0;
    m.end = 2;
    m.captures.push_back(cap(0, 0, 2));
    m.captures.push_back(cap(1, 1, 99));  // end past the line: unusable

    EXPECT_EQ(substituteBackreferences("x\\1y", line, m), "xy");
}

// ---------------------------------------------------------------------------
// substituteScopeCaptures
// ---------------------------------------------------------------------------

TEST(SubstituteScopeCaptures, DollarFormsAndTransforms) {
    const std::string_view line = "Foo-BAR";
    MatchResult m;
    m.begin = 0;
    m.end = 7;
    m.captures.push_back(cap(0, 0, 7));
    m.captures.push_back(cap(1, 0, 3));  // "Foo"
    m.captures.push_back(cap(2, 4, 7));  // "BAR"

    EXPECT_EQ(substituteScopeCaptures("entity.$1", line, m), "entity.Foo");
    EXPECT_EQ(substituteScopeCaptures("a.${1}.b", line, m), "a.Foo.b");
    EXPECT_EQ(substituteScopeCaptures("$1.$2", line, m), "Foo.BAR");
    EXPECT_EQ(substituteScopeCaptures("$0", line, m), "Foo-BAR");
    EXPECT_EQ(substituteScopeCaptures("${1:/downcase}", line, m), "foo");
    EXPECT_EQ(substituteScopeCaptures("${2:/downcase}", line, m), "bar");
    EXPECT_EQ(substituteScopeCaptures("${1:/upcase}", line, m), "FOO");
    EXPECT_EQ(substituteScopeCaptures("x.${1:/nosuch}", line, m), "x.Foo");
}

TEST(SubstituteScopeCaptures, MissingGroupsAndNonReferences) {
    const std::string_view line = "abc";
    MatchResult m;
    m.begin = 0;
    m.end = 3;
    m.captures.push_back(cap(0, 0, 3));
    m.captures.push_back(absentCap(1));

    EXPECT_EQ(substituteScopeCaptures("plain.scope", line, m), "plain.scope");
    EXPECT_EQ(substituteScopeCaptures("", line, m), "");
    EXPECT_EQ(substituteScopeCaptures("a.$1.b", line, m), "a..b");   // absent group
    EXPECT_EQ(substituteScopeCaptures("a.$9.b", line, m), "a..b");   // no such group
    EXPECT_EQ(substituteScopeCaptures("cost.$", line, m), "cost.$");
    EXPECT_EQ(substituteScopeCaptures("$x", line, m), "$x");
    EXPECT_EQ(substituteScopeCaptures("${1", line, m), "${1");
    EXPECT_EQ(substituteScopeCaptures("${}", line, m), "${}");
    EXPECT_EQ(substituteScopeCaptures("${1:/downcase", line, m), "${1:/downcase");
}

// ---------------------------------------------------------------------------
// rewriteAnchors
// ---------------------------------------------------------------------------

TEST(RewriteAnchors, KeepsAnchorsWhenAllowed) {
    bool sawA = true;
    bool sawG = true;
    EXPECT_EQ(rewriteAnchors("\\Gfoo", true, true, &sawA, &sawG), "\\Gfoo");
    EXPECT_FALSE(sawA);
    EXPECT_TRUE(sawG);

    EXPECT_EQ(rewriteAnchors("\\Afoo", true, true, &sawA, &sawG), "\\Afoo");
    EXPECT_TRUE(sawA);
    EXPECT_FALSE(sawG);

    EXPECT_EQ(rewriteAnchors("plain", true, true, &sawA, &sawG), "plain");
    EXPECT_FALSE(sawA);
    EXPECT_FALSE(sawG);
    EXPECT_EQ(rewriteAnchors("", true, true), "");
    EXPECT_EQ(rewriteAnchors("\\", true, true), "\\");
    EXPECT_EQ(rewriteAnchors("a$", true, true), "a$");
}

TEST(RewriteAnchors, DisabledAnchorsBecomeNeverMatchingAssertions) {
    const std::string never(kNeverMatchAssertion);
    EXPECT_EQ(rewriteAnchors("\\Gfoo", true, false), never + "foo");
    EXPECT_EQ(rewriteAnchors("\\Afoo", false, true), never + "foo");
    EXPECT_EQ(rewriteAnchors("\\A\\Gx", false, false), never + never + "x");
    EXPECT_EQ(rewriteAnchors("(\\G|a)", true, false), "(" + never + "|a)");
    // Only the disabled one is rewritten.
    EXPECT_EQ(rewriteAnchors("\\A\\Gx", true, false), "\\A" + never + "x");
    EXPECT_EQ(rewriteAnchors("\\A\\Gx", false, true), never + "\\Gx");
}

TEST(RewriteAnchors, InsideCharacterClassesTheyAreLiteralLetters) {
    bool sawA = true;
    bool sawG = true;
    EXPECT_EQ(rewriteAnchors("[\\A\\G]", false, false, &sawA, &sawG), "[\\A\\G]");
    EXPECT_FALSE(sawA);
    EXPECT_FALSE(sawG);

    // A leading '^' and a leading ']' inside the class must not end it.
    EXPECT_EQ(rewriteAnchors("[^]\\A]", false, false), "[^]\\A]");
    EXPECT_EQ(rewriteAnchors("[]\\G]", false, false), "[]\\G]");
    // POSIX bracket members are copied verbatim.
    EXPECT_EQ(rewriteAnchors("[[:alpha:]]\\A", false, false),
              "[[:alpha:]]" + std::string(kNeverMatchAssertion));
    // After the class closes, anchors are anchors again.
    EXPECT_EQ(rewriteAnchors("[a]\\G", false, false), "[a]" + std::string(kNeverMatchAssertion));
}

TEST(RewriteAnchors, EscapedBackslashIsNotAnAnchor) {
    bool sawA = true;
    // "\\A" is an escaped backslash followed by a literal 'A'.
    EXPECT_EQ(rewriteAnchors("\\\\A", false, false, &sawA, nullptr), "\\\\A");
    EXPECT_FALSE(sawA);
    EXPECT_EQ(rewriteAnchors("\\\\\\A", false, false), "\\\\" + std::string(kNeverMatchAssertion));
}

}  // namespace
