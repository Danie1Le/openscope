// The queue between live mode's processing and measuring threads (#71): oldest first, bounded in
// captures and in samples, waiting rather than dropping, and released by a stop request.

#include "openscope/measuring_queue.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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
using test::kPatience;

/// A completed capture of `samples` samples, spread over two channels.
Completed capture_of(std::size_t samples) {
  Capture capture;
  capture.channels.at(0).emplace().samples.resize(samples / 2);
  capture.channels.at(2).emplace().samples.resize(samples - (samples / 2));
  return {.capture = std::make_shared<const Capture>(std::move(capture)), .generation = 0};
}

/// The oldest capture in `queue`, which must hold one: front would wait for it otherwise.
std::shared_ptr<const Capture> front_capture(MeasuringQueue& queue) {
  return queue.front({}).value_or(Completed{}).capture;
}

/// Long enough that a call still waiting after it is waiting, not just slow to start.
constexpr std::chrono::milliseconds kStillWaiting{50};

/// Whether `call` is still waiting after kStillWaiting.
template <typename T>
bool still_waiting(const std::future<T>& call) {
  return call.wait_for(kStillWaiting) == std::future_status::timeout;
}

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

  std::future<bool> pushed =
      std::async(std::launch::async, [&] { return queue.push(capture_of(10), {}); });

  EXPECT_TRUE(still_waiting(pushed));
  queue.pop();
  ASSERT_EQ(pushed.wait_for(kPatience), std::future_status::ready);
  EXPECT_TRUE(pushed.get());
}

TEST(MeasuringQueueTest, PushWaitsWhileItsSamplesWouldPassTheSampleLimit) {
  MeasuringQueue queue(4, 100);
  ASSERT_TRUE(queue.push(capture_of(60), {}));
  ASSERT_TRUE(queue.push(capture_of(40), {}));  // Exactly at the limit.

  std::future<bool> pushed =
      std::async(std::launch::async, [&] { return queue.push(capture_of(1), {}); });

  EXPECT_TRUE(still_waiting(pushed));
  queue.pop();  // Frees 60 samples.
  ASSERT_EQ(pushed.wait_for(kPatience), std::future_status::ready);
  EXPECT_TRUE(pushed.get());
}

TEST(MeasuringQueueTest, StopReleasesAPushWaitingForRoomWithoutAppending) {
  MeasuringQueue queue(1, 1000);
  const Completed queued = capture_of(10);
  ASSERT_TRUE(queue.push(queued, {}));
  std::stop_source stopping;

  std::future<bool> pushed = std::async(
      std::launch::async, [&] { return queue.push(capture_of(10), stopping.get_token()); });
  ASSERT_TRUE(still_waiting(pushed));
  stopping.request_stop();

  ASSERT_EQ(pushed.wait_for(kPatience), std::future_status::ready);
  EXPECT_FALSE(pushed.get());
  EXPECT_EQ(front_capture(queue), queued.capture);
  queue.pop();
  EXPECT_EQ(queue.front(stopping.get_token()), std::nullopt);  // Nothing else was appended.
}

TEST(MeasuringQueueTest, FrontWaitsForACapture) {
  MeasuringQueue queue(4, 1000);
  const Completed arriving = capture_of(10);

  std::future<std::optional<Completed>> taken =
      std::async(std::launch::async, [&] { return queue.front({}); });
  ASSERT_TRUE(still_waiting(taken));
  ASSERT_TRUE(queue.push(arriving, {}));

  ASSERT_EQ(taken.wait_for(kPatience), std::future_status::ready);
  const std::optional<Completed> front = taken.get();
  ASSERT_TRUE(front.has_value());
  EXPECT_EQ(front.value_or(Completed{}).capture, arriving.capture);
}

TEST(MeasuringQueueTest, StopReleasesAFrontWaitingForACapture) {
  MeasuringQueue queue(4, 1000);
  std::stop_source stopping;

  std::future<std::optional<Completed>> taken =
      std::async(std::launch::async, [&] { return queue.front(stopping.get_token()); });
  ASSERT_TRUE(still_waiting(taken));
  stopping.request_stop();

  ASSERT_EQ(taken.wait_for(kPatience), std::future_status::ready);
  EXPECT_EQ(taken.get(), std::nullopt);
}

}  // namespace
}  // namespace openscope
