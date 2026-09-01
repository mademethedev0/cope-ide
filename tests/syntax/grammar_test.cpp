#include "syntax_test_util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ide/syntax/grammar.h>
#include <ide/syntax/json_lite.h>

namespace {

namespace json = ide::syntax::json;
using ide::syntax::Grammar;
using ide::syntax::GrammarId;
using ide::syntax::GrammarRegistry;
using ide::syntax::IncludeKind;
using ide::syntax::IncludeRef;
using ide::syntax::kInvalidGrammarId;
using ide::syntax::kInvalidRuleId;
using ide::syntax::kMaxRuleNestingDepth;
using ide::syntax::loadGrammar;
using ide::syntax::loadGrammarJson;
using ide::syntax::parseIncludeRef;
using ide::syntax::patternHasBackrefs;
using ide::syntax::Rule;
using ide::syntax::RuleId;
using ide::syntax::RuleKind;
using ide::syntax::RuleRef;

// ---------------------------------------------------------------------------
// parseIncludeRef / patternHasBackrefs
// ---------------------------------------------------------------------------

TEST(ParseIncludeRef, EveryTextMateSpelling) {
    const IncludeRef repo = parseIncludeRef("#strings");
    EXPECT_EQ(repo.kind, IncludeKind::Repository);
    EXPECT_EQ(repo.ruleName, "strings");
    EXPECT_EQ(repo.scopeName, "");
    EXPECT_EQ(repo.raw, "#strings");

    EXPECT_EQ(parseIncludeRef("$self").kind, IncludeKind::Self);
    EXPECT_EQ(parseIncludeRef("$base").kind, IncludeKind::Base);

    const IncludeRef ext = parseIncludeRef("source.css");
    EXPECT_EQ(ext.kind, IncludeKind::ExternalGrammar);
    EXPECT_EQ(ext.scopeName, "source.css");
    EXPECT_EQ(ext.ruleName, "");

    const IncludeRef extRule = parseIncludeRef("source.css#media");
    EXPECT_EQ(extRule.kind, IncludeKind::ExternalRule);
    EXPECT_EQ(extRule.scopeName, "source.css");
    EXPECT_EQ(extRule.ruleName, "media");

    // "$self#name" / "$base#name" mean this grammar's repository.
    const IncludeRef selfRule = parseIncludeRef("$self#media");
    EXPECT_EQ(selfRule.kind, IncludeKind::Repository);
    EXPECT_EQ(selfRule.ruleName, "media");
    EXPECT_EQ(parseIncludeRef("$base#media").kind, IncludeKind::Repository);

    // A '#' inside a repository name is part of the name.
    EXPECT_EQ(parseIncludeRef("#a#b").ruleName, "a#b");

    EXPECT_EQ(parseIncludeRef("").kind, IncludeKind::Invalid);
    EXPECT_EQ(parseIncludeRef("#").kind, IncludeKind::Invalid);
    EXPECT_EQ(parseIncludeRef("source.css#").kind, IncludeKind::Invalid);
    EXPECT_EQ(parseIncludeRef("#x").raw, "#x");
}

TEST(PatternHasBackrefs, DistinguishesEscapedBackslashes) {
    EXPECT_TRUE(patternHasBackrefs("\\1"));
    EXPECT_TRUE(patternHasBackrefs("end\\2here"));
    EXPECT_TRUE(patternHasBackrefs("\\\\\\1"));  // escaped backslash, then \1
    EXPECT_FALSE(patternHasBackrefs("\\\\1"));   // escaped backslash, literal 1
    EXPECT_FALSE(patternHasBackrefs(""));
    EXPECT_FALSE(patternHasBackrefs("\\"));
    EXPECT_FALSE(patternHasBackrefs("abc1"));
    EXPECT_FALSE(patternHasBackrefs("\\d+"));
    EXPECT_FALSE(patternHasBackrefs("[\\w]"));
}

// ---------------------------------------------------------------------------
// loadGrammar: metadata and rules
// ---------------------------------------------------------------------------

constexpr std::string_view kMetaGrammar = R"json({
  "scopeName": "source.meta",
  "name": "Legacy Name",
  "firstLineMatch": "^#!/bin/sh",
  "injectionSelector": "L:source.js",
  "fileTypes": ["mt", "mtx", ""],
  "patterns": [ { "match": "a", "name": "m.a" } ],
  "repository": { "kw": { "match": "k", "name": "keyword" } },
  "injections": { "string.quoted": { "patterns": [ { "match": "i", "name": "inj" } ] } }
})json";

