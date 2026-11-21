// The phase helpers a Generator and its Faults share.

#include "openscope/phase.hpp"

#include <gtest/gtest.h>

namespace openscope {
namespace {

TEST(PhaseTest, CyclesPerSampleIsTheFrequencyOverTheSampleRate) {
  EXPECT_EQ(cycles_per_sample(250.0, 1000.0), 0.25);
  EXPECT_EQ(cycles_per_sample(0.0, 1000.0), 0.0);
}

TEST(PhaseTest, CyclesPerSampleDropsWholeCyclesExactly) {
  EXPECT_EQ(cycles_per_sample(3250.0, 1000.0), 0.25);
  EXPECT_EQ(cycles_per_sample(1000.0, 1000.0), 0.0);
}

TEST(PhaseTest, CyclesPerSampleKeepsTheSignOfABackwardsFrequency) {
  EXPECT_EQ(cycles_per_sample(-250.0, 1000.0), -0.25);
  EXPECT_EQ(cycles_per_sample(-3250.0, 1000.0), -0.25);
}

TEST(PhaseTest, WrapPhaseFoldsAnyNumberOfCyclesIntoZeroToOne) {
  EXPECT_EQ(wrap_phase(0.25), 0.25);
  EXPECT_EQ(wrap_phase(1.0), 0.0);
  EXPECT_EQ(wrap_phase(3.75), 0.75);
  EXPECT_EQ(wrap_phase(-0.25), 0.75);
  EXPECT_EQ(wrap_phase(-2.5), 0.5);
}

// -1e-20 cycles floors to -1, and -1e-20 - (-1) rounds to exactly 1, which is not a phase.
TEST(PhaseTest, WrapPhaseNeverReturnsOne) { EXPECT_EQ(wrap_phase(-1e-20), 0.0); }

}  // namespace
}  // namespace openscope
