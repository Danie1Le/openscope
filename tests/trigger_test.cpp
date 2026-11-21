// The edge trigger. EdgeTrigger's own tests: which samples are trigger events.

#include "openscope/trigger.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace openscope {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

// Level 0 V with a 0.1 V hysteresis band, as for a 1 V/div trigger channel.
constexpr double kLevel = 0.0;
constexpr double kHysteresis = 0.1;

/// The trigger events `trigger` finds in `samples`, the next samples on its channel. Takes a
/// vector so tests can pass a braced list, which C++20 does not convert to a span.
std::vector<std::size_t> find(EdgeTrigger& trigger, const std::vector<float>& samples) {
  return trigger.find(samples);
}

TEST(EdgeTriggerTest, PositiveSlopeFiresWhereTheSignalReachesTheLevelFromBelowTheBand) {
  EdgeTrigger trigger{kLevel, TriggerSlope::kPositive, kHysteresis};

  EXPECT_THAT(find(trigger, {-0.5F, -0.2F, 0.0F, 0.5F, -0.5F, 0.3F}), ElementsAre(2, 5));
}

TEST(EdgeTriggerTest, NegativeSlopeFiresWhereTheSignalReachesTheLevelFromAboveTheBand) {
  EdgeTrigger trigger{kLevel, TriggerSlope::kNegative, kHysteresis};

  EXPECT_THAT(find(trigger, {0.5F, 0.2F, 0.0F, -0.5F, 0.5F, -0.3F}), ElementsAre(2, 5));
}

// For a falling edge the band lies above the level: 1.4 V is inside it, 1.6 V beyond it.
TEST(EdgeTriggerTest, NegativeSlopeBandLiesAboveTheLevel) {
  EdgeTrigger trigger{1.0, TriggerSlope::kNegative, 0.5};

  EXPECT_THAT(find(trigger, {1.4F, 1.0F, 1.6F, 1.0F, 0.0F}), ElementsAre(3));
}

TEST(EdgeTriggerTest, NoEventUntilTheSignalHasBeenBeyondTheBand) {
  EdgeTrigger trigger{kLevel, TriggerSlope::kPositive, kHysteresis};

  // Starting above the level is not an edge, and dipping inside the band does not arm it.
  EXPECT_THAT(find(trigger, {0.5F, -0.05F, 0.5F, -0.09F, 0.5F}), IsEmpty());
}

TEST(EdgeTriggerTest, ReachingTheBandEdgeArmsIt) {
  EdgeTrigger trigger{1.0, TriggerSlope::kPositive, 0.5};

  EXPECT_THAT(find(trigger, {0.5F, 1.0F}), ElementsAre(1));
}

// Samples are float, so a level such as 0.7 V is compared as the float nearest to it: a sample at
// the level fires and a sample at the band edge arms, whichever way the literal rounds.
TEST(EdgeTriggerTest, LevelAndBandEdgeAreInclusiveForLevelsInexactInBinary) {
  struct Case {
    double level;
    float below_band;  // The sample at level - kHysteresis.
    float above_band;  // The sample at level + kHysteresis.
  };

  for (const auto& [level, below_band, above_band] :
       {Case{0.7, 0.6F, 0.8F}, Case{0.3, 0.2F, 0.4F}}) {
    SCOPED_TRACE(level);
    const auto at_level = static_cast<float>(level);
    EdgeTrigger rising{level, TriggerSlope::kPositive, kHysteresis};
    EdgeTrigger falling{level, TriggerSlope::kNegative, kHysteresis};

    EXPECT_THAT(find(rising, {below_band, at_level}), ElementsAre(1));
    EXPECT_THAT(find(falling, {above_band, at_level}), ElementsAre(1));
  }
}

// Noise around the level re-crosses it many times; only a swing past the band re-arms.
TEST(EdgeTriggerTest, CrossingsInsideTheBandAfterAnEventAreIgnored) {
  EdgeTrigger trigger{kLevel, TriggerSlope::kPositive, kHysteresis};

  EXPECT_THAT(find(trigger, {-0.2F, 0.01F, -0.05F, 0.02F, -0.09F, 0.05F, -0.1F, 0.0F}),
              ElementsAre(1, 7));
}

TEST(EdgeTriggerTest, CarriesItsArmingFromOneBlockToTheNext) {
  EdgeTrigger trigger{kLevel, TriggerSlope::kPositive, kHysteresis};

  EXPECT_THAT(find(trigger, {0.5F, -0.5F}), IsEmpty());
  EXPECT_THAT(find(trigger, {0.5F}), ElementsAre(0));
  EXPECT_THAT(find(trigger, {}), IsEmpty());
  EXPECT_THAT(find(trigger, {0.6F}), IsEmpty());
}

}  // namespace
}  // namespace openscope
