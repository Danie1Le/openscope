#include "openscope/fft.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/fft_window.hpp"

namespace openscope {
namespace {

using Complex = std::complex<double>;

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The coefficients a_m of a cosine-sum window, w[n] = sum a_m cos(2 pi m n / N). a_0 is the
/// window's coherent gain: its mean over N points whenever N > m for every term, which holds from
/// kMinWindowedLength on.
std::span<const double> window_coefficients(FftWindow window) {
  static constexpr std::array<double, 1> kRectangular{1.0};
  static constexpr std::array<double, 2> kHann{0.5, -0.5};
  static constexpr std::array<double, 4> kBlackmanHarris{0.35875, -0.48829, 0.14128, -0.01168};
  switch (window) {
    case FftWindow::kRectangular:
      return kRectangular;
    case FftWindow::kHann:
      return kHann;
    case FftWindow::kBlackmanHarris:
      return kBlackmanHarris;
  }
  throw std::invalid_argument("unknown FFT window");
}

/// The periodic window value at sample `n` of `length`: periodic rather than symmetric, so a tone
/// centred on a bin spreads over exactly the window's main lobe and nowhere else.
double window_at(std::span<const double> coefficients, std::size_t n, std::size_t length) {
  double value = coefficients[0];
  if (coefficients.size() == 1) {
    return value;
  }
  // One cos per sample: the higher harmonics follow from cos((m+1)x) = 2 cos x cos mx - cos(m-1)x.
  const double cos_x = std::cos(kTwoPi * static_cast<double>(n) / static_cast<double>(length));
  double cos_previous = 1.0;
  double cos_m = cos_x;
  for (std::size_t m = 1; m < coefficients.size(); ++m) {
    value += coefficients[m] * cos_m;
    const double cos_next = (2.0 * cos_x * cos_m) - cos_previous;
    cos_previous = cos_m;
    cos_m = cos_next;
  }
  return value;
}

}  // namespace

Fft::Fft(std::size_t length) : length_(length) {
  if (!std::has_single_bit(length)) {
    throw std::invalid_argument("FFT length must be a power of two");
  }
  twiddles_.reserve(length / 2);
  for (std::size_t k = 0; k < length / 2; ++k) {
    // Each factor straight from cos and sin: a recurrence would accumulate rounding error.
    twiddles_.push_back(
        std::polar(1.0, -kTwoPi * static_cast<double>(k) / static_cast<double>(length)));
  }
}

void Fft::transform(std::span<Complex> data) const {
  if (data.size() != length_) {
    throw std::invalid_argument("FFT data length does not match the transform");
  }
  // Iterative decimation in time: put the points in bit-reversed order, then combine pairs of
  // ever longer sub-transforms with butterflies.
  for (std::size_t i = 1, j = 0; i < length_; ++i) {
    std::size_t bit = length_ >> 1U;
    for (; (j & bit) != 0; bit >>= 1U) {
      j ^= bit;
    }
    j |= bit;
    if (i < j) {
      std::swap(data[i], data[j]);
    }
  }
  for (std::size_t half = 1; half < length_; half *= 2) {
    const std::size_t stride = length_ / (2 * half);
    for (std::size_t start = 0; start < length_; start += 2 * half) {
      for (std::size_t k = 0; k < half; ++k) {
        const Complex even = data[start + k];
        const Complex odd = data[start + k + half] * twiddles_[k * stride];
        data[start + k] = even + odd;
        data[start + k + half] = even - odd;
      }
    }
  }
}

std::size_t spectrum_length(std::size_t record_length) { return std::bit_floor(record_length); }

std::vector<float> magnitude_spectrum_db(std::span<const float> samples, FftWindow window) {
  if (samples.empty()) {
    throw std::invalid_argument("no samples for the spectrum");
  }
  const std::size_t length = spectrum_length(samples.size());
  const std::span<const double> coefficients =
      window_coefficients(length < kMinWindowedLength ? FftWindow::kRectangular : window);
  std::vector<Complex> data(length);
  for (std::size_t n = 0; n < length; ++n) {
    data[n] = samples[n] * window_at(coefficients, n, length);
  }
  Fft(length).transform(data);

  // |X[k]| / (N a_0) is the peak amplitude a bin-centred tone contributes to each of its two
  // mirrored bins. Bins 1 to N/2 - 1 fold both halves, so their RMS is twice that over sqrt 2; DC
  // and N/2 have no mirror and their RMS is the amplitude itself. Levels come from |X|^2 with
  // 10 log10, which needs no square root.
  const double scale = 1.0 / (static_cast<double>(length) * coefficients[0]);
  std::vector<float> levels((length / 2) + 1);
  for (std::size_t k = 0; k < levels.size(); ++k) {
    const bool unmirrored = k == 0 || k == length / 2;
    const double gain = scale * (unmirrored ? 1.0 : std::numbers::sqrt2);
    const double power = std::norm(data[k]) * gain * gain;
    levels[k] = std::max(static_cast<float>(10.0 * std::log10(power)), kSpectrumFloorDb);
  }
  return levels;
}

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
