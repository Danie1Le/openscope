#pragma once

#include <cstddef>
#include <cstdint>

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
};

}  // namespace openscope
