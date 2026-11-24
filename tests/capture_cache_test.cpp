// CaptureCache and ComputedOnce: the per-capture caches measurements and spectra share.

#include "openscope/capture_cache.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "openscope/capture.hpp"

namespace openscope {
namespace {

TEST(CaptureCacheTest, KeepsValuesWhileTheSameCaptureIsAskedAbout) {
  CaptureCache<int> cache;
  const auto capture = std::make_shared<const Capture>();

  *cache.of(capture) = 7;

  EXPECT_EQ(*cache.of(capture), 7);
}

TEST(CaptureCacheTest, StartsAfreshForAnotherCapture) {
  CaptureCache<int> cache;
  const auto first = std::make_shared<const Capture>();
  *cache.of(first) = 7;

  const auto second = std::make_shared<const Capture>();

  EXPECT_EQ(*cache.of(second), 0);
  EXPECT_EQ(*cache.of(first), 0);  // Asking about the first again starts afresh too.
}

// A weak pointer: the cache does not keep a capture alive, and a capture that takes the old
// one's place (possibly at its address) is not mistaken for it.
TEST(CaptureCacheTest, DoesNotKeepTheCaptureAlive) {
  CaptureCache<int> cache;
  auto capture = std::make_shared<const Capture>();
  const std::weak_ptr<const Capture> watched = capture;
  *cache.of(capture) = 7;

  capture = std::make_shared<const Capture>();

  EXPECT_TRUE(watched.expired());
  EXPECT_EQ(*cache.of(capture), 0);
}

// A query still using the values of an older capture keeps them when another capture resets the
// cache: Sessions query at the same time (#67).
TEST(CaptureCacheTest, ValuesOfAnOlderCaptureOutliveTheReset) {
  CaptureCache<int> cache;
  const auto first = std::make_shared<const Capture>();
  const std::shared_ptr<int> held = cache.of(first);
  *held = 7;

  (void)cache.of(std::make_shared<const Capture>());

  EXPECT_EQ(*held, 7);
}

TEST(ComputedOnceTest, ComputesOnceForCallersOnSeveralThreads) {
  ComputedOnce<int> value;
  std::atomic<int> computations = 0;
  const auto compute = [&] {
    ++computations;
    return 42;
  };
  std::array<int, 4> seen{};
  std::vector<std::thread> callers;
  callers.reserve(seen.size());
  for (int& result : seen) {
    callers.emplace_back([&] { result = value.get(compute); });
  }
  for (std::thread& caller : callers) {
    caller.join();
  }

  EXPECT_EQ(computations.load(), 1);
  EXPECT_THAT(seen, testing::Each(42));
}

}  // namespace
}  // namespace openscope
