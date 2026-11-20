# OpenScope

A software oscilloscope engine in C++20: simulated signals are sampled, triggered into captures, measured, and exposed to scripts over SCPI. See `GLOSSARY.md` for the domain vocabulary and `docs/adr/` for the main design decisions.

## Build and test

Requirements: Linux (developed on Ubuntu 24.04), GCC 14 or Clang 18, CMake 3.25+, Ninja, and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` pointing at it. Dependencies are declared in `vcpkg.json` and installed automatically on the first configure.

```sh
cmake --preset debug      # configure (pick the compiler with CXX=g++-14 or CXX=clang++-18)
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (AddressSanitizer + UndefinedBehaviorSanitizer), `tsan` (ThreadSanitizer) and `coverage` (always Clang, for llvm-cov). Each builds into `build/<preset>`. Warnings are errors in every preset.

Formatting and lint use the committed `.clang-format` and `.clang-tidy`:

```sh
clang-format-18 --dry-run --Werror $(git ls-files '*.cpp' '*.hpp')
run-clang-tidy-18 -p build/debug -quiet   # on a build configured with CXX=clang++-18
```
