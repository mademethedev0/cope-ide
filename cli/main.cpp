// cope_cli -- development harness for the text engine.
//
// This is NOT shipped and NOT part of core/: it is allowed to touch the real
// filesystem precisely so that core/ never has to. It exists to prove
// round-tripping, to measure the piece table on real files, and - since phase
// 3b - to render syntax highlighting in the terminal, which is the first
// visible payoff of the whole engine.
//
//   cope_cli cat   <file>   print the file back through Document, line by line
//   cope_cli lines <file>   print line count and byte size
//   cope_cli bench <file>   time open, then 10000 inserts and 10000 erases
//   cope_cli hl <file> [--theme N] [--grammar X] [--lines N] [--no-color]
//                            print the file highlighted with 24-bit ANSI color
//   cope_cli scopes <file> [--tier grammar|fallback] [--engine std|pcre2]
//                            print each token's line:range and scope stack
//   cope_cli quality <file|--all> [--engine std|pcre2] [--theme N]
//                            print the highlight quality/coverage metric
//   cope_cli difftest <file|--all>   differential run: std::regex vs PCRE2
//                            on the same input, diffing token streams (slice 3)
//   cope_cli grammars       list discovered grammar scope names
//   cope_cli themes         list discovered theme names
//   cope_cli help           full command and flag reference
//   cope_cli version        build facts: version, regex backends, asset dirs
//
// The old commands read via std::ifstream (kept as-is); every new command goes
// through ide::host::PosixHost, which is the point of phase 3b: proving the
// injected-Host boundary against real files.

#include <posix_host.h>

#include <ide/highlight/highlighter.h>
#include <ide/highlight/quality.h>
#include <ide/syntax/grammar.h>
#include <ide/syntax/json_lite.h>
#include <ide/syntax/regex_factory.h>
#include <ide/syntax/std_regex_engine.h>
#ifdef COPE_HAS_PCRE2
#include <ide/syntax/regex_pcre2.h>
#endif
#include <ide/text/document.h>
#include <ide/theme/theme.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
  const Clock::duration elapsed = Clock::now() - start;
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
         1.0e6;
}

// ---------------------------------------------------------------------------
// Terminal styling. One detection point per stream: color is on only when
// that stream is a tty, NO_COLOR is unset, and TERM is neither empty nor
// "dumb". Every helper returns "" when off, so piped output and CI logs see
// exactly the plain text they always saw.
// ---------------------------------------------------------------------------

struct CliStyle {
  bool on = false;

  explicit CliStyle(int fd) {
    const char* term = std::getenv("TERM");
    if (::isatty(fd) == 1 && std::getenv("NO_COLOR") == nullptr && term != nullptr &&
        *term != '\0' && std::string_view(term) != "dumb") {
      on = true;
    }
  }

  const char* dim() const { return on ? "\033[2m" : ""; }
  const char* bold() const { return on ? "\033[1m" : ""; }
  const char* red() const { return on ? "\033[31m" : ""; }
  const char* green() const { return on ? "\033[32m" : ""; }
  const char* yellow() const { return on ? "\033[33m" : ""; }
  const char* reset() const { return on ? "\033[0m" : ""; }
};

const CliStyle outStyle(STDOUT_FILENO);
const CliStyle errStyle(STDERR_FILENO);

/// All error output goes through here: red prefix on a tty, plain otherwise.
/// The message always states the real reason and the real action.
void cliError(const std::string& message) {
  std::fprintf(stderr, "%s%s%s %s\n", errStyle.red(), "cope_cli:", errStyle.reset(),
               message.c_str());
}

bool readWholeFile(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff end = in.tellg();
  if (end < 0) {
    return false;
  }
  out.assign(static_cast<size_t>(end), '\0');
  in.seekg(0, std::ios::beg);
  if (!out.empty()) {
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    if (in.gcount() != static_cast<std::streamsize>(out.size())) {
      return false;
    }
  }
  return true;
}

/// Full command reference. `help` prints this to stdout and exits 0; error
/// paths print the one-line hint from usage() to stderr and exit 2.
void printHelp(std::FILE* out) {
  std::fputs(
      "usage: cope_cli <command> [args]\n"
      "\n"
      "buffer:\n"
      "  cat <file>              round-trip the file through Document, line by line\n"
      "  lines <file>            print line count and byte size\n"
      "  bench <file>            time open + 10000 inserts + 10000 erases\n"
      "\n"
      "highlighting:\n"
      "  hl <file>               print the file with 24-bit ANSI highlighting\n"
      "  scopes <file>           print line:range + scope stack for every token\n"
      "  quality <file|--all>    print the highlight quality metric\n"
      "  difftest <file|--all>   diff std::regex vs PCRE2 token streams (PCRE2 build)\n"
      "\n"
      "assets:\n"
      "  grammars                list discovered grammar scope names\n"
      "  themes                  list discovered theme names\n"
      "\n"
      "  help                    this text\n"
      "  version                 version, regex backends, asset directories\n"
      "\n"
      "`cope_cli <file>` is shorthand for `cope_cli hl <file>`.\n"
      "\n"
      "flags:\n"
      "  --theme N               hl/quality: theme index (see `cope_cli themes`)\n"
      "  --grammar X             hl: force grammar by scope substring\n"
      "  --lines N               hl: stop after N lines\n"
      "  --no-color              hl: disable ANSI styling\n"
      "  --engine std|pcre2      hl/scopes/quality: force the regex backend\n"
      "  --tier grammar|fallback scopes: inspect one tier, ignoring the probe\n",
      out);
}

