// The pacer that releases samples at wall-clock speed in live mode, driven with made-up time
// points so every test is exact.

#include "openscope/pacer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace openscope {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;

constexpr Pacer::Clock::time_point kStart{std::chrono::hours(1)};

/// A limit no test reaches.
constexpr auto kNoLimit = std::numeric_limits<std::uint64_t>::max();

TEST(PacerTest, NothingIsDueAtTheStart) {
  Pacer pacer(kStart);

  EXPECT_EQ(pacer.release(kStart, 1e6, kNoLimit), 0U);
}

TEST(PacerTest, ReleasesOneSamplePerSamplePeriodThatHasPassed) {
  Pacer pacer(kStart);

  EXPECT_EQ(pacer.release(kStart + milliseconds(1), 1e6, kNoLimit), 1000U);
  EXPECT_EQ(pacer.release(kStart + microseconds(1500), 1e6, kNoLimit), 500U);
}

TEST(PacerTest, ReleasesEachSampleOnce) {
  Pacer pacer(kStart);
  ASSERT_EQ(pacer.release(kStart + milliseconds(1), 1e6, kNoLimit), 1000U);

  EXPECT_EQ(pacer.release(kStart + milliseconds(1), 1e6, kNoLimit), 0U);
}

TEST(PacerTest, TimeBeforeTheStartReleasesNothing) {
  Pacer pacer(kStart);

  EXPECT_EQ(pacer.release(kStart - milliseconds(1), 1e6, kNoLimit), 0U);
}

// 1.5 samples fall due per millisecond: the half samples carry over, so the total never drifts
// from the wall clock.
TEST(PacerTest, PartSamplePeriodsCarryOver) {
  Pacer pacer(kStart);

  EXPECT_EQ(pacer.release(kStart + milliseconds(1), 1500.0, kNoLimit), 1U);
  EXPECT_EQ(pacer.release(kStart + milliseconds(2), 1500.0, kNoLimit), 2U);
  std::uint64_t total = 3;
  for (int ms = 3; ms <= 1000; ++ms) {
    total += pacer.release(kStart + milliseconds(ms), 1500.0, kNoLimit);
  }
  EXPECT_EQ(total, 1500U);
}

// The new rate counts from when the last sample released fell due (1 ms in), not from the
// start, so the samples already released are neither released again nor lost.
TEST(PacerTest, NewSampleRateCountsFromTheLastSampleReleased) {
  Pacer pacer(kStart);
  ASSERT_EQ(pacer.release(kStart + milliseconds(1), 1e6, kNoLimit), 1000U);

  EXPECT_EQ(pacer.release(kStart + milliseconds(2), 2e6, kNoLimit), 2000U);
  EXPECT_EQ(pacer.release(kStart + milliseconds(3), 2e6, kNoLimit), 2000U);
}

TEST(PacerTest, NewSampleRateAfterAPartSamplePeriodLosesNoTime) {
  Pacer pacer(kStart);
  // Due at 1 and 2 ms.
  ASSERT_EQ(pacer.release(kStart + microseconds(2500), 1000.0, kNoLimit), 2U);

  // 1 ms at the new rate.
  EXPECT_EQ(pacer.release(kStart + milliseconds(3), 2000.0, kNoLimit), 2U);
}

TEST(PacerTest, NothingIsSkippedWhileTheCallerKeepsUp) {
  Pacer pacer(kStart);

  ASSERT_EQ(pacer.release(kStart + milliseconds(4), 1e6, 4000), 4000U);

  EXPECT_EQ(pacer.skipped(), 0U);
}

// 10 ms behind with room for 4 ms: the other 6 ms are skipped and counted, and pacing carries on
// from there.
TEST(PacerTest, CallerTooFarBehindSkipsAndCountsWhatIsOverTheLimit) {
  Pacer pacer(kStart);

  EXPECT_EQ(pacer.release(kStart + milliseconds(10), 1e6, 4000), 4000U);
  EXPECT_EQ(pacer.skipped(), 6000U);
  EXPECT_EQ(pacer.release(kStart + milliseconds(11), 1e6, 4000), 1000U);
  EXPECT_EQ(pacer.skipped(), 6000U);
}

// A sample rate the caller never sustains: each call is over the limit, and every sample that
// fell due is either released or counted as skipped, across a rate change too.
TEST(PacerTest, ReleasedAndSkippedAddUpToEverySampleDue) {
  Pacer pacer(kStart);
  std::uint64_t released = 0;
  for (int ms = 1; ms <= 100; ++ms) {
    released += pacer.release(kStart + milliseconds(ms), 1e8, 65536);
  }
  for (int ms = 101; ms <= 200; ++ms) {
    released += pacer.release(kStart + milliseconds(ms), 1e9, 65536);
  }

  EXPECT_EQ(released, 200U * 65536U);
  EXPECT_EQ(released + pacer.skipped(), 10'000'000U + 100'000'000U);
}

}  // namespace
}  // namespace openscope
