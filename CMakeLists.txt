cmake_minimum_required(VERSION 3.25)

project(openscope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warning flags for every OpenScope target. The presets turn warnings into errors
# with CMAKE_COMPILE_WARNING_AS_ERROR, so a local build can still opt out.
add_library(openscope_warnings INTERFACE)
target_compile_options(openscope_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

enable_testing()

option(OPENSCOPE_FUZZ "Build the libFuzzer targets (Clang only; the fuzz preset sets it)" OFF)

add_subdirectory(src)
add_subdirectory(tests)
if(OPENSCOPE_FUZZ)
  add_subdirectory(fuzz)
endif()
