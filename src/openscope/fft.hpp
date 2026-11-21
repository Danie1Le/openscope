#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

/// A radix-2 complex FFT of one fixed power-of-two length. Construction computes the twiddle
/// factors and every transform reuses them, as a KissFFT plan does, so the two time the same work.
class Fft {
 public:
  /// Prepares transforms of `length` points. Throws std::invalid_argument unless `length` is a
  /// power of two (1 included).
  explicit Fft(std::size_t length);

  /// Replaces `data` with its discrete Fourier transform, X[k] = sum x[n] e^(-2 pi i k n / N),
  /// unscaled. Throws std::invalid_argument when `data` does not hold the constructor's `length`
  /// points.
  void transform(std::span<std::complex<double>> data) const;

 private:
  std::size_t length_;
  /// e^(-2 pi i k / N) for k from 0 to N/2 - 1.
  std::vector<std::complex<double>> twiddles_;
};

/// The level `magnitude_spectrum_db` gives a bin with no signal, instead of minus infinity, so
/// every level is a finite float a client can plot.
inline constexpr float kSpectrumFloorDb = -300.0F;

/// The fewest samples a window shapes. Every window's mean is its coherent gain a_0 only from 4
/// samples on (fewer and the cosine terms no longer cancel), so shorter spectra are rectangular.
inline constexpr std::size_t kMinWindowedLength = 4;

/// The number of samples the spectrum of a capture with record length `record_length` uses: the
/// largest power of two that is not more than the record length. The radix-2 FFT needs a power of
/// two, and dropping the newest samples keeps every bin exact where zero padding would smear a tone
/// across bins.
std::size_t spectrum_length(std::size_t record_length);

/// The single-sided magnitude spectrum of the first spectrum_length(samples.size()) `samples`
/// (volts) under `window`, in dBV RMS: bins 0 (DC) to N/2 (half the sample rate), N/2 + 1 values,
/// with bin k at k sample rate / N hertz. The window's coherent gain is divided out, so a sine of
/// peak amplitude A centred on a bin reads 20 log10(A / sqrt 2) whatever the window, and DC reads
/// 20 log10 of its voltage. Below kMinWindowedLength samples `window` is ignored. Levels below
/// kSpectrumFloorDb read kSpectrumFloorDb. Throws std::invalid_argument when `samples` is empty.
std::vector<float> magnitude_spectrum_db(std::span<const float> samples, FftWindow window);

}  // namespace openscope