int usage() {
  std::fputs("usage: cope_cli <command> [args] -- run 'cope_cli help' for details\n", stderr);
  return 2;
}

// ---------------------------------------------------------------------------
// Asset loading (themes + grammars), all through PosixHost.
//
// Asset roots are compile definitions by default, so a moved asset tree is a
// build error rather than a silent empty listing. Env overrides
// (COPE_THEMES_DIR / COPE_GRAMMARS_DIR) win at runtime, so a binary can
// point at any checkout (e.g. a phone-side copy) without rebuilding.
// ---------------------------------------------------------------------------

std::string themesDir() {
  if (const char* env = std::getenv("COPE_THEMES_DIR")) {
    return std::string(env);
  }
  return std::string(COPE_THEMES_DIR);
}

std::string grammarsDir() {
  if (const char* env = std::getenv("COPE_GRAMMARS_DIR")) {
    return std::string(env);
  }
  return std::string(COPE_GRAMMARS_DIR);
}

/// One loaded theme plus the file it came from, for sorted listings.
struct ThemeEntry {
  ide::theme::Theme theme;
  std::string fileName;
};

/// Sorted list of *.json paths directly under `dir`; empty when unreadable.
std::vector<std::string> listJsonFiles(const std::string& dir, ide::host::Host& host) {
  std::vector<std::string> files;
  for (const std::string& name : host.readDir(dir)) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
      files.push_back(dir + "/" + name);
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

/// Loads every theme in dark/ then light/, sorted by theme name. Themes that
/// fail to parse are skipped silently here; `themes` / `hl` report counts.
std::vector<ThemeEntry> loadAllThemes(ide::host::Host& host) {
  std::vector<ThemeEntry> themes;
  for (const std::string_view sub : {std::string_view("/dark"), std::string_view("/light")}) {
    for (const std::string& path : listJsonFiles(themesDir() + std::string(sub), host)) {
      const auto text = host.readFile(path);
      if (!text.has_value()) {
        continue;
      }
      const auto parsed = ide::syntax::json::parse(*text);
      if (!parsed.ok) {
        continue;
      }
      auto theme = ide::theme::Theme::fromJson(parsed.root, nullptr);
      if (!theme.has_value()) {
        continue;
      }
      const size_t slash = path.find_last_of('/');
      themes.push_back(ThemeEntry{std::move(*theme),
                                  slash == std::string::npos ? path : path.substr(slash + 1)});
    }
  }
  std::sort(themes.begin(), themes.end(), [](const ThemeEntry& a, const ThemeEntry& b) {
    return a.theme.name() < b.theme.name();
  });
  return themes;
}

/// Parses every grammar file into `registry`. Returns how many loaded.
/// Loading is eager and total: a broken file costs one entry, never the run.
size_t loadAllGrammars(ide::host::Host& host, ide::syntax::GrammarRegistry& registry) {
  size_t loaded = 0;
  for (const std::string& path : listJsonFiles(grammarsDir(), host)) {
    const auto text = host.readFile(path);
    if (!text.has_value()) {
      continue;
    }
    const auto parsed = ide::syntax::json::parse(*text);
    if (!parsed.ok) {
      continue;
    }
    if (registry.addGrammar(parsed.root, nullptr) != ide::syntax::kInvalidGrammarId) {
      ++loaded;
    }
  }
  return loaded;
}

/// Lowercase extension of a path ("" when there is none). Registry matching is
/// case sensitive and grammar fileTypes are lowercase, so lowering here makes
/// "Foo.CPP" resolve the same as "foo.cpp".
std::string extensionOf(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 == path.size()) {
    return std::string();
  }
  std::string ext(path.substr(dot + 1));
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return ext;
}

/// Canonical extension overrides for grammars that ship without fileTypes —
/// the tmLanguage idiom where the editor, not the grammar, owns the mapping
/// (javascript.json and typescript.json both do this). Applied only when the
/// target scope actually loaded, so a trimmed asset dir degrades to tier 2
/// instead of pointing at a grammar that does not exist.
void applyCanonicalExtensionMap(ide::syntax::GrammarRegistry& registry) {
  const std::pair<const char*, const char*> canonical[] = {
      {"js", "source.js"},    {"mjs", "source.js"},   {"cjs", "source.js"},
      {"jsx", "source.js"},   {"ts", "source.ts"},    {"tsx", "source.ts"},
      {"php", "source.php"},  {"html", "text.html.basic"}, {"htm", "text.html.basic"},
      {"css", "source.css"},  {"scss", "source.css"}, {"less", "source.css"},
      {"py", "source.python"}, {"pyi", "source.python"},
      {"json", "source.json"}, {"c", "source.c"},      {"h", "source.c"},
      {"cpp", "source.cpp"},  {"cc", "source.cpp"},   {"hpp", "source.cpp"},
      {"cxx", "source.cpp"},  {"hxx", "source.cpp"},  {"ino", "source.cpp"},
      {"md", "text.html.markdown"}, {"markdown", "text.html.markdown"},
  };
  for (const auto& [extension, scopeName] : canonical) {
    if (registry.idForScope(scopeName) != ide::syntax::kInvalidGrammarId) {
      registry.mapExtension(extension, scopeName);
    }
  }
}

/// Resolves an --engine name to a backend. nullptr when the name is unknown
/// or names an engine this build does not contain.
std::unique_ptr<ide::syntax::IRegexEngine> makeNamedEngine(const std::string& name) {
  if (name == "std") {
    return ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kStd);
  }
  if (name == "pcre2") {
    return ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kPcre2);
  }
  return nullptr;
}