TEST(LoadGrammar, Metadata) {
    Grammar g;
    std::string error = "unset";
    ASSERT_TRUE(loadGrammarJson(kMetaGrammar, g, &error)) << error;
    EXPECT_EQ(g.scopeName(), "source.meta");
    EXPECT_EQ(g.displayName(), "Legacy Name");  // legacy "name" is the fallback
    EXPECT_EQ(g.firstLineMatch(), "^#!/bin/sh");
    EXPECT_EQ(g.injectionSelector(), "L:source.js");
    ASSERT_EQ(g.fileTypes().size(), 2u);  // the empty entry is dropped
    EXPECT_EQ(g.fileTypes()[0], "mt");
    EXPECT_EQ(g.fileTypes()[1], "mtx");
    EXPECT_EQ(g.id(), kInvalidGrammarId);  // ids are assigned by the registry

    ASSERT_TRUE(g.validRule(g.rootRule()));
    EXPECT_EQ(g.rule(g.rootRule()).kind, RuleKind::Container);
    EXPECT_EQ(g.rule(g.rootRule()).name, "source.meta");
    EXPECT_EQ(g.rule(g.rootRule()).debugName, "$self");
    ASSERT_EQ(g.rule(g.rootRule()).patterns.size(), 1u);

    const RuleId kw = g.repositoryRule("kw");
    ASSERT_NE(kw, kInvalidRuleId);
    EXPECT_EQ(g.rule(kw).match, "k");
    EXPECT_EQ(g.rule(kw).debugName, "repository.kw");
    EXPECT_EQ(g.repositoryRule("nope"), kInvalidRuleId);
    EXPECT_EQ(g.repository().size(), 1u);

    ASSERT_EQ(g.injections().size(), 1u);
    EXPECT_EQ(g.injections()[0].selector, "string.quoted");
    ASSERT_TRUE(g.validRule(g.injections()[0].rule));
    EXPECT_EQ(g.rule(g.injections()[0].rule).kind, RuleKind::Container);

    EXPECT_TRUE(g.externalScopeRefs().empty());
    EXPECT_TRUE(g.brokenIncludes().empty());
}

TEST(LoadGrammar, DisplayNamePrefersDisplayNameOverName) {
    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammarJson(
        R"json({"scopeName":"s","displayName":"Preferred","name":"Legacy"})json", g, &error))
        << error;
    EXPECT_EQ(g.displayName(), "Preferred");
}

TEST(LoadGrammar, InvalidRuleIdYieldsASharedEmptyRule) {
    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammarJson(R"json({"scopeName":"s"})json", g, &error)) << error;
    EXPECT_FALSE(g.validRule(-1));
    EXPECT_FALSE(g.validRule(9999));
    const Rule& empty = g.rule(9999);
    EXPECT_EQ(empty.id, kInvalidRuleId);
    EXPECT_EQ(empty.kind, RuleKind::Container);
    EXPECT_TRUE(empty.name.empty());
    EXPECT_EQ(&empty, &g.rule(-1));  // the same shared instance
    EXPECT_EQ(g.rule(g.rootRule()).patterns.size(), 0u);
}

constexpr std::string_view kRulesGrammar = R"json({
  "scopeName": "source.rules",
  "patterns": [
    { "match": "a", "name": "m.a", "captures": { "0": {"name":"whole"}, "2": {"name":"two"} } },
    { "begin": "b", "end": "\\1", "name": "be", "contentName": "be.content",
      "applyEndPatternLast": true,
      "beginCaptures": { "1": {"name":"bc"} },
      "endCaptures": { "1": {"name":"ec"} } },
    { "begin": "c", "while": "\\2", "name": "bw" },
    { "patterns": [ { "match": "z", "name": "m.z" } ] },
    { "name": "decoration.only" },
    { "begin": "(x)", "end": "(y)", "captures": { "1": {"name":"shared"} } },
    { "match": "q", "captures": { "abc": {"name":"bogus"}, "300": {"name":"too-big"} } }
  ]
})json";

