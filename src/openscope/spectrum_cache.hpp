#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/fft_window.hpp"

namespace openscope {

/// The spectra `FFT:DATA?` replies, computed once per capture, channel and window, as
/// MeasurementEngine computes measurements once per capture, so a line of many `FFT:DATA?`
/// transforms a capture only once (#67). Any thread may use it: Sessions transform at the same
/// time, and two asking for the same spectrum share one computation.
class SpectrumCache {
 public:
  /// magnitude_spectrum_db of channel `channel` (1 to 4) of `capture` under `window`. Asking
  /// about a different capture drops every cached spectrum. Throws std::out_of_range when
  /// `channel` is not in `capture`.
  std::shared_ptr<const std::vector<float>> spectrum(const std::shared_ptr<const Capture>& capture,
                                                     std::size_t channel, FftWindow window);

 private:
  /// How many windows FftWindow names.
  static constexpr std::size_t kWindowCount =
      static_cast<std::size_t>(FftWindow::kBlackmanHarris) + 1;

  /// One capture's spectra, by channel number - 1 and window.
  using Spectra =
      std::array<std::array<ComputedOnce<std::vector<float>>, kWindowCount>, kChannelCount>;

  /// The spectra of the capture last asked about.
  CaptureCache<Spectra> spectra_;
};

}  // namespace openscope