/// The engine context shared by hl / scopes / quality. One regex engine and one
/// registry per process: pattern compiles and grammars are cached, so
/// `quality --all` pays for each grammar once, not 244 times.
struct EngineContext {
  ide::host::PosixHost host;
  std::unique_ptr<ide::syntax::IRegexEngine> regexEngine;
  ide::syntax::GrammarRegistry registry;
  std::vector<ThemeEntry> themes;

  /// `engineName` empty = the default backend (PCRE2 when built in); otherwise
  /// a forced backend for differential measurement.
  explicit EngineContext(const std::string& engineName = std::string())
      : regexEngine(engineName.empty() ? ide::syntax::makeRegexEngine()
                                       : makeNamedEngine(engineName)) {}
  bool setup() {
    loadAllGrammars(host, registry);
    applyCanonicalExtensionMap(registry);
    themes = loadAllThemes(host);
    return !themes.empty() && registry.grammarCount() > 0;
  }

  /// First dark theme in the sorted listing, or the first theme overall.
  const ide::theme::Theme& defaultTheme() const {
    for (const ThemeEntry& entry : themes) {
      if (entry.theme.isDark()) {
        return entry.theme;
      }
    }
    return themes.front().theme;
  }
};

/// Prints the real reason a requested engine is unusable. Every command that
/// takes --engine calls this before touching the context.
bool engineAvailable(const EngineContext& context, const std::string& engineName) {
  if (context.regexEngine != nullptr) {
    return true;
  }
  if (engineName != "std" && engineName != "pcre2") {
    cliError("unknown engine '" + engineName + "' (std|pcre2)");
  } else {
    std::string message = "engine '" + engineName + "' is not in this build";
#ifndef COPE_HAS_PCRE2
    message += " (configure with -DCOPE_USE_PCRE2=ON)";
#endif
    cliError(message);
  }
  return false;
}

/// Reads `path` through the host and splits it into Document lines.
/// Content lines exclude their terminators: [start, end) of lineAt().
/// (rawEnd() includes the terminator — using it here once caused the CLI to
/// print each '\n' inside an active SGR run, bleeding the background color
/// across the terminal and double-spacing every line.)
bool loadLines(ide::host::Host& host, const std::string& path,
               std::vector<std::string>& lines, size_t& byteSize) {
  const auto bytes = host.readFile(path);
  if (!bytes.has_value()) {
    return false;
  }
  byteSize = bytes->size();
  const ide::text::Document document(*bytes);
  lines.reserve(static_cast<size_t>(document.lineCount()));
  std::string line;
  for (int64_t i = 0; i < document.lineCount(); ++i) {
    // Document::lineContent is private; copy the raw range instead.
    const ide::text::LineRange range = document.lineAt(i);
    line.assign(range.end - range.start, '\0');
    if (!line.empty()) {
      document.copyOut(range.start, std::span<char>(line.data(), line.size()));
    }
    lines.push_back(line);
  }
  return true;
}

/// One SGR escape for a style, or "" when the style paints nothing. Bold /
/// italic / underline bits come from the theme's fontStyle; fg and bg are
/// emitted as 24-bit truecolor params.
std::string sgrFor(const ide::theme::Style& style) {
  std::string out = "\033[";
  bool any = false;
  const auto add = [&out, &any](std::string_view param) {
    if (any) {
      out += ';';
    }
    out += param;
    any = true;
  };
  if (ide::theme::hasFontStyle(style.fontStyle, ide::theme::FontStyle::kBold)) {
    add("1");
  }
  if (ide::theme::hasFontStyle(style.fontStyle, ide::theme::FontStyle::kItalic)) {
    add("3");
  }
  if (ide::theme::hasFontStyle(style.fontStyle, ide::theme::FontStyle::kUnderline)) {
    add("4");
  }
  if (style.hasFg) {
    add("38");
    add("2");
    add(std::to_string(style.fg.r));
    add(std::to_string(style.fg.g));
    add(std::to_string(style.fg.b));
  }
  if (style.hasBg) {
    add("48");
    add("2");
    add(std::to_string(style.bg.r));
    add(std::to_string(style.bg.g));
    add(std::to_string(style.bg.b));
  }
  if (!any) {
    return std::string();
  }
  out += 'm';
  return out;
}

/// Constructs (in place - Highlighter is neither copyable nor movable, which
/// is why the out-param is an optional) a Highlighter for `path` over
/// `context`, optionally overriding the grammar by scope substring. Returns
/// false (error already printed) when the override matched nothing.
bool makeHighlighter(EngineContext& context, ide::syntax::IRegexEngine& regexEngine,
                     const std::string& path,
                     const std::string& grammarOverride, const ide::theme::Theme* themeOverride,
                     const std::vector<std::string>& lines, size_t byteSize,
                     std::optional<ide::highlight::Highlighter>& out) {
  if (context.themes.empty()) {
    cliError("no themes found");
    return false;
  }
  if (!grammarOverride.empty()) {
    const ide::syntax::Grammar* match = nullptr;
    for (ide::syntax::GrammarId id = 0;
         static_cast<size_t>(id) < context.registry.grammarCount(); ++id) {
      const ide::syntax::Grammar* grammar = context.registry.grammarById(id);
      if (grammar == nullptr) {
        continue;
      }
      if (grammar->scopeName().find(grammarOverride) != std::string::npos) {
        match = grammar;
        break;
      }
    }
    if (match == nullptr) {
      cliError("no grammar scope matches '" + grammarOverride + "'");
      return false;
    }
    // Route the file's extension at the chosen grammar.
    context.registry.mapExtension(extensionOf(path), match->scopeName());
  }

  // The theme reference must outlive the highlighter: it always points into
  // context.themes, which the caller keeps alive longer.
  const ide::theme::Theme& theme = themeOverride != nullptr ? *themeOverride : context.defaultTheme();

  ide::highlight::FileInfo info;
  info.name = path;
  info.byteSize = byteSize;
  info.lineCount = lines.size();

  out.emplace(context.registry, regexEngine, theme, info);
  std::vector<std::string_view> views(lines.begin(), lines.end());
  out->probe(views);
  return true;
}

