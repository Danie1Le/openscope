// A Fault on its own: the voltage a glitch or dropout leaves at an index, the phase a frequency
// step has gained, and the defaults and limits of each kind. Faults in a Generator's output are
// tested in generator_test.cpp.

#include "openscope/fault.hpp"

#include <gtest/gtest.h>

namespace openscope {
namespace {

constexpr double kSampleRate = 1000.0;

TEST(FaultTest, GlitchAddsItsMagnitudeOverItsDurationOnly) {
  const Fault glitch{FaultKind::kGlitch, 3.0, 2, 10, kSampleRate};

  EXPECT_EQ(glitch.start(), 10U);
  EXPECT_EQ(glitch.end(), 12U);
  EXPECT_EQ(glitch.apply(9, 0.5), 0.5);
  EXPECT_EQ(glitch.apply(10, 0.5), 3.5);
  EXPECT_EQ(glitch.apply(11, 0.5), 3.5);
  EXPECT_EQ(glitch.apply(12, 0.5), 0.5);
  EXPECT_EQ(glitch.phase_shift(11), 0.0);
}

TEST(FaultTest, DropoutReplacesTheSampleWithItsLevelOverItsDuration) {
  const Fault dropout{FaultKind::kDropout, -0.25, 3, 4, kSampleRate};

  EXPECT_EQ(dropout.apply(3, 1.0), 1.0);
  EXPECT_EQ(dropout.apply(4, 1.0), -0.25);
  EXPECT_EQ(dropout.apply(6, 1.0), -0.25);
  EXPECT_EQ(dropout.apply(7, 1.0), 1.0);
  EXPECT_EQ(dropout.phase_shift(6), 0.0);
}

// 250 Hz at 1 kSa/s is a quarter cycle per sample: the shift is 0 at the start, grows a quarter
// cycle per sample over the 2-sample duration, then stays at half a cycle.
TEST(FaultTest, FrequencyStepGainsPhaseOverItsDurationAndKeepsItAfter) {
  const Fault step{FaultKind::kFrequencyStep, 250.0, 2, 4, kSampleRate};

  EXPECT_EQ(step.phase_shift(0), 0.0);
  EXPECT_EQ(step.phase_shift(4), 0.0);
  EXPECT_EQ(step.phase_shift(5), 0.25);
  EXPECT_EQ(step.phase_shift(6), 0.5);
  EXPECT_EQ(step.phase_shift(7), 0.5);
  EXPECT_EQ(step.phase_shift(1'000'000), 0.5);
  EXPECT_EQ(step.apply(5, 1.0), 1.0);
}

TEST(FaultTest, FrequencyStepGoesBackwardsForANegativeMagnitude) {
  const Fault step{FaultKind::kFrequencyStep, -250.0, 2, 0, kSampleRate};

  EXPECT_EQ(step.phase_shift(2), -0.5);
}

TEST(FaultTest, FrequencyStepDropsWholeCyclesOfItsFrequency) {
  const Fault step{FaultKind::kFrequencyStep, (3 * kSampleRate) + 250.0, 1, 0, kSampleRate};

  EXPECT_EQ(step.phase_shift(1), 0.25);
}

TEST(FaultTest, EachKindHasItsOwnDefaultsAndLimits) {
  const FaultLimits glitch = fault_limits(FaultKind::kGlitch);
  EXPECT_EQ(glitch.default_magnitude, 2.0);
  EXPECT_EQ(glitch.default_duration, 1U);
  EXPECT_EQ(glitch.max_magnitude, 100.0);

  const FaultLimits dropout = fault_limits(FaultKind::kDropout);
  EXPECT_EQ(dropout.default_magnitude, 0.0);
  EXPECT_EQ(dropout.default_duration, 100U);
  EXPECT_EQ(dropout.max_magnitude, 100.0);

  const FaultLimits step = fault_limits(FaultKind::kFrequencyStep);
  EXPECT_EQ(step.default_magnitude, 1e3);
  EXPECT_EQ(step.default_duration, 1000U);
  EXPECT_EQ(step.max_magnitude, 1e9);
}

}  // namespace
}  // namespace openscope
