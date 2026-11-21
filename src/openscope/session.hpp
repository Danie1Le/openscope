#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

#include "openscope/fft_window.hpp"
#include "openscope/scpi_error.hpp"

namespace openscope {

/// How `WAVeform:DATA?` writes samples (`WAVeform:FORMat`). The order matches the SCPI keywords.
enum class WaveformFormat : std::uint8_t {
  /// Comma-separated decimal values.
  kAscii,
  /// An IEEE 488.2 definite-length block of float32.
  kReal,
};

/// One Session's waveform-transfer settings. The defaults are a new Session's.
struct WaveformSettings {
  /// The channel (1 to 4) `WAV:DATA?` and `WAV:PRE?` read (`WAVeform:SOURce`).
  std::size_t source = 1;
  /// How `WAV:DATA?` writes the samples (`WAVeform:FORMat`).
  WaveformFormat format = WaveformFormat::kAscii;
};

/// One Session's FFT settings. The defaults are a new Session's.
struct FftSettings {
  /// The channel (1 to 4) `FFT:DATA?` and `FFT:SPAC?` read (`FFT:SOURce`).
  std::size_t source = 1;
  /// The window applied before the transform (`FFT:WINDow`).
  FftWindow window = FftWindow::kHann;
};

/// The state one connected client owns (ADR 0002): its error queue, its waveform-transfer
/// settings (`WAV:SOUR`, `WAV:FORM`) and its FFT settings (`FFT:SOUR`, `FFT:WIND`). Everything
/// that changes the signal or acquisition is instrument-wide and lives in the Instrument.
class Session {
 public:
  /// The most errors the queue holds, the -350 entry included.
  static constexpr std::size_t kErrorQueueCapacity = 20;

  /// Appends `error` behind the errors already queued. On a full queue it follows SCPI-99: the
  /// newest entry becomes -350 "Queue overflow" and later errors are dropped, so the oldest
  /// errors, which name the first cause of a failure, survive.
  void push_error(ScpiError error) {
    if (errors_.size() < kErrorQueueCapacity) {
      errors_.push_back(std::move(error));
    } else if (errors_.back().code != kQueueOverflow.code) {
      errors_.back() = kQueueOverflow;
    }
  }

  /// Removes and returns the oldest error, or `kNoError` when the queue is empty.
  ScpiError pop_error() {
    if (errors_.empty()) {
      return kNoError;
    }
    ScpiError oldest = std::move(errors_.front());
    errors_.pop_front();
    return oldest;
  }

  /// Empties the error queue, as `*CLS` does for the calling Session only.
  void clear_errors() { errors_.clear(); }

  /// This Session's `WAV:SOUR` and `WAV:FORM`, which no other Session sees and `*RST` leaves
  /// alone.
  WaveformSettings& waveform() { return waveform_; }

  /// This Session's `FFT:SOUR` and `FFT:WIND`, which no other Session sees and `*RST` leaves
  /// alone.
  FftSettings& fft() { return fft_; }

 private:
  std::deque<ScpiError> errors_;
  WaveformSettings waveform_;
  FftSettings fft_;
};

}  // namespace openscope
