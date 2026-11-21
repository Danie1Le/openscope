#include "openscope/generator.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "openscope/fault.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {
namespace {

using testing::ElementsAre;
using testing::FloatNear;

constexpr double kSampleRate = 1000.0;

/// `count` samples of `generator` from index `first`.
std::vector<float> samples(const Generator& generator, SampleIndex first, std::size_t count) {
  std::vector<float> out(count);
  generator.generate(first, out);
  return out;
}

/// A Generator at `sample_rate` configured at index 0 with `settings`.
Generator make(const GeneratorSettings& settings, std::uint64_t seed = 0,
               double sample_rate = kSampleRate) {
  Generator generator{seed, sample_rate};
  generator.configure(settings, sample_rate, 0);
  return generator;
}

TEST(GeneratorTest, DefaultsAreATwoVoltPeakToPeakOneKilohertzSine) {
  const Generator generator{0, 1e6};

  EXPECT_EQ(generator.settings().function, GeneratorFunction::kSine);
  EXPECT_EQ(generator.settings().frequency, 1e3);
  EXPECT_EQ(generator.settings().amplitude, 2.0);
  EXPECT_EQ(generator.settings().offset, 0.0);
  EXPECT_EQ(generator.settings().noise, 0.0);
  // A quarter cycle of 1 kHz at 1 MSa/s is 250 samples.
  EXPECT_THAT(samples(generator, 0, 1), ElementsAre(0.0F));
  EXPECT_THAT(samples(generator, 250, 1), ElementsAre(1.0F));
  EXPECT_THAT(samples(generator, 750, 1), ElementsAre(-1.0F));
}

TEST(GeneratorTest, SineSpansItsPeakToPeakAmplitudeAroundItsOffset) {
  const Generator generator = make({.function = GeneratorFunction::kSine,
                                    .frequency = 250.0,
                                    .amplitude = 4.0,
                                    .offset = 0.5,
                                    .noise = 0.0});

  constexpr float kTolerance = 1e-6F;
  EXPECT_THAT(samples(generator, 0, 5),
              ElementsAre(FloatNear(0.5F, kTolerance), FloatNear(2.5F, kTolerance),
                          FloatNear(0.5F, kTolerance), FloatNear(-1.5F, kTolerance),
                          FloatNear(0.5F, kTolerance)));
}

TEST(GeneratorTest, SquareIsHighForTheFirstHalfOfEachCycle) {
  const Generator generator = make({.function = GeneratorFunction::kSquare,
                                    .frequency = 250.0,
                                    .amplitude = 4.0,
                                    .offset = 1.0,
                                    .noise = 0.0});

  EXPECT_THAT(samples(generator, 0, 6), ElementsAre(3.0F, 3.0F, -1.0F, -1.0F, 3.0F, 3.0F));
}

TEST(GeneratorTest, TriangleRisesFromZeroToPeakThenFallsToTrough) {
  const Generator generator = make({.function = GeneratorFunction::kTriangle,
                                    .frequency = 125.0,
                                    .amplitude = 2.0,
                                    .offset = 0.0,
                                    .noise = 0.0});

  EXPECT_THAT(samples(generator, 0, 9),
              ElementsAre(0.0F, 0.5F, 1.0F, 0.5F, 0.0F, -0.5F, -1.0F, -0.5F, 0.0F));
}

TEST(GeneratorTest, RampRisesFromTroughAcrossEachCycle) {
  const Generator generator = make({.function = GeneratorFunction::kRamp,
                                    .frequency = 125.0,
                                    .amplitude = 2.0,
                                    .offset = 0.0,
                                    .noise = 0.0});

  EXPECT_THAT(samples(generator, 0, 9),
              ElementsAre(-1.0F, -0.75F, -0.5F, -0.25F, 0.0F, 0.25F, 0.5F, 0.75F, -1.0F));
}

TEST(GeneratorTest, DcIsItsOffsetAndIgnoresAmplitudeAndFrequency) {
  const Generator generator = make({.function = GeneratorFunction::kDc,
                                    .frequency = 123.0,
                                    .amplitude = 5.0,
                                    .offset = -1.25,
                                    .noise = 0.0});

  EXPECT_THAT(samples(generator, 0, 3), ElementsAre(-1.25F, -1.25F, -1.25F));
}

TEST(GeneratorTest, NoiseFunctionSpansItsPeakToPeakAmplitudeAroundItsOffset) {
  const Generator generator = make({.function = GeneratorFunction::kNoise,
                                    .frequency = 1.0,
                                    .amplitude = 4.0,
                                    .offset = 1.0,
                                    .noise = 0.0});

  const std::vector<float> noise = samples(generator, 0, 100'000);

  const auto [low, high] = std::ranges::minmax(noise);
  EXPECT_GE(low, -1.0F);
  EXPECT_LT(high, 3.0F);
  // Uniform over [-1, 3): it reaches close to both ends and averages the offset.
  EXPECT_LT(low, -0.99F);
  EXPECT_GT(high, 2.99F);
  const double mean =
      std::accumulate(noise.begin(), noise.end(), 0.0) / static_cast<double>(noise.size());
  EXPECT_NEAR(mean, 1.0, 0.02);
}

TEST(GeneratorTest, AddedNoiseHasTheRequestedRms) {
  const Generator generator = make({.function = GeneratorFunction::kDc,
                                    .frequency = 1.0,
                                    .amplitude = 0.0,
                                    .offset = 2.0,
                                    .noise = 0.1});

  const std::vector<float> noisy = samples(generator, 0, 100'000);

  double sum = 0.0;
  double sum_of_squares = 0.0;
  for (const float sample : noisy) {
    sum += sample - 2.0;
    sum_of_squares += (sample - 2.0) * (sample - 2.0);
  }
  const auto count = static_cast<double>(noisy.size());
  EXPECT_NEAR(sum / count, 0.0, 0.002);
  EXPECT_NEAR(std::sqrt(sum_of_squares / count), 0.1, 0.002);
}

TEST(GeneratorTest, NoiseAtAnIndexDoesNotDependOnWhichRangeWasGenerated) {
  const Generator generator = make({.function = GeneratorFunction::kNoise,
                                    .frequency = 1.0,
                                    .amplitude = 1.0,
                                    .offset = 0.0,
                                    .noise = 0.5});

  const std::vector<float> whole = samples(generator, 100, 50);
  std::vector<float> pieces = samples(generator, 100, 20);
  const std::vector<float> rest = samples(generator, 120, 30);
  pieces.insert(pieces.end(), rest.begin(), rest.end());

  EXPECT_EQ(pieces, whole);
}

TEST(GeneratorTest, SameSeedGivesIdenticalSamplesAndAnotherSeedDoesNot) {
  const GeneratorSettings noisy{.function = GeneratorFunction::kSine,
                                .frequency = 50.0,
                                .amplitude = 1.0,
                                .offset = 0.0,
                                .noise = 0.2};

  const std::vector<float> first = samples(make(noisy, 7), 0, 1000);

  EXPECT_EQ(samples(make(noisy, 7), 0, 1000), first);
  EXPECT_NE(samples(make(noisy, 8), 0, 1000), first);
}

TEST(GeneratorTest, NoiseFunctionAndAddedNoiseAreIndependent) {
  const Generator function_only = make({.function = GeneratorFunction::kNoise,
                                        .frequency = 1.0,
                                        .amplitude = 1.0,
                                        .offset = 0.0,
                                        .noise = 0.0});
  const Generator added_only = make({.function = GeneratorFunction::kDc,
                                     .frequency = 1.0,
                                     .amplitude = 0.0,
                                     .offset = 0.0,
                                     .noise = 1.0});

  const std::vector<float> a = samples(function_only, 0, 1000);
  const std::vector<float> b = samples(added_only, 0, 1000);

  double correlation = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    correlation += static_cast<double>(a[i]) * b[i];
  }
  EXPECT_LT(std::abs(correlation / static_cast<double>(a.size())), 0.1);
}

// Phase continuity: after a frequency change at index 1 the ramp keeps rising from where it was
// (-0.5) at the new rate, instead of jumping to the phase the new frequency has from index 0.
TEST(GeneratorTest, FrequencyChangeKeepsThePhaseContinuous) {
  GeneratorSettings ramp{.function = GeneratorFunction::kRamp,
                         .frequency = 250.0,
                         .amplitude = 2.0,
                         .offset = 0.0,
                         .noise = 0.0};
  Generator generator = make(ramp);
  EXPECT_THAT(samples(generator, 0, 2), ElementsAre(-1.0F, -0.5F));

  ramp.frequency = 125.0;
  generator.configure(ramp, kSampleRate, 1);

  EXPECT_THAT(samples(generator, 1, 4), ElementsAre(-0.5F, -0.25F, 0.0F, 0.25F));
}

TEST(GeneratorTest, SampleRateChangeKeepsThePhaseContinuous) {
  const GeneratorSettings ramp{.function = GeneratorFunction::kRamp,
                               .frequency = 250.0,
                               .amplitude = 2.0,
                               .offset = 0.0,
                               .noise = 0.0};
  Generator generator = make(ramp);

  generator.configure(ramp, 2 * kSampleRate, 3);  // Phase 0.75 at index 3.

  EXPECT_THAT(samples(generator, 3, 4), ElementsAre(0.5F, 0.75F, -1.0F, -0.75F));
}

TEST(GeneratorTest, RepeatedChangesEachKeepThePhaseContinuous) {
  GeneratorSettings ramp{.function = GeneratorFunction::kRamp,
                         .frequency = 125.0,
                         .amplitude = 2.0,
                         .offset = 0.0,
                         .noise = 0.0};
  Generator generator = make(ramp);
  for (SampleIndex at = 1; at <= 4; ++at) {
    ramp.frequency = at % 2 == 0 ? 125.0 : 250.0;
    generator.configure(ramp, kSampleRate, at);
  }
  // Steps of 1/8, 1/4, 1/8, 1/4 then 1/8 per sample from phase 0: 0, 1/8, 3/8, 1/2, 3/4, 7/8.
  EXPECT_THAT(samples(generator, 4, 2), ElementsAre(0.5F, 0.75F));
}

TEST(GeneratorTest, ConfigureReplacesTheSettings) {
  Generator generator{0, kSampleRate};
  const GeneratorSettings square{.function = GeneratorFunction::kSquare,
                                 .frequency = 10.0,
                                 .amplitude = 3.0,
                                 .offset = -1.0,
                                 .noise = 0.5};

  generator.configure(square, kSampleRate, 0);

  EXPECT_EQ(generator.settings().function, GeneratorFunction::kSquare);
  EXPECT_EQ(generator.settings().frequency, 10.0);
  EXPECT_EQ(generator.settings().amplitude, 3.0);
  EXPECT_EQ(generator.settings().offset, -1.0);
  EXPECT_EQ(generator.settings().noise, 0.5);
}

TEST(GeneratorTest, PhaseStaysExactFarAlongTheSampleClock) {
  const Generator generator = make({.function = GeneratorFunction::kRamp,
                                    .frequency = 125.0,
                                    .amplitude = 2.0,
                                    .offset = 0.0,
                                    .noise = 0.0});

  // 8e12 samples is 1e12 whole cycles: the ramp is back at the start of its cycle.
  EXPECT_THAT(samples(generator, 8'000'000'000'000, 3), ElementsAre(-1.0F, -0.75F, -0.5F));
}

// Whole cycles per sample never change the phase, so a frequency above the sample rate samples
// exactly like its alias below it, even far along the sample clock.
TEST(GeneratorTest, FrequencyAboveTheSampleRateMatchesItsAliasFarAlongTheSampleClock) {
  const auto sine_at = [](double frequency) {
    return make({.function = GeneratorFunction::kSine,
                 .frequency = frequency,
                 .amplitude = 2.0,
                 .offset = 0.0,
                 .noise = 0.0},
                0, 1e6);
  };

  constexpr SampleIndex kFarAlong = 100'000'000'000;
  EXPECT_EQ(samples(sine_at(100'000'037.0), kFarAlong, 16), samples(sine_at(37.0), kFarAlong, 16));
  EXPECT_EQ(samples(sine_at(1'000'000'037.0), kFarAlong, 16),
            samples(sine_at(37.0), kFarAlong, 16));
}

TEST(GeneratorTest, IndexesBeforeTheLastChangeFollowTheNewSettingsBackwards) {
  Generator generator{0, kSampleRate};
  generator.configure({.function = GeneratorFunction::kRamp,
                       .frequency = 125.0,
                       .amplitude = 2.0,
                       .offset = 0.0,
                       .noise = 0.0},
                      kSampleRate, 8);  // The default sine is at phase 8 * 1 kHz / 1 kSa/s = 0.

  EXPECT_THAT(samples(generator, 6, 3), ElementsAre(0.5F, 0.75F, -1.0F));
}

/// A 250 Hz square wave of +-1 V at 1 kSa/s, which runs 1, 1, -1, -1 in a repeat of 4 samples, so
/// a fault shows up in exact values.
Generator make_square() {
  return make({.function = GeneratorFunction::kSquare,
               .frequency = 250.0,
               .amplitude = 2.0,
               .offset = 0.0,
               .noise = 0.0});
}

/// The Fault `kind` of `magnitude` over `duration` samples from `start` at the test sample rate.
Fault fault(FaultKind kind, double magnitude, std::size_t duration, SampleIndex start) {
  return {kind, magnitude, duration, start, kSampleRate};
}

TEST(GeneratorFaultTest, GlitchAddsItsMagnitudeToTheSignalForItsDurationOnly) {
  Generator generator = make_square();

  ASSERT_TRUE(generator.inject(fault(FaultKind::kGlitch, 0.5, 2, 5)));

  // Without the glitch: 1, 1, -1, -1, 1, 1, -1, -1, 1, 1.
  EXPECT_THAT(samples(generator, 0, 10),
              ElementsAre(1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.5F, -0.5F, -1.0F, 1.0F, 1.0F));
}

TEST(GeneratorFaultTest, NegativeGlitchGoesBelowTheSignal) {
  Generator generator = make_square();

  ASSERT_TRUE(generator.inject(fault(FaultKind::kGlitch, -3.0, 1, 1)));

  EXPECT_THAT(samples(generator, 0, 3), ElementsAre(1.0F, -2.0F, -1.0F));
}

TEST(GeneratorFaultTest, DropoutHoldsItsLevelForItsDurationOnly) {
  Generator generator = make_square();

  ASSERT_TRUE(generator.inject(fault(FaultKind::kDropout, 0.25, 3, 2)));

  EXPECT_THAT(samples(generator, 0, 8),
              ElementsAre(1.0F, 1.0F, 0.25F, 0.25F, 0.25F, 1.0F, -1.0F, -1.0F));
}

TEST(GeneratorFaultTest, DropoutReplacesTheNoiseToo) {
  Generator generator = make({.function = GeneratorFunction::kSine,
                              .frequency = 50.0,
                              .amplitude = 1.0,
                              .offset = 0.0,
                              .noise = 0.2});
  const std::vector<float> clean = samples(generator, 0, 12);

  ASSERT_TRUE(generator.inject(fault(FaultKind::kDropout, 0.0, 4, 4)));

  const std::vector<float> faulty = samples(generator, 0, 12);
  EXPECT_EQ(std::vector<float>(faulty.begin() + 4, faulty.begin() + 8),
            (std::vector<float>{0.0F, 0.0F, 0.0F, 0.0F}));
  EXPECT_EQ(std::vector<float>(faulty.begin(), faulty.begin() + 4),
            std::vector<float>(clean.begin(), clean.begin() + 4));
  EXPECT_EQ(std::vector<float>(faulty.begin() + 8, faulty.end()),
            std::vector<float>(clean.begin() + 8, clean.end()));
}

// +250 Hz for 2 samples from index 4 moves the square half a cycle for good: the samples of the
// step (4, 5) follow the doubled frequency, and the waveform then carries on shifted.
TEST(GeneratorFaultTest, FrequencyStepShiftsThePhaseWithoutAJumpAndKeepsTheShift) {
  Generator generator = make_square();

  ASSERT_TRUE(generator.inject(fault(FaultKind::kFrequencyStep, 250.0, 2, 4)));

  // Phases: 4 -> 0, 5 -> 0.5, 6 -> 0.0 (0.5 + 0.5), 7 -> 0.25, 8 -> 0.5. Unfaulted: 1, 1, -1, -1.
  EXPECT_THAT(samples(generator, 0, 12), ElementsAre(1.0F, 1.0F, -1.0F, -1.0F, 1.0F, -1.0F, 1.0F,
                                                     1.0F, -1.0F, -1.0F, 1.0F, 1.0F));
}

TEST(GeneratorFaultTest, FrequencyStepMovesTheRampAContinuousStepAtAHigherSlope) {
  Generator generator = make({.function = GeneratorFunction::kRamp,
                              .frequency = 125.0,
                              .amplitude = 2.0,
                              .offset = 0.0,
                              .noise = 0.0});

  // 125 Hz is 0.125 cycles per sample; +125 Hz makes it 0.25 for 4 samples from index 2.
  ASSERT_TRUE(generator.inject(fault(FaultKind::kFrequencyStep, 125.0, 4, 2)));

  // Phases 0, .125, .25, then .5, .75, 1.0 (= 0) at .25 per sample, then .25, .375, .5 at .125
  // per sample again: the step added half a cycle for good.
  EXPECT_THAT(samples(generator, 0, 9),
              ElementsAre(-1.0F, -0.75F, -0.5F, 0.0F, 0.5F, -1.0F, -0.5F, -0.25F, 0.0F));
}

TEST(GeneratorFaultTest, FrequencyStepsAddTheirShifts) {
  Generator generator = make_square();

  ASSERT_TRUE(generator.inject(fault(FaultKind::kFrequencyStep, 250.0, 1, 0)));
  ASSERT_TRUE(generator.inject(fault(FaultKind::kFrequencyStep, 250.0, 1, 0)));

  // Each adds a quarter cycle from index 1 on, half a cycle together.
  EXPECT_THAT(samples(generator, 0, 6), ElementsAre(1.0F, -1.0F, 1.0F, 1.0F, -1.0F, -1.0F));
}

TEST(GeneratorFaultTest, FrequencyStepKeepsItsHertzWhenTheFrequencyChangesDuringIt) {
  Generator generator = make({.function = GeneratorFunction::kRamp,
                              .frequency = 250.0,
                              .amplitude = 2.0,
                              .offset = 0.0,
                              .noise = 0.0});
  ASSERT_TRUE(generator.inject(fault(FaultKind::kFrequencyStep, 250.0, 4, 4)));

  generator.configure({.function = GeneratorFunction::kRamp,
                       .frequency = 125.0,
                       .amplitude = 2.0,
                       .offset = 0.0,
                       .noise = 0.0},
                      kSampleRate, 6);

  // Phase 0 at 6; then .125 + .25 per sample until the step ends at 8 (.375 -> .75), and .125
  // per sample on the shifted phase after it (.75 + .125 = .875 at index 9).
  EXPECT_THAT(samples(generator, 6, 4), ElementsAre(-1.0F, -0.25F, 0.5F, 0.75F));
}

TEST(GeneratorFaultTest, FrequencyStepDoesNothingToDcOrNoise) {
  for (const GeneratorFunction function : {GeneratorFunction::kDc, GeneratorFunction::kNoise}) {
    const GeneratorSettings settings{
        .function = function, .frequency = 250.0, .amplitude = 2.0, .offset = 0.5, .noise = 0.0};
    Generator faulty = make(settings, 3);
    const Generator clean = make(settings, 3);

    ASSERT_TRUE(faulty.inject(fault(FaultKind::kFrequencyStep, 250.0, 4, 2)));

    EXPECT_EQ(samples(faulty, 0, 12), samples(clean, 0, 12));
  }
}

TEST(GeneratorFaultTest, GlitchAndDropoutApplyToDcToo) {
  Generator generator = make({.function = GeneratorFunction::kDc,
                              .frequency = 1.0,
                              .amplitude = 0.0,
                              .offset = 0.5,
                              .noise = 0.0});

  ASSERT_TRUE(generator.inject(fault(FaultKind::kGlitch, 1.0, 2, 1)));
  ASSERT_TRUE(generator.inject(fault(FaultKind::kDropout, 0.0, 1, 2)));

  EXPECT_THAT(samples(generator, 0, 5), ElementsAre(0.5F, 1.5F, 0.0F, 0.5F, 0.5F));
}

TEST(GeneratorFaultTest, FaultsApplyInTheOrderTheyWereInjected) {
  Generator glitch_then_dropout = make_square();
  Generator dropout_then_glitch = make_square();

  ASSERT_TRUE(glitch_then_dropout.inject(fault(FaultKind::kGlitch, 1.0, 1, 0)));
  ASSERT_TRUE(glitch_then_dropout.inject(fault(FaultKind::kDropout, 0.5, 1, 0)));
  ASSERT_TRUE(dropout_then_glitch.inject(fault(FaultKind::kDropout, 0.5, 1, 0)));
  ASSERT_TRUE(dropout_then_glitch.inject(fault(FaultKind::kGlitch, 1.0, 1, 0)));

  EXPECT_THAT(samples(glitch_then_dropout, 0, 2), ElementsAre(0.5F, 1.0F));
  EXPECT_THAT(samples(dropout_then_glitch, 0, 2), ElementsAre(1.5F, 1.0F));
}

TEST(GeneratorFaultTest, FaultsChangeNothingBeforeTheirStart) {
  Generator generator = make_square();
  const std::vector<float> clean = samples(generator, 0, 100);

  ASSERT_TRUE(generator.inject(fault(FaultKind::kGlitch, 5.0, 3, 50)));
  ASSERT_TRUE(generator.inject(fault(FaultKind::kFrequencyStep, 500.0, 3, 60)));

  const std::vector<float> faulty = samples(generator, 0, 100);
  EXPECT_EQ(std::vector<float>(faulty.begin(), faulty.begin() + 50),
            std::vector<float>(clean.begin(), clean.begin() + 50));
}

TEST(GeneratorFaultTest, FaultOutputIsAFunctionOfTheSeedAndTheInjectionsOnly) {
  const auto run = [](std::uint64_t seed) {
    Generator generator = make({.function = GeneratorFunction::kSine,
                                .frequency = 50.0,
                                .amplitude = 1.0,
                                .offset = 0.0,
                                .noise = 0.2},
                               seed);
    EXPECT_TRUE(generator.inject(fault(FaultKind::kGlitch, 1.0, 5, 100)));
    EXPECT_TRUE(generator.inject(fault(FaultKind::kFrequencyStep, 30.0, 40, 103)));
    return generator;
  };

  const std::vector<float> first = samples(run(7), 0, 400);

  EXPECT_EQ(samples(run(7), 0, 400), first);
  // Any range of indexes reads the same as inside the whole run.
  const std::vector<float> tail = samples(run(7), 120, 100);
  EXPECT_EQ(tail, std::vector<float>(first.begin() + 120, first.begin() + 220));
  EXPECT_NE(samples(run(8), 0, 400), first);
}

TEST(GeneratorFaultTest, FinishedFaultsAreForgottenWithTheirPhaseKept) {
  Generator forgotten = make_square();
  Generator kept = make_square();
  ASSERT_TRUE(forgotten.inject(fault(FaultKind::kFrequencyStep, 250.0, 2, 4)));
  ASSERT_TRUE(kept.inject(fault(FaultKind::kFrequencyStep, 250.0, 2, 4)));

  // Index 100 is long after both faults, so injecting there forgets them.
  ASSERT_TRUE(forgotten.inject(fault(FaultKind::kGlitch, 0.0, 1, 100)));

  EXPECT_EQ(samples(forgotten, 100, 12), samples(kept, 100, 12));
}

TEST(GeneratorFaultTest, AtMostMaxFaultsAreInPlayAndFinishedOnesFreeASlot) {
  Generator generator = make_square();
  for (std::size_t i = 0; i < Generator::kMaxFaults; ++i) {
    ASSERT_TRUE(generator.inject(fault(FaultKind::kGlitch, 1.0, 2, 10)));
  }
  const std::vector<float> full = samples(generator, 0, 20);

  EXPECT_FALSE(generator.inject(fault(FaultKind::kGlitch, 1.0, 2, 10)));
  EXPECT_FALSE(generator.inject(fault(FaultKind::kGlitch, 1.0, 2, 11)));  // Still running at 11.
  EXPECT_EQ(samples(generator, 0, 20), full);

  EXPECT_TRUE(generator.inject(fault(FaultKind::kGlitch, 1.0, 2, 12)));  // They ended at 12.
}

}  // namespace
}  // namespace openscope
