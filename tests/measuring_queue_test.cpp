// The queue between live mode's processing and measuring threads (#71): oldest first, bounded in
// captures and in samples, waiting rather than dropping, and released by a stop request.

#include "openscope/measuring_queue.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "eventually.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"

namespace openscope {
namespace {

using Completed = Instrument::CompletedCapture;

/// A completed capture of `samples` samples, spread over two channels.
Completed capture_of(std::size_t samples) {
  Capture capture;
  capture.channels.at(0).emplace().samples.resize(samples / 2);
  capture.channels.at(2).emplace().samples.resize(samples - (samples / 2));
  return {.capture = std::make_shared<const Capture>(std::move(capture)), .statistics_epoch = 0};
}

/// The oldest capture in `queue`, which must hold one: front would wait for it otherwise.
std::shared_ptr<const Capture> front_capture(MeasuringQueue& queue) {
  return queue.front({}).value_or(Completed{}).capture;
}

/// A queue call that may wait, run on a thread of its own with a stop token. Going out of scope
/// requests the stop, so a call a failing test left waiting ends instead of hanging the test.
template <typename Result>
class Waiting {
 public:
  explicit Waiting(std::function<Result(const std::stop_token&)> call)
      : result_(std::async(std::launch::async, [this, call = std::move(call)] {
          return call(stopping_.get_token());
        })) {}
  Waiting(const Waiting&) = delete;
  Waiting& operator=(const Waiting&) = delete;
  Waiting(Waiting&&) = delete;
  Waiting& operator=(Waiting&&) = delete;
  ~Waiting() { stopping_.request_stop(); }

  /// True when the call has not returned 50 ms on: long enough that it waits, not just starts.
  [[nodiscard]] bool still_waiting() const {
    return result_.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
  }

  /// Requests the stop the call was given.
  void stop() { stopping_.request_stop(); }

  /// What the call returned, or nullopt when it has not within test::kPatience.
  std::optional<Result> result() {
    if (result_.wait_for(test::kPatience) != std::future_status::ready) {
      return std::nullopt;
    }
    return result_.get();
  }

 private:
  std::stop_source stopping_;
  std::future<Result> result_;
};

TEST(MeasuringQueueTest, TakesCapturesOldestFirst) {
  MeasuringQueue queue(4, 1000);
  const Completed first = capture_of(10);
  const Completed second = capture_of(10);

  ASSERT_TRUE(queue.push(first, {}));
  ASSERT_TRUE(queue.push(second, {}));

  EXPECT_EQ(front_capture(queue), first.capture);
  EXPECT_EQ(front_capture(queue), first.capture);  // Taken only by pop.
  queue.pop();
  EXPECT_EQ(front_capture(queue), second.capture);
}

// However long, a capture never waits forever: an empty queue always takes it.
TEST(MeasuringQueueTest, EmptyQueueTakesACaptureOverTheSampleLimit) {
  MeasuringQueue queue(4, 100);

  EXPECT_TRUE(queue.push(capture_of(1000), {}));
}

// Processing waits for room rather than dropping a capture; the one being measured, returned by
// front and not yet popped, takes room too.
TEST(MeasuringQueueTest, PushWaitsWhileTheCaptureLimitIsReached) {
  MeasuringQueue queue(2, 1000);
  ASSERT_TRUE(queue.push(capture_of(10), {}));
  ASSERT_TRUE(queue.push(capture_of(10), {}));
  ASSERT_TRUE(queue.front({}));  // Being measured.

  Waiting<bool> pushed(
      [&](const std::stop_token& stop) { return queue.push(capture_of(10), stop); });

  EXPECT_TRUE(pushed.still_waiting());
  queue.pop();
  EXPECT_EQ(pushed.result(), true);
}

TEST(MeasuringQueueTest, PushWaitsWhileItsSamplesWouldPassTheSampleLimit) {
  MeasuringQueue queue(4, 100);
  ASSERT_TRUE(queue.push(capture_of(60), {}));
  ASSERT_TRUE(queue.push(capture_of(40), {}));  // Exactly at the limit.

  Waiting<bool> pushed(
      [&](const std::stop_token& stop) { return queue.push(capture_of(1), stop); });

  EXPECT_TRUE(pushed.still_waiting());
  queue.pop();  // Frees 60 samples.
  EXPECT_EQ(pushed.result(), true);
}

TEST(MeasuringQueueTest, StopReleasesAPushWaitingForRoomWithoutAppending) {
  MeasuringQueue queue(1, 1000);
  const Completed queued = capture_of(10);
  ASSERT_TRUE(queue.push(queued, {}));

  Waiting<bool> pushed(
      [&](const std::stop_token& stop) { return queue.push(capture_of(10), stop); });
  ASSERT_TRUE(pushed.still_waiting());
  pushed.stop();

  EXPECT_EQ(pushed.result(), false);
  EXPECT_EQ(front_capture(queue), queued.capture);
  queue.pop();
  const std::stop_source stopped;
  stopped.request_stop();
  EXPECT_EQ(queue.front(stopped.get_token()), std::nullopt);  // Nothing else was appended.
}

TEST(MeasuringQueueTest, FrontWaitsForACapture) {
  MeasuringQueue queue(4, 1000);
  const Completed arriving = capture_of(10);

  Waiting<std::optional<Completed>> taken(
      [&](const std::stop_token& stop) { return queue.front(stop); });
  ASSERT_TRUE(taken.still_waiting());
  ASSERT_TRUE(queue.push(arriving, {}));

  const std::optional<std::optional<Completed>> front = taken.result();
  ASSERT_TRUE(front.has_value());
  EXPECT_EQ(front.value_or(std::nullopt).value_or(Completed{}).capture, arriving.capture);
}

TEST(MeasuringQueueTest, StopReleasesAFrontWaitingForACapture) {
  MeasuringQueue queue(4, 1000);

  Waiting<std::optional<Completed>> taken(
      [&](const std::stop_token& stop) { return queue.front(stop); });
  ASSERT_TRUE(taken.still_waiting());
  taken.stop();

  const std::optional<std::optional<Completed>> front = taken.result();
  ASSERT_TRUE(front.has_value());  // It returned.
  EXPECT_EQ(front.value_or(Completed{}), std::nullopt);
}

}  // namespace
}  // namespace openscope