// ---------------------------------------------------------------------------
// Old commands (unchanged).
// ---------------------------------------------------------------------------

int commandCat(const std::string& path) {
  std::string bytes;
  if (!readWholeFile(path, bytes)) {
    cliError("cannot read " + path);
    return 1;
  }
  const ide::text::Document document(std::move(bytes));

  // Line ranges exclude the terminator, so print [start, rawEnd) to reproduce
  // the original bytes exactly -- including CRLF and a missing final newline.
  const int64_t lines = document.lineCount();
  std::string buffer;
  for (int64_t line = 0; line < lines; ++line) {
    const ide::text::LineRange range = document.lineAt(line);
    const size_t length = range.rawEnd() - range.start;
    if (length == 0) {
      continue;
    }
    buffer.assign(length, '\0');
    document.copyOut(range.start, std::span<char>(buffer.data(), buffer.size()));
    std::cout.write(buffer.data(), static_cast<std::streamsize>(length));
  }
  std::cout.flush();
  return 0;
}

int commandLines(const std::string& path) {
  std::string bytes;
  if (!readWholeFile(path, bytes)) {
    cliError("cannot read " + path);
    return 1;
  }
  const ide::text::Document document(std::move(bytes));
  std::printf("%slines%s %lld\n", outStyle.dim(), outStyle.reset(),
              static_cast<long long>(document.lineCount()));
  std::printf("%sbytes%s %llu\n", outStyle.dim(), outStyle.reset(),
              static_cast<unsigned long long>(document.size()));
  return 0;
}

int commandBench(const std::string& path) {
  constexpr int kOps = 10000;

  const Clock::time_point readStart = Clock::now();
  std::string bytes;
  if (!readWholeFile(path, bytes)) {
    cliError("cannot read " + path);
    return 1;
  }
  const double readMs = millisSince(readStart);
  const size_t fileBytes = bytes.size();

  const Clock::time_point openStart = Clock::now();
  ide::text::Document document(std::move(bytes));
  const double openMs = millisSince(openStart);

  // Fixed seed so runs are comparable. Timestamps are pinned and coalescing is
  // off so the numbers describe the buffer, not the clock or the undo tree.
  std::mt19937_64 rng(0xC0FFEEULL);
  ide::text::EditOptions options;
  options.timestampMs = 0;
  options.coalesce = false;

  const Clock::time_point insertStart = Clock::now();
  for (int i = 0; i < kOps; ++i) {
    const size_t offset = static_cast<size_t>(rng() % (document.size() + 1));
    document.insert(offset, "x", options);
  }
  const double insertMs = millisSince(insertStart);

  const Clock::time_point eraseStart = Clock::now();
  int erased = 0;
  for (int i = 0; i < kOps; ++i) {
    if (document.size() == 0) {
      break;
    }
    const size_t offset = static_cast<size_t>(rng() % document.size());
    document.erase(offset, 1, options);
    ++erased;
  }
  const double eraseMs = millisSince(eraseStart);

  const double insertPerOp = insertMs * 1000.0 / static_cast<double>(kOps);
  const double erasePerOp =
      erased > 0 ? eraseMs * 1000.0 / static_cast<double>(erased) : 0.0;

  const char* const dim = outStyle.dim();
  const char* const reset = outStyle.reset();
  std::printf("%s%-13s%s %s\n", dim, "file", reset, path.c_str());
  std::printf("%s%-13s%s %llu\n", dim, "bytes", reset,
              static_cast<unsigned long long>(fileBytes));
  std::printf("%s%-13s%s %lld\n", dim, "lines", reset,
              static_cast<long long>(document.lineCount()));
  std::printf("%s%-13s%s %.3f ms %s(ifstream, not part of the engine)%s\n", dim, "read", reset,
              readMs, dim, reset);
  std::printf("%s%-13s%s %.3f ms %s(Document construction)%s\n", dim, "open", reset, openMs, dim,
              reset);
  std::printf("%s%-13s%s %d ops, %.3f ms total, %.3f us/op\n", dim, "inserts", reset, kOps,
              insertMs, insertPerOp);
  std::printf("%s%-13s%s %d ops, %.3f ms total, %.3f us/op\n", dim, "erases", reset, erased,
              eraseMs, erasePerOp);
  std::printf("%s%-13s%s %llu\n", dim, "final bytes", reset,
              static_cast<unsigned long long>(document.size()));
  std::printf("%s%-13s%s %llu\n", dim, "pieces", reset,
              static_cast<unsigned long long>(document.pieces().pieceCount()));
  std::printf("%s%-13s%s %lld\n", dim, "tree height", reset,
              static_cast<long long>(document.pieces().treeHeight()));
  std::printf("%s%-13s%s %llu bytes\n", dim, "add buffer", reset,
              static_cast<unsigned long long>(document.pieces().addBufferSize()));
  std::printf("%s%-13s%s %lld\n", dim, "version", reset,
              static_cast<long long>(document.version()));
  return 0;
}

// ---------------------------------------------------------------------------
// New commands.
// ---------------------------------------------------------------------------

