# Google Benchmark targets. Built only by the `benchmark` preset, which installs the vcpkg
# `benchmarks` feature (Google Benchmark).
find_package(benchmark CONFIG REQUIRED)
find_package(Threads REQUIRED)

# KissFFT is the reference our FFT is measured against. It is compiled here, from the same release
# vcpkg packages, with the compiler and flags the build type gives our own code (Release: -O3
# -DNDEBUG, strict floating point). KissFFT's own CMake would add -ffast-math, and vcpkg builds it
# with the system C compiler, so neither is used: the two sides must differ only in the algorithm.
enable_language(C)
include(FetchContent)
FetchContent_Declare(kissfft
  URL https://github.com/mborgerding/kissfft/archive/131.2.0.tar.gz
  URL_HASH SHA512=5d02802a9e191e7cb77c26e9a34659a5d47c4e85bcfdf86a7cffdda66d8b79261f7fe5795ffabd78644b6094c01b32a84841669fbc0009ac9268ae1ba521af9e
  # Fetch the sources only: there is no CMakeLists.txt in this subdirectory to add.
  SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(kissfft)

add_library(kissfft_reference STATIC ${kissfft_SOURCE_DIR}/kiss_fft.c)
target_include_directories(kissfft_reference SYSTEM PUBLIC ${kissfft_SOURCE_DIR})
target_compile_definitions(kissfft_reference PUBLIC kiss_fft_scalar=double)

# Every Google Benchmark target is named `*_benchmark`: CI runs each one it finds, and it must also
# be in the `benchmark` build preset's targets. docs/benchmarks.md lists them all.
add_executable(fft_benchmark fft_benchmark.cpp)
target_link_libraries(fft_benchmark PRIVATE openscope openscope_warnings benchmark::benchmark_main
  kissfft_reference)

add_executable(ring_buffer_benchmark ring_buffer_benchmark.cpp)
target_link_libraries(ring_buffer_benchmark PRIVATE openscope openscope_warnings
  benchmark::benchmark_main Threads::Threads)

# Not a Google Benchmark: runs the live pipeline in real time and prints its drop counts.
add_executable(live_run live_run.cpp)
target_link_libraries(live_run PRIVATE openscope openscope_warnings)