TEST(LoadGrammar, RuleShapes) {
    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammarJson(kRulesGrammar, g, &error)) << error;
    const std::vector<RuleId>& top = g.rule(g.rootRule()).patterns;
    ASSERT_EQ(top.size(), 7u);

    const Rule& match = g.rule(top[0]);
    EXPECT_EQ(match.kind, RuleKind::Match);
    EXPECT_TRUE(match.hasMatch);
    EXPECT_FALSE(match.hasBegin);
    EXPECT_EQ(match.match, "a");
    EXPECT_EQ(match.name, "m.a");
    EXPECT_EQ(match.debugName, "$self.patterns[0]");
    ASSERT_EQ(match.captures.size(), 3u);
    for (size_t i = 0; i < match.captures.size(); ++i) {
        EXPECT_EQ(match.captures[i].index, static_cast<int>(i));
    }
    EXPECT_EQ(match.captures[0].name, "whole");
    EXPECT_TRUE(match.captures[1].isNull());  // no decoration, but still present
    EXPECT_EQ(match.captures[2].name, "two");
    EXPECT_EQ(match.captures[2].patternsRule, kInvalidRuleId);
    EXPECT_TRUE(match.beginCaptures.empty());

    const Rule& beginEnd = g.rule(top[1]);
    EXPECT_EQ(beginEnd.kind, RuleKind::BeginEnd);
    EXPECT_TRUE(beginEnd.hasBegin);
    EXPECT_TRUE(beginEnd.hasEnd);
    EXPECT_FALSE(beginEnd.hasWhile);
    EXPECT_EQ(beginEnd.begin, "b");
    EXPECT_EQ(beginEnd.end, "\\1");
    EXPECT_TRUE(beginEnd.endHasBackrefs);
    EXPECT_FALSE(beginEnd.whileHasBackrefs);
    EXPECT_TRUE(beginEnd.applyEndPatternLast);
    EXPECT_EQ(beginEnd.contentName, "be.content");
    ASSERT_EQ(beginEnd.beginCaptures.size(), 2u);
    EXPECT_EQ(beginEnd.beginCaptures[1].name, "bc");
    ASSERT_EQ(beginEnd.endCaptures.size(), 2u);
    EXPECT_EQ(beginEnd.endCaptures[1].name, "ec");

    const Rule& beginWhile = g.rule(top[2]);
    EXPECT_EQ(beginWhile.kind, RuleKind::BeginWhile);
    EXPECT_TRUE(beginWhile.hasWhile);
    EXPECT_EQ(beginWhile.whilePattern, "\\2");
    EXPECT_TRUE(beginWhile.whileHasBackrefs);
    EXPECT_FALSE(beginWhile.applyEndPatternLast);

    const Rule& container = g.rule(top[3]);
    EXPECT_EQ(container.kind, RuleKind::Container);
    ASSERT_EQ(container.patterns.size(), 1u);
    EXPECT_EQ(g.rule(container.patterns[0]).debugName, "$self.patterns[3].patterns[0]");
    EXPECT_EQ(g.rule(container.patterns[0]).match, "z");

    const Rule& nameOnly = g.rule(top[4]);
    EXPECT_EQ(nameOnly.kind, RuleKind::Container);
    EXPECT_EQ(nameOnly.name, "decoration.only");
    EXPECT_TRUE(nameOnly.patterns.empty());

    // TextMate compatibility: "captures" on a begin/end rule feeds both sides.
    const Rule& shared = g.rule(top[5]);
    ASSERT_EQ(shared.captures.size(), 2u);
    ASSERT_EQ(shared.beginCaptures.size(), 2u);
    ASSERT_EQ(shared.endCaptures.size(), 2u);
    EXPECT_EQ(shared.beginCaptures[1].name, "shared");
    EXPECT_EQ(shared.endCaptures[1].name, "shared");

    // Non-numeric and out-of-range capture keys are ignored, never allocated.
    const Rule& bogus = g.rule(top[6]);
    EXPECT_TRUE(bogus.captures.empty());
}

