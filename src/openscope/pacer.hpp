#pragma once

#include <chrono>
#include <cstdint>

namespace openscope {

/// Releases sample indexes at wall-clock speed in live mode. Time on the instrument stays the
/// sample index (ADR 0003); the pacer only decides when the next samples may be taken, so live
/// mode runs no faster than real time. Tests step the sample clock by hand and use no pacer.
class Pacer {
 public:
  using Clock = std::chrono::steady_clock;

  /// A pacer whose samples start falling due at `start`.
  explicit Pacer(Clock::time_point start) : anchor_(start) {}

  /// Releases the samples that have fallen due by `now` at `sample_rate` samples per second and
  /// not been released yet, and returns how many, but at most `most`: one per sample period that
  /// has passed. A caller that has fallen further behind than `most` skips the rest, so it
  /// carries on that far behind the wall clock rather than catching up in a burst. A new sample
  /// rate counts from when the last sample released fell due, so a change neither drops nor
  /// repeats samples.
  std::uint64_t release(Clock::time_point now, double sample_rate, std::uint64_t most);

 private:
  /// When the samples counted in released_ started falling due, at rate_.
  Clock::time_point anchor_;
  double rate_ = 0.0;
  std::uint64_t released_ = 0;
};

}  // namespace openscope
