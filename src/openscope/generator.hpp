#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "openscope/fault.hpp"
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
  /// Peak-to-peak amplitude in volts, the SCPI generator convention: the signal swings from
  /// offset - amplitude / 2 to offset + amplitude / 2. DC ignores it.
  double amplitude = 2.0;
  /// Volts added to the whole signal.
  double offset = 0.0;
  /// RMS volts of Gaussian noise added on top of the function.
  double noise = 0.0;
};

/// The simulated signal producer feeding one channel. Its output is a pure function of its seed,
/// the settings and Faults it has been given since construction, the sample indexes they took
/// effect at, and the sample index (ADR 0003): the same seed and the same configure and inject
/// calls at the same indexes give bit-identical samples on every run, and the noise at an index
/// never depends on which other indexes were generated.
class Generator {
 public:
  /// The most Faults a Generator keeps at once, which bounds the work done per sample.
  static constexpr std::size_t kMaxFaults = 64;

  /// A Generator with default settings and phase zero at sample index 0. Generators with
  /// different seeds produce independent noise.
  Generator(std::uint64_t seed, double sample_rate);

  /// What the Generator produces from the last configure call on.
  [[nodiscard]] const GeneratorSettings& settings() const { return settings_; }

  /// Applies `settings` at `sample_rate` from sample index `at` on. The phase at `at` stays what it
  /// was, so a change of frequency or sample rate never puts a jump in the waveform.
  void configure(const GeneratorSettings& settings, double sample_rate, SampleIndex at);

  /// Adds `fault` to the signal. Faults from before `fault`'s start that have ended are forgotten
  /// first, so only Faults still to be generated count towards kMaxFaults. Returns false and adds
  /// nothing when kMaxFaults are still in play. Faults change nothing before their start, so call
  /// this with a start at or after every index still to be generated, as the sample clock does.
  [[nodiscard]] bool inject(const Fault& fault);

  /// Writes the samples for indexes `first` to `first + samples.size() - 1`, in volts, under the
  /// current settings and Faults. Faults apply in the order they were injected.
  void generate(SampleIndex first, std::span<float> samples) const;

 private:
  /// The phase of the settings alone at `index`, in cycles and not yet wrapped into [0, 1).
  [[nodiscard]] double settings_cycles_at(SampleIndex index) const;
  /// The phase at `index` in cycles with the frequency steps, in [0, 1).
  [[nodiscard]] double phase_at(SampleIndex index) const;

  std::uint64_t seed_;
  GeneratorSettings settings_;
  /// The fraction of a cycle advanced per sample: frequency / sample rate without its whole cycles.
  double cycles_per_sample_;
  /// The phase is `anchor_phase_` at `anchor_index_` and advances by cycles_per_sample_ from there.
  SampleIndex anchor_index_ = 0;
  double anchor_phase_ = 0.0;
  /// The Faults injected and not yet forgotten, in injection order.
  std::vector<Fault> faults_;
};

}  // namespace openscope
