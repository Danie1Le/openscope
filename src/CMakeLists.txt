find_package(Threads REQUIRED)
find_package(boost_asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

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

# The SCPI server over TCP (#13), apart from the instrument so only transports need Boost.Asio.
add_library(openscope_server
  openscope/server/json_log.cpp
  openscope/server/scpi_server.cpp)
target_link_libraries(openscope_server PUBLIC openscope Boost::asio nlohmann_json::nlohmann_json
  PRIVATE openscope_warnings)

# The runnable engine: the live instrument behind the SCPI server. Option parsing is a library so
# the tests can reach it without Boost.Asio.
add_library(openscope_engine engine/engine_options.cpp)
target_link_libraries(openscope_engine PUBLIC openscope PRIVATE openscope_warnings)
add_executable(openscope_engine_main engine/main.cpp)
set_target_properties(openscope_engine_main PROPERTIES OUTPUT_NAME openscope-engine)
target_link_libraries(openscope_engine_main PRIVATE openscope_engine openscope_server
  openscope_warnings)
