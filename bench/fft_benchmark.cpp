// Our radix-2 FFT against KissFFT, the reference, from 4K to 1M samples; and the whole FFT:DATA?
// spectrum on top of it. Both FFTs are built by this project with the same compiler and flags
// (bench/CMakeLists.txt). Results are recorded in docs/benchmarks.md.

#include <benchmark/benchmark.h>
#include <kiss_fft.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "openscope/fft.hpp"

namespace openscope {
namespace {

static_assert(std::is_same_v<kiss_fft_scalar, double>, "the reference must be KissFFT's double");

/// `points` samples of a sine plus a smaller one at an unrelated frequency, so no bin is
/// trivially zero.
std::vector<float> test_signal(std::size_t points) {
  std::vector<float> samples(points);
  for (std::size_t n = 0; n < points; ++n) {
    const auto t = static_cast<double>(n);
    samples[n] = static_cast<float>(std::sin(0.01 * t) + (0.25 * std::sin(0.37 * t)));
  }
  return samples;
}

/// test_signal as the complex input of a transform, with zero imaginary parts.
std::vector<std::complex<double>> complex_test_signal(std::size_t points) {
  const std::vector<float> samples = test_signal(points);
  return {samples.begin(), samples.end()};
}

/// Reports points per second.
void set_counters(benchmark::State& state) {
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Google Benchmark's `for (auto _ : state)` loop never reads its variable by design.
// NOLINTBEGIN(clang-analyzer-deadcode.DeadStores)

/// Our transform. It works in place, so each iteration first copies the input back: a cost of
/// N complex copies that KissFFT, which writes to a separate output, does not pay.
void BM_OpenScopeFft(benchmark::State& state) {
  const auto points = static_cast<std::size_t>(state.range(0));
  const Fft fft(points);
  const std::vector<std::complex<double>> input = complex_test_signal(points);
  std::vector<std::complex<double>> data(points);
  for (auto _ : state) {
    data = input;
    fft.transform(data);
    benchmark::DoNotOptimize(data.data());
    benchmark::ClobberMemory();
  }
  set_counters(state);
}

/// The copy BM_OpenScopeFft makes before each transform, alone: how much of that time is not the
/// transform.
void BM_InputCopy(benchmark::State& state) {
  const auto points = static_cast<std::size_t>(state.range(0));
  const std::vector<std::complex<double>> input = complex_test_signal(points);
  std::vector<std::complex<double>> data(points);
  for (auto _ : state) {
    data = input;
    benchmark::DoNotOptimize(data.data());
    benchmark::ClobberMemory();
  }
  set_counters(state);
}

/// KissFFT's complex double transform of the same input, its plan made once outside the loop as
/// ours is.
void BM_KissFft(benchmark::State& state) {
  const auto points = static_cast<std::size_t>(state.range(0));
  const std::unique_ptr<kiss_fft_state, decltype(&kiss_fft_free)> plan(
      kiss_fft_alloc(static_cast<int>(points), 0, nullptr, nullptr), &kiss_fft_free);
  std::vector<kiss_fft_cpx> input;
  input.reserve(points);
  for (const float sample : test_signal(points)) {
    input.push_back({.r = sample, .i = 0.0});
  }
  std::vector<kiss_fft_cpx> output(points);
  for (auto _ : state) {
    kiss_fft(plan.get(), input.data(), output.data());
    benchmark::DoNotOptimize(output.data());
    benchmark::ClobberMemory();
  }
  set_counters(state);
}

/// What `FFT:DATA?` computes for one capture: window, transform and dB levels.
void BM_MagnitudeSpectrum(benchmark::State& state) {
  const std::vector<float> samples = test_signal(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(magnitude_spectrum_db(samples, FftWindow::kBlackmanHarris));
  }
  set_counters(state);
}

// NOLINTEND(clang-analyzer-deadcode.DeadStores)

/// Every benchmark runs at 4K, 16K, 64K, 256K and 1M samples, reported in microseconds.
void four_k_to_one_m_samples(benchmark::Benchmark* family) {
  family->RangeMultiplier(4)
      ->Range(std::int64_t{1} << 12U, std::int64_t{1} << 20U)
      ->Unit(benchmark::kMicrosecond);
}

BENCHMARK(BM_OpenScopeFft)->Apply(four_k_to_one_m_samples);
BENCHMARK(BM_InputCopy)->Apply(four_k_to_one_m_samples);
BENCHMARK(BM_KissFft)->Apply(four_k_to_one_m_samples);
BENCHMARK(BM_MagnitudeSpectrum)->Apply(four_k_to_one_m_samples);

}  // namespace
}  // namespace openscope
