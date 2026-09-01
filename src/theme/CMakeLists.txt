# ide_theme - VS Code JSON theme loader, TextMate scope selector matching and the
# interned style palette.
#
# Links ide_syntax for the vendored JSON reader (ide::syntax::json, aliased to
# ide::json by include/ide/theme/theme.h). That is the only dependency: no
# third-party library, no filesystem, no platform code. Colour and selector
# handling live in their own translation units so they can be tested without
# touching the loader.

add_library(ide_theme STATIC
    style.cpp
    scope_selector.cpp
    theme.cpp
)

target_include_directories(ide_theme PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ide_theme PUBLIC cxx_std_20)

# PUBLIC: theme.h includes <ide/syntax/json_lite.h>, so consumers need it too.
target_link_libraries(ide_theme PUBLIC ide_syntax)

if(TARGET ember_warnings)
    target_link_libraries(ide_theme PRIVATE ember_warnings)
endif()