TEST(LoadGrammar, CaptureWithPatternsGetsASyntheticContainer) {
    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammarJson(R"json({
      "scopeName": "source.cap",
      "patterns": [
        { "match": "(a)", "name": "m",
          "captures": { "1": { "name": "c.one", "contentName": "c.inner",
                               "patterns": [ { "match": "x", "name": "sub.x" } ] } } }
      ]
    })json",
                                g, &error))
        << error;
    const RuleId ruleId = g.rule(g.rootRule()).patterns.at(0);
    const Rule& rule = g.rule(ruleId);
    ASSERT_EQ(rule.captures.size(), 2u);
    const RuleId synthetic = rule.captures[1].patternsRule;
    ASSERT_NE(synthetic, kInvalidRuleId);
    EXPECT_EQ(rule.captures[1].name, "c.one");
    EXPECT_EQ(rule.captures[1].contentName, "c.inner");
    EXPECT_FALSE(rule.captures[1].isNull());
    EXPECT_EQ(g.rule(synthetic).kind, RuleKind::Container);
    EXPECT_EQ(g.rule(synthetic).debugName, "$self.patterns[0].captures.1");
    ASSERT_EQ(g.rule(synthetic).patterns.size(), 1u);
    EXPECT_EQ(g.rule(g.rule(synthetic).patterns[0]).name, "sub.x");
}

// ---------------------------------------------------------------------------
// include resolution
// ---------------------------------------------------------------------------

constexpr std::string_view kIncludeGrammar = R"json({
  "scopeName": "source.inc",
  "patterns": [
    { "include": "#kw" },
    { "include": "$self" },
    { "include": "$base" },
    { "include": "#missing" },
    { "include": "source.other" },
    { "include": "source.other#deep" },
    { "include": "" },
    { "include": "$self#kw" }
  ],
  "repository": { "kw": { "match": "k", "name": "keyword.k" } }
})json";

TEST(LoadGrammar, IncludesAreRulesAndResolveLocallyWherePossible) {
    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammarJson(kIncludeGrammar, g, &error)) << error;
    const std::vector<RuleId>& top = g.rule(g.rootRule()).patterns;
    ASSERT_EQ(top.size(), 8u);
    for (const RuleId id : top) {
        EXPECT_EQ(g.rule(id).kind, RuleKind::Include);
    }
    const RuleId kw = g.repositoryRule("kw");
    ASSERT_NE(kw, kInvalidRuleId);

    EXPECT_EQ(g.rule(top[0]).include.kind, IncludeKind::Repository);
    EXPECT_EQ(g.rule(top[0]).include.ruleName, "kw");
    EXPECT_EQ(g.rule(top[0]).includeTarget, kw);

    EXPECT_EQ(g.rule(top[1]).include.kind, IncludeKind::Self);
    EXPECT_EQ(g.rule(top[1]).includeTarget, g.rootRule());

    // Without a registry, "$base" falls back to this grammar's root.
    EXPECT_EQ(g.rule(top[2]).include.kind, IncludeKind::Base);
    EXPECT_EQ(g.rule(top[2]).includeTarget, g.rootRule());

    EXPECT_EQ(g.rule(top[3]).include.kind, IncludeKind::Repository);
    EXPECT_EQ(g.rule(top[3]).includeTarget, kInvalidRuleId);

    EXPECT_EQ(g.rule(top[4]).include.kind, IncludeKind::ExternalGrammar);
    EXPECT_EQ(g.rule(top[4]).include.scopeName, "source.other");
    EXPECT_EQ(g.rule(top[4]).includeTarget, kInvalidRuleId);

    EXPECT_EQ(g.rule(top[5]).include.kind, IncludeKind::ExternalRule);
    EXPECT_EQ(g.rule(top[5]).include.ruleName, "deep");

    EXPECT_EQ(g.rule(top[6]).include.kind, IncludeKind::Invalid);
    EXPECT_EQ(g.rule(top[7]).includeTarget, kw);

    // Diagnostics: one deduplicated external scope, two broken includes in rule
    // order.
    ASSERT_EQ(g.externalScopeRefs().size(), 1u);
    EXPECT_EQ(g.externalScopeRefs()[0], "source.other");
    ASSERT_EQ(g.brokenIncludes().size(), 2u);
    EXPECT_EQ(g.brokenIncludes()[0], "#missing");
    EXPECT_EQ(g.brokenIncludes()[1], "");
}

