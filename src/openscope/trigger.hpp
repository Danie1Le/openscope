#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace openscope {

/// The direction of the edge a trigger event marks (`TRIGger:SLOPe`). The order matches the SCPI
/// keywords.
enum class TriggerSlope : std::uint8_t {
  /// The trigger channel rises through the level.
  kPositive,
  /// The trigger channel falls through the level.
  kNegative,
};

/// What the instrument does when no trigger event arrives (`TRIGger:MODE`). The order matches the
/// SCPI keywords. Single is run control, not a trigger mode.
enum class TriggerMode : std::uint8_t {
  /// Takes an untriggered capture after one record length of samples without a trigger event.
  kAuto,
  /// Waits for a trigger event, however long that takes.
  kNormal,
};

/// The edge trigger's settings. The defaults are the `*RST` values.
struct TriggerSettings {
  /// The hysteresis band, in vertical divisions of the trigger channel, that the signal must pass
  /// on the far side of the level before the next trigger event (spec #1: fixed, not a setting).
  static constexpr double kHysteresisDivisions = 0.1;

  /// The trigger channel (1 to 4), which need not be enabled (`TRIGger:SOURce`).
  std::size_t source = 1;
  /// The trigger level in volts (`TRIGger:LEVel`).
  double level = 0.0;
  /// The edge that is a trigger event (`TRIGger:SLOPe`).
  TriggerSlope slope = TriggerSlope::kPositive;
  /// What happens without trigger events (`TRIGger:MODE`).
  TriggerMode mode = TriggerMode::kAuto;
  /// Seconds after a capture's trigger event before another can place a capture
  /// (`TRIGger:HOLDoff`). Counted in samples on the sample clock (ADR 0003).
  double holdoff = 0.0;
  /// How far into the capture time zero sits, as a percentage of the record
  /// (`TRIGger:POSition`): the share of the capture taken before the trigger event.
  double position = 50.0;

  /// The sample of a `record_length` capture (at least 1) where time zero sits: `position`
  /// percent of the way from the first sample to the last, rounded to the nearest sample.
  [[nodiscard]] std::size_t pre_trigger_samples(std::size_t record_length) const;
  /// `holdoff` as a count of samples at `sample_rate`, rounded to the nearest sample.
  [[nodiscard]] std::uint64_t holdoff_samples(double sample_rate) const;
};

/// Finds trigger events on the trigger channel: sample indexes where the signal reaches the level
/// in the slope's direction after first passing at least one hysteresis band beyond it on the
/// other side, so noise around the level does not fire it again. It sees every sample of the
/// trigger channel in order and knows nothing about captures, which decide what to do with each
/// event.
class EdgeTrigger {
 public:
  /// A trigger at `level` volts on the `slope` edge with a band of `hysteresis` volts. It starts
  /// unarmed: the first event needs the signal to pass the band first.
  EdgeTrigger(double level, TriggerSlope slope, double hysteresis);

  /// The offsets in `samples` of the trigger events among them, in order. `samples` are the next
  /// samples on the trigger channel after those of the previous call.
  std::vector<std::size_t> find(std::span<const float> samples);

 private:
  /// +1 for a positive slope, -1 for a negative one: samples and level are multiplied by it, so a
  /// falling edge is found as a rising one.
  double direction_;
  double level_;
  double hysteresis_;
  /// True once the signal has passed the band since the last trigger event.
  bool armed_ = false;
};

}  // namespace openscope
