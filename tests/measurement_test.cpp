// Amplitude measurements (MEAS:VPP?, VMIN?, VMAX?, VAV?, VRMS?), the measurement status each
// result carries (OK, Clipped, or No signal for a measurement that needs edges or cycles; Too few
// cycles comes with the timing measurements) and the top and base levels the statuses start from.

#include "openscope/measurement.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/scpi/response.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::captured;
using test::kDataStaleReply;
using test::kDataTypeErrorReply;
using test::kIllegalParameterValueReply;
using test::kMissingParameterReply;
using test::kNoErrorReply;
using test::kParameterNotAllowedReply;
using test::kResetRecordLength;
using test::kSettingsConflictReply;
using test::ScpiSession;
using testing::DoubleNear;
using testing::Optional;
using testing::StartsWith;

constexpr auto kClippedReply = R"(-231,"Data questionable;Clipped")";

/// Every amplitude measurement query, short form.
constexpr std::array<std::string_view, 5> kAmplitudeQueries{"MEAS:VPP?", "MEAS:VMIN?", "MEAS:VMAX?",
                                                            "MEAS:VAV?", "MEAS:VRMS?"};

/// The number a measurement query replied, as a client parses it. Fails the test and returns NaN
/// when the reply is missing or not one number.
double number(const std::optional<std::string>& reply) {
  const std::string text = reply.value_or("");
  double value = std::nan("");
  const char* const end = std::to_address(text.cend());
  if (text.empty() || std::from_chars(text.data(), end, value).ptr != end) {
    ADD_FAILURE() << "not a number: " << text;
  }
  return value;
}

/// Trigger settings under which Auto takes untriggered captures back to back from sample 0: time
/// zero on the first sample and a level no signal reaches. Measurements read the whole capture, so
/// these tests need each capture to complete one record length after the last.
constexpr auto kUntriggeredCaptures = "TRIG:POS 0;LEV 100";

class MeasurementTest : public test::InstrumentFixture {
 protected:
  void SetUp() override { session.send(kUntriggeredCaptures); }
};

