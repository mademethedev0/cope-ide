# Shared warning flags for all Ember targets.
#
# Link `ember_warnings` into every target:
#   target_link_libraries(<target> PRIVATE ember_warnings)
#
# It is an INTERFACE library: no sources, flags propagate to consumers only.

add_library(ember_warnings INTERFACE)

option(EMBER_WERROR "Treat compiler warnings as errors" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ember_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wno-unused-parameter
  )
  if(EMBER_WERROR)
    target_compile_options(ember_warnings INTERFACE -Werror)
  endif()
endif()
