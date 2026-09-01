# Cope test suite.
#
# Module agents add tests as tests/<module>/<name>_test.cpp — the glob below
# picks them up automatically; nobody needs to edit this file.

include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googletest)

file(GLOB_RECURSE COPE_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp"
)

if(NOT COPE_TEST_SOURCES)
  # No tests have landed yet — configuring must still succeed.
  message(STATUS "Cope: no *_test.cpp files found — cope_tests target skipped")
  return()
endif()

add_executable(cope_tests ${COPE_TEST_SOURCES})

# Real asset locations for the smoke-parse test (all themes + all grammars).
# Compile definitions, not runtime paths: the assets ship with the repo, and a
# wrong path should be a compile error, not a silently skipped test.
target_compile_definitions(cope_tests PRIVATE
    COPE_THEMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../textmate/themes"
    COPE_GRAMMARS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../textmate/grammars"
)

target_link_libraries(cope_tests PRIVATE GTest::gtest_main cope_warnings)

# Link whichever core modules have landed. Lanes are developed in parallel, so a
# module may not exist yet when CI configures; linking conditionally keeps the
# suite buildable instead of hard-failing on a missing target.
foreach(_cope_module IN ITEMS ide_text ide_syntax ide_theme ide_host ide_highlight)
  if(TARGET ${_cope_module})
    target_link_libraries(cope_tests PRIVATE ${_cope_module})
  endif()
endforeach()

include(GoogleTest)
gtest_discover_tests(cope_tests)
