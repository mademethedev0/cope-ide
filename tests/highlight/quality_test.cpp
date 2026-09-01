#include <gtest/gtest.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/highlight/quality.h>

#include "highlight_test_util.h"

namespace {

using ember_highlight_test::Config;
using ember_highlight_test::Harness;
using ide::highlight::analyzeDocument;
using ide::highlight::analyzeLines;
using ide::highlight::formatQualityReport;
using ide::highlight::QualityReport;
using ide::highlight::Tier;

constexpr std::string_view kCrippledGrammar = R"json({
  "scopeName": "source.crippled",
  "fileTypes": ["c"],
  "patterns": [
    { "name": "storage.type.crippled", "match": "\\bint\\b" }
  ]
})json";

/// 19 bytes, 16 of them non-whitespace: "int" (3) + "foobar" (6) + "=" (1) +
/// "12345" (5) + ";" (1). Every expectation below is derived from these numbers
/// by hand, which is the point: the metric must be checkable without running it.
constexpr std::string_view kFixtureLine = "int foobar = 12345;";
constexpr size_t kFixtureLines = 25;
constexpr size_t kNonWhitespacePerLine = 16;
constexpr size_t kScopedPerLine = 3;  // only "int" is scoped by the grammar

[[nodiscard]] std::vector<std::string_view> fixture() {
    return std::vector<std::string_view>(kFixtureLines, kFixtureLine);
}

// --- the happy path ----------------------------------------------------------

TEST(HighlightQualityTest, FallbackOnlyFileScoresFullCoverage) {
    Config config;
    config.fileName = "a.c";  // no grammar: tier 2
    Harness harness(config);

    const std::vector<std::string_view> lines{"int x = 1;", "return x;"};
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));

    EXPECT_EQ(report.tier, Tier::kFallback);
    EXPECT_EQ(report.lineCount, 2u);
    EXPECT_EQ(report.totalBytes, 19u);
    EXPECT_EQ(report.nonWhitespaceBytes, 15u);
    EXPECT_EQ(report.scopedBytes, 15u);
    EXPECT_DOUBLE_EQ(report.coverage(), 1.0);
    EXPECT_EQ(report.defaultStyledBytes, 0u);
    EXPECT_EQ(report.unstyledRuns, 0u);
    EXPECT_EQ(report.repairedBytes, 0u);  // tier 2 does not "repair", it lexes
    EXPECT_GE(report.distinctStyles, 5u);
    EXPECT_FALSE(report.suspicious()) << formatQualityReport(report);
}

// --- the failure the module exists to detect ---------------------------------

TEST(HighlightQualityTest, LowCoverageIsSuspiciousWithHandComputedNumbers) {
    Config config;
    config.grammarJson = kCrippledGrammar;
    config.fileName = "a.c";
    config.limits.repairWithFallback = false;  // raw tier 1: the broken-looking case
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);

    const std::vector<std::string_view> lines = fixture();
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));

    EXPECT_EQ(report.tier, Tier::kGrammar);
    EXPECT_EQ(report.lineCount, kFixtureLines);
    EXPECT_EQ(report.totalBytes, kFixtureLine.size() * kFixtureLines);
    EXPECT_EQ(report.nonWhitespaceBytes, kNonWhitespacePerLine * kFixtureLines);
    EXPECT_EQ(report.scopedBytes, kScopedPerLine * kFixtureLines);
    EXPECT_DOUBLE_EQ(report.coverage(), 3.0 / 16.0);
    // "foobar" and "12345;" are unstyled runs of 6; "=" is a run of 1 and does
    // not count. Two runs per line, longest 6.
    EXPECT_EQ(report.unstyledRuns, 2u * kFixtureLines);
    EXPECT_EQ(report.longestUnstyledRun, 6u);
    EXPECT_EQ(report.defaultStyledBytes, 13u * kFixtureLines);
    // Only storage.type and the default style are ever used.
    EXPECT_EQ(report.distinctStyles, 2u);

    EXPECT_TRUE(report.coverageSuspicious());
    EXPECT_TRUE(report.styleVarietySuspicious());
    EXPECT_FALSE(report.repairSuspicious());
    EXPECT_TRUE(report.suspicious());
}

