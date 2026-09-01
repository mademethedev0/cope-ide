cmake_minimum_required(VERSION 3.22)

project(cope VERSION 0.1.0 LANGUAGES CXX)

# --- Language -----------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Options ------------------------------------------------------------------
option(COPE_BUILD_TESTS "Build the GoogleTest suite" ON)
option(COPE_BUILD_CLI "Build the cope_cli host dev harness" ON)
option(COPE_USE_PCRE2 "Fetch and build PCRE2 (regex backend, slice 1: build wiring only)" OFF)

# --- Helper modules -----------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CopeWarnings)

# --- Release size/speed flags (GCC / Clang) ------------------------------------
# Host builds: --gc-sections only in Release, so Debug test builds stay fast
# and debuggable. Android builds: always --gc-sections, size is the budget.
# Never -Os on the host: tests must exercise unoptimized paths too.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    $<$<CONFIG:Release>:-O2>
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
  )
  if(ANDROID)
    add_link_options(-Wl,--gc-sections)
  else()
    add_link_options($<$<CONFIG:Release>:-Wl,--gc-sections>)
  endif()
endif()

# --- PCRE2 (optional regex backend) --------------------------------------------
# Build wiring only: no engine code links this yet. When OFF, nothing below
# has any effect on the rest of the project.
if(COPE_USE_PCRE2)
  include(FetchContent)
  # PCRE2 is a C library; the root project declares CXX only.
  enable_language(C)

  set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
  set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
  set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    pcre2
    GIT_REPOSITORY https://github.com/PCRE2Project/pcre2
    GIT_TAG pcre2-10.48 # latest stable at time of writing
  )
  FetchContent_MakeAvailable(pcre2)

  # Interface target for future consumers (src/syntax). The width is a
  # preprocessor contract: consumers must see PCRE2_CODE_UNIT_WIDTH=8 to get
  # the pcre2_xxx_8 API from pcre2.h.
  add_library(cope::regex_pcre2 INTERFACE)
  target_link_libraries(cope::regex_pcre2 INTERFACE pcre2-8)
  target_compile_definitions(cope::regex_pcre2 INTERFACE
    COPE_HAS_PCRE2=1
    PCRE2_CODE_UNIT_WIDTH=8
    PCRE2_STATIC
  )
endif()

# --- Core modules -------------------------------------------------------------
# Modules are added only if their CMakeLists exists; CI may configure at a
# moment when a parallel lane has not landed yet. Missing modules warn loudly
# instead of hard-failing configure.
foreach(_cope_module IN ITEMS text syntax theme host highlight)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_cope_module}/CMakeLists.txt")
    add_subdirectory("src/${_cope_module}")
  else()
    message(WARNING "Cope: src/${_cope_module}/CMakeLists.txt not found — module skipped")
  endif()
endforeach()

# --- Host-only dev harness ----------------------------------------------------
if(COPE_BUILD_CLI)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt")
    add_subdirectory(cli)
  else()
    message(STATUS "Cope: cli/ not present — cope_cli skipped")
  endif()
endif()

# --- Tests --------------------------------------------------------------------
if(COPE_BUILD_TESTS)
  enable_testing()
  include(CTest)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
  else()
    message(STATUS "Cope: tests/CMakeLists.txt not present — tests skipped")
  endif()
endif()

# --- Configuration summary ----------------------------------------------------
message(STATUS "Cope configuration:")
message(STATUS "  PCRE2 (JIT): ${COPE_USE_PCRE2}")
