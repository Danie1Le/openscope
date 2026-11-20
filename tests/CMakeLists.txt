find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(openscope_tests ascii_test.cpp command_tree_test.cpp instrument_test.cpp
  session_test.cpp)
target_link_libraries(openscope_tests PRIVATE openscope openscope_warnings GTest::gmock_main)
gtest_discover_tests(openscope_tests)
