#pragma once

// Why this file exists
// --------------------
// Single selection point for the regex backend. Callers (CLI today, the
// Android host later) must never construct a concrete engine themselves:
// they ask here and get the best available one -- PCRE2 when the build has
// COPE_USE_PCRE2=ON, std::regex otherwise. This keeps the "never a hard
// dependency on one engine" rule enforceable by construction.

#include <memory>
#include <string_view>

#include <ide/syntax/regex.h>

namespace ide::syntax {

/// Which backend to build. kDefault = best available (PCRE2 when built in,
/// std::regex otherwise). The named entries exist for differential testing:
/// tooling (cope_cli difftest, quality --engine) forces a specific engine so
/// both backends can be run over the same inputs and their outputs diffed.
enum class RegexBackend { kDefault, kStd, kPcre2 };

/// Returns a fresh engine: Pcre2RegexEngine when built with COPE_HAS_PCRE2,
/// StdRegexEngine otherwise. Never returns nullptr (allocation failure
/// terminates; there is no engine to fall back to below std::regex).
/// Each returned engine owns its own pattern cache.
[[nodiscard]] std::unique_ptr<IRegexEngine> makeRegexEngine() noexcept;

/// Named backend variant. Returns nullptr (never throws, never falls back)
/// when the requested backend is not compiled in: asking for kPcre2 in a
/// build without COPE_HAS_PCRE2 is an error the caller must report, because
/// silently substituting std::regex would defeat differential testing.
[[nodiscard]] std::unique_ptr<IRegexEngine> makeRegexEngine(RegexBackend backend) noexcept;

}  // namespace ide::syntax