TEST(HighlightQualityTest, RepairLiftsCoverageButFlagsTheGrammar) {
    Config config;
    config.grammarJson = kCrippledGrammar;
    config.fileName = "a.c";
    Harness harness(config);

    const std::vector<std::string_view> lines = fixture();
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));

    // The repair layer scopes every remaining byte, so the output looks right...
    EXPECT_DOUBLE_EQ(report.coverage(), 1.0);
    EXPECT_EQ(report.unstyledRuns, 0u);
    EXPECT_EQ(report.defaultStyledBytes, 0u);
    EXPECT_GE(report.distinctStyles, 5u);
    EXPECT_FALSE(report.coverageSuspicious());
    EXPECT_FALSE(report.styleVarietySuspicious());

    // ... and the report still says the grammar is doing almost none of the work.
    EXPECT_EQ(report.repairedBytes, 13u * kFixtureLines);
    EXPECT_DOUBLE_EQ(report.repairRatio(), 13.0 / 16.0);
    EXPECT_TRUE(report.repairSuspicious());
    EXPECT_TRUE(report.suspicious()) << formatQualityReport(report);
}

TEST(HighlightQualityTest, AThemeThatDoesNotWorkShowsUpAsTooFewStyles) {
    Config config;
    config.fileName = "a.c";  // tier 2: scopes are produced for everything
    config.themeJson = ember_highlight_test::kPoorThemeJson;
    Harness harness(config);

    const std::vector<std::string_view> lines = fixture();
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));

    // Coverage is perfect: the tokenizer did its job...
    EXPECT_DOUBLE_EQ(report.coverage(), 1.0);
    EXPECT_FALSE(report.coverageSuspicious());
    // ... but the theme colours none of it, which a reader sees and coverage does
    // not. This is why unstyledRuns is measured on StyleIds, not on scopes.
    EXPECT_EQ(report.distinctStyles, 1u);
    EXPECT_GT(report.unstyledRuns, 0u);
    EXPECT_TRUE(report.styleVarietySuspicious());
    EXPECT_TRUE(report.suspicious());
}

TEST(HighlightQualityTest, AShortFileIsNotJudgedOnStyleVariety) {
    Config config;
    config.fileName = "a.c";
    config.themeJson = ember_highlight_test::kPoorThemeJson;
    Harness harness(config);

    const std::vector<std::string_view> lines(5, kFixtureLine);
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));
    EXPECT_EQ(report.distinctStyles, 1u);
    EXPECT_FALSE(report.styleVarietySuspicious()) << "5 lines is too little evidence";
}

TEST(HighlightQualityTest, PlainTierIsNeverSuspicious) {
    Config config;
    config.grammarJson = kCrippledGrammar;
    config.fileName = "a.c";
    Harness harness(config);
    harness.h().forceTier(Tier::kPlain);

    const std::vector<std::string_view> lines = fixture();
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));
    EXPECT_EQ(report.tier, Tier::kPlain);
    EXPECT_EQ(report.scopedBytes, 0u);
    EXPECT_DOUBLE_EQ(report.coverage(), 0.0);
    EXPECT_FALSE(report.suspicious()) << "a file over the size limit is meant to be plain";
}

TEST(HighlightQualityTest, EmptyDocumentIsPerfectByDefinition) {
    Config config;
    config.fileName = "a.c";
    Harness harness(config);
    const std::vector<std::string_view> lines;
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));
    EXPECT_EQ(report.lineCount, 0u);
    EXPECT_EQ(report.nonWhitespaceBytes, 0u);
    EXPECT_DOUBLE_EQ(report.coverage(), 1.0);
    EXPECT_DOUBLE_EQ(report.repairRatio(), 0.0);
    EXPECT_FALSE(report.suspicious());
}

TEST(HighlightQualityTest, BlankLinesDoNotCountAgainstCoverage) {
    Config config;
    config.fileName = "a.c";
    Harness harness(config);
    const std::vector<std::string_view> lines{"", "   ", "\t", "int x;"};
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));
    EXPECT_EQ(report.lineCount, 4u);
    EXPECT_EQ(report.totalBytes, 10u);
    EXPECT_EQ(report.nonWhitespaceBytes, 5u);  // i n t x ;
    EXPECT_DOUBLE_EQ(report.coverage(), 1.0);
}

// --- bookkeeping -------------------------------------------------------------

