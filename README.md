# OpenScope

A software oscilloscope engine in C++20: simulated signals are sampled, triggered into captures, measured, and exposed to scripts over SCPI. See `GLOSSARY.md` for the domain vocabulary and `docs/adr/` for the main design decisions.

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

## Fuzzing

The `fuzz` preset (always Clang) builds a libFuzzer target that feeds raw bytes, split into lines, to the instrument's execute-line entry point under ASan and UBSan. CI runs it for 60 seconds on every pull request; run it longer locally:

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p fuzz-corpus
build/fuzz/fuzz/execute_line_fuzzer -max_total_time=600 -max_len=8192 -dict=fuzz/scpi.dict \
  fuzz-corpus fuzz/corpus
```

New inputs go to `fuzz-corpus`, the first directory; `fuzz/corpus` holds the committed seeds. A crash leaves the input in a `crash-*` file; pass that file to the fuzzer to replay it.
