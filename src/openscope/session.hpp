#pragma once

#include <cstddef>
#include <deque>
#include <utility>

#include "openscope/scpi_error.hpp"

namespace openscope {

/// The state one connected client owns (ADR 0002): its error queue and its waveform-transfer
/// settings (`WAV:SOUR`, `WAV:FORM`). Everything that changes the signal or acquisition is
/// instrument-wide and lives in the Instrument.
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

 private:
  std::deque<ScpiError> errors_;
};

}  // namespace openscope
