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

}  // namespace ide::syntax
