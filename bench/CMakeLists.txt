# Google Benchmark targets. Built only by the `benchmark` preset, which installs the vcpkg
# `benchmarks` feature (Google Benchmark, and KissFFT as the reference our FFT is measured against).
find_package(benchmark CONFIG REQUIRED)
find_package(kissfft CONFIG REQUIRED)

add_executable(fft_benchmark fft_benchmark.cpp)
target_link_libraries(fft_benchmark PRIVATE openscope openscope_warnings benchmark::benchmark_main
  kissfft::kissfft-double)
