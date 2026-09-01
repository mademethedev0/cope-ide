# ide_host - the injected IO boundary.
#
# MemoryHost lives here today (pure std::map, no OS calls, safe for every
# platform including a future web build). PosixHost lands in this same target
# but is host-only: it is compiled only when the platform is not Android, so
# the Android app supplies its own Host instead.

add_library(ide_host STATIC
    memory_host.cpp
    posix_host.cpp
)

target_include_directories(ide_host PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    # PosixHost is an implementation, not engine API, so its declaration lives
    # in src/host. Applications that inject the real filesystem (ember_cli, the
    # Linux tests) include it as <posix_host.h>.
    ${PROJECT_SOURCE_DIR}/src/host
)
target_compile_features(ide_host PUBLIC cxx_std_20)

# PUBLIC: host.h is self-contained, but mapped_source.h exposes text::ByteSource
# and host.h's own contract references nothing else. Consumers of the adapter
# need ide_text's headers.
target_link_libraries(ide_host PUBLIC ide_text)

if(TARGET ember_warnings)
    target_link_libraries(ide_host PRIVATE ember_warnings)
endif()
