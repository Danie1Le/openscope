#pragma once

#include <cstddef>
#include <cstdint>

namespace openscope {

/// The window applied to the samples before the FFT (`FFT:WINDow`). The order matches the SCPI
/// keywords. Each trades frequency resolution against leakage: rectangular has the narrowest main
/// lobe and the most leakage, Blackman-Harris the widest lobe and the least.
enum class FftWindow : std::uint8_t {
  /// No window: every sample weighs the same.
  kRectangular,
  /// The periodic Hann window, 0.5 - 0.5 cos(2 pi n / N).
  kHann,
  /// The periodic 4-term Blackman-Harris window with sidelobes below -92 dB (Harris 1978).
  kBlackmanHarris,
};

/// How many windows FftWindow names.
inline constexpr std::size_t kFftWindowCount =
    static_cast<std::size_t>(FftWindow::kBlackmanHarris) + 1;

}  // namespace openscope
