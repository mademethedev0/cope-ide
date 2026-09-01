#include <ide/highlight/quality.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ide/highlight/highlighter.h>

namespace ide::highlight {
namespace {

/// Fixed-point formatting: no iostreams, no locale, byte-identical on every
/// runner, which is what makes a CI gate on this text possible.
[[nodiscard]] std::string formatRatio(double value) {
    if (!(value > 0.0)) return "0.00";  // also catches NaN
    if (value > 999.0) value = 999.0;
    const long long scaled = static_cast<long long>(value * 100.0 + 0.5);
    std::string out = std::to_string(scaled / 100);
    out += '.';
    const long long fraction = scaled % 100;
    if (fraction < 10) out += '0';
    out += std::to_string(fraction);
    return out;
}

[[nodiscard]] std::string formatPercent(double value) { return formatRatio(value * 100.0) + "%"; }

[[nodiscard]] double ratioOf(size_t part, size_t whole) noexcept {
    if (whole == 0u) return 0.0;
    return static_cast<double>(part) / static_cast<double>(whole);
}

}  // namespace

double QualityReport::coverage() const noexcept {
    if (nonWhitespaceBytes == 0u) return 1.0;
    return ratioOf(scopedBytes, nonWhitespaceBytes);
}

double QualityReport::repairRatio() const noexcept {
    return ratioOf(repairedBytes, nonWhitespaceBytes);
}

double QualityReport::unstyledRatio() const noexcept {
    return ratioOf(defaultStyledBytes, nonWhitespaceBytes);
}

bool QualityReport::coverageSuspicious() const noexcept {
    if (tier == Tier::kPlain || nonWhitespaceBytes == 0u) return false;
    return coverage() < kSuspiciousCoverage;
}

bool QualityReport::styleVarietySuspicious() const noexcept {
    if (tier == Tier::kPlain) return false;
    if (lineCount <= kStyleVarietyMinLines) return false;
    return distinctStyles < kMinDistinctStyles;
}

bool QualityReport::repairSuspicious() const noexcept {
    if (tier != Tier::kGrammar || nonWhitespaceBytes == 0u) return false;
    return repairRatio() > kSuspiciousRepairRatio;
}

bool QualityReport::suspicious() const noexcept {
    return coverageSuspicious() || styleVarietySuspicious() || repairSuspicious();
}

QualityReport analyzeLines(Highlighter& highlighter, std::span<const std::string_view> lines) {
    QualityReport report;
    const Highlighter::Stats before = highlighter.stats();

    LineState state = highlighter.initialState();
    std::vector<ScopedSpan> spans;
    std::unordered_set<theme::StyleId> styles;

    size_t run = 0;
    const auto flushRun = [&report, &run]() noexcept {
        if (run > report.longestUnstyledRun) report.longestUnstyledRun = run;
        if (run >= kMinUnstyledRunLength) ++report.unstyledRuns;
        run = 0;
    };

    for (const std::string_view line : lines) {
        ++report.lineCount;
        report.totalBytes += line.size();
        highlighter.scopeLine(line, state, spans);
        for (const ScopedSpan& span : spans) {
            if (span.end <= span.begin) continue;
            const bool rootOnly = highlighter.isRootOnly(span.scopes);
            const theme::StyleId style = highlighter.styleFor(span.scopes);
            styles.insert(style);
            const bool painted = style != Highlighter::defaultStyleId();
            const size_t stop = (span.end < line.size()) ? span.end : line.size();
            for (size_t i = span.begin; i < stop; ++i) {
                if (isWhitespaceByte(line[i])) {
                    flushRun();
                    continue;
                }
                ++report.nonWhitespaceBytes;
                if (!rootOnly) ++report.scopedBytes;
                if (painted) {
                    flushRun();
                } else {
                    ++report.defaultStyledBytes;
                    ++run;
                }
            }
        }
        flushRun();  // a run never crosses a line boundary
    }

    const Highlighter::Stats after = highlighter.stats();
    report.tier = highlighter.tier();
    report.distinctStyles = styles.size();
    report.refusedPatterns = highlighter.refusedPatternCount();
    report.unusableRules = highlighter.unusableRuleCount();
    report.repairedBytes = after.repairedBytes - before.repairedBytes;
    report.repairRegions = after.repairRegions - before.repairRegions;
    report.plainLines = after.plainLines - before.plainLines;
    return report;
}

QualityReport analyzeDocument(Highlighter& highlighter, const std::vector<std::string>& lines) {
    std::vector<std::string_view> views;
    views.reserve(lines.size());
    for (const std::string& line : lines) views.emplace_back(line);
    return analyzeLines(highlighter, std::span<const std::string_view>(views));
}

std::string formatQualityReport(const QualityReport& report) {
    std::string out = "highlight: tier=";
    out += tierName(report.tier);
    out += " lines=" + std::to_string(report.lineCount);
    out += " cov=" + formatRatio(report.coverage());
    out += " (" + std::to_string(report.scopedBytes) + "/" +
           std::to_string(report.nonWhitespaceBytes) + " bytes)";
    out += " styles=" + std::to_string(report.distinctStyles);
    out += " repaired=" + formatPercent(report.repairRatio());
    out += " unstyled-runs=" + std::to_string(report.unstyledRuns);
    out += " (max " + std::to_string(report.longestUnstyledRun) + ")";
    out += " refused=" + std::to_string(report.refusedPatterns);
    out += " unusable=" + std::to_string(report.unusableRules);
    out += " plain=" + std::to_string(report.plainLines);
    out += report.suspicious() ? " SUSPECT" : " OK";
    return out;
}

}  // namespace ide::highlight
