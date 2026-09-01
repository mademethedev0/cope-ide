# ide_syntax - TextMate grammar engine (regex abstraction, JSON model, tokenizer)
#
# Deliberately independent of ide_text: the tokenizer consumes std::string_view
# lines, so it works with a piece table, a flat buffer, or a test literal.
# The only dependency is the C++ standard library.

add_library(ide_syntax STATIC
    json_lite.cpp
    std_regex_engine.cpp
    grammar.cpp
    scope_stack.cpp
    tokenizer.cpp
)

target_include_directories(ide_syntax PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ide_syntax PUBLIC cxx_std_20)

if(TARGET cope_warnings)
    target_link_libraries(ide_syntax PRIVATE cope_warnings)
endif()
