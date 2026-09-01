# Shared warning flags for all Cope targets.
#
# Link `cope_warnings` into every target:
#   target_link_libraries(<target> PRIVATE cope_warnings)
#
# It is an INTERFACE library: no sources, flags propagate to consumers only.

add_library(cope_warnings INTERFACE)

option(COPE_WERROR "Treat compiler warnings as errors" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cope_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wno-unused-parameter
  )
  if(COPE_WERROR)
    target_compile_options(cope_warnings INTERFACE -Werror)
  endif()
endif()
