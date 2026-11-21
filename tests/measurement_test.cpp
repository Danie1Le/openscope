// Amplitude measurements (MEAS:VPP?, VMIN?, VMAX?), the measurement status each result carries
// (OK, Clipped, or No signal for a measurement that needs edges or cycles; Too few cycles comes
// with the timing measurements) and the top and base levels the statuses start from.

#include "openscope/measurement.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

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
using testing::Optional;
using testing::StartsWith;

constexpr auto kClippedReply = R"(-231,"Data questionable;Clipped")";

/// Every amplitude measurement query, short form.
constexpr std::array<std::string_view, 3> kAmplitudeQueries{"MEAS:VPP?", "MEAS:VMIN?",
                                                            "MEAS:VMAX?"};

using MeasurementTest = test::InstrumentFixture;

TEST_F(MeasurementTest, CleanSineHasTheGeneratorsPeaks) {
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "1");
  EXPECT_EQ(session.send("MEAS:VMIN? CHAN1"), "-1");
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// 1024 Hz at 2^20 samples per second puts exactly 512 high and 512 low samples in each cycle, so
// every value is exact.
TEST_F(MeasurementTest, SquareWaveMeasurementsAreExact) {
  session.send("ACQ:SRAT 1048576;POIN 10240;:SOUR1:FUNC SQU;FREQ 1024;AMPL 2;OFFS 0.5");
  instrument.advance(10'240);

  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "1.5");
  EXPECT_EQ(session.send("MEAS:VMIN? CHAN1"), "-0.5");
  EXPECT_EQ(session.send("MEAS:VPP? CHAN1"), "2");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// A flat signal has an amplitude: No signal is only for measurements that need edges or cycles.
TEST_F(MeasurementTest, FlatSignalHasRealAmplitudeValues) {
  session.send("SOUR1:FUNC DC;OFFS 0.75");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1;VMIN? CHAN1;VMAX? CHAN1"), "0;0.75;0.75");
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

  EXPECT_EQ(session.send("MEAS:VPP? CHAN1;VMAX? CHAN1;VMIN? CHAN1"), "2;1;-1");
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

// A record longer than one sample block is measured as a whole.
TEST_F(MeasurementTest, LongestRecordIsMeasuredWhole) {
  session.send("ACQ:POIN 1048576;:SOUR1:FREQ 10");
  instrument.advance(1'048'576);

  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1;VMIN? CHAN1"), "1;-1");
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

}  // namespace
}  // namespace openscope
