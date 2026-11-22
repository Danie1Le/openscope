# OpenScope

A software oscilloscope engine in C++20: simulated signals are sampled, triggered into captures, measured, and exposed to scripts over SCPI. See `GLOSSARY.md` for the domain vocabulary, `docs/adr/` for the main design decisions and `docs/scpi-reference.md` for the SCPI grammar, error codes and commands.

## Build and test

Requirements: Linux (developed on Ubuntu 24.04), GCC 14 or Clang 18, CMake 3.25+, Ninja, and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` pointing at it. Dependencies are declared in `vcpkg.json` and installed automatically on the first configure.

```sh
cmake --preset debug      # configure
cmake --build --preset debug
ctest --preset debug
```

CMake reads `CXX` only when it creates a build directory, and every preset reuses `build/<preset>`. To pick or switch the compiler, configure with `--fresh`, which discards the cached configuration:

```sh
CXX=clang++-18 cmake --fresh --preset debug
```

Presets: `debug`, `release`, `asan` (AddressSanitizer + UndefinedBehaviorSanitizer), `tsan` (ThreadSanitizer) and `coverage` (always Clang, for llvm-cov). Each builds into `build/<preset>`. Warnings are errors in every preset.

Formatting and lint use the committed `.clang-format` and `.clang-tidy`:

```sh
clang-format-18 --dry-run --Werror $(git ls-files '*.cpp' '*.hpp')
CXX=clang++-18 cmake --fresh --preset debug -DCMAKE_CXX_CLANG_TIDY=clang-tidy-18   # then build: findings fail it
```

With Clang, the sanitizer and coverage presets need the compiler runtime (`libclang-rt-18-dev` on Ubuntu).

## Running the engine

`openscope-engine` runs the instrument live (4 channels on a 1 MS/s sample clock at `*RST`) and serves SCPI over TCP until Ctrl+C or SIGTERM. The debug, release, asan, tsan and coverage presets build it into `build/<preset>/src/`.

```sh
build/release/src/openscope-engine            # 127.0.0.1:5025
build/release/src/openscope-engine --port 0   # a free port, named in the "listening" log line
```

| Option | Default | Meaning |
|---|---|---|
| `--address <ip>` | `127.0.0.1` | Local IP address to listen on. Any other address exposes the instrument, which has no authentication, to the network. Host names are not resolved. |
| `--port <0-65535>` | `5025` | TCP port; `0` picks a free one. |
| `--idle-timeout <1-86400>` | `300` | Seconds a client may send no command and get no reply before it is disconnected. A client waiting on `*OPC?` is never idle. |
| `--seed <n>` | `0` | Seed of the generators' noise: the same seed and the same commands give the same signals. |
| `-h`, `--help` | | Print the options and exit. |

Logs go to standard error, one JSON object per line with `time` and `event` (`listening`, `connect`, `refuse`, `disconnect`, `error`, `stopping`). An invalid option exits with status 2; a failure to start, such as a port in use, logs an `error` event and exits with status 1.

```console
$ printf '*IDN?\n' | nc -q 1 127.0.0.1 5025
OpenScope,OS-4,000001,0.1.0
```

See `docs/scpi-reference.md` for the connection rules (one Session per connection, 4 KB lines, 8 clients) and the commands.

## Python client

`python/` holds `openscope-client`, a Python 3.12+ package with typed methods over the SCPI commands. It talks to the engine over a raw TCP socket (standard library only) or through PyVISA (the `visa` extra), with the same API. After every line it reads the Session's error queue and raises `ScpiError`, carrying the SCPI code and message, when the line queued errors.

```python
from openscope_client import Measurement, OpenScope

with OpenScope.connect(port=5025) as scope:  # or OpenScope.open_visa("TCPIP0::localhost::5025::SOCKET")
    print(scope.identify())
    scope.generator(1).frequency = 2000
    scope.single()
    scope.wait_complete()  # SINGle;*OPC?: the capture is complete
    waveform = scope.waveform.read()  # float32 block, placed in time with WAV:PRE?
    print(scope.measure(Measurement.FREQUENCY, channel=1))
```

Its tools and test dependencies are pinned in `python/requirements-dev.txt`:

```sh
python3 -m venv .venv && .venv/bin/pip install -r python/requirements-dev.txt -e python
cd python && ../.venv/bin/ruff check . && ../.venv/bin/ruff format --check . && ../.venv/bin/pytest
```

## Fuzzing

The `fuzz` preset (always Clang) builds a libFuzzer target that feeds raw bytes, split into lines, to the instrument's execute-line entry point under ASan and UBSan. Each line goes to one of two Sessions, picked by the low bit of its first byte, so cross-Session paths are fuzzed too. CI runs it for 60 seconds on every pull request; run it longer locally:

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p fuzz-corpus
build/fuzz/fuzz/execute_line_fuzzer -max_total_time=600 -max_len=8192 -dict=fuzz/scpi.dict \
  fuzz-corpus fuzz/corpus
```

New inputs go to `fuzz-corpus`, the first directory; `fuzz/corpus` holds the committed seeds. A crash leaves the input in a `crash-*` file; pass that file to the fuzzer to replay it.

## Benchmarks

The `benchmark` preset (Release) installs the vcpkg `benchmarks` feature (Google Benchmark), fetches KissFFT as a reference only, and builds `fft_benchmark`, which times our FFT against KissFFT, compiled with the same compiler and flags, from 4K to 1M points, and `ring_buffer_benchmark`, which times the SPSC ring buffer between two threads. CI runs both on every pull request and reports the numbers without gating on them; a crash fails the step. `docs/benchmarks.md` records measured results.

```sh
CC=gcc-14 CXX=g++-14 cmake --preset benchmark && cmake --build --preset benchmark
build/benchmark/bench/fft_benchmark
build/benchmark/bench/ring_buffer_benchmark
build/benchmark/bench/live_run 60   # live mode at 4 channels x 1 MS/s for 60 s (a sample rate may follow): prints skipped samples, drop counts and *IDN? latency
```
