// Timing measurements: MEAS:FREQ?, PER? and DUTY? from 50% reference-level crossings with
// hysteresis, averaged over the whole cycles in a capture; Too few cycles when there is less than
// one.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "instrument_fixture.hpp"
#include "measurement_fixture.hpp"
#include "openscope/channel.hpp"
#include "openscope/instrument.hpp"
#include "openscope/measurement.hpp"
#include "openscope/scpi/response.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::kClippedReply;
using test::kNoErrorReply;
using test::kResetRecordLength;
using test::kTooFewCyclesReply;
using test::kUntriggeredCaptures;
using test::MeasurementTest;
using test::number;
using test::pulse_train;
using test::ScpiSession;
using testing::DoubleNear;

using TimingMeasurementTest = MeasurementTest;

/// Every timing measurement of channel 1, short form, in one line.
constexpr auto kTimingQueries = "MEAS:FREQ? CHAN1;PER? CHAN1;DUTY? CHAN1";

TEST_F(TimingMeasurementTest, SquareWaveRepliesAreExact) {
  session.send("SOUR1:FUNC SQU;:CHAN2:DISP ON;:SOUR2:FUNC SQU;FREQ 2.5KHZ");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:FREQ? CHAN1;PER? CHAN1;DUTY? CHAN1"), "1000;0.001;50");
  EXPECT_EQ(session.send("MEAS:FREQ? CHAN2;PER? CHAN2;DUTY? CHAN2"), "2500;4E-04;50");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(TimingMeasurementTest, LongFormsAreAccepted) {
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEASure:FREQuency? CHANnel1;PERiod? CHANnel1;DUTYcycle? CHANnel1"),
            session.send(kTimingQueries));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

/// A clean sine's frequency and the acquisition that captures it.
struct SineTiming {
  double frequency;
  double sample_rate;
  std::size_t record_length;
};

class CleanSineTimingTest : public MeasurementTest,
                            public testing::WithParamInterface<SineTiming> {};

// NFR-5: frequency within 0.1% of the Generator's on a clean sine with at least 10 cycles and 10
// samples per cycle, including captures ending part-way through a cycle and the longest record.
// The 50% crossings of a sine split each cycle in half.
TEST_P(CleanSineTimingTest, FrequencyIsWithinATenthOfAPercent) {
  const SineTiming sine = GetParam();
  session.send(std::format("ACQ:SRAT {};POIN {};:SOUR1:FREQ {}", sine.sample_rate,
                           sine.record_length, sine.frequency));
  instrument.advance(sine.record_length);

  EXPECT_THAT(number(session.send("MEAS:FREQ? CHAN1")),
              DoubleNear(sine.frequency, 1e-3 * sine.frequency));
  EXPECT_THAT(number(session.send("MEAS:PER? CHAN1")),
              DoubleNear(1.0 / sine.frequency, 1e-3 / sine.frequency));
  EXPECT_THAT(number(session.send("MEAS:DUTY? CHAN1")), DoubleNear(50.0, 0.05));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

INSTANTIATE_TEST_SUITE_P(CleanSines, CleanSineTimingTest,
                         testing::Values(SineTiming{1e3, 1e6, 10'000}, SineTiming{1e5, 1e6, 100},
                                         SineTiming{33.3e3, 333e3, 100}, SineTiming{73e3, 1e6, 137},
                                         SineTiming{1234.5, 1e6, 10'000},
                                         SineTiming{10.0, 1e6, 1'048'576}));

// Each shape's 50% crossings fall half a cycle apart, so each one's duty cycle is 50%.
TEST_F(TimingMeasurementTest, EveryShapeHasTheGeneratorsFrequency) {
  for (const std::string_view shape : {"SIN", "SQU", "TRI", "RAMP"}) {
    session.send(std::format("SOUR1:FUNC {}", shape));
    instrument.advance(kResetRecordLength);

    EXPECT_THAT(number(session.send("MEAS:FREQ? CHAN1")), DoubleNear(1e3, 1e-3)) << shape;
    EXPECT_THAT(number(session.send("MEAS:DUTY? CHAN1")), DoubleNear(50.0, 0.01)) << shape;
    EXPECT_EQ(session.next_error(), kNoErrorReply) << shape;
  }
}

/// A signal with noise at 1% of its amplitude, and the acquisition that captures it.
struct NoisySignal {
  std::string_view function;
  double frequency;
  double sample_rate;
  std::size_t record_length;
};

/// The reply to `query` (on channel 1), then to `SYST:ERR?`, on an Instrument seeded `seed` once it
/// has captured `signal` with noise at 1% of its amplitude (20 mV RMS on 2 Vpp).
std::pair<double, std::string> measure_noisy(std::uint64_t seed, const NoisySignal& signal,
                                             std::string_view query) {
  Instrument instrument{seed};
  ScpiSession session{instrument};
  session.send(kUntriggeredCaptures);
  session.send(std::format("ACQ:SRAT {};POIN {};:SOUR1:FUNC {};FREQ {};NOIS 0.02",
                           signal.sample_rate, signal.record_length, signal.function,
                           signal.frequency));
  instrument.advance(signal.record_length);
  const double value = number(session.send(std::format("{} CHAN1", query)));
  return {value, session.next_error()};
}

/// The seeds that the noisy signals are checked over.
constexpr std::uint64_t kNoiseSeeds = 5;

class NoisySignalTimingTest : public testing::TestWithParam<NoisySignal> {};

// NFR-6: frequency within 0.5% of the Generator's with noise at 1% of the amplitude, over several
// seeds. Noise makes a slow edge cross its 50% level several times; the hysteresis between the 10%
// and 90% levels counts each edge once.
TEST_P(NoisySignalTimingTest, FrequencyIsWithinHalfAPercent) {
  const NoisySignal signal = GetParam();
  for (std::uint64_t seed = 1; seed <= kNoiseSeeds; ++seed) {
    const auto [frequency, error] = measure_noisy(seed, signal, "MEAS:FREQ?");
    EXPECT_THAT(frequency, DoubleNear(signal.frequency, 5e-3 * signal.frequency)) << seed;
    EXPECT_EQ(error, kNoErrorReply) << seed;
  }
}

INSTANTIATE_TEST_SUITE_P(OnePercentNoise, NoisySignalTimingTest,
                         testing::Values(NoisySignal{"SIN", 1e3, 1e6, 10'000},
                                         NoisySignal{"SIN", 1e5, 1e6, 100},
                                         NoisySignal{"SIN", 1234.5, 1e6, 100'000},
                                         NoisySignal{"SQU", 1e3, 1e6, 10'000},
                                         NoisySignal{"TRI", 1e3, 1e6, 10'000}));

/// A 1 kHz square wave, with noise at 1% of its amplitude in NoisySquareTimingTest.
constexpr NoisySignal kNoisySquare{"SQU", 1e3, 1e6, 10'000};

// A square wave's edges are one sample long, so noise barely moves its 50% crossings.
TEST(NoisySquareTimingTest, DutyCycleIsHalf) {
  for (std::uint64_t seed = 1; seed <= kNoiseSeeds; ++seed) {
    const auto [duty, error] = measure_noisy(seed, kNoisySquare, "MEAS:DUTY?");
    EXPECT_THAT(duty, DoubleNear(50.0, 0.01)) << seed;
    EXPECT_EQ(error, kNoErrorReply) << seed;
  }
}

// Spec story 57: spikes past top and base move no reference level and cross none, and glitches
// that cross the 50% level without reaching the far reference level (a dip to -0.2 V from the
// high level, a bump to 0.2 V from the low one) are inside the hysteresis, so none of them is an
// edge and none changes a timing measurement.
TEST_F(TimingMeasurementTest, SpikesAndShallowGlitchesAreNotEdges) {
  session.send("SOUR1:FUNC SQU");
  instrument.advance(1250);  // A quarter into a cycle: the high level.
  session.send("SOUR1:FAUL GLIT,2.5");
  instrument.advance(100);
  session.send("SOUR1:FAUL GLIT,-1.2,20");
  instrument.advance(500);  // The low level.
  session.send("SOUR1:FAUL GLIT,-2.5");
  instrument.advance(100);
  session.send("SOUR1:FAUL GLIT,1.2,20");
  instrument.advance(kResetRecordLength - 1950);

  EXPECT_EQ(session.send("MEAS:FREQ? CHAN1;DUTY? CHAN1"), "1000;50");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(TimingMeasurementTest, ClippedSignalReturnsTheValueWithAWarning) {
  session.send("SOUR1:FUNC SQU;:CHAN1:SCAL 0.2");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send(kTimingQueries), "1000;0.001;50");
  for (int entry = 0; entry < 3; ++entry) {
    EXPECT_EQ(session.next_error(), kClippedReply) << entry;
  }
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// A 1 kHz sine from phase zero: 1500 samples hold its falling and rising 50% crossings at 500 and
// 1000 but not the falling one at 1500, which the signal confirms only on reaching its 10% level
// at about 1648. The first crossing at 0 does not count: the capture never saw the signal below
// 10% before it.
TEST_F(TimingMeasurementTest, UnderOneWholeCycleIsTooFewCycles) {
  session.send("ACQ:POIN 1500");
  instrument.advance(1500);
  EXPECT_EQ(session.send("MEAS:FREQ? CHAN1;PER? CHAN1;DUTY? CHAN1"),
            std::format("{0};{0};{0}", scpi::kNotANumber));
  for (int entry = 0; entry < 3; ++entry) {
    EXPECT_EQ(session.next_error(), kTooFewCyclesReply) << entry;
  }
  EXPECT_EQ(session.next_error(), kNoErrorReply);

  session.send("ACQ:POIN 2000");
  instrument.advance(2000);
  EXPECT_THAT(number(session.send("MEAS:FREQ? CHAN1")), DoubleNear(1e3, 1.0));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// A frequency step from the start of the second capture to past its end puts the stepped frequency
// in that capture, with no edge lost or added where the step starts.
TEST_F(TimingMeasurementTest, FrequencyStepShowsInTheNextCapture) {
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:FREQ? CHAN1"), "1000");

  session.send("SOUR1:FAUL FST,500,20000");
  instrument.advance(kResetRecordLength);
  EXPECT_THAT(number(session.send("MEAS:FREQ? CHAN1")), DoubleNear(1500.0, 1.5));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

/// The replies to the timing queries after a frequency step injected one capture in, with the
/// sample clock advanced `step` samples at a time.
std::string stepped_timing(std::size_t step) {
  Instrument instrument;
  ScpiSession session{instrument};
  session.send(kUntriggeredCaptures);
  test::advance_in_steps(instrument, kResetRecordLength, step);
  session.send("SOUR1:FAUL FST,500,20000");
  test::advance_in_steps(instrument, kResetRecordLength, step);
  return session.send(std::format("{};:SYST:ERR?", kTimingQueries)).value_or("");
}

TEST(TimingSteppingTest, RepliesDoNotDependOnHowTheClockIsStepped) {
  const std::string whole = stepped_timing(kResetRecordLength);
  EXPECT_THAT(whole, testing::EndsWith(kNoErrorReply));
  for (const std::size_t step :
       {std::size_t{1}, std::size_t{997}, kResetRecordLength - 1, kResetRecordLength + 1}) {
    EXPECT_EQ(stepped_timing(step), whole) << "step " << step;
  }
}

/// The measurement `mnemonic` of hand-built `samples` on a channel at its `*RST` settings
/// (1 V/div, so a 1 V swing is a signal), taken at one sample per second so that times read in
/// samples.
MeasurementResult measure(std::string_view mnemonic, std::span<const float> samples) {
  const std::span<const Measurement> all = measurements();
  const auto row = std::ranges::find(all, mnemonic, &Measurement::mnemonic);
  if (row == all.end()) {
    ADD_FAILURE() << "no measurement " << mnemonic;
    return {.status = MeasurementStatus::kOk, .value = std::nullopt};
  }
  return evaluate(*row, ChannelSettings{},
                  {.samples = samples, .levels = find_levels(samples), .sample_rate = 1.0});
}

/// A pulse train's high samples per 100-sample period and where its first period starts.
struct Pulses {
  std::size_t high;
  std::size_t delay;
};

class DutyCycleTest : public testing::TestWithParam<Pulses> {};

// Duty cycle is the high time over the period whichever level the capture starts at.
TEST_P(DutyCycleTest, IsTheHighTimeOverThePeriod) {
  const Pulses pulses = GetParam();
  const std::vector<float> samples = pulse_train(1000, 100, pulses.high, pulses.delay);

  EXPECT_EQ(measure("DUTYcycle", samples).value, static_cast<double>(pulses.high));
  EXPECT_EQ(measure("PERiod", samples).value, 100.0);
  EXPECT_EQ(measure("FREQuency", samples).value, 0.01);
}

INSTANTIATE_TEST_SUITE_P(NarrowAndWidePulses, DutyCycleTest,
                         testing::Values(Pulses{25, 0}, Pulses{25, 60}, Pulses{75, 0},
                                         Pulses{75, 10}, Pulses{2, 0}, Pulses{98, 0}));

// One whole cycle runs from a 50% crossing to the next one in the same direction. A square wave
// low for 50 samples, high for 50, low for 50: its crossings at 49.5 and 99.5 are less than one
// whole cycle; one more high sample adds the crossing at 149.5 and so the cycle.
TEST(TooFewCyclesTest, StartsAtOneWholeCycle) {
  std::vector<float> samples = pulse_train(150, 100, 50);
  for (const std::string_view mnemonic : {"FREQuency", "PERiod", "DUTYcycle"}) {
    EXPECT_EQ(measure(mnemonic, samples).status, MeasurementStatus::kTooFewCycles) << mnemonic;
  }

  samples.push_back(1.0F);
  EXPECT_EQ(measure("PERiod", samples).value, 100.0);
  EXPECT_EQ(measure("DUTYcycle", samples).value, 50.0);
}

// The crossing at 49.5 counts only because the signal was seen below its 10% level before it: a
// capture that starts between the 10% and 90% levels has no edge until it reaches one of them.
TEST(TooFewCyclesTest, CrossingFromAnUnseenLevelIsNoEdge) {
  std::vector<float> samples = pulse_train(151, 100, 50);
  std::fill_n(samples.begin(), 50, 0.5F);

  EXPECT_EQ(measure("PERiod", samples).status, MeasurementStatus::kTooFewCycles);
}

// A pulse's high level held by fewer than 1/128 of the samples is set aside like a glitch (top and
// base are both the low level), so pulses that narrow are No signal: 10 pulses of 7 samples in
// 10000 are 70 samples, under 78.125; 10 of 8 are a signal.
TEST(LowDutyPulsesTest, UnderOneSampleIn128IsNoSignal) {
  EXPECT_EQ(measure("FREQuency", pulse_train(10'000, 1000, 7)).status,
            MeasurementStatus::kNoSignal);
  EXPECT_EQ(measure("FREQuency", pulse_train(10'000, 1000, 8)).value, 0.001);
}

}  // namespace
}  // namespace openscope
