find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

add_executable(openscope_tests acquisition_test.cpp ascii_test.cpp block_assembler_test.cpp
  block_processing_test.cpp capture_test.cpp command_tree_test.cpp engine_options_test.cpp
  engine_test.cpp
  fault_command_test.cpp fault_test.cpp fft_commands_test.cpp fft_test.cpp generator_test.cpp
  instrument_test.cpp json_log_test.cpp live_pipeline_test.cpp measurement_statistics_test.cpp
  measurement_test.cpp measuring_queue_test.cpp pacer_test.cpp phase_test.cpp
  reply_numbers_test.cpp response_test.cpp
  run_control_test.cpp running_statistics_test.cpp scpi_server_test.cpp session_test.cpp
  settings_test.cpp spsc_ring_buffer_test.cpp statistics_commands_test.cpp test_helpers_test.cpp
  timing_measurement_test.cpp trigger_test.cpp waveform_test.cpp)
target_link_libraries(openscope_tests PRIVATE openscope openscope_engine openscope_server
  openscope_warnings GTest::gmock_main Threads::Threads)
# engine_test.cpp runs the engine binary.
add_dependencies(openscope_tests openscope_engine_main)
target_compile_definitions(openscope_tests
  PRIVATE OPENSCOPE_ENGINE_PATH="$<TARGET_FILE:openscope_engine_main>")
gtest_discover_tests(openscope_tests)

add_test(NAME NoShellOrFilesystemAccess
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/src
    -P ${CMAKE_CURRENT_SOURCE_DIR}/no_shell_or_filesystem.cmake)