int commandHl(int argc, char** argv) {
  std::string path;
  std::string grammarOverride;
  std::string engineName;
  int themeIndex = -1;
  long lineLimit = -1;  // negative = all lines
  bool noColorFlag = false;

  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--no-color") {
      noColorFlag = true;
    } else if (arg == "--theme" && i + 1 < argc) {
      themeIndex = std::atoi(argv[++i]);
    } else if (arg == "--grammar" && i + 1 < argc) {
      grammarOverride = argv[++i];
    } else if (arg == "--engine" && i + 1 < argc) {
      engineName = argv[++i];
    } else if (arg == "--lines" && i + 1 < argc) {
      lineLimit = std::atol(argv[++i]);
    } else if (!arg.empty() && arg[0] != '-' && path.empty()) {
      path = std::string(arg);
    } else {
      return usage();
    }
  }
  if (path.empty()) {
    return usage();
  }

  // Color is opt-out three ways: the flag, NO_COLOR, or a non-tty stdout.
  const bool useColor =
      !noColorFlag && std::getenv("NO_COLOR") == nullptr && ::isatty(STDOUT_FILENO) == 1;

  EngineContext context(engineName);
  if (!engineAvailable(context, engineName)) {
    return 1;
  }
  context.setup();

  const ide::theme::Theme* theme = nullptr;
  if (themeIndex >= 0) {
    if (static_cast<size_t>(themeIndex) >= context.themes.size()) {
      cliError("theme index " + std::to_string(themeIndex) + " out of range (0.." +
               std::to_string(context.themes.empty() ? 0u : context.themes.size() - 1u) + ")");
      return 1;
    }
    theme = &context.themes[static_cast<size_t>(themeIndex)].theme;
  } else if (context.themes.empty()) {
    cliError("no themes found");
    return 1;
  }

  std::vector<std::string> lines;
  size_t byteSize = 0;
  if (!loadLines(context.host, path, lines, byteSize)) {
    cliError("cannot read " + path);
    return 1;
  }

  const size_t limit = lineLimit < 0
                           ? lines.size()
                           : std::min(static_cast<size_t>(lineLimit), lines.size());

  // A file ending in '\n' has a phantom empty last line in lineCount(); drop
  // it so the output ends like `cat`, without a stray blank line.
  size_t printLimit = limit;
  if (printLimit == lines.size() && !lines.empty() && lines.back().empty()) {
    --printLimit;
  }

  std::optional<ide::highlight::Highlighter> highlighter;
  if (!makeHighlighter(context, *context.regexEngine, path, grammarOverride, theme, lines,
                        byteSize, highlighter)) {
    return 1;
  }

  ide::highlight::LineState state = highlighter->initialState();
  std::vector<ide::highlight::StyledSpan> spans;
  for (size_t i = 0; i < printLimit; ++i) {
    const std::string& line = lines[i];
    highlighter->highlightLine(line, state, spans);
    bool styled = false;
    for (const ide::highlight::StyledSpan& span : spans) {
      if (useColor && span.style != ide::highlight::Highlighter::defaultStyleId()) {
        // Copy the Style: styleAt() returns a reference that a later resolve
        // inside the next highlightLine call could invalidate.
        const std::string sgr = sgrFor(highlighter->theme().styleAt(span.style));
        if (!sgr.empty()) {
          std::cout << sgr;
          styled = true;
        }
      }
      std::cout.write(line.data() + span.begin,
                      static_cast<std::streamsize>(span.end - span.begin));
    }
    if (styled) {
      std::cout << "\033[0m";
    }
    std::cout << '\n';
  }
  // Tty-only trailing summary: how much was printed and under what tier and
  // theme, so a pretty render also states its own facts.
  if (useColor) {
    const ide::theme::Theme& summaryTheme = theme != nullptr ? *theme : context.defaultTheme();
    std::cout << outStyle.dim() << "-- " << printLimit << " line"
              << (printLimit == 1 ? "" : "s") << " \u00b7 tier "
              << ide::highlight::tierName(highlighter->tier()) << " \u00b7 theme "
              << summaryTheme.name() << outStyle.reset() << '\n';
  }
  std::cout.flush();
  return 0;
}

int commandScopes(int argc, char** argv) {
  std::string path;
  std::string tierName;
  std::string engineName;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--tier" && i + 1 < argc) {
      tierName = argv[++i];
    } else if (arg == "--engine" && i + 1 < argc) {
      engineName = argv[++i];
    } else if (!arg.empty() && arg[0] != '-' && path.empty()) {
      path = std::string(arg);
    } else {
      return usage();
    }
  }
  if (path.empty()) {
    return usage();
  }

  EngineContext context(engineName);
  if (!engineAvailable(context, engineName)) {
    return 1;
  }
  context.setup();

  std::vector<std::string> lines;
  size_t byteSize = 0;
  if (!loadLines(context.host, path, lines, byteSize)) {
    cliError("cannot read " + path);
    return 1;
  }

  std::optional<ide::highlight::Highlighter> highlighter;
  if (!makeHighlighter(context, *context.regexEngine, path, std::string(), nullptr, lines,
                        byteSize, highlighter)) {
    return 1;
  }
  // --tier grammar: ignore the probe's demotion decision and inspect tier 1
  // directly. This is the debugging path for "why did this file demote".
  if (tierName == "grammar") {
    highlighter->forceTier(ide::highlight::Tier::kGrammar);
  } else if (tierName == "fallback") {
    highlighter->forceTier(ide::highlight::Tier::kFallback);
  } else if (!tierName.empty()) {
    cliError("unknown tier '" + tierName + "' (grammar|fallback)");
    return 1;
  }

  // One line per token: "line:start-end" then the scope stack, outermost
  // first. Ranges are byte offsets into the line (0-based).
  ide::highlight::LineState state = highlighter->initialState();
  std::vector<ide::highlight::ScopedSpan> scoped;
  std::vector<std::string_view> stack;
  for (size_t i = 0; i < lines.size(); ++i) {
    highlighter->scopeLine(lines[i], state, scoped);
    for (const ide::highlight::ScopedSpan& span : scoped) {
      std::printf("%s%zu:%zu-%zu%s ", outStyle.dim(), i, span.begin, span.end,
                  outStyle.reset());
      stack.clear();
      highlighter->scopeTable().resolve(span.scopes, stack);
      for (size_t s = 0; s < stack.size(); ++s) {
        if (s > 0) {
          std::putchar(' ');
        }
        std::fwrite(stack[s].data(), 1, stack[s].size(), stdout);
      }
      std::putchar('\n');
    }
  }
  return 0;
}