constexpr std::string_view kCyclicGrammar = R"json({
  "scopeName": "source.cyc",
  "patterns": [ { "include": "#a" } ],
  "repository": {
    "a": { "patterns": [ { "include": "#b" }, { "match": "x", "name": "hit.x" } ] },
    "b": { "patterns": [ { "include": "#a" } ] }
  }
})json";

TEST(LoadGrammar, CyclicRepositoryIncludesLoadWithoutRecursing) {
    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammarJson(kCyclicGrammar, g, &error)) << error;
    const RuleId a = g.repositoryRule("a");
    const RuleId b = g.repositoryRule("b");
    ASSERT_NE(a, kInvalidRuleId);
    ASSERT_NE(b, kInvalidRuleId);

    ASSERT_EQ(g.rule(a).patterns.size(), 2u);
    const Rule& aToB = g.rule(g.rule(a).patterns[0]);
    EXPECT_EQ(aToB.kind, RuleKind::Include);
    EXPECT_EQ(aToB.includeTarget, b);

    ASSERT_EQ(g.rule(b).patterns.size(), 1u);
    const Rule& bToA = g.rule(g.rule(b).patterns[0]);
    EXPECT_EQ(bToA.kind, RuleKind::Include);
    EXPECT_EQ(bToA.includeTarget, a);

    EXPECT_TRUE(g.brokenIncludes().empty());
}

TEST(LoadGrammar, RuleNestingIsBounded) {
    // Built as a JSON tree directly: 120 nested "patterns" levels would exceed
    // the JSON parser's own depth limit if it were written as text.
    json::Value node;
    node.becomeObject();
    json::Value match;
    match.setString("deepest");
    node.insert("match", match);
    const int kLevels = 120;
    for (int i = 0; i < kLevels; ++i) {
        json::Value list;
        list.becomeArray();
        list.push(node);
        json::Value outer;
        outer.becomeObject();
        outer.insert("patterns", list);
        node = outer;
    }
    json::Value top;
    top.becomeArray();
    top.push(node);
    json::Value root;
    root.becomeObject();
    json::Value scope;
    scope.setString("source.deep");
    root.insert("scopeName", scope);
    root.insert("patterns", top);

    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammar(root, g, &error)) << error;

    // Levels 1..kMaxRuleNestingDepth exist; the one below the limit has no
    // children because its child would have been at depth limit + 1.
    ASSERT_EQ(g.rule(g.rootRule()).patterns.size(), 1u);
    RuleId current = g.rule(g.rootRule()).patterns[0];
    for (int level = 1; level < kMaxRuleNestingDepth; ++level) {
        ASSERT_TRUE(g.validRule(current)) << "level " << level;
        ASSERT_EQ(g.rule(current).patterns.size(), 1u) << "level " << level;
        current = g.rule(current).patterns[0];
    }
    EXPECT_TRUE(g.rule(current).patterns.empty());
}

// ---------------------------------------------------------------------------
// loadGrammar failures
// ---------------------------------------------------------------------------

TEST(LoadGrammar, StructurallyUnusableInputIsRejected) {
    Grammar g;
    std::string error;

    EXPECT_FALSE(loadGrammarJson("[1,2]", g, &error));
    EXPECT_EQ(error, "grammar root is not a JSON object");

    EXPECT_FALSE(loadGrammarJson("{}", g, &error));
    EXPECT_EQ(error, "grammar has no scopeName");

    EXPECT_FALSE(loadGrammarJson(R"json({"scopeName":""})json", g, &error));
    EXPECT_EQ(error, "grammar has no scopeName");

    EXPECT_FALSE(loadGrammarJson(R"json({"scopeName":42})json", g, &error));
    EXPECT_EQ(error, "grammar has no scopeName");

    error.clear();
    EXPECT_FALSE(loadGrammarJson("{oops", g, &error));
    EXPECT_EQ(error.compare(0, 6, "json: "), 0) << error;

    // A failed load leaves an empty grammar behind, not a half-built one.
    EXPECT_TRUE(g.scopeName().empty());
    EXPECT_EQ(g.ruleCount(), 0u);
    EXPECT_EQ(g.rootRule(), kInvalidRuleId);

    // The error pointer is optional.
    EXPECT_FALSE(loadGrammarJson("{}", g, nullptr));
}

