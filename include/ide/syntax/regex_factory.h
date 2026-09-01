#pragma once

// Why this file exists
// --------------------
// Single selection point for the regex backend. Callers (CLI today, the
// Android host later) must never construct a concrete engine themselves:
// they ask here and get the best available one -- PCRE2 when the build has
// COPE_USE_PCRE2=ON, std::regex otherwise. This keeps the "never a hard
// dependency on one engine" rule enforceable by construction.

#include <memory>

#include <ide/syntax/regex.h>

namespace ide::syntax {

/// Returns a fresh engine: Pcre2RegexEngine when built with COPE_HAS_PCRE2,
/// StdRegexEngine otherwise. Never returns nullptr (allocation failure
/// terminates; there is no engine to fall back to below std::regex).
/// Each returned engine owns its own pattern cache.
[[nodiscard]] std::unique_ptr<IRegexEngine> makeRegexEngine() noexcept;

}  // namespace ide::syntax