/// Highlights one file and prints its quality report line. Shared by
/// `quality <file>` and `quality --all`. When `suspicious` is non-null it
/// receives the report's verdict for the aggregate summary.
int qualityForFile(EngineContext& context, const std::string& path, bool* suspicious,
                    const ide::theme::Theme* themeOverride) {
  std::vector<std::string> lines;
  size_t byteSize = 0;
  if (!loadLines(context.host, path, lines, byteSize)) {
    cliError("cannot read " + path);
    return 1;
  }

  std::optional<ide::highlight::Highlighter> highlighter;
  if (!makeHighlighter(context, *context.regexEngine, path, std::string(), themeOverride, lines,
                        byteSize, highlighter)) {
    return 1;
  }

  const ide::highlight::QualityReport report = ide::highlight::analyzeDocument(*highlighter, lines);
  if (suspicious != nullptr) {
    *suspicious = report.suspicious();
  }
  // Same text as formatQualityReport with the verdict colored (green OK,
  // yellow SUSPECT). The wrappers are empty strings off-tty, so piped output
  // and CI logs are byte-identical to the uncolored form.
  const std::string text = ide::highlight::formatQualityReport(report);
  const size_t verdict = text.rfind(' ');
  std::cout << outStyle.bold() << path << outStyle.reset() << ": " << text.substr(0, verdict)
            << ' ' << (report.suspicious() ? outStyle.yellow() : outStyle.green())
            << text.substr(verdict + 1) << outStyle.reset() << '\n';
  return 0;
}

int commandQuality(int argc, char** argv) {
  std::string what;
  std::string engineName;
  int themeIndex = -1;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--engine" && i + 1 < argc) {
      engineName = argv[++i];
    } else if (arg == "--theme" && i + 1 < argc) {
      themeIndex = std::atoi(argv[++i]);
    } else if (arg == "--all" && what.empty()) {
      what = "--all";
    } else if (!arg.empty() && arg[0] != '-' && what.empty()) {
      what = std::string(arg);
    } else {
      return usage();
    }
  }
  if (what.empty()) {
    return usage();
  }

  EngineContext context(engineName);
  if (!engineAvailable(context, engineName)) {
    return 1;
  }
  if (!context.setup()) {
    cliError("no themes or grammars found");
    return 1;
  }

  const ide::theme::Theme* theme = nullptr;
  if (themeIndex >= 0) {
    if (static_cast<size_t>(themeIndex) >= context.themes.size()) {
      cliError("theme index " + std::to_string(themeIndex) + " out of range (0.." +
               std::to_string(context.themes.empty() ? 0u : context.themes.size() - 1u) + ")");
      return 1;
    }
    theme = &context.themes[static_cast<size_t>(themeIndex)].theme;
  }

  if (what == "--all") {
    int failures = 0;
    size_t files = 0;
    size_t suspiciousCount = 0;
    for (const std::string& path : listJsonFiles(grammarsDir(), context.host)) {
      bool suspicious = false;
      if (qualityForFile(context, path, &suspicious, theme) != 0) {
        ++failures;
        continue;
      }
      ++files;
      if (suspicious) {
        ++suspiciousCount;
      }
    }
    std::printf("%s%zu files, %zu suspicious%s\n", outStyle.dim(), files, suspiciousCount,
                outStyle.reset());
    return failures == 0 ? 0 : 1;
  }
  bool suspicious = false;
  return qualityForFile(context, std::string(what), &suspicious, theme);
}

// ---------------------------------------------------------------------------
// difftest: the phase-5 slice-3 differential gate. Runs the SAME input through
// the std::regex engine and the PCRE2 engine and compares the token streams.
// Token differences are EXPECTED and reported (they are exactly what the
// PCRE2 upgrade fixes); what fails the run is any pattern BOTH engines
// refuse -- std::regex rejecting ~15% of corpus patterns is the known state,
// and PCRE2 inheriting a refusal is a regression against the phase-5 goal.
// ---------------------------------------------------------------------------

/// Tokenizes `lines` into one string per token: "line:begin-end scope stack",
/// directly comparable across engines.
void collectScopeRecords(ide::highlight::Highlighter& highlighter,
                         const std::vector<std::string>& lines, std::vector<std::string>& out) {
  ide::highlight::LineState state = highlighter.initialState();
  std::vector<ide::highlight::ScopedSpan> scoped;
  std::vector<std::string_view> stack;
  std::string record;
  for (size_t i = 0; i < lines.size(); ++i) {
    highlighter.scopeLine(lines[i], state, scoped);
    for (const ide::highlight::ScopedSpan& span : scoped) {
      record = std::to_string(i);
      record += ':';
      record += std::to_string(span.begin);
      record += '-';
      record += std::to_string(span.end);
      record += ' ';
      stack.clear();
      highlighter.scopeTable().resolve(span.scopes, stack);
      for (size_t s = 0; s < stack.size(); ++s) {
        if (s > 0) {
          record += ' ';
        }
        record.append(stack[s]);
      }
      out.push_back(record);
    }
  }
}

