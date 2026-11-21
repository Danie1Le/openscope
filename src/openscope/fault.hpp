#pragma once

#include <cstddef>
#include <cstdint>

#include "openscope/sample_clock.hpp"

namespace openscope {

/// The kinds of Fault (`SOURce<n>:FAULt`). The order matches the SCPI keywords.
enum class FaultKind : std::uint8_t { kGlitch, kDropout, kFrequencyStep };

/// What a kind of Fault does when its magnitude or duration is left out, and how far its magnitude
/// may go. The magnitude is volts for a glitch (added) and a dropout (the level held), and hertz
/// for a frequency step (added to the frequency).
struct FaultLimits {
  double default_magnitude;
  std::size_t default_duration;
  /// The magnitude is accepted from -max_magnitude to max_magnitude.
  double max_magnitude;
};

/// The longest Fault, in sample indexes.
inline constexpr std::size_t kMaxFaultDuration = std::size_t{1} << 20U;

/// The defaults and limits of `kind`.
[[nodiscard]] FaultLimits fault_limits(FaultKind kind);

/// A one-off disturbance a Generator adds to its signal over a run of sample indexes (GLOSSARY:
/// Fault). Its effect depends only on its own fields and the sample index, so the Generator's
/// output stays a function of the seed, its history and the index (ADR 0003).
class Fault {
 public:
  /// A Fault of `kind` covering `duration` sample indexes from `start`, at the `sample_rate` that
  /// turns a frequency step's hertz into a phase advance per sample. The magnitude is in the unit
  /// of FaultLimits.
  Fault(FaultKind kind, double magnitude, std::size_t duration, SampleIndex start,
        double sample_rate);

  /// The first sample index the Fault covers.
  [[nodiscard]] SampleIndex start() const { return start_; }

  /// The first sample index after the Fault.
  [[nodiscard]] SampleIndex end() const { return end_; }

  /// The cycles a frequency step has added to the phase by `index`: none up to its start, a
  /// steady gain over its duration and its whole gain after it, so the waveform carries on from
  /// where the step left it with no jump. Zero for the other kinds.
  [[nodiscard]] double phase_shift(SampleIndex index) const;

  /// `volts` as a glitch (which adds its magnitude) or a dropout (which replaces the sample with
  /// its level) leaves it at `index`; `volts` unchanged outside the Fault and for a frequency step.
  [[nodiscard]] double apply(SampleIndex index, double volts) const;

 private:
  FaultKind kind_;
  double magnitude_;
  SampleIndex start_;
  SampleIndex end_;
  /// The cycles per sample a frequency step adds, from its hertz at the sample rate it was made at.
  double cycles_per_sample_ = 0.0;
};

}  // namespace openscope
