#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "openscope/capture.hpp"

namespace openscope {

/// A capture `Instrument::process` has published but not yet counted, with the statistics reset
/// it completed after: what `Instrument::count_measured` needs to count it later, on another
/// thread (#71).
struct CompletedCapture {
  std::shared_ptr<const Capture> capture;
  /// How many statistics resets came before the capture completed.
  std::uint64_t statistics_epoch = 0;
};

/// Takes each capture `Instrument::process` publishes, in order, for `Instrument::count_measured`.
using CaptureHandOff = std::function<void(CompletedCapture completed)>;

}  // namespace openscope