int commandDifftest(int argc, char** argv) {
  if (argc != 3) {
    return usage();
  }
#ifndef COPE_HAS_PCRE2
  cliError("difftest needs the PCRE2 backend; configure with -DCOPE_USE_PCRE2=ON and rebuild");
  return 2;
#else
  const std::string what = argv[2];
  EngineContext context;
  if (!context.setup()) {
    cliError("no themes or grammars found");
    return 1;
  }

  // Two independent engines over the one shared registry. Each Highlighter
  // owns its tokenizer and pattern cache, so nothing leaks across engines.
  auto stdEngine = ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kStd);
  auto pcre2Engine = ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kPcre2);

  std::vector<std::string> paths;
  if (what == "--all") {
    paths = listJsonFiles(grammarsDir(), context.host);
  } else {
    paths.push_back(what);
  }

  size_t files = 0, filesDiffering = 0, tokensStd = 0, tokensPcre2 = 0, differingTokens = 0;
  for (const std::string& path : paths) {
    std::vector<std::string> lines;
    size_t byteSize = 0;
    if (!loadLines(context.host, path, lines, byteSize)) {
      cliError("cannot read " + path);
      return 1;
    }

    // The point of --all is to run every grammar's OWN patterns through both
    // engines, not to tokenize 244 JSON files with source.json. Each grammar
    // file is therefore fed to its own grammar: route the file's extension at
    // the grammar its JSON declares. A grammar that failed to load falls back
    // to tier 2 identically under both engines, which the diff still covers.
    std::string scopeLabel;
    if (what == "--all") {
      // Reset first: a file whose scopeName is missing must not inherit the
      // previous iteration's mapping.
      context.registry.mapExtension("json", "source.json");
      const auto text = context.host.readFile(path);
      if (text.has_value()) {
        const auto parsed = ide::syntax::json::parse(*text);
        const auto* scopeName = parsed.ok ? parsed.root.find("scopeName") : nullptr;
        if (scopeName != nullptr && scopeName->isString()) {
          scopeLabel = scopeName->string();
          context.registry.mapExtension("json", scopeLabel);
        }
      }
    }

    std::optional<ide::highlight::Highlighter> stdHighlighter, pcre2Highlighter;
    if (!makeHighlighter(context, *stdEngine, path, std::string(), nullptr, lines, byteSize,
                         stdHighlighter) ||
        !makeHighlighter(context, *pcre2Engine, path, std::string(), nullptr, lines, byteSize,
                         pcre2Highlighter)) {
      return 1;
    }

    std::vector<std::string> stdRecords, pcre2Records;
    collectScopeRecords(*stdHighlighter, lines, stdRecords);
    collectScopeRecords(*pcre2Highlighter, lines, pcre2Records);
    tokensStd += stdRecords.size();
    tokensPcre2 += pcre2Records.size();
    size_t fileDiff = 0;
    const size_t count = std::max(stdRecords.size(), pcre2Records.size());
    for (size_t j = 0; j < count; ++j) {
      if (j >= stdRecords.size() || j >= pcre2Records.size() ||
          stdRecords[j] != pcre2Records[j]) {
        ++fileDiff;
      }
    }
    differingTokens += fileDiff;
    if (fileDiff > 0) {
      ++filesDiffering;
    }

    // Quality under each engine on the same lines: repaired% is the headline
    // metric of the phase-5 upgrade (the Kotlin benchmark case).
    const ide::highlight::QualityReport stdReport =
        ide::highlight::analyzeDocument(*stdHighlighter, lines);
    const ide::highlight::QualityReport pcre2Report =
        ide::highlight::analyzeDocument(*pcre2Highlighter, lines);
    std::printf(
        "%s%s%s%s: tokens std=%zu pcre2=%zu differ=%zu repaired std=%.2f%% pcre2=%.2f%% (tier %s -> "
        "%s)\n",
        path.c_str(), scopeLabel.empty() ? "" : " [", scopeLabel.c_str(),
        scopeLabel.empty() ? "" : "]", stdRecords.size(), pcre2Records.size(), fileDiff,
        stdReport.repairRatio() * 100.0, pcre2Report.repairRatio() * 100.0,
        ide::highlight::tierName(stdReport.tier).data(),
        ide::highlight::tierName(pcre2Report.tier).data());
    ++files;
  }

  const auto& stdRejections =
      static_cast<ide::syntax::StdRegexEngine*>(stdEngine.get())->rejections();
  const auto& pcre2Rejections =
      static_cast<ide::syntax::Pcre2RegexEngine*>(pcre2Engine.get())->rejections();
  size_t recovered = 0, refusedByBoth = 0, pcre2Only = 0;
  for (const auto& [pattern, reason] : stdRejections) {
    if (pcre2Rejections.find(pattern) != pcre2Rejections.end()) {
      ++refusedByBoth;
    } else {
      ++recovered;
    }
  }
  for (const auto& [pattern, reason] : pcre2Rejections) {
    if (stdRejections.find(pattern) == stdRejections.end()) {
      ++pcre2Only;
    }
  }
  const ide::syntax::RegexEngineStats stdStats = stdEngine->stats();
  const ide::syntax::RegexEngineStats pcre2Stats = pcre2Engine->stats();
  std::printf(
      "%sfiles %zu (%zu differing token streams) tokens std=%zu pcre2=%zu differing=%zu%s\n"
      "%sengine std::regex:%s compiled=%zu rejected=%zu lossy=%zu\n"
      "%sengine pcre2:%s      compiled=%zu rejected=%zu\n"
      "%srecovered%s (std refused, pcre2 compiled): %s%zu%s\n"
      "%srefused by both engines: %zu (informational: identical degradation)%s\n"
      "%srefused by pcre2 but compiled by std: %zu%s\n",
      outStyle.bold(), files, filesDiffering, tokensStd, tokensPcre2, differingTokens,
      outStyle.reset(), outStyle.dim(), outStyle.reset(), stdStats.compiled, stdStats.rejected,
      stdStats.lossy, outStyle.dim(), outStyle.reset(), pcre2Stats.compiled, pcre2Stats.rejected,
      outStyle.dim(), outStyle.reset(), outStyle.green(), recovered, outStyle.reset(),
      outStyle.dim(), refusedByBoth, outStyle.reset(), outStyle.dim(), pcre2Only, outStyle.reset());
  // Known-hard patterns both engines refuse (unbounded lookbehind,
  // codepoint escapes above U+00FF inside byte-mode classes) degrade
  // identically under both engines: informational, not a gate.
  if (refusedByBoth > 0) {
    size_t shown = 0;
    for (const auto& [pattern, reason] : stdRejections) {
      if (pcre2Rejections.find(pattern) == stdRejections.end()) {
        continue;
      }
      std::printf("%s  both refuse:%s %.120s\n", outStyle.dim(), outStyle.reset(),
                  pattern.c_str());
      if (++shown >= 20) {
        break;
      }
    }
  }
  // The actual gate: a pattern std::regex compiles that PCRE2 refuses is a
  // regression introduced by the upgrade (or its translator).
  if (pcre2Only > 0) {
    size_t shown = 0;
    for (const auto& [pattern, reason] : pcre2Rejections) {
      if (stdRejections.find(pattern) != stdRejections.end()) {
        continue;
      }
      std::printf("%s  pcre2-only refusal:%s %.120s -- %.120s\n", outStyle.red(),
                  outStyle.reset(), pattern.c_str(), reason.c_str());
      if (++shown >= 30) {
        break;
      }
    }
    cliError(std::to_string(pcre2Only) +
             " pattern(s) compiled by std::regex but refused by PCRE2");
    return 1;
  }
  return 0;
