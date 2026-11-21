#pragma once

#include <cstdint>
#include <span>

#include "openscope/sample_clock.hpp"

namespace openscope {

/// The shape a Generator produces (`SOURce<n>:FUNCtion`). The order matches the SCPI keywords.
enum class GeneratorFunction : std::uint8_t { kSine, kSquare, kTriangle, kRamp, kDc, kNoise };

/// What a Generator is set to produce. The defaults are the `*RST` values.
struct GeneratorSettings {
  /// The shape of each cycle.
  GeneratorFunction function = GeneratorFunction::kSine;
  /// Cycles per second of the periodic functions.
  double frequency = 1e3;
  /// Peak amplitude in volts: the signal swings from offset - amplitude to offset + amplitude.
  /// DC ignores it.
  double amplitude = 1.0;
  /// Volts added to the whole signal.
  double offset = 0.0;
  /// RMS volts of Gaussian noise added on top of the function.
  double noise = 0.0;
};

/// The simulated signal producer feeding one channel. Its output is a pure function of its seed,
/// its settings and the sample index (ADR 0003): the same seed and the same configure calls give
/// bit-identical samples on every run, and the noise at an index never depends on which other
/// indexes were generated.
class Generator {
 public:
  /// A Generator with default settings and phase zero at sample index 0. Generators with
  /// different seeds produce independent noise.
  Generator(std::uint64_t seed, double sample_rate);

  /// What the Generator produces from the last configure call on.
  [[nodiscard]] const GeneratorSettings& settings() const { return settings_; }

  /// Applies `settings` at `sample_rate` from sample index `at` on. The phase at `at` stays what it
  /// was, so a change of frequency or sample rate never puts a jump in the waveform.
  void configure(const GeneratorSettings& settings, double sample_rate, SampleIndex at);

  /// Writes the samples for indexes `first` to `first + samples.size() - 1`, in volts, under the
  /// current settings.
  void generate(SampleIndex first, std::span<float> samples) const;

 private:
  /// The phase at `index` in cycles, in [0, 1).
  [[nodiscard]] double phase_at(SampleIndex index) const;

  std::uint64_t seed_;
  GeneratorSettings settings_;
  /// The fraction of a cycle advanced per sample: frequency / sample rate without its whole cycles.
  double cycles_per_sample_;
  /// The phase is `anchor_phase_` at `anchor_index_` and advances by cycles_per_sample_ from there.
  SampleIndex anchor_index_ = 0;
  double anchor_phase_ = 0.0;
};

}  // namespace openscope
