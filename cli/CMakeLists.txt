# ember_cli: host-only development harness. Not shipped in the Android build.
#
# Since phase 3b it is the first visible payoff of the engine: `hl` renders
# real syntax highlighting in the terminal, `scopes` dumps token scope stacks,
# `quality` prints the coverage metric. That needs the whole highlight stack
# plus the real filesystem Host.

add_executable(ember_cli main.cpp)

target_compile_definitions(ember_cli PRIVATE
    # Same compile-definition approach as the test suite: asset locations are
    # build facts, not runtime guesses.
    EMBER_THEMES_DIR="${PROJECT_SOURCE_DIR}/textmate/themes"
    EMBER_GRAMMARS_DIR="${PROJECT_SOURCE_DIR}/textmate/grammars"
)

target_link_libraries(ember_cli PRIVATE
    ide_text
    ide_syntax
    ide_theme
    ide_highlight
    ide_host
)
target_compile_features(ember_cli PRIVATE cxx_std_20)

if(TARGET ember_warnings)
    target_link_libraries(ember_cli PRIVATE ember_warnings)
endif()