#endif
}

int commandVersion() {
  std::printf("cope_cli %s\n", COPE_VERSION);
  std::fputs("regex backends: std", stdout);
#ifdef COPE_HAS_PCRE2
  std::fputs(", pcre2", stdout);
#endif
  std::fputc('\n', stdout);
  std::printf("%sgrammars dir%s %s\n", outStyle.dim(), outStyle.reset(), grammarsDir().c_str());
  std::printf("%sthemes dir%s   %s\n", outStyle.dim(), outStyle.reset(), themesDir().c_str());
  return 0;
}

int commandList(const std::string& what) {
  EngineContext context;
  context.setup();

  if (what == "grammars") {
    for (ide::syntax::GrammarId id = 0;
         static_cast<size_t>(id) < context.registry.grammarCount(); ++id) {
      const ide::syntax::Grammar* grammar = context.registry.grammarById(id);
      if (grammar != nullptr) {
        std::printf("%s\n", grammar->scopeName().c_str());
      }
    }
    return 0;
  }

  // themes: name plus type, one per line. Sorted by name (loadAllThemes).
  for (const ThemeEntry& entry : context.themes) {
    std::printf("%s (%s%s%s)\n", std::string(entry.theme.name()).c_str(), outStyle.dim(),
                entry.theme.isDark() ? "dark" : "light", outStyle.reset());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }
  const std::string command = argv[1];

  if (command == "help" || command == "--help" || command == "-h") {
    printHelp(stdout);
    return 0;
  }
  if (command == "version" || command == "--version" || command == "-V") {
    return commandVersion();
  }

  if (command == "cat") {
    if (argc != 3) {
      return usage();
    }
    return commandCat(argv[2]);
  }
  if (command == "lines") {
    if (argc != 3) {
      return usage();
    }
    return commandLines(argv[2]);
  }
  if (command == "bench") {
    if (argc != 3) {
      return usage();
    }
    return commandBench(argv[2]);
  }
  if (command == "hl") {
    return commandHl(argc, argv);
  }
  if (command == "scopes") {
    return commandScopes(argc, argv);
  }
  if (command == "quality") {
    return commandQuality(argc, argv);
  }
  if (command == "difftest") {
    return commandDifftest(argc, argv);
  }
  if (command == "grammars" || command == "themes") {
    if (argc != 2) {
      return usage();
    }
    return commandList(command);
  }
  // Anything else starting with '-' is an unknown option, not a file.
  if (!command.empty() && command[0] == '-') {
    cliError("unknown option '" + command + "' -- run 'cope_cli help'");
    return 2;
  }
  // Convenience: `cope_cli <file> [flags...]` means `cope_cli hl <file> ...`.
  // commandHl parses from argv[2], so rebuild the argument vector with "hl"
  // inserted in front.
  static char hlCommand[] = "hl";
  std::vector<char*> hlArgv;
  hlArgv.reserve(static_cast<size_t>(argc) + 1);
  hlArgv.push_back(argv[0]);
  hlArgv.push_back(hlCommand);
  for (int i = 1; i < argc; ++i) {
    hlArgv.push_back(argv[i]);
  }
  return commandHl(static_cast<int>(hlArgv.size()), hlArgv.data());
}
