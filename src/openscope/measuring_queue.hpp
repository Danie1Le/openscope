#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

#include "openscope/completed_capture.hpp"

namespace openscope {

/// The captures that live mode's processing thread has published and its measuring thread has yet
/// to measure and count, oldest first (#71). It is bounded twice, so its memory stays
/// small whatever the record length: by captures, for many short ones, and by samples summed over
/// channels, for long ones. A capture always gets in when the queue is empty, so even one longer
/// than the sample limit never waits forever. One thread pushes and one other thread takes.
class MeasuringQueue {
 public:
  /// A queue of at most `capture_limit` captures and `sample_limit` samples.
  MeasuringQueue(std::size_t capture_limit, std::size_t sample_limit)
      : capture_limit_(capture_limit), sample_limit_(sample_limit) {}

  /// Producer only: appends `completed`, first waiting until there is room for it. Returns false,
  /// without appending, when `stop` is requested while it waits.
  [[nodiscard]] bool push(CompletedCapture completed, const std::stop_token& stop);

  /// Consumer only: the oldest capture, first waiting until there is one; nullopt when `stop` is
  /// requested while it waits. The capture stays in the queue, and counts against its limits,
  /// until `pop`, so one being measured takes room too.
  std::optional<CompletedCapture> front(const std::stop_token& stop);

  /// Consumer only: removes the oldest capture, the one `front` returned.
  void pop();

 private:
  std::size_t capture_limit_;
  std::size_t sample_limit_;
  std::mutex mutex_;
  /// Notified when a capture joins an empty queue and whenever one leaves.
  std::condition_variable_any changed_;
  /// The captures, oldest first, and their samples summed. Both need mutex_.
  std::deque<CompletedCapture> captures_;
  std::size_t samples_ = 0;
};

}  // namespace openscope
