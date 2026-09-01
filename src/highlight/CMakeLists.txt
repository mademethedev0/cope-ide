# ide_highlight - the highlight cascade.
#
# Tier 1 is the TextMate grammar (ide_syntax), tier 2 the regex-free fallback
# lexer, tier 3 a plain span; the theme resolver (ide_theme) turns scope stacks
# into palette ids so the UI never sees a scope string.
#
# Depends on ide_syntax and ide_theme only: no filesystem, no JNI, no Android,
# and deliberately not on ide_text - the cascade consumes std::string_view lines,
# so it works with a piece table, a flat buffer or a test literal.

add_library(ide_highlight STATIC
    fallback_lexer.cpp
    highlighter.cpp
    quality.cpp
)

target_include_directories(ide_highlight PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ide_highlight PUBLIC cxx_std_20)

# PUBLIC: highlighter.h exposes ide::syntax::State / ScopeStackId and
# ide::theme::StyleId in its own API, so consumers need both headers.
target_link_libraries(ide_highlight PUBLIC ide_syntax ide_theme)

if(TARGET cope_warnings)
    target_link_libraries(ide_highlight PRIVATE cope_warnings)
endif()
