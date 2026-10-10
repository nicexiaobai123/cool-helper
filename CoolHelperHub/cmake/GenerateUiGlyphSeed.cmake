# Collect UTF-8 non-ASCII characters from first-party sources, including UI
# literals and comments. ASCII syntax is discarded rather than parsing C++:
# this also covers adjacent literals, wide strings and NUL-separated combos.
# No runtime settings, API responses or credentials are read here.
file(GLOB_RECURSE ui_glyph_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/coolhelper/*.h"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ui_glyph_sources})
set(UI_GLYPH_SEED "")
foreach(ui_glyph_source IN LISTS ui_glyph_sources)
  file(READ "${ui_glyph_source}" ui_glyph_content)
  string(REGEX REPLACE "[\t\r\n -~]" "" ui_glyph_content "${ui_glyph_content}")
  string(APPEND UI_GLYPH_SEED "${ui_glyph_content}")
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/UiGlyphSeed.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/UiGlyphSeed.h" @ONLY)
