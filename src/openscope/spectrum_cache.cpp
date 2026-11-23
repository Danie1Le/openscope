#include "openscope/spectrum_cache.hpp"

#include <cstddef>
#include <memory>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/fft.hpp"
#include "openscope/fft_window.hpp"

namespace openscope {

std::shared_ptr<const std::vector<float>> SpectrumCache::spectrum(
    const std::shared_ptr<const Capture>& capture, std::size_t channel, FftWindow window) {
  const CapturedChannel& captured = captured_channel(*capture, channel);
  std::shared_ptr<const std::vector<float>>& cached = spectra_.of(capture)[{channel, window}];
  if (!cached) {
    cached =
        std::make_shared<const std::vector<float>>(magnitude_spectrum_db(captured.samples, window));
  }
  return cached;
}

}  // namespace openscope
