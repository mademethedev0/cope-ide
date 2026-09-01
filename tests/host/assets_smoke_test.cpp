// The highest-value test in the suite: every shipped theme and every shipped
// grammar must actually parse and load. A grammar file that regresses does not
// fail anywhere else until a user opens a file of that language.
//
// Output discipline (important): this test must never print lists of asset
// filenames on success - a listing of theme names has tripped the provider's
// content filter before. On success it prints ONE summary line. On failure it
// prints at most the first five failing file NAMES, never contents.

#include <ide/syntax/grammar.h>
#include <ide/syntax/json_lite.h>
#include <ide/theme/theme.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using ide::syntax::json::parse;

/// Reads one file as a string; empty string on any failure. Test-only helper,
/// so a plain ifstream is fine (core/ never sees it).
std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::string();
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Sorted *.json file paths under `dir`; empty when the directory is missing.
std::vector<fs::path> jsonFilesUnder(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return files;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

struct SmokeFailure {
    std::string fileName;
    std::string reason;
};

/// Reports at most `kMaxReported` failures by NAME only, then FAILs.
void reportFailures(const std::vector<SmokeFailure>& failures, std::string_view what,
                    size_t total, size_t parsed) {
    constexpr size_t kMaxReported = 5;
    for (size_t i = 0; i < failures.size() && i < kMaxReported; ++i) {
        ADD_FAILURE() << what << " failed to load: " << failures[i].fileName << " ("
                      << failures[i].reason << ")";
    }
    if (failures.size() > kMaxReported) {
        ADD_FAILURE() << "... and " << (failures.size() - kMaxReported) << " more failing files";
    }
    FAIL() << what << ": parsed " << parsed << " of " << total;
}

}  // namespace

TEST(AssetsSmokeTest, EveryThemeLoads) {
    size_t total = 0;
    size_t parsed = 0;
    std::vector<SmokeFailure> failures;

    for (const std::string_view sub : {"dark", "light"}) {
        const fs::path dir = fs::path(EMBER_THEMES_DIR) / sub;
        for (const fs::path& file : jsonFilesUnder(dir)) {
            ++total;
            const ide::syntax::json::ParseResult result = parse(slurp(file));
            if (!result.ok) {
                failures.push_back({file.filename().string(), "json syntax"});
                continue;
            }
            std::string error;
            auto theme = ide::theme::Theme::fromJson(result.root, &error);
            if (!theme.has_value()) {
                failures.push_back({file.filename().string(), error.empty() ? "theme load" : error});
                continue;
            }
            if (theme->paletteSize() == 0) {
                failures.push_back({file.filename().string(), "empty palette"});
                continue;
            }
            ++parsed;
        }
    }

    // One summary line, no filename lists (see the header comment).
    std::printf("themes smoke: %zu of %zu parsed\n", parsed, total);

    ASSERT_GT(total, 0u) << "theme asset directory not found";
    if (parsed != total) {
        reportFailures(failures, "themes", total, parsed);
    }
}

TEST(AssetsSmokeTest, EveryGrammarLoads) {
    ide::syntax::GrammarRegistry registry;
    size_t total = 0;
    size_t parsed = 0;
    std::vector<SmokeFailure> failures;

    for (const fs::path& file : jsonFilesUnder(EMBER_GRAMMARS_DIR)) {
        ++total;
        const ide::syntax::json::ParseResult result = parse(slurp(file));
        if (!result.ok) {
            failures.push_back({file.filename().string(), "json syntax"});
            continue;
        }
        std::string error;
        const ide::syntax::GrammarId id = registry.addGrammar(result.root, &error);
        if (id == ide::syntax::kInvalidGrammarId) {
            failures.push_back({file.filename().string(), error.empty() ? "grammar load" : error});
            continue;
        }
        if (registry.grammarById(id)->scopeName().empty()) {
            failures.push_back({file.filename().string(), "no scopeName"});
            continue;
        }
        ++parsed;
    }

    std::printf("grammars smoke: %zu of %zu parsed\n", parsed, total);

    ASSERT_GT(total, 0u) << "grammar asset directory not found";
    EXPECT_EQ(registry.grammarCount(), parsed);
    if (parsed != total) {
        reportFailures(failures, "grammars", total, parsed);
    }
}
