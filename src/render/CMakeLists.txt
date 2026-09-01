# ide_render - markdown document tree + CommonMark-subset parser
# (include/ide/render/markdown.h, design: docs/design/markdown-renderer.md).
# Pure string -> tree; no third-party dependencies, no OS calls.

add_library(ide_render STATIC
    markdown.cpp
)

target_include_directories(ide_render PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ide_render PUBLIC cxx_std_20)

if(TARGET cope_warnings)
    target_link_libraries(ide_render PRIVATE cope_warnings)
endif()
