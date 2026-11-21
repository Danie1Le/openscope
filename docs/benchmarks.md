# Benchmarks

Measured numbers for the performance claims in the spec. CI runs the same benchmarks on every pull request and reports them without gating on them (shared runners are too noisy for thresholds), so this document holds the reference measurements.

## How to run

```sh
CC=gcc-14 CXX=g++-14 cmake --preset benchmark && cmake --build --preset benchmark
build/benchmark/bench/fft_benchmark --benchmark_repetitions=10 --benchmark_enable_random_interleaving=true --benchmark_report_aggregates_only=true
build/benchmark/bench/ring_buffer_benchmark --benchmark_repetitions=10 --benchmark_enable_random_interleaving=true --benchmark_report_aggregates_only=true
```

The `benchmark` preset is a Release build with the vcpkg `benchmarks` feature, which adds Google Benchmark. KissFFT is a reference for comparison only; OpenScope never links it outside `bench/`.

## FFT against KissFFT

`bench/fft_benchmark.cpp` times four things at record lengths of 4K, 16K, 64K, 256K and 1M samples:

- `BM_OpenScopeFft`: our radix-2 complex FFT (`openscope::Fft::transform`), twiddles computed once outside the loop. It transforms in place, so each iteration also copies the N-point input back, which KissFFT does not have to do.
- `BM_InputCopy`: that copy alone.
- `BM_KissFft`: KissFFT 131.2.0's complex double transform (`kiss_fft`), its plan made once outside the loop.
- `BM_MagnitudeSpectrum`: everything `FFT:DATA?` computes for one capture under the Blackman-Harris window: twiddles, window, transform and dB levels.

Both FFTs take the same double-precision complex input (a real signal with zero imaginary parts).

### Build flags

Both FFTs are compiled by this project's `benchmark` preset (`bench/CMakeLists.txt`), so they differ only in the algorithm:

| | Compiler | Flags |
|---|---|---|
| OpenScope FFT (`src/openscope/fft.cpp`) | g++-14 (14.2.0) | `-O3 -DNDEBUG` |
| KissFFT (`kiss_fft.c` from the 131.2.0 release, `kiss_fft_scalar=double`) | gcc-14 (14.2.0) | `-O3 -DNDEBUG` |

Neither side uses `-ffast-math`. KissFFT's own CMake project and the vcpkg `kissfft` package do add `-ffast-math` and build with the system C compiler (gcc 13.3 in the image), so a number from that package is not comparable with ours.

### Machine

Measured 2026-11-21 on an Intel Core i7-9700F (8 cores, 3.0 GHz, 12 MiB L3) under Windows 11, in the `openscope-dev:24.04` container on Docker Desktop (WSL2 kernel 6.18.58.1, no CPU limit on the container, so all 8 CPUs). The machine was shared with other builds and tests: the load average was about 6 on 8 cores for the whole run. Treat the numbers as rough.

Median of 10 repetitions with random interleaving (repetitions of the different benchmarks alternate, so load drift hits both sides alike), CPU time. The coefficient of variation between repetitions was 8 to 37 % for the FFTs, 10 to 39 % for the copy, so ratios closer than about 1.3 do not separate.

| Record length | OpenScope FFT | KissFFT | OpenScope / KissFFT | `BM_InputCopy` | `FFT:DATA?` spectrum |
|---|---|---|---|---|---|
| 4K (4096) | 70 µs | 46 µs | 1.5 | 1.6 µs | 186 µs |
| 16K (16384) | 377 µs | 215 µs | 1.8 | 8.1 µs | 889 µs |
| 64K (65536) | 2.1 ms | 1.3 ms | 1.6 | 36 µs | 3.8 ms |
| 256K (262144) | 11.7 ms | 11.8 ms | 1.0 | 362 µs | 19.4 ms |
| 1M (1048576) | 88 ms | 98 ms | 0.9 | 1.7 ms | 126 ms |

What the table shows:

- Up to 64K samples our FFT took 1.5 to 1.8 times as long as KissFFT. At 256K and 1M the two were within the run-to-run spread, so no difference is claimed there.
- The in-place copy is 2 to 3 % of our transform time at every size, so it does not explain the gap.
- Time per point grew with size for both FFTs (per N log2 N, from about 1.4 ns at 4K to 4.2 ns at 1M for ours), so a single O(N log N) coefficient would describe only part of the range. No complexity fit is reported.

Not measured, so only hypotheses: that KissFFT's radix-4 butterflies need fewer passes and so lead at the sizes that fit in cache, and that the growth in time per point is memory traffic once the data outgrows the caches.

### Against NFR-4

The whole `FFT:DATA?` spectrum on a 1M-sample capture took 126 ms (median of 10 repetitions; no p95 was measured), well above the 20 ms p95 target NFR-4 sets for measurement queries. KissFFT's transform alone took 98 ms at that size, so swapping the transform would not reach the target. What would bring it closer has not been measured: ideas are a real-input FFT (half the work), twiddles and window cached per record length, and less memory traffic. Nobody has tried them yet.

## SPSC ring buffer throughput

`bench/ring_buffer_benchmark.cpp` passes `std::uint64_t` items from a producer thread to a consumer thread through the ring buffer the live pipeline uses (`openscope::SpscRingBuffer`), with queues of 16, 256 and 4096 slots. The producer tries again whenever it finds the queue full, so the rate is the sustained hand-over rate between two cores; `full_per_item` is how many of those retries there were per item.

Measured 2026-11-21 on the machine above (Intel Core i7-9700F, `openscope-dev:24.04` on Docker Desktop, g++-14 `-O3 -DNDEBUG`), shared with other builds and tests. Median of 10 repetitions with random interleaving, wall time per item:

| Queue slots | Time per item | Items per second | `full_per_item` | CV of the rate |
|---|---|---|---|---|
| 16 | 34 ns | 29 M/s | 0.52 | 9 % |
| 256 | 28 ns | 36 M/s | 0.28 | 9 % |
| 4096 | 21 ns | 48 M/s | 0.12 | 5 % |

What the table shows: a hand-over took 21 to 34 ns, and fewer pushes found the queue full as it grew. The live pipeline is meant to hand over one sample block per millisecond at 1 MS/s, about 1000 blocks per second, so the queue is tens of thousands of times faster than it needs to be: the cost of a block is generating and assembling its samples, not passing it on.

Not measured, so only a hypothesis: that most of each hand-over is the two cores passing the indexes' and the slot's cache lines back and forth.
