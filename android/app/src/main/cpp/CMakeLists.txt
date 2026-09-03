# libcope_jni.so — the only native library in the APK.
#
# It pulls the engine in by adding the repository root as a subproject, so there
# is exactly one definition of ide_text / ide_syntax / ide_theme / ide_highlight
# / ide_host / ide_render and the Android build can never drift from the host
# build. Tests and the CLI are switched off from Gradle (see app/build.gradle.kts).
cmake_minimum_required(VERSION 3.22)

project(cope_jni LANGUAGES CXX)

# .../android/app/src/main/cpp  ->  repository root is five levels up.
get_filename_component(COPE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

if(NOT EXISTS "${COPE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Cope: engine root not found at ${COPE_ROOT}")
endif()

add_subdirectory("${COPE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/cope-core")

add_library(cope_jni SHARED cope_jni.cpp)

target_compile_features(cope_jni PRIVATE cxx_std_20)

# Deliberately NOT linking cope_warnings: it enables -Wconversion, and JNI glue
# is nothing but jint/jlong/size_t conversions. The engine keeps the strict
# warnings; the boundary keeps explicit casts and -Wall -Wextra.
#
# Exceptions and RTTI stay ON: std::regex, std::function and the JSON parser all
# rely on them, and a bad_alloc unwinding through a -fno-exceptions frame would
# be an abort instead of a caught failure. Every JNI entry point catches.
target_compile_options(cope_jni PRIVATE -Wall -Wextra)

target_link_libraries(cope_jni PRIVATE
    ide_text
    ide_syntax
    ide_theme
    ide_highlight
    ide_host
    ide_render
    android   # AAssetManager
    log
)

# Size: the JNI entry points must stay visible, everything else can go. The root
# CMakeLists already sets -fvisibility=hidden and --gc-sections for ANDROID.
target_link_options(cope_jni PRIVATE -Wl,--exclude-libs,ALL)
