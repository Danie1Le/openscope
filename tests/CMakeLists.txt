find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(openscope_tests acquisition_test.cpp ascii_test.cpp capture_test.cpp
  command_tree_test.cpp fault_command_test.cpp fault_test.cpp fft_commands_test.cpp fft_test.cpp
  generator_test.cpp instrument_test.cpp measurement_test.cpp phase_test.cpp response_test.cpp
  run_control_test.cpp session_test.cpp settings_test.cpp timing_measurement_test.cpp
  trigger_test.cpp waveform_test.cpp)
target_link_libraries(openscope_tests PRIVATE openscope openscope_warnings GTest::gmock_main)
gtest_discover_tests(openscope_tests)
