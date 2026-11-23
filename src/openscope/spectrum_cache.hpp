#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/fft_window.hpp"

namespace openscope {

/// The spectra `FFT:DATA?` replies, computed once per capture, channel and window, as
/// MeasurementEngine computes measurements once per capture, so a line of many `FFT:DATA?`
/// transforms a capture only once (#67).
class SpectrumCache {
 public:
  /// magnitude_spectrum_db of channel `channel` (1 to 4) of `capture` under `window`. Asking
  /// about a different capture drops every cached spectrum. Throws std::out_of_range when
  /// `channel` is not in `capture`.
  std::shared_ptr<const std::vector<float>> spectrum(const std::shared_ptr<const Capture>& capture,
                                                     std::size_t channel, FftWindow window);

 private:
  /// The spectra of the capture last asked about, by channel number and window.
  CaptureCache<
      std::map<std::pair<std::size_t, FftWindow>, std::shared_ptr<const std::vector<float>>>>
      spectra_;
};

}  // namespace openscope
