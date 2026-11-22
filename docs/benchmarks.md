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

## SCPI latency (NFR-4)

`python/tests/integration/test_latency.py` starts the engine and times SCPI commands over a raw TCP socket on loopback, each from just before it is sent to when its reply has arrived. NFR-4 asks for a setter p95 under 5 ms and a measurement query on a 1M-sample record p95 under 20 ms. In CI the same tests run on every pull request (release and ASan engines) but assert only ceilings of 50 ms, 100 ms and 2 s, so a shared runner cannot fail them; the numbers here are from the machine below.

```sh
cd python && OPENSCOPE_ENGINE_PATH=../build/release/src/openscope-engine pytest -m latency -s
```

- **Setter**: 10 000 × `SOUR1:FREQ 1000` through `openscope-client`, which sends `SYST:ERR?` behind each line and reads its reply: one round trip per setter, as a script sees it. Each one drops the capture in progress, as every setting does.
- **Measurement on a 1M-sample record**: one capture of 1 048 576 samples taken with `SINGle;*OPC?` (so it stays the latest capture), then 10 000 queries cycling through the ten `MEAS:<type>? CHAN1`, each alone on its line with no error check behind it (see the client note below). Results are cached per capture, and an untimed first round of the ten computes them, so all 10 000 timed queries read the cache.
- **First measurement of a fresh 1M-sample record**: the first query of each of the ten measurements on three fresh captures, 30 queries, each one computing its result. The first of the ten also finds top and base, which the others reuse. Thirty samples make a rough p95: `statistics.quantiles(n=100, method="inclusive")` puts it 0.55 of the way from the third-slowest to the second-slowest.

Measured 2026-11-22 on the Intel Core i7-9700F machine above (`openscope-dev:24.04` on Docker Desktop, WSL2 kernel 6.18.58.1, 8 CPUs), release engine (g++-14 `-O3 -DNDEBUG`), Python 3.12 client, with the machine otherwise idle (load average about 1.1). Three runs:

| | Count | Median | p95 | p99 | Worst |
|---|---|---|---|---|---|
| Setter | 10 000 | 0.085 to 0.096 ms | 0.119 to 0.144 ms | 0.143 to 0.200 ms | 0.58 to 8.97 ms |
| Measurement on a 1M-sample record (cached) | 10 000 | 0.076 ms | 0.112 to 0.114 ms | 0.129 to 0.136 ms | 0.46 to 0.54 ms |
| First measurement of a fresh 1M-sample record | 30 | 1.94 to 2.07 ms | 7.99 to 8.17 ms | 8.12 to 8.66 ms | 8.15 to 8.75 ms |

What the table shows: both NFR-4 targets held, by a wide margin. A setter's p95 was about 0.13 ms against 5 ms. A measurement query's p95 on a 1M-sample record was 0.11 ms from the cache and under 9 ms when it computes the result (the first measurement of a capture, which also finds top and base, takes about 8 ms; the other nine about 2 ms). These are loopback times on one machine with nothing else running; a network adds its round trip to each.

**Through `openscope-client`, a query takes about 0.1 ms.** The client sends `SYST:ERR?` behind every line, so a query's two replies leave the server back to back. Until bug #58 was fixed they took about 44 ms: the server did not set `TCP_NODELAY`, so Nagle's algorithm held the second reply until the client's delayed acknowledgement of the first arrived (setting `TCP_NODELAY` on the client's socket changed nothing). The server now sets `TCP_NODELAY` on the acceptor before it listens, and accepted sockets inherit it, as they do the keepalive options. `test_pipelined_replies_are_not_delayed` asserts the fix (median of 50 `*IDN?` through the client under 20 ms).

`*IDN?` through the client, 2 000 queries per run, three runs on the machine above (release engine, load average about 1):

| Path | Median | p95 | p99 | Worst |
|---|---|---|---|---|
| Raw socket (`OpenScope.connect`), before the fix | about 44 ms | not measured | not measured | not measured |
| Raw socket (`OpenScope.connect`) | 0.096 to 0.097 ms | 0.134 to 0.140 ms | 0.153 to 0.166 ms | 0.92 to 1.57 ms |
| PyVISA (`OpenScope.open_visa`, pyvisa-py) | 0.104 to 0.203 ms | 0.198 to 0.248 ms | 0.238 to 0.356 ms | 0.87 to 2.10 ms |

With the issue's reproducer (raw socket, median of 20): `*IDN?` then `SYST:ERR?` in one write took 44.06 ms before and 0.10 ms after; `*IDN?` alone 0.07 and 0.09 ms; a setter then `SYST:ERR?` 0.10 and 0.08 ms.

