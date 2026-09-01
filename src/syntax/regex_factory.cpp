#include <ide/syntax/regex_factory.h>

#include <ide/syntax/std_regex_engine.h>
#ifdef COPE_HAS_PCRE2
#include <ide/syntax/regex_pcre2.h>
#endif

namespace ide::syntax {

std::unique_ptr<IRegexEngine> makeRegexEngine() noexcept {
#ifdef COPE_HAS_PCRE2
    return std::make_unique<Pcre2RegexEngine>();
#else
    return std::make_unique<StdRegexEngine>();
#endif
}

std::unique_ptr<IRegexEngine> makeRegexEngine(RegexBackend backend) noexcept {
    switch (backend) {
        case RegexBackend::kDefault:
            return makeRegexEngine();
        case RegexBackend::kStd:
            return std::make_unique<StdRegexEngine>();
        case RegexBackend::kPcre2:
#ifdef COPE_HAS_PCRE2
            return std::make_unique<Pcre2RegexEngine>();
#else
            return nullptr;  // caller reports; never silently substitute
#endif
    }
    return nullptr;
}

}  // namespace ide::syntax
