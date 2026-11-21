add_library(openscope
  openscope/capture.cpp
  openscope/generator.cpp
  openscope/instrument.cpp
  openscope/sample_block.cpp
  openscope/scpi/command_tree.cpp
  openscope/scpi/parser.cpp)
target_include_directories(openscope PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openscope PRIVATE openscope_warnings)
target_compile_definitions(openscope PRIVATE OPENSCOPE_VERSION="${PROJECT_VERSION}")
