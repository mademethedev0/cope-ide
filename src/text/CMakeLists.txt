# The text lane's static library: piece table, line index, UTF-8 and undo.
# No third-party dependencies, no OS calls -- everything IO-shaped goes through
# the injected ByteSource (and later the Host interface).

add_library(ide_text STATIC
  byte_source.cpp
  line_index.cpp
  utf8.cpp
  piece_table.cpp
  undo.cpp
  document.cpp
  search.cpp
)

target_include_directories(ide_text PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ide_text PUBLIC cxx_std_20)

if(TARGET cope_warnings)
  target_link_libraries(ide_text PRIVATE cope_warnings)
endif()
