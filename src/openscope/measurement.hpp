#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"

namespace openscope {

/// The verdict that travels with every measured value, so a script never mistakes a bad value for
/// a real one.
enum class MeasurementStatus : std::uint8_t {
  /// The value is valid.
  kOk,
  /// The measurement needs edges or cycles and top minus base is under kNoSignalDivisions: the
  /// signal is flat or missing, so there is no value.
  kNoSignal,
  /// A sample in the capture hit an input-range limit: the value is reported but may be wrong.
  kClipped,
  /// Too little of the signal for the measurement (under one whole cycle, or no complete edge), so
  /// there is no value.
  kTooFewCycles,
};

/// One measurement of one channel of a capture.
struct MeasurementResult {
  /// Whether the value is valid, questionable (Clipped) or missing.
  MeasurementStatus status;
  /// The measured value in the measurement's unit; present for OK and Clipped only.
  std::optional<double> value;
};

/// The levels of one channel's samples that every measurement starts from.
struct SignalLevels {
  /// The lowest sample.
  double minimum;
  /// The highest sample.
  double maximum;
  /// The settled high level: the mean of the samples in the fullest bin of the upper half of the
  /// histogram, or the highest sample that is not an outlier when that half has no settled level.
  /// A spike or short glitch is never top.
  double top;
  /// The settled low level: the same from the lower half of the histogram, or the lowest sample
  /// that is not an outlier.
  double base;
};

/// The levels of `samples`, which hold at least one sample. Top and base come from a histogram,
/// so samples that stray from the rest briefly (a spike, a short glitch) do not move them.
SignalLevels find_levels(std::span<const float> samples);

/// What a measurement reads: one channel's samples from a capture and their levels.
struct MeasuredSignal {
  /// The channel's samples in volts, at least one.
  std::span<const float> samples;
  /// The levels of those samples.
  SignalLevels levels;
  /// Samples per second the capture was taken at, which turns sample counts into seconds.
  double sample_rate;
};

/// One automatic measurement: the SCPI mnemonic it is queried by and how it is computed. The
/// engine applies No signal and Clipped itself, so a new measurement is one function and one entry
/// in measurements().
struct Measurement {
  /// The header mnemonic of `MEASure:<mnemonic>?`, written like a header pattern (`VAVerage`). It
  /// views a string literal: measurements() is a constant table.
  std::string_view mnemonic;
  /// The value measured from `signal`, or nullopt when the signal holds too little for it (Too few
  /// cycles).
  std::optional<double> (*measure)(const MeasuredSignal& signal);
  /// True for a measurement that needs edges or cycles (the timing measurements), which a flat
  /// signal does not have: it reports No signal. A flat signal's amplitude is a real value.
  bool needs_signal = false;
};

/// Every automatic measurement, in a fixed order: a measurement's position here identifies it.
std::span<const Measurement> measurements();

/// Top minus base under this many vertical divisions is No signal.
inline constexpr double kNoSignalDivisions = 0.2;

/// `measurement` of `signal`, taken by a channel with `settings`, with its status: No signal first
/// (for a measurement that needs a signal), then Too few cycles, then Clipped.
MeasurementResult evaluate(const Measurement& measurement, const ChannelSettings& settings,
                           const MeasuredSignal& signal);

/// The measurement engine: measures channels of a capture on demand and caches each result for as
/// long as that capture is the one asked about, so repeated queries of the latest capture cost
/// nothing after the first.
class MeasurementEngine {
 public:
  /// Measurement `measurement` (a position in measurements()) of channel `channel` (1 to 4) of
  /// `capture`. Asking about a different capture drops every cached result. Throws
  /// std::out_of_range when `measurement` is not in measurements() or `channel` is not in
  /// `capture`.
  MeasurementResult measure(const std::shared_ptr<const Capture>& capture, std::size_t channel,
                            std::size_t measurement);

 private:
  /// One channel's levels and the results asked for so far, by measurement position.
  struct ChannelResults {
    SignalLevels levels;
    std::vector<std::optional<MeasurementResult>> results;
  };

  /// The capture the cached results belong to. A weak pointer, so the cache never keeps a capture
  /// alive; once that capture is gone it locks to null and matches no capture, even one that
  /// reuses its address.
  std::weak_ptr<const Capture> capture_;
  std::array<std::optional<ChannelResults>, kChannelCount> channels_;
};

}  // namespace openscope
