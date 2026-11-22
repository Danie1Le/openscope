#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/measurement.hpp"
#include "openscope/running_statistics.hpp"

namespace openscope {

/// Every measurement of every channel of one capture: indexed by channel number - 1, then by
/// position in measurements(); nullopt for a channel that is not in the capture.
using CaptureResults = std::array<std::optional<std::vector<MeasurementResult>>, kChannelCount>;

/// Every measurement of every channel in `capture`. It reads nothing but `capture` and keeps no
/// state, so any thread may call it without a lock.
CaptureResults measure_all(const Capture& capture);

/// One measurement of one channel across the captures since the last statistics reset.
struct MeasurementSeries {
  /// The result of the most recent capture, whatever its status.
  MeasurementResult current{};
  /// The values of the results that had one (OK and Clipped). No signal and Too few cycles have
  /// no value, so they are left out, of the count too.
  RunningStatistics values;
  /// True when one of `values` came from a Clipped result, so the statistics may be wrong.
  bool clipped = false;

  /// Makes `result` the current result and counts its value, if it has one.
  void add(const MeasurementResult& result);
};

/// Measurement statistics (GLOSSARY): every measurement of every channel across the captures
/// since the last reset. Not thread safe: it changes with every capture added and every setting
/// change, so its owner guards it the way it guards its settings.
class MeasurementStatistics {
 public:
  /// Counts one capture's results. Every capture completed since the last reset is added, oldest
  /// first, untriggered ones included (spec #1, story 65).
  void add(const CaptureResults& results);

  /// Forgets every capture added.
  void reset() { *this = {}; }

  /// True when no capture has been added since the last reset.
  [[nodiscard]] bool empty() const { return empty_; }

  /// The series of measurement `measurement` (a position in measurements()) of channel `channel`
  /// (1 to 4). Throws std::out_of_range when no capture added since the last reset holds the
  /// channel, or `measurement` is not in measurements().
  [[nodiscard]] const MeasurementSeries& at(std::size_t channel, std::size_t measurement) const;

 private:
  bool empty_ = true;
  /// By channel number - 1, then by measurement position; nullopt for a channel not captured.
  std::array<std::optional<std::vector<MeasurementSeries>>, kChannelCount> channels_;
};

}  // namespace openscope