TEST(HighlightQualityTest, AnalyzingTwiceReportsPerRunNumbers) {
    Config config;
    config.grammarJson = kCrippledGrammar;
    config.fileName = "a.c";
    Harness harness(config);

    const std::vector<std::string_view> lines = fixture();
    const QualityReport first = analyzeLines(harness.h(), std::span<const std::string_view>(lines));
    const QualityReport second = analyzeLines(harness.h(), std::span<const std::string_view>(lines));
    EXPECT_EQ(first.repairedBytes, second.repairedBytes);
    EXPECT_EQ(first.repairRegions, second.repairRegions);
    // The highlighter's own counters, in contrast, accumulate.
    EXPECT_EQ(harness.h().stats().repairedBytes, first.repairedBytes * 2u);
}

TEST(HighlightQualityTest, AnalyzeDocumentMatchesAnalyzeLines) {
    Config config;
    config.fileName = "a.c";
    Harness harness(config);

    const std::vector<std::string> owned{"int x = 1;", "return x;"};
    const std::vector<std::string_view> views{owned[0], owned[1]};
    const QualityReport fromViews =
        analyzeLines(harness.h(), std::span<const std::string_view>(views));
    const QualityReport fromDocument = analyzeDocument(harness.h(), owned);

    EXPECT_EQ(fromViews.nonWhitespaceBytes, fromDocument.nonWhitespaceBytes);
    EXPECT_EQ(fromViews.scopedBytes, fromDocument.scopedBytes);
    EXPECT_EQ(fromViews.distinctStyles, fromDocument.distinctStyles);
    EXPECT_EQ(formatQualityReport(fromViews), formatQualityReport(fromDocument));
}

TEST(HighlightQualityTest, RefusedPatternsAreReported) {
    // A grammar whose rule uses a construct std::regex cannot compile: the engine
    // must refuse it, the tokenizer must count it, and the report must show it.
    constexpr std::string_view kUnsupported = R"json({
      "scopeName": "source.unsupported",
      "fileTypes": ["c"],
      "patterns": [
        { "name": "keyword.control.sub", "match": "\\g<recurse>" },
        { "name": "storage.type.ok", "match": "\\bint\\b" }
      ]
    })json";
    Config config;
    config.grammarJson = kUnsupported;
    config.fileName = "a.c";
    Harness harness(config);
    ASSERT_EQ(harness.h().tier(), Tier::kGrammar);

    const std::vector<std::string_view> lines{"int x = 1;"};
    const QualityReport report = analyzeLines(harness.h(), std::span<const std::string_view>(lines));
    EXPECT_GT(report.refusedPatterns, 0u);
    EXPECT_NE(formatQualityReport(report).find("refused="), std::string::npos);
}

// --- formatting --------------------------------------------------------------

TEST(HighlightQualityTest, FormatIsCompactAndDeterministic) {
    QualityReport report;
    report.tier = Tier::kGrammar;
    report.lineCount = 3;
    report.nonWhitespaceBytes = 100;
    report.scopedBytes = 93;
    report.repairedBytes = 4;
    report.distinctStyles = 11;
    report.unstyledRuns = 2;
    report.longestUnstyledRun = 7;
    report.refusedPatterns = 17;
    report.unusableRules = 1;

    const std::string text = formatQualityReport(report);
    EXPECT_EQ(text,
              "highlight: tier=grammar lines=3 cov=0.93 (93/100 bytes) styles=11 repaired=4.00% "
              "unstyled-runs=2 (max 7) refused=17 unusable=1 plain=0 OK");
    EXPECT_EQ(text, formatQualityReport(report));  // no hidden state
}

TEST(HighlightQualityTest, FormatMarksSuspectFiles) {
    QualityReport report;
    report.tier = Tier::kGrammar;
    report.lineCount = 100;
    report.nonWhitespaceBytes = 100;
    report.scopedBytes = 10;
    report.distinctStyles = 9;
    const std::string text = formatQualityReport(report);
    EXPECT_NE(text.find("SUSPECT"), std::string::npos) << text;
    EXPECT_NE(text.find("cov=0.10"), std::string::npos) << text;
}

TEST(HighlightQualityTest, ThresholdsAreTheDocumentedOnes) {
    EXPECT_DOUBLE_EQ(ide::highlight::kSuspiciousCoverage, 0.85);
    EXPECT_EQ(ide::highlight::kMinDistinctStyles, 4u);
    EXPECT_EQ(ide::highlight::kStyleVarietyMinLines, 20u);
    EXPECT_DOUBLE_EQ(ide::highlight::kSuspiciousRepairRatio, 0.30);
    EXPECT_EQ(ide::highlight::kMinUnstyledRunLength, 3u);
}

}  // namespace
