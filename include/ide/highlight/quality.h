#pragma once

// Why this file exists
// -------------------
// "It looks trash" is not a bug report and cannot be regression-tested. This
// header turns the perceived quality of highlighting into numbers CI can gate
// on, so a grammar or regex-engine regression is caught by a failing threshold
// instead of by a user losing faith in the editor.
//
// THE THRESHOLDS WE GATE ON
// -------------------------
//   * coverage < 0.85              suspicious. More than one non-whitespace byte
//                                  in seven carries no scope beyond the root,
//                                  which is where "random grey words" starts to
//                                  be visible.
//   * distinctStyles < 4 on a file
//     of more than 20 lines        the theme or the grammar is effectively not
//                                  working. Real code on a real theme uses far
//                                  more than four palette entries; three or
//                                  fewer means comments/strings/keywords are all
//                                  landing on the same style.
//   * repairRatio > 0.30           the fallback tier is carrying the file. The
//                                  output may look fine, but the grammar (or the
//                                  regex engine's coverage of it) is broken and
//                                  is worth reporting upstream.
//
// A report is a measurement, never a verdict: suspicious() only says "look at
// this file", and nothing in the engine changes behaviour because of it.
//
// Note on definitions, because two plausible ones disagree:
//   * `scopedBytes` (and therefore coverage) counts bytes whose scope stack has
//     something beyond the grammar's root scope - that measures the TOKENIZER.
//   * `unstyledRuns` counts runs of bytes whose resolved StyleId is the default
//     style - that measures what a READER SEES, which is the thing that killed
//     five previous attempts. A file can have coverage 1.0 and still be full of
//     unstyled runs if the theme has no rule for the scopes the grammar emits.
//   * `repairedBytes` counts tier-1 repair only. At tier 2 the fallback produced
//     everything by definition, so a repair ratio there would be meaningless.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ide/highlight/highlighter.h>

namespace ide::highlight {

/// Coverage below this is suspicious (see the header comment).
inline constexpr double kSuspiciousCoverage = 0.85;
/// Fewer than this many distinct StyleIds on a file longer than
/// kStyleVarietyMinLines means the theme or the grammar is not working.
inline constexpr size_t kMinDistinctStyles = 4;
inline constexpr size_t kStyleVarietyMinLines = 20;
/// Above this fraction of non-whitespace bytes repaired by the fallback, the
/// grammar or the regex engine has a problem worth reporting.
inline constexpr double kSuspiciousRepairRatio = 0.30;
/// Shortest run of default-styled non-whitespace bytes a reader notices. Two
/// bytes is punctuation; three is a word.
inline constexpr size_t kMinUnstyledRunLength = 3;

/// One measurement of one document. Plain data: printable, comparable, and small
/// enough to log per file.
struct QualityReport {
    Tier tier = Tier::kPlain;
    size_t lineCount = 0;
    size_t totalBytes = 0;
    size_t nonWhitespaceBytes = 0;
    /// Non-whitespace bytes whose scope stack goes beyond the grammar's root.
    size_t scopedBytes = 0;
    /// Non-whitespace bytes that resolved to the theme's default style, i.e.
    /// what the reader sees as uncoloured text.
    size_t defaultStyledBytes = 0;
    /// Non-whitespace bytes the tier-1 repair layer claimed.
    size_t repairedBytes = 0;
    /// Runs of at least kMinUnstyledRunLength default-styled non-whitespace
    /// bytes. Whitespace and a change of style both end a run.
    size_t unstyledRuns = 0;
    size_t longestUnstyledRun = 0;
    /// Distinct StyleIds used, including the default style.
    size_t distinctStyles = 0;
    /// Patterns the regex engine refused for this grammar, and begin/end rules
    /// skipped because their end pattern was unusable.
    size_t refusedPatterns = 0;
    size_t unusableRules = 0;
    /// Lines that fell through to tier 3 (too long).
    size_t plainLines = 0;
    size_t repairRegions = 0;

    /// scopedBytes / nonWhitespaceBytes. A document with no visible bytes at all
    /// is perfect by definition (1.0), not a division by zero.
    [[nodiscard]] double coverage() const noexcept;
    /// repairedBytes / nonWhitespaceBytes.
    [[nodiscard]] double repairRatio() const noexcept;
    /// defaultStyledBytes / nonWhitespaceBytes.
    [[nodiscard]] double unstyledRatio() const noexcept;

    [[nodiscard]] bool coverageSuspicious() const noexcept;
    [[nodiscard]] bool styleVarietySuspicious() const noexcept;
    [[nodiscard]] bool repairSuspicious() const noexcept;
    /// Any of the three gates tripped. Tier 3 is never suspicious: a file over
    /// the size limit is *meant* to be unstyled.
    [[nodiscard]] bool suspicious() const noexcept;
};

/// Highlights `lines` with `highlighter` and measures the result.
///
/// Starts from the highlighter's initial state, so this analyses the document
/// from line 0 - which is what makes the numbers reproducible. The
/// highlighter's own Stats are left exactly as they were, so analysing is safe
/// in the middle of a session.
[[nodiscard]] QualityReport analyzeLines(Highlighter& highlighter,
                                         std::span<const std::string_view> lines);

/// Convenience overload for a document held as owned lines. Deliberately not
/// taking ide::text::Document: the highlight module links only ide_syntax and
/// ide_theme, and a whole-buffer dependency would buy nothing here.
[[nodiscard]] QualityReport analyzeDocument(Highlighter& highlighter,
                                            const std::vector<std::string>& lines);

/// One compact human-readable line, e.g.
/// "highlight: tier=grammar lines=120 cov=0.93 (1240/1330 bytes) styles=11
///  repaired=4.10% unstyled-runs=2 (max 7) refused=17 unusable=1 plain=0 OK".
/// Deterministic and locale-independent, so it can go straight into a CI log.
[[nodiscard]] std::string formatQualityReport(const QualityReport& report);

}  // namespace ide::highlight
