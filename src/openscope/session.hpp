#pragma once

#include <deque>

#include "openscope/scpi_error.hpp"

namespace openscope {

/// The state one connected client owns (ADR 0002): its error queue. Everything else is
/// instrument-wide and lives in the Instrument.
class Session {
 public:
  /// Appends `error` behind the errors already queued.
  void push_error(ScpiError error) { errors_.push_back(error); }

  /// Removes and returns the oldest error, or `kNoError` when the queue is empty.
  ScpiError pop_error() {
    if (errors_.empty()) {
      return kNoError;
    }
    const ScpiError oldest = errors_.front();
    errors_.pop_front();
    return oldest;
  }

  /// Empties the error queue, as `*CLS` does for the calling Session only.
  void clear_errors() { errors_.clear(); }

 private:
  std::deque<ScpiError> errors_;
};

}  // namespace openscope