## Long-running stability (soak test)

`python/tests/integration/test_soak.py` runs the engine live while four clients use it at once: one takes Singles and checks each one's statistics and channel 1's frequency against its Generator (NFR-6), one reads waveforms, spectra and measurements of the latest capture, one changes settings and injects faults, and one connects, makes a mistake and disconnects over and over. It fails if any check fails (the first failure stops every client within a round and is reported at once), if the engine's resident memory grows by 32 MiB or more after the clients have warmed up (its first reading, 5 s in), or if the engine does not exit cleanly. It is not part of the pull-request run: `pytest` deselects it unless given `--soak`, whatever `-m` selects. Run it on a release engine (ASan holds freed memory in quarantine, which the memory check would count) with:

```sh
cd python && OPENSCOPE_ENGINE_PATH=../build/release/src/openscope-engine OPENSCOPE_SOAK_SECONDS=3600 pytest --soak -m soak -s
```

or from the Actions tab with the **Soak** workflow (`workflow_dispatch`, length in whole seconds from 1 to 18000, 3600 by default; the run is stopped 600 s past that length and its JUnit report uploaded either way).

One run of the default 600 s, on 2026-11-22 on the machine above (release engine), passed: 3 967 Singles, 4 471 reader rounds, 11 819 setting changes and 1 534 933 connections, with the engine's resident memory between 5.0 and 5.6 MiB throughout. That is ten minutes, not the hour the workflow defaults to; no hour-long run has been recorded yet.

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

## Live mode at 4 channels x 1 MS/s (NFR-1)

`bench/live_run.cpp` runs the instrument live (`openscope::LivePipeline`) with all four channels enabled, each a 1 kHz sine with 10 mV RMS noise, at the record length of 10 000 and the `*RST` 1 MS/s or the sample rate given second. Once a second it prints how far the latest capture reaches on the sample clock against the samples the wall clock has released, the skipped samples (released by the wall clock but never acquired, because acquisition fell more than one round behind), the dropped-block and discarded-capture counts, and the slowest `*IDN?` of that second, sent every 5 ms from the main thread while the pipeline runs:

```sh
build/benchmark/bench/live_run 60        # 1 MS/s
build/benchmark/bench/live_run 10 1E7    # 10 MS/s
```

Measured 2026-11-21 on the machine above (same container and build flags as the ring buffer), while other builds and tests shared the machine. Three 60-second runs at 1 MS/s:

| | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Skipped samples | 0 | 0 | 0 |
| Dropped blocks | 0 | 1 (at about 35 s) | 0 |
| Captures discarded for spanning a dropped block | 0 | 1 | 0 |
| Latest capture's end behind the wall clock, sampled each second | 899 to 12 010 samples | 1 387 to 12 774 samples | 1 023 to 23 399 samples |
| Slowest `*IDN?` in any second | 0.08 ms | 0.51 ms | 2.39 ms |

What the table shows: NFR-1's target of zero dropped blocks at 4 channels x 1 MS/s held for runs 1 and 3 and missed by one block in run 2, all while other builds shared the machine; the capture that block would have split was discarded and counted, never published with a gap. Captures complete every 10 000 samples, so the latest capture's end trails the wall clock by up to one record length (10 ms) plus the time to acquire and process the block that completes it. Most seconds stayed under 13.1 ms; run 3 saw 21.0 and 23.4 ms in two separate seconds, and the lag did not grow over any run, so live mode kept pace with real time. These are three 60-second runs, not a soak test.

### Above what the machine sustains

Ten seconds at each sample rate, same setup:

| `ACQ:SRAT` | Wall-clock samples | Latest capture ends at | Skipped samples | Dropped blocks | Slowest `*IDN?` in any second |
|---|---|---|---|---|---|
| 1E7 | 100 017 000 | 43 919 977 | 47 409 383 | 0 | 0.06 ms |
| 1E8 | 1 000 436 604 | 44 619 675 | 870 357 048 | 0 | 0.06 ms |

What the table shows: this machine acquires about 4.4 M sample indexes per second on four noisy channels, so at 1E7 and above acquisition falls behind the wall clock every round. The pacer then skips what is over a round and counts it: at 1E8, 87 % of the samples the wall clock released were skipped, and the skipped count says so where the dropped-block count alone (0) would not. `*IDN?` stayed under 0.1 ms at these rates and under 2.4 ms in every run, because `Instrument::acquire` holds the Instrument's mutex only to take the indexes and a settings snapshot and samples outside it.
