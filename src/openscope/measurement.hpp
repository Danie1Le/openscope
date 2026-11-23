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
  MeasurementStatus status = MeasurementStatus::kOk;
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

/// The whole cycles of a signal, each from one edge's 50% crossing to the next crossing in the same
/// direction, counted with edges (GLOSSARY) across 30% and 70%. Times are in sample periods.
struct WholeCycles {
  /// How many whole cycles there are.
  std::size_t count = 0;
  /// From the first edge's 50% crossing to that of the last edge that ends a whole cycle.
  double length = 0.0;
  /// How much of `length` the signal spent above the 50% level.
  double high = 0.0;
};

/// The complete edges (GLOSSARY) in one direction across the 10% and 90% reference levels: how
/// many, and how long they took from one level to the other, summed, in sample periods.
struct EdgeDurations {
  std::size_t count = 0;
  double total = 0.0;
};

/// The rising edges, which rise time averages, and the falling ones, which fall time averages.
struct RiseFallDurations {
  EdgeDurations rising;
  EdgeDurations falling;
};

/// What a measurement reads: one channel's samples from a capture, their levels, and what the
/// timing measurements share. The levels are found when it is made; the whole cycles and the
/// rise/fall edges on first use, once however many measurements read them (FREQ, PER and DUTY share
/// one search for whole cycles, RIS and FALL one for edges). Only those few figures are kept, not
/// the edges. Any thread may read it.
class MeasuredSignal {
 public:
  /// The signal of `samples` (at least one, in volts), taken at `sample_rate` samples per second.
  /// It views `samples`, which must outlive it.
  explicit MeasuredSignal(std::span<const float> samples, double sample_rate = 1.0);

  /// The channel's samples in volts.
  [[nodiscard]] std::span<const float> samples() const { return samples_; }
  /// The levels of those samples.
  [[nodiscard]] const SignalLevels& levels() const { return levels_; }
  /// Samples per second the capture was taken at, which turns sample counts into seconds.
  [[nodiscard]] double sample_rate() const { return sample_rate_; }
  /// The whole cycles, or nullopt when there is not one or they are not known: between two edges
  /// across 30% and 70%, the signal crossed 40% and 60% and back for long enough to be a cycle
  /// (GLOSSARY: Doubtful cycle).
  [[nodiscard]] const std::optional<WholeCycles>& whole_cycles() const;
  /// The complete edges across the 10% and 90% reference levels, rising and falling.
  [[nodiscard]] const RiseFallDurations& rise_fall() const;

 private:
  std::span<const float> samples_;
  SignalLevels levels_;
  double sample_rate_;
  mutable ComputedOnce<std::optional<WholeCycles>> whole_cycles_;
  mutable ComputedOnce<RiseFallDurations> rise_fall_;
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

/// Every measurement of every channel of one capture: indexed by channel number - 1, then by
/// position in measurements(); nullopt for a channel that is not in the capture.
using CaptureResults = std::array<std::optional<std::vector<MeasurementResult>>, kChannelCount>;

/// Every measurement of every channel in `capture`. It reads nothing but `capture` and keeps no
/// state, so any thread may call it without a lock.
CaptureResults measure_all(const Capture& capture);

/// The measurement engine: measures channels of a capture on demand and caches each result for as
/// long as that capture is the one asked about, so repeated queries of the latest capture cost
/// nothing after the first. Any thread may use it: Sessions measure at the same time, and two
/// asking for the same result share one computation.
class MeasurementEngine {
 public:
  /// Measurement `measurement` (a position in measurements()) of channel `channel` (1 to 4) of
  /// `capture`. Asking about a different capture starts the cache afresh. Throws
  /// std::out_of_range when `measurement` is not in measurements() or `channel` is not in
  /// `capture`.
  MeasurementResult measure(const std::shared_ptr<const Capture>& capture, std::size_t channel,
                            std::size_t measurement);

 private:
  /// One channel's signal and its results, by measurement position. The signal views the
  /// capture's samples: the cache drops it when another capture is asked about, and only callers
  /// holding its capture still read it.
  struct ChannelResults {
    ComputedOnce<std::unique_ptr<const MeasuredSignal>> signal;
    std::vector<ComputedOnce<MeasurementResult>> results =
        std::vector<ComputedOnce<MeasurementResult>>(measurements().size());
  };

  /// Each channel's results for the capture last asked about, by channel number - 1.
  CaptureCache<std::array<ChannelResults, kChannelCount>> channels_;
};

}  // namespace openscope
