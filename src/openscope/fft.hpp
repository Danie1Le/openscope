#pragma once

#include <complex>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/fft_window.hpp"

namespace openscope {

/// A radix-2 complex FFT of one fixed power-of-two length. Construction computes the twiddle
/// factors and every transform reuses them.
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

/// The spectra `FFT:DATA?` replies, computed once per capture, channel and window, as
/// MeasurementEngine computes measurements once per capture: a line of many `FFT:DATA?` on a
/// 1M-sample capture would otherwise transform it each time, about 0.1 s apiece (#67).
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
