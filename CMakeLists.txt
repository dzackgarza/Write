cmake_minimum_required(VERSION 3.25)
project(write_headless LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(write_headless STATIC
  syncscribble/basics.cpp
  syncscribble/strokebuilder.cpp
  syncscribble/element.cpp
  syncscribble/page.cpp
  syncscribble/syncundo.cpp
  syncscribble/selection.cpp
  syncscribble/document.cpp
  ulib/geom.cpp
  ulib/image.cpp
  ulib/path2d.cpp
  ulib/painter.cpp
  usvg/svgnode.cpp
  usvg/svgstyleparser.cpp
  usvg/svgparser.cpp
  usvg/svgpainter.cpp
  usvg/svgwriter.cpp
  usvg/cssparser.cpp
  nanovgXC/src/nanovg.c
  pugixml/src/pugixml.cpp
  miniz/miniz.c
  miniz/miniz_tdef.c
  miniz/miniz_tinfl.c
  headless/platform.cpp)

target_include_directories(write_headless PUBLIC . syncscribble
  nanovgXC/src nanovgXC/glad pugixml/src stb)
target_compile_definitions(write_headless PRIVATE PUGIXML_NO_XPATH PUGIXML_NO_EXCEPTIONS
  NO_PAINTER_GL NO_PAINTER_SW NO_PAINTER_SWU)

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
  if(BUILD_TESTING)
    add_executable(write_headless_smoke headless/smoke.cpp)
    target_link_libraries(write_headless_smoke PRIVATE write_headless)
    add_test(NAME write_headless_smoke COMMAND write_headless_smoke)
  endif()
endif()