TEST_F(MeasurementTest, CleanSineHasTheGeneratorsPeaks) {
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "1");
  EXPECT_EQ(session.send("MEAS:VMIN? CHAN1"), "-1");
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// 1024 Hz at 2^20 samples per second puts exactly 512 high and 512 low samples in each cycle, so
// every value is exact: the mean is the offset and the RMS is sqrt((1.5^2 + 0.5^2) / 2).
TEST_F(MeasurementTest, SquareWaveMeasurementsAreExact) {
  session.send("ACQ:SRAT 1048576;POIN 10240;:SOUR1:FUNC SQU;FREQ 1024;AMPL 2;OFFS 0.5");
  instrument.advance(10'240);

  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "1.5");
  EXPECT_EQ(session.send("MEAS:VMIN? CHAN1"), "-0.5");
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");
  EXPECT_EQ(session.send("MEAS:VAV? CHAN1"), "0.5");
  EXPECT_EQ(session.send("MEAS:VRMS? CHAN1"), "1.118034");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

/// One clean sine: its Generator settings and the vertical scale that keeps it unclipped.
struct SineCase {
  double amplitude;
  double frequency;
  double offset;
  double scale;
};

class SineAccuracyTest : public MeasurementTest, public testing::WithParamInterface<SineCase> {};

// NFR-5: peak-to-peak and RMS within 1% of the Generator's ground truth on a clean sine, including
// captures that end part-way through a cycle (12.345 and 33.5 cycles). With an offset the capture
// holds whole cycles, since a part cycle shifts the mean and so the RMS of an offset sine by more
// than 1%.
TEST_P(SineAccuracyTest, PeakToPeakAndRmsAreWithinOnePercent) {
  const SineCase sine = GetParam();
  session.send(std::format("SOUR1:AMPL {};FREQ {};OFFS {};:CHAN1:SCAL {}", sine.amplitude,
                           sine.frequency, sine.offset, sine.scale));
  instrument.advance(kResetRecordLength);

  const double peak = sine.amplitude / 2.0;
  const double rms = std::sqrt((sine.offset * sine.offset) + (peak * peak / 2.0));
  EXPECT_THAT(number(session.send("MEAS:VPP? CHAN1")),
              DoubleNear(sine.amplitude, 0.01 * sine.amplitude));
  EXPECT_THAT(number(session.send("MEAS:VRMS? CHAN1")), DoubleNear(rms, 0.01 * rms));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

INSTANTIATE_TEST_SUITE_P(CleanSines, SineAccuracyTest,
                         testing::Values(SineCase{2.0, 1e3, 0.0, 1.0},
                                         SineCase{5.0, 1234.5, 0.0, 2.0},
                                         SineCase{0.5, 3.35e3, 0.0, 0.2},
                                         SineCase{6.0, 1.5e3, -1.5, 1.5}));

// A flat signal has an amplitude: No signal is only for measurements that need edges or cycles.
TEST_F(MeasurementTest, FlatSignalHasRealAmplitudeValues) {
  session.send("SOUR1:FUNC DC;OFFS 0.75");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1;VMIN? CHAN1;VMAX? CHAN1;VAV? CHAN1;VRMS? CHAN1"),
            "0;0.75;0.75;0.75;0.75");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// Noise alone spreads the samples, and the amplitude measurements report that spread.
TEST_F(MeasurementTest, NoiseAloneHasRealAmplitudeValues) {
  session.send("SOUR1:FUNC DC;NOIS 0.1");
  instrument.advance(kResetRecordLength);
  const auto samples = captured(instrument, 1).samples;
  const auto [lowest, highest] = std::ranges::minmax(samples);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), scpi::format_real(highest - lowest));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(MeasurementTest, ClippedReturnsTheValueWithAWarning) {
  session.send("CHAN1:SCAL 0.2");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "1.6");
  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "0.8");
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// Clipped means a sample hit an input-range limit, so a peak exactly at the limit counts. The
// input range here is -6 V to 2 V, so only the top limit is in reach.
TEST_F(MeasurementTest, PeakAtTheTopLimitIsClipped) {
  session.send("CHAN1:OFFS -2;:SOUR1:AMPL 4");
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "2");
  EXPECT_EQ(session.next_error(), kClippedReply);

  session.send("SOUR1:AMPL 3.99");
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "1.995");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// The input range here is -2 V to 6 V, so only the bottom limit is in reach.
TEST_F(MeasurementTest, PeakAtTheBottomLimitIsClipped) {
  session.send("CHAN1:OFFS 2;:SOUR1:AMPL 4");
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:VMIN? CHAN1"), "-2");
  EXPECT_EQ(session.next_error(), kClippedReply);

  session.send("SOUR1:AMPL 3.99");
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:VMIN? CHAN1"), "-1.995");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// A signal held flat at a limit still has an amplitude, which may be wrong.
TEST_F(MeasurementTest, SignalClippedFlatIsClipped) {
  session.send("SOUR1:FUNC DC;OFFS 10");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1;VPP? CHAN1"), "4;0");
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// A status entry reports on a result that was returned, so it does not stop the line.
TEST_F(MeasurementTest, StatusEntriesLetTheRestOfTheLineRun) {
  session.send("CHAN1:SCAL 0.2");
  instrument.advance(kResetRecordLength);
  EXPECT_THAT(session.send("MEAS:VMAX? CHAN1;VMIN? CHAN1;*IDN?"),
              Optional(StartsWith("0.8;-0.8;OpenScope,")));

  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// Each query of a result reports its status, cached or not, and only to the Session that asked.
TEST_F(MeasurementTest, EveryQueryReportsItsStatusToItsOwnSession) {
  ScpiSession other{instrument};
  session.send("CHAN1:SCAL 0.2");
  instrument.advance(kResetRecordLength);

  session.send("MEAS:VPP? CHAN1");
  session.send("MEAS:VPP? CHAN1");

  EXPECT_EQ(other.next_error(), kNoErrorReply);
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(MeasurementTest, EveryEnabledChannelCanBeMeasured) {
  session.send("CHAN2:DISP ON;:CHAN3:DISP ON;:CHAN4:DISP ON");
  session.send("SOUR2:AMPL 3;:SOUR3:AMPL 4;:SOUR4:AMPL 5");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");
  EXPECT_EQ(session.send("MEAS:VPP? CHAN2"), "3");
  EXPECT_EQ(session.send("MEASure:VPP? CHANnel3"), "4");
  EXPECT_EQ(session.send("meas:vpp? chan4"), "5");
  EXPECT_EQ(session.send("MEAS:VPP? CHAN"), "2");
}

TEST_F(MeasurementTest, LongFormsAreAccepted) {
  session.send("SOUR1:OFFS 0.5");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEASure:VMAX? CHANnel1"), "1.5");
  EXPECT_EQ(session.send("MEASure:VMIN? CHANnel1"), "-0.5");
  EXPECT_EQ(session.send("MEASure:VAVerage? CHANnel1"), session.send("MEAS:VAV? CHAN1"));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(MeasurementTest, DisabledChannelIsASettingsConflict) {
  instrument.advance(kResetRecordLength);

  for (const std::string_view query : kAmplitudeQueries) {
    EXPECT_EQ(session.send(std::string(query) + " CHAN2"), std::nullopt) << query;
    EXPECT_EQ(session.next_error(), kSettingsConflictReply) << query;
  }
  EXPECT_EQ(session.send("*IDN?;:MEAS:VPP? CHAN3;*IDN?"), session.send("*IDN?"));
  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(MeasurementTest, ChannelEnabledSinceTheLatestCaptureIsASettingsConflict) {
  instrument.advance(kResetRecordLength);

  session.send("CHAN2:DISP ON");

  EXPECT_EQ(session.send("MEAS:VPP? CHAN2"), std::nullopt);
  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
}

TEST_F(MeasurementTest, ChannelDisabledSinceTheLatestCaptureCanStillBeMeasured) {
  instrument.advance(kResetRecordLength);

  session.send("CHAN1:DISP OFF");

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");
}

TEST_F(MeasurementTest, NoCaptureYetIsStaleData) {
  instrument.advance(kResetRecordLength - 1);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);

  instrument.advance(1);
  session.send("*RST");
  EXPECT_EQ(session.send("MEAS:VMIN? CHAN1"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// Results are cached per capture: a later capture is measured afresh, while settings changed since
// the latest capture do not touch its results.
TEST_F(MeasurementTest, ResultsFollowTheLatestCapture) {
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");

  session.send("SOUR1:AMPL 1");
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");

  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "1");

  session.send("*RST");
  session.send(kUntriggeredCaptures);
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");
}

TEST_F(MeasurementTest, OneSampleCaptureIsMeasured) {
  session.send("ACQ:POIN 1;:SOUR1:FUNC DC;OFFS 0.5");
  instrument.advance(1);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1;VMIN? CHAN1;VMAX? CHAN1"), "0;0.5;0.5");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(MeasurementTest, TwoSampleCaptureHasBothLevels) {
  session.send("ACQ:POIN 2;:SOUR1:FUNC SQU;FREQ 500KHZ");
  instrument.advance(2);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1;VMAX? CHAN1;VMIN? CHAN1;VAV? CHAN1;VRMS? CHAN1"),
            "2;1;-1;0;1");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(MeasurementTest, ChannelParameterIsRequiredAndChecked) {
  instrument.advance(kResetRecordLength);

  session.send("MEAS:VPP?");
  session.send("MEAS:VPP? CHAN5");
  session.send("MEAS:VPP? 1");
  session.send("MEAS:VPP? CHAN1,CHAN2");
  session.send("MEAS:VPP? SOUR1");

  EXPECT_EQ(session.next_error(), kMissingParameterReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.next_error(), kParameterNotAllowedReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// A record longer than one sample block is measured as a whole. It ends part-way through a cycle
// (10.48576 cycles), so the expected mean and RMS are summed from the sine itself. The tolerance
// is far above the float samples' rounding but below what summing the 2^20 samples in float
// instead of double costs (whole cycles would hide that: their rounding errors cancel).
TEST_F(MeasurementTest, LongestRecordIsMeasuredWhole) {
  constexpr std::size_t kLongest = 1'048'576;
  session.send("ACQ:POIN 1048576;:SOUR1:FREQ 10");
  instrument.advance(kLongest);

  double sum = 0.0;
  double sum_of_squares = 0.0;
  for (std::size_t index = 0; index < kLongest; ++index) {
    const double volts = std::sin(2.0 * std::numbers::pi * 10.0 * static_cast<double>(index) / 1e6);
    sum += volts;
    sum_of_squares += volts * volts;
  }
  const double mean = sum / static_cast<double>(kLongest);
  const double rms = std::sqrt(sum_of_squares / static_cast<double>(kLongest));
  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1;VMIN? CHAN1"), "1;-1");
  EXPECT_THAT(number(session.send("MEAS:VAV? CHAN1")), DoubleNear(mean, 1e-6 * rms));
  EXPECT_THAT(number(session.send("MEAS:VRMS? CHAN1")), DoubleNear(rms, 1e-6 * rms));
}

/// A measurement that needs a signal, as the timing measurements do; its value is top minus base.
constexpr Measurement kNeedsSignal{
    .mnemonic = "TEST",
    .measure = [](const MeasuredSignal& signal) -> std::optional<double> {
      return signal.levels.top - signal.levels.base;
    },
    .needs_signal = true};

/// kNeedsSignal of channel 1 of `instrument`'s latest capture.
MeasurementResult measure_needing_signal(const Instrument& instrument) {
  const CapturedChannel channel = captured(instrument, 1);
  if (channel.samples.empty()) {
    return {.status = MeasurementStatus::kOk, .value = std::nullopt};  // captured() failed.
  }
  return evaluate(kNeedsSignal, channel, find_levels(channel.samples));
}

// No signal is top minus base under 0.2 vertical divisions: at 1.25 V/div that is 0.25 V, which
// a square wave of exactly 0.25 Vpp reaches.
TEST_F(MeasurementTest, NoSignalBelowTwoTenthsOfADivision) {
  session.send("CHAN1:SCAL 1.25;:SOUR1:FUNC SQU;AMPL 0.25");
  instrument.advance(kResetRecordLength);
  EXPECT_EQ(measure_needing_signal(instrument).status, MeasurementStatus::kOk);

  session.send("SOUR1:AMPL 0.2499");
  instrument.advance(kResetRecordLength);
  const MeasurementResult result = measure_needing_signal(instrument);
  EXPECT_EQ(result.status, MeasurementStatus::kNoSignal);
  EXPECT_EQ(result.value, std::nullopt);
}

// Top and base are the most common levels, not the extremes: noise alone spreads the samples over
// more than 0.2 div, but its histogram has no separate high and low level.
TEST_F(MeasurementTest, NoiseAloneIsNoSignal) {
  session.send("SOUR1:FUNC DC;NOIS 0.1");
  instrument.advance(kResetRecordLength);
  const auto samples = captured(instrument, 1).samples;
  const auto [lowest, highest] = std::ranges::minmax(samples);
  ASSERT_GT(highest - lowest, 0.2F);

  EXPECT_EQ(measure_needing_signal(instrument).status, MeasurementStatus::kNoSignal);
}

// A signal held flat at a limit has no edges or cycles, so No signal wins over Clipped.
TEST_F(MeasurementTest, SignalClippedFlatIsNoSignalForAMeasurementThatNeedsASignal) {
  session.send("SOUR1:FUNC DC;OFFS 10");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(measure_needing_signal(instrument).status, MeasurementStatus::kNoSignal);
}

TEST_F(MeasurementTest, NoAmplitudeMeasurementNeedsASignal) {
  for (const Measurement& measurement : measurements()) {
    EXPECT_FALSE(measurement.needs_signal) << measurement.mnemonic;
  }
}

/// `length` samples of a square wave between 0 V and 1 V with a period of 100 samples: low for
/// samples 0 to 49 of each period, high for 50 to 99.
std::vector<float> square_wave(std::size_t length) {
  std::vector<float> samples(length);
  for (std::size_t index = 0; index < length; ++index) {
    samples[index] = index % 100 < 50 ? 0.0F : 1.0F;
  }
  return samples;
}

/// The length of the hand-built captures that top and base are checked on.
constexpr std::size_t kLength = 10'000;

TEST(SignalLevelsTest, SquareWaveLevelsAreItsHighAndLow) {
  const SignalLevels levels = find_levels(square_wave(kLength));

  EXPECT_DOUBLE_EQ(levels.top, 1.0);
  EXPECT_DOUBLE_EQ(levels.base, 0.0);
}

/// Samples of the square wave overwritten with one voltage: a spike or a short glitch.
struct Disturbance {
  std::size_t first;
  std::size_t length;
  float volts;
};

/// A one-sample spike and a 20-sample glitch out from each level of square_wave().
constexpr std::array kDisturbances{Disturbance{.first = 75, .length = 1, .volts = 5.0F},
                                   Disturbance{.first = 25, .length = 1, .volts = -4.0F},
                                   Disturbance{.first = 150, .length = 20, .volts = 3.0F},
                                   Disturbance{.first = 210, .length = 20, .volts = -2.0F}};

/// Overwrites `samples` with `disturbance`.
void disturb(std::vector<float>& samples, const Disturbance& disturbance) {
  std::fill_n(samples.begin() + static_cast<std::ptrdiff_t>(disturbance.first), disturbance.length,
              disturbance.volts);
}

class DisturbedSquareWaveTest : public testing::TestWithParam<Disturbance> {};

// Spec story 57: noise spikes do not distort top and base. Each disturbance is alone on its side
// of the wave, so it alone sets the maximum or minimum.
TEST_P(DisturbedSquareWaveTest, MovesNeitherTopNorBase) {
  const Disturbance disturbance = GetParam();
  std::vector<float> samples = square_wave(kLength);
  disturb(samples, disturbance);

  const SignalLevels levels = find_levels(samples);

  EXPECT_DOUBLE_EQ(levels.top, 1.0);
  EXPECT_DOUBLE_EQ(levels.base, 0.0);
  EXPECT_DOUBLE_EQ(std::max(levels.maximum, -levels.minimum), std::abs(disturbance.volts));
}

INSTANTIATE_TEST_SUITE_P(SpikesAndShortGlitches, DisturbedSquareWaveTest,
                         testing::ValuesIn(kDisturbances));

// Outliers at both ends are set aside together.
TEST(SignalLevelsTest, SpikesAndGlitchesOnBothLevelsAtOnce) {
  std::vector<float> samples = square_wave(kLength);
  for (const Disturbance& disturbance : kDisturbances) {
    disturb(samples, disturbance);
  }

  const SignalLevels levels = find_levels(samples);

  EXPECT_DOUBLE_EQ(levels.top, 1.0);
  EXPECT_DOUBLE_EQ(levels.base, 0.0);
}

// In a short capture one sample is already 1/128 of it, but a settled level needs two.
TEST(SignalLevelsTest, SingleSpikeInAShortCaptureIsNotTop) {
  std::vector<float> samples = square_wave(100);
  samples.at(75) = 5.0F;

  const SignalLevels levels = find_levels(samples);

  EXPECT_DOUBLE_EQ(levels.top, 1.0);
  EXPECT_DOUBLE_EQ(levels.base, 0.0);
}

// Setting the lone extremes of two or three samples aside would leave too few for a level.
TEST(SignalLevelsTest, TinyCapturesKeepEverySample) {
  for (const std::vector<float>& samples :
       {std::vector<float>{-1.0F, 1.0F}, std::vector<float>{-1.0F, 0.0F, 1.0F}}) {
    const SignalLevels levels = find_levels(samples);

    EXPECT_DOUBLE_EQ(levels.top, 1.0) << samples.size();
    EXPECT_DOUBLE_EQ(levels.base, -1.0) << samples.size();
  }
}

/// `kLength` samples of a pulse train between 0 V and 1 V, high for `duty` of each 1000-sample
/// period, with 10 mV RMS of Gaussian noise from a fixed seed.
std::vector<float> noisy_pulse_train(double duty) {
  std::mt19937 random(9);
  std::normal_distribution<double> noise(0.0, 0.01);
  std::vector<float> samples(kLength);
  for (std::size_t index = 0; index < kLength; ++index) {
    const double level = static_cast<double>(index % 1000) < duty * 1000.0 ? 1.0 : 0.0;
    samples[index] = static_cast<float>(level + noise(random));
  }
  return samples;
}

class NoisyPulseTrainTest : public testing::TestWithParam<double> {};

// Noise spreads a level over several bins, so no single bin holds its share of the samples: a
// narrow pulse's top (or, mirrored, a wide pulse's base) above 1/128 of the samples is still its
// level, not the highest noise sample.
TEST_P(NoisyPulseTrainTest, LevelsAreThePulseLevels) {
  const SignalLevels levels = find_levels(noisy_pulse_train(GetParam()));

  EXPECT_NEAR(levels.top, 1.0, 0.01);
  EXPECT_NEAR(levels.base, 0.0, 0.01);
}

INSTANTIATE_TEST_SUITE_P(NarrowAndWidePulses, NoisyPulseTrainTest,
                         testing::Values(0.015, 0.02, 0.03, 0.04, 0.96, 0.985));

/// `length` samples of a triangle wave between 0 V and 1 V that runs `cycles` cycles, starting
/// `start` of a cycle in.
std::vector<float> triangle_wave(std::size_t length, double cycles, double start) {
  std::vector<float> samples(length);
  for (std::size_t index = 0; index < length; ++index) {
    const double phase =
        std::fmod((cycles * static_cast<double>(index) / static_cast<double>(length)) + start, 1.0);
    samples[index] = static_cast<float>(phase < 0.5 ? 2.0 * phase : 2.0 - (2.0 * phase));
  }
  return samples;
}

class ShortTriangleTest : public testing::TestWithParam<std::size_t> {};

// A short triangle's evenly stepped samples land in every second or third bin, so single bins hold
// twice the average; no level stands above the rest, so top and base are its extremes (within a
// tenth of its span: a lone sample at an extreme may be set aside as an outlier), never a level in
// the middle.
TEST_P(ShortTriangleTest, FallsBackToItsExtremes) {
  for (const double cycles : {2.0, 3.0, 4.5, 5.0, 7.5, 11.0}) {
    for (const double start : {0.0, 0.13, 0.37}) {
      const std::vector<float> samples = triangle_wave(GetParam(), cycles, start);
      const auto [lowest, highest] = std::ranges::minmax(samples);

      const SignalLevels levels = find_levels(samples);

      EXPECT_NEAR(levels.top, highest, 0.1) << cycles << " cycles from " << start;
      EXPECT_NEAR(levels.base, lowest, 0.1) << cycles << " cycles from " << start;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ShortCaptures, ShortTriangleTest,
                         testing::Values(64, 128, 256, 512, 1000, 2000));

// A level counts once it holds 1/128 of the samples (78.125 of 10000): a pulse of 79 samples is
// top, one of 78 is a glitch.
TEST(SignalLevelsTest, LevelHoldingOneSampleIn128IsSettled) {
  std::vector<float> samples = square_wave(kLength);
  std::fill_n(samples.begin() + 150, 79, 3.0F);
  EXPECT_DOUBLE_EQ(find_levels(samples).top, 3.0);

  samples.at(150) = 1.0F;
  EXPECT_DOUBLE_EQ(find_levels(samples).top, 1.0);
}

TEST(SignalLevelsTest, SpikeOnAFlatSignalIsNeitherTopNorBase) {
  std::vector<float> samples(kLength, 0.5F);
  samples.at(5000) = 2.5F;

  const SignalLevels levels = find_levels(samples);

  EXPECT_DOUBLE_EQ(levels.top, 0.5);
  EXPECT_DOUBLE_EQ(levels.base, 0.5);
}

// Spec story 57 on a Generator's own glitches: a one-sample glitch up from the square wave's high
// level and a 20-sample glitch up from its low level (to 1.5 V, between the high level and the
// spike) move neither top nor base.
TEST_F(MeasurementTest, GeneratorGlitchesMoveNeitherTopNorBase) {
  session.send("SOUR1:FUNC SQU");
  instrument.advance(2250);  // A quarter into a 1 kHz cycle: the high level.
  session.send("SOUR1:FAUL GLIT,2.5");
  instrument.advance(5500);  // Three quarters into a cycle: the low level.
  session.send("SOUR1:FAUL GLIT,2.5,20");
  instrument.advance(kResetRecordLength - 7750);

  const SignalLevels levels = find_levels(captured(instrument, 1).samples);

  EXPECT_DOUBLE_EQ(levels.maximum, 3.5);
  EXPECT_DOUBLE_EQ(levels.minimum, -1.0);
  EXPECT_DOUBLE_EQ(levels.top, 1.0);
  EXPECT_DOUBLE_EQ(levels.base, -1.0);
}

// A triangle spends equal time at every level, so its histogram has no mode to take: top and base
// fall back to its extremes, and a spike past them is not one of them.
TEST_F(MeasurementTest, SignalWithoutSettledLevelsFallsBackToItsExtremes) {
  session.send("SOUR1:FUNC TRI");
  instrument.advance(kResetRecordLength);
  std::vector<float> samples = captured(instrument, 1).samples;
  const auto [lowest, highest] = std::ranges::minmax(samples);
  EXPECT_DOUBLE_EQ(find_levels(samples).top, highest);
  EXPECT_DOUBLE_EQ(find_levels(samples).base, lowest);

  samples.at(1234) = 3.0F;
  samples.at(4321) = -3.0F;
  const SignalLevels levels = find_levels(samples);
  EXPECT_DOUBLE_EQ(levels.top, highest);
  EXPECT_DOUBLE_EQ(levels.base, lowest);
}

}  // namespace
}  // namespace openscope
