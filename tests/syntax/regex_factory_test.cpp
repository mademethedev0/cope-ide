// Tests for the regex engine factory (phase 5 slice 3).
//
// The factory is the single selection point for the backend; these tests pin
// its contract: a default engine always exists, the std::regex backend is
// always nameable, and asking for PCRE2 behaves differently depending on
// whether the build actually contains it — nullptr when it does not (never a
// silent fallback), a live engine when it does.

#include <ide/syntax/regex.h>
#include <ide/syntax/regex_factory.h>
#include <ide/syntax/std_regex_engine.h>
#ifdef COPE_HAS_PCRE2
#include <ide/syntax/regex_pcre2.h>
#endif

#include <gtest/gtest.h>

#include <memory>

namespace {

TEST(RegexFactory, DefaultEngineIsAlwaysAvailable) {
    auto engine = ide::syntax::makeRegexEngine();
    ASSERT_NE(engine, nullptr);
    EXPECT_FALSE(engine->name().empty());
}

TEST(RegexFactory, StdBackendIsAlwaysAvailable) {
    auto engine = ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kStd);
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->name(), "std::regex");
    // Every engine must accept the tokenizer's never-match assertion.
    EXPECT_NE(engine->compile(ide::syntax::kNeverMatchAssertion), nullptr);
}

#ifdef COPE_HAS_PCRE2

TEST(RegexFactory, Pcre2BackendWhenBuilt) {
    auto engine = ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kPcre2);
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->name(), "pcre2");
    EXPECT_NE(engine->compile(ide::syntax::kNeverMatchAssertion), nullptr);
}

TEST(RegexFactory, DefaultPrefersPcre2WhenBuilt) {
    // The default and the explicitly-forced PCRE2 backend must agree on name:
    // a PCRE2 build that silently ran std::regex would defeat every
    // differential measurement.
    const auto forced = ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kPcre2);
    ASSERT_NE(forced, nullptr);
    EXPECT_EQ(ide::syntax::makeRegexEngine()->name(), forced->name());
}

#else

TEST(RegexFactory, Pcre2BackendIsNullWithoutTheOption) {
    // Never a silent fallback to std::regex: differential tooling relies on
    // nullptr meaning "this build has no PCRE2".
    EXPECT_EQ(ide::syntax::makeRegexEngine(ide::syntax::RegexBackend::kPcre2), nullptr);
}

#endif

}  // namespace