TEST(LoadGrammar, UnusablePatternEntriesAreSkippedNotFatal) {
    Grammar g;
    std::string error;
    ASSERT_TRUE(loadGrammarJson(
        R"json({"scopeName":"s","patterns":[1,"x",null,{"match":"a"},[]],"repository":3})json", g,
        &error))
        << error;
    // Only the object survives; the rest are not rules at all.
    ASSERT_EQ(g.rule(g.rootRule()).patterns.size(), 1u);
    EXPECT_EQ(g.rule(g.rule(g.rootRule()).patterns[0]).match, "a");
    EXPECT_TRUE(g.repository().empty());
}

// ---------------------------------------------------------------------------
// GrammarRegistry
// ---------------------------------------------------------------------------

constexpr std::string_view kGrammarA = R"json({
  "scopeName": "source.a",
  "fileTypes": ["aa", "a2"],
  "patterns": [ { "match": "a", "name": "a.match" } ],
  "repository": { "kw": { "match": "ka", "name": "a.kw" } }
})json";

constexpr std::string_view kGrammarB = R"json({
  "scopeName": "source.b",
  "patterns": [
    { "include": "$base" },
    { "include": "source.a#kw" },
    { "include": "source.a" },
    { "include": "source.a#nope" },
    { "include": "source.gone" },
    { "match": "b", "name": "b.match" }
  ]
})json";

TEST(GrammarRegistryTest, AddLookupAndDuplicateScopeNames) {
    GrammarRegistry reg;
    std::string error;
    const GrammarId a = reg.addGrammarJson(kGrammarA, &error);
    ASSERT_NE(a, kInvalidGrammarId) << error;
    EXPECT_EQ(reg.grammarCount(), 1u);
    EXPECT_EQ(reg.idForScope("source.a"), a);
    EXPECT_EQ(reg.idForScope("source.zzz"), kInvalidGrammarId);
    ASSERT_NE(reg.grammarById(a), nullptr);
    EXPECT_EQ(reg.grammarById(a)->id(), a);
    EXPECT_EQ(reg.grammarById(a)->scopeName(), "source.a");
    EXPECT_EQ(reg.grammarById(-1), nullptr);
    EXPECT_EQ(reg.grammarById(99), nullptr);

    // Re-adding the same scope keeps the first registration and its id, because
    // live tokenizer states may already reference its rule ids.
    const GrammarId again = reg.addGrammarJson(kGrammarA, &error);
    EXPECT_EQ(again, a);
    EXPECT_EQ(reg.grammarCount(), 1u);

    // fileTypes become extension mappings.
    EXPECT_EQ(reg.scopeForExtension("aa"), "source.a");
    EXPECT_EQ(reg.scopeForExtension("a2"), "source.a");
    EXPECT_EQ(reg.scopeForExtension(".aa"), "source.a");  // leading dot stripped
    EXPECT_EQ(reg.scopeForExtension("nope"), "");
    EXPECT_EQ(reg.scopeForExtension(""), "");
    ASSERT_NE(reg.grammarForExtension("aa"), nullptr);
    EXPECT_EQ(reg.grammarForExtension("aa")->scopeName(), "source.a");
    EXPECT_EQ(reg.grammarForExtension("unknown"), nullptr);

    reg.mapExtension(".cc", "source.cpp");
    EXPECT_EQ(reg.scopeForExtension("cc"), "source.cpp");
    reg.mapExtension("", "source.cpp");  // ignored, must not crash
    reg.mapExtension(".", "source.cpp");
    EXPECT_EQ(reg.scopeForExtension("."), "");

    EXPECT_EQ(reg.addGrammarJson("{not json", &error), kInvalidGrammarId);
    EXPECT_EQ(reg.grammarCount(), 1u);
}

