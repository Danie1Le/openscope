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
  const std::shared_ptr<Spectra> spectra = spectra_.of(capture);
  const std::vector<float>& spectrum =
      spectra->at(channel - 1).at(static_cast<std::size_t>(window)).get([&] {
        return magnitude_spectrum_db(captured.samples, window);
      });
  // Shares ownership of the capture's spectra, so the reply outlives a newer capture's reset.
  return {spectra, &spectrum};
}

}  // namespace openscope
