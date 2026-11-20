# libFuzzer targets. Built only by the `fuzz` preset, which compiles everything with
# -fsanitize=fuzzer-no-link plus ASan and UBSan; linking adds libFuzzer's main.
add_executable(execute_line_fuzzer execute_line_fuzzer.cpp)
target_link_libraries(execute_line_fuzzer PRIVATE openscope openscope_warnings)
target_link_options(execute_line_fuzzer PRIVATE -fsanitize=fuzzer)
