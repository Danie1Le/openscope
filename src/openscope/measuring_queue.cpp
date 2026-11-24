#include "openscope/measuring_queue.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

#include "openscope/capture.hpp"
#include "openscope/completed_capture.hpp"

namespace openscope {
namespace {

/// The samples `capture` holds, summed over its channels.
std::size_t samples_in(const Capture& capture) {
  std::size_t samples = 0;
  for (const std::optional<CapturedChannel>& channel : capture.channels) {
    samples += channel ? channel->samples.size() : 0;
  }
  return samples;
}

}  // namespace

bool MeasuringQueue::push(CompletedCapture completed, const std::stop_token& stop) {
  const std::size_t samples = samples_in(*completed.capture);
  std::unique_lock lock(mutex_);
  const bool room = changed_.wait(lock, stop, [&] {
    return captures_.empty() ||
           (captures_.size() < capture_limit_ && samples_ + samples <= sample_limit_);
  });
  if (!room) {
    return false;
  }
  // Only the consumer waits for a capture, and only on an empty queue: a capture added to one
  // already holding others wakes no one, so short captures cost no wake each.
  const bool was_empty = captures_.empty();
  captures_.push_back(std::move(completed));
  samples_ += samples;
  lock.unlock();
  if (was_empty) {
    changed_.notify_all();
  }
  return true;
}

std::optional<CompletedCapture> MeasuringQueue::front(const std::stop_token& stop) {
  std::unique_lock lock(mutex_);
  if (!changed_.wait(lock, stop, [&] { return !captures_.empty(); })) {
    return std::nullopt;
  }
  return captures_.front();
}

void MeasuringQueue::pop() {
  {
    const std::scoped_lock lock(mutex_);
    samples_ -= samples_in(*captures_.front().capture);
    captures_.pop_front();
  }
  changed_.notify_all();  // The producer may wait for the room this made.
}

}  // namespace openscope
