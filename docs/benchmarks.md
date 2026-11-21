# Benchmarks

Measured numbers for the performance claims in the spec. CI runs the same benchmarks on every pull request and reports them without gating on them (shared runners are too noisy for thresholds), so this document holds the reference measurements.

## How to run

```sh
cmake --preset benchmark && cmake --build --preset benchmark
build/benchmark/bench/fft_benchmark --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

The `benchmark` preset is a Release build with the vcpkg `benchmarks` feature, which adds Google Benchmark and KissFFT. KissFFT is a reference for comparison only; OpenScope never links it outside `bench/`.

## FFT against KissFFT

`bench/fft_benchmark.cpp` times three things at 4K, 16K, 64K, 256K and 1M points:

- `BM_OpenScopeFft`: our radix-2 complex FFT (`openscope::Fft::transform`), twiddles computed once outside the loop. It transforms in place, so each iteration also copies the N-point input back, which KissFFT does not have to do.
- `BM_KissFft`: KissFFT 131.2.0's complex double transform (`kiss_fft`), its plan made once outside the loop. It uses mixed radix-4/radix-2 butterflies.
- `BM_MagnitudeSpectrum`: everything `FFT:DATA?` computes for one capture under the Blackman-Harris window: twiddles, window, transform and dB levels.

Both FFTs take the same double-precision complex input (a real signal with zero imaginary parts).

Measured 2026-11-21. Intel Core i7-9700F (8 cores, 3.0 GHz) in the `openscope-dev:24.04` container (g++-14, Release `-O3`). Mean of 3 repetitions, CPU time. The machine was running other builds (load average about 10 on 8 cores), so absolute times are pessimistic by up to about 15% (the coefficient of variation between repetitions). The ratio between the two FFTs is the useful number.

| Points | OpenScope FFT | KissFFT | OpenScope / KissFFT | `FFT:DATA?` spectrum |
|---|---|---|---|---|
| 4K (4096) | 94 µs | 55 µs | 1.7 | 241 µs |
| 16K (16384) | 482 µs | 347 µs | 1.4 | 1.0 ms |
| 64K (65536) | 3.3 ms | 3.1 ms | 1.1 | 6.0 ms |
| 256K (262144) | 22 ms | 18 ms | 1.2 | 28 ms |
| 1M (1048576) | 143 ms | 121 ms | 1.2 | 166 ms |

Fitted complexity: 6.7 N log2 N ns for ours and 5.7 N log2 N ns for KissFFT. Both are O(N log N).

Reading the results:

- Our plain radix-2 FFT is 1.1 to 1.7 times slower than KissFFT. KissFFT's radix-4 butterflies need fewer passes over the data, and our in-place copy costs extra at small sizes.
- The whole `FFT:DATA?` spectrum on a 1M-sample capture took 166 ms. That is well above the 20 ms p95 target NFR-4 sets for measurement queries. With KissFFT's transform it would still be over the target. Getting there would take a real-input FFT (half the work), cached twiddles and window per record length, and less memory traffic. Nobody has done that yet.
