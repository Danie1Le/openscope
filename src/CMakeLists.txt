find_package(Threads REQUIRED)

add_library(openscope
  openscope/capture.cpp
  openscope/fault.cpp
  openscope/fft.cpp
  openscope/generator.cpp
  openscope/instrument.cpp
  openscope/live_pipeline.cpp
  openscope/measurement.cpp
  openscope/measurement_statistics.cpp
  openscope/pacer.cpp
  openscope/sample_block.cpp
  openscope/scpi/command_tree.cpp
  openscope/scpi/parser.cpp
  openscope/scpi/response.cpp
  openscope/trigger.cpp)
target_include_directories(openscope PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openscope PUBLIC Threads::Threads PRIVATE openscope_warnings)
target_compile_definitions(openscope PRIVATE OPENSCOPE_VERSION="${PROJECT_VERSION}")
