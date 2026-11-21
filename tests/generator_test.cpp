#include "openscope/generator.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

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

}  // namespace
}  // namespace openscope