TEST(GrammarRegistryTest, InlineFragmentGrammarsNeverClaimExtensions) {
    // es-tag-css.json (scope "inline.es6-css") ships fileTypes js/jsx/ts/tsx/
    // html/vue. Those grammars are embedded fragments included by a host
    // grammar by scope reference; letting them claim whole-file extensions
    // hijacks every .js open (and decides the lazy-load path), so their
    // fileTypes must be ignored.
    GrammarRegistry reg;
    std::string error;
    constexpr std::string_view kInlineFragment = R"json({
  "scopeName": "inline.es6-css",
  "fileTypes": ["js", "html"],
  "patterns": [ { "match": "x", "name": "x.match" } ]
})json";
    ASSERT_NE(reg.addGrammarJson(kInlineFragment, &error), kInvalidGrammarId) << error;
    EXPECT_EQ(reg.scopeForExtension("js"), "");
    EXPECT_EQ(reg.scopeForExtension("html"), "");
    // The grammar itself is still reachable for embedding by scope name.
    ASSERT_NE(reg.grammarForScope("inline.es6-css"), nullptr);
    // A later real grammar still gets the extension when it ships fileTypes.
    constexpr std::string_view kRealJs = R"json({
  "scopeName": "source.js",
  "fileTypes": ["js"],
  "patterns": [ { "match": "y", "name": "y.match" } ]
})json";
    ASSERT_NE(reg.addGrammarJson(kRealJs, &error), kInvalidGrammarId) << error;
    EXPECT_EQ(reg.scopeForExtension("js"), "source.js");
}

TEST(GrammarRegistryTest, ResolveIncludeAcrossGrammars) {
    GrammarRegistry reg;
    std::string error;
    const GrammarId a = reg.addGrammarJson(kGrammarA, &error);
    ASSERT_NE(a, kInvalidGrammarId) << error;
    const GrammarId b = reg.addGrammarJson(kGrammarB, &error);
    ASSERT_NE(b, kInvalidGrammarId) << error;

    const Grammar* ga = reg.grammarById(a);
    const Grammar* gb = reg.grammarById(b);
    ASSERT_NE(ga, nullptr);
    ASSERT_NE(gb, nullptr);
    const std::vector<RuleId>& top = gb->rule(gb->rootRule()).patterns;
    ASSERT_EQ(top.size(), 6u);

    // "$base" resolves to the root of the grammar being tokenized.
    const RuleRef base = reg.resolveInclude(b, top[0], a);
    EXPECT_EQ(base.grammar, a);
    EXPECT_EQ(base.rule, ga->rootRule());
    EXPECT_TRUE(base.valid());
    // With no base grammar, "$base" behaves like "$self".
    const RuleRef selfish = reg.resolveInclude(b, top[0], kInvalidGrammarId);
    EXPECT_EQ(selfish.grammar, b);
    EXPECT_EQ(selfish.rule, gb->rootRule());

    // "source.a#kw" reaches into another grammar's repository.
    const RuleRef external = reg.resolveInclude(b, top[1], b);
    EXPECT_EQ(external.grammar, a);
    EXPECT_EQ(external.rule, ga->repositoryRule("kw"));

    // "source.a" resolves to that grammar's root rule.
    const RuleRef whole = reg.resolveInclude(b, top[2], b);
    EXPECT_EQ(whole.grammar, a);
    EXPECT_EQ(whole.rule, ga->rootRule());

    // Unresolvable references are an ordinary invalid RuleRef, never a throw.
    EXPECT_FALSE(reg.resolveInclude(b, top[3], b).valid());  // missing repo entry
    EXPECT_FALSE(reg.resolveInclude(b, top[4], b).valid());  // grammar not loaded
    ASSERT_EQ(reg.missingScopes().size(), 1u);
    EXPECT_EQ(reg.missingScopes()[0], "source.gone");
    // Asking again does not re-report it.
    EXPECT_FALSE(reg.resolveInclude(b, top[4], b).valid());
    EXPECT_EQ(reg.missingScopes().size(), 1u);

    // A non-include rule resolves to itself; nonsense input is invalid.
    const RuleRef plain = reg.resolveInclude(b, top[5], b);
    EXPECT_EQ(plain.grammar, b);
    EXPECT_EQ(plain.rule, top[5]);
    EXPECT_FALSE(reg.resolveInclude(b, 9999, b).valid());
    EXPECT_FALSE(reg.resolveInclude(77, top[0], b).valid());
}

TEST(GrammarRegistryTest, RuleRefPackingAndComparison) {
    const RuleRef a{3, 7};
    const RuleRef same{3, 7};
    const RuleRef other{7, 3};
    EXPECT_TRUE(a == same);
    EXPECT_TRUE(a != other);
    EXPECT_EQ(a.key(), same.key());
    EXPECT_NE(a.key(), other.key());
    EXPECT_TRUE(a.valid());
    EXPECT_FALSE(RuleRef{}.valid());
    EXPECT_FALSE((RuleRef{0, -1}).valid());
    EXPECT_FALSE((RuleRef{-1, 0}).valid());
}

TEST(GrammarRegistryTest, LazyLoadingThroughTheSourceLoader) {
    GrammarRegistry reg;
    int calls = 0;
    std::vector<std::string> requested;
    reg.setLoader([&calls, &requested](std::string_view scope) -> std::optional<std::string> {
        ++calls;
        requested.emplace_back(scope);
        if (scope == "source.a") return std::string(kGrammarA);
        return std::nullopt;
    });

    const Grammar* g = reg.grammarForScope("source.a");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->scopeName(), "source.a");
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(reg.grammarCount(), 1u);

    // Second lookup is served from the map: no second load.
    EXPECT_EQ(reg.grammarForScope("source.a"), g);
    EXPECT_EQ(calls, 1);

    // A miss is remembered so a broken reference costs one attempt per process.
    EXPECT_EQ(reg.grammarForScope("source.missing"), nullptr);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(reg.grammarForScope("source.missing"), nullptr);
    EXPECT_EQ(calls, 2);
    ASSERT_EQ(reg.missingScopes().size(), 1u);
    EXPECT_EQ(reg.missingScopes()[0], "source.missing");
    ASSERT_EQ(requested.size(), 2u);
    EXPECT_EQ(requested[0], "source.a");
    EXPECT_EQ(requested[1], "source.missing");

    EXPECT_EQ(reg.grammarForScope(""), nullptr);
}

TEST(GrammarRegistryTest, MisbehavingLoaderCannotBreakTokenization) {
    GrammarRegistry reg;
    reg.setLoader([](std::string_view) -> std::optional<std::string> {
        throw std::runtime_error("loader exploded");
    });
    EXPECT_EQ(reg.grammarForScope("source.boom"), nullptr);
    ASSERT_EQ(reg.missingScopes().size(), 1u);
    EXPECT_EQ(reg.missingScopes()[0], "source.boom");

    // A loader that returns garbage is also just a miss.
    GrammarRegistry junk;
    junk.setLoader([](std::string_view) -> std::optional<std::string> {
        return std::string("{not a grammar");
    });
    EXPECT_EQ(junk.grammarForScope("source.junk"), nullptr);
    ASSERT_EQ(junk.missingScopes().size(), 1u);

    // A file that declares a different scopeName than the one requested.
    GrammarRegistry mismatch;
    mismatch.setLoader([](std::string_view) -> std::optional<std::string> {
        return std::string(kGrammarA);
    });
    EXPECT_EQ(mismatch.grammarForScope("source.expected"), nullptr);
    ASSERT_EQ(mismatch.missingScopes().size(), 1u);
    EXPECT_EQ(mismatch.missingScopes()[0], "source.expected");
    // ...but the grammar it did contain is now registered under its real name.
    EXPECT_EQ(mismatch.grammarCount(), 1u);
    ASSERT_NE(mismatch.grammarById(0), nullptr);
    EXPECT_EQ(mismatch.grammarById(0)->scopeName(), "source.a");
}

TEST(GrammarRegistryTest, NoLoaderMeansEveryMissIsRecordedOnce) {
    GrammarRegistry reg;
    EXPECT_EQ(reg.grammarForScope("source.nothing"), nullptr);
    EXPECT_EQ(reg.grammarForScope("source.nothing"), nullptr);
    ASSERT_EQ(reg.missingScopes().size(), 1u);
    EXPECT_EQ(reg.missingScopes()[0], "source.nothing");
}

}  // namespace
