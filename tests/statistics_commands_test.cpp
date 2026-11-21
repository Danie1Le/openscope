// Measurement statistics through SCPI: MEAS:STAT? <type>,CHAN<n> replies
// <current>,<mean>,<minimum>,<maximum>,<standard deviation>,<count> over the captures since the
// last reset, which MEAS:STAT:RES, every instrument-wide setting change but the timebase, and *RST
// make.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "binary_block.hpp"
#include "instrument_fixture.hpp"
#include "openscope/acquired_block.hpp"
#include "openscope/instrument.hpp"
#include "reply_numbers.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::kClippedReply;
using test::kDataStaleReply;
using test::kDataTypeErrorReply;
using test::kIllegalParameterValueReply;
using test::kMissingParameterReply;
using test::kNoErrorReply;
using test::kParameterNotAllowedReply;
using test::kSettingsConflictReply;
using test::kUndefinedHeaderReply;
using test::kUntriggeredCaptures;
using test::numbers;
using test::ScpiSession;
using testing::DoubleNear;

/// The record length these tests capture at: short, so many captures are quick.
constexpr std::size_t kRecord = 1000;

/// Sets kRecord as the record length: with kUntriggeredCaptures, every kRecord samples complete one
/// untriggered capture.
std::string short_records() { return std::format("ACQ:POIN {}", kRecord); }

/// The fields of a MEAS:STAT? reply, in reply order.
struct Statistics {
  double current;
  double mean;
  double minimum;
  double maximum;
  double standard_deviation;
  double count;
};

/// The fields of a MEAS:STAT? reply. Fails the test and returns -1 in every field when the reply
/// is not six numbers.
Statistics statistics_of(const std::optional<std::string>& reply) {
  const std::vector<double> fields = numbers(reply);
  if (fields.size() != 6) {
    ADD_FAILURE() << "not a statistics reply: " << reply.value_or("<no reply>");
    return {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
  }
  return {.current = fields.at(0),
          .mean = fields.at(1),
          .minimum = fields.at(2),
          .maximum = fields.at(3),
          .standard_deviation = fields.at(4),
          .count = fields.at(5)};
}

/// The count a MEAS:STAT? reply ends with (-1 and a test failure for anything else).
double count_of(const std::optional<std::string>& reply) { return statistics_of(reply).count; }

/// What MEAS:STAT? should reply after captures that measured `values` (at least one), the last of
/// them current: worked out in two passes as a textbook does, independently of the instrument's
/// running update.
Statistics expected_of(const std::vector<double>& values) {
  const auto count = static_cast<double>(values.size());
  const double mean = std::accumulate(values.begin(), values.end(), 0.0) / count;
  double squares = 0.0;
  for (const double value : values) {
    squares += (value - mean) * (value - mean);
  }
  return {.current = values.back(),
          .mean = mean,
          .minimum = std::ranges::min(values),
          .maximum = std::ranges::max(values),
          .standard_deviation = std::sqrt(squares / count),
          .count = count};
}

/// Checks a MEAS:STAT? reply against `expected`, to within the float32 precision replies are
/// written at.
void expect_statistics(const std::optional<std::string>& reply, const Statistics& expected) {
  const auto near = [](double value) {
    return DoubleNear(value, 1e-6 * std::max(1.0, std::abs(value)));
  };
  const Statistics actual = statistics_of(reply);
  EXPECT_THAT(actual.current, near(expected.current));
  EXPECT_THAT(actual.mean, near(expected.mean));
  EXPECT_THAT(actual.minimum, near(expected.minimum));
  EXPECT_THAT(actual.maximum, near(expected.maximum));
  EXPECT_THAT(actual.standard_deviation, near(expected.standard_deviation));
  EXPECT_EQ(actual.count, expected.count);
}

class StatisticsTest : public test::InstrumentFixture {
 protected:
  void SetUp() override {
    session.send(kUntriggeredCaptures);
    session.send(short_records());
  }

  /// Advances through `captures` captures.
  void capture(std::size_t captures) { instrument.advance(captures * kRecord); }
};

// Ground truth from the Generator: a 0.5 V DC level, with a 1 V glitch at the start of captures
// 3 and 7 of 10. VMAX is 1.5 in those two and 0.5 in the rest: mean 0.7, and deviations of 0.8 and
// -0.2 give a population variance of (2 x 0.64 + 8 x 0.04) / 10 = 0.16.
TEST_F(StatisticsTest, GlitchedCapturesGiveExactStatistics) {
  session.send("SOUR1:FUNC DC;OFFS 0.5");
  for (std::size_t n = 0; n < 10; ++n) {
    if (n == 3 || n == 7) {
      session.send("SOUR1:FAUL GLIT,1");  // A fault is not a setting: it keeps the statistics.
    }
    capture(1);
  }

  EXPECT_EQ(session.send("MEAS:STAT? VMAX,CHAN1"), "0.5,0.7,0.5,1.5,0.4,10");
  EXPECT_EQ(session.send("MEAS:STAT? VMIN,CHAN1"), "0.5,0.5,0.5,0.5,0,10");
  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1"), "0,0.2,0,1,0.4,10");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// Ground truth from the samples: each capture's maximum and mean are read from WAV:DATA? as it
// completes and the statistics are worked out from them in two passes.
TEST_F(StatisticsTest, StatisticsMatchTheSamplesOfEveryCapture) {
  session.send("SOUR1:FREQ 1234;NOIS 0.2;:WAV:FORM REAL");
  std::vector<double> maxima;
  std::vector<double> means;
  for (std::size_t n = 0; n < 25; ++n) {
    capture(1);
    const std::vector<float> samples =
        test::decode_float32_block(session.send("WAV:DATA?").value_or(""));
    ASSERT_EQ(samples.size(), kRecord);
    maxima.push_back(*std::ranges::max_element(samples));
    means.push_back(std::accumulate(samples.begin(), samples.end(), 0.0) /
                    static_cast<double>(kRecord));
  }

  expect_statistics(session.send("MEAS:STAT? VMAX,CHAN1"), expected_of(maxima));
  expect_statistics(session.send("MEAS:STAT? VAV,CHAN1"), expected_of(means));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(StatisticsTest, CurrentIsTheLatestCapturesResult) {
  session.send("SOUR1:NOIS 0.2");
  capture(3);

  EXPECT_EQ(statistics_of(session.send("MEAS:STAT? VRMS,CHAN1")).current,
            test::number(session.send("MEAS:VRMS? CHAN1")));
}

TEST_F(StatisticsTest, EveryCaptureCountsUpToManyCaptures) {
  session.send("ACQ:POIN 1");  // A capture every sample: 5000 of them over five whole cycles.
  instrument.advance(5000);

  const Statistics statistics = statistics_of(session.send("MEAS:STAT? VMAX,CHAN1"));
  EXPECT_THAT(statistics.mean, DoubleNear(0.0, 1e-6));
  EXPECT_EQ(statistics.minimum, -1.0);
  EXPECT_EQ(statistics.maximum, 1.0);
  EXPECT_EQ(statistics.count, 5000.0);
}

TEST_F(StatisticsTest, UntriggeredCapturesCount) {
  capture(4);

  EXPECT_EQ(session.send("WAV:PRE?"), "1E+06,1000,1,0,0,1");  // The last field: untriggered.
  EXPECT_EQ(count_of(session.send("MEAS:STAT? VPP,CHAN1")), 4.0);
}

/// How a test drives the sample clock: test::advance_in_steps, or test::acquire_and_process as
/// live mode's two threads do.
using Driver = void (*)(Instrument& instrument, std::size_t samples, std::size_t step);

/// MEAS:STAT? of every measurement on `instrument`, seed 7 with noise, after `samples` samples
/// that `drive` takes `step` at a time.
std::optional<std::string> stepped_statistics(std::size_t samples, std::size_t step, Driver drive) {
  Instrument instrument(7);
  ScpiSession session(instrument);
  session.send(kUntriggeredCaptures);
  session.send(short_records());
  session.send("SOUR1:FREQ 1234;NOIS 0.2");
  drive(instrument, samples, step);
  return session.send(
      "MEAS:STAT? VPP,CHAN1;STAT? VMIN,CHAN1;STAT? VMAX,CHAN1;STAT? VAV,CHAN1;"
      "STAT? VRMS,CHAN1;:SYST:ERR?");
}

// More captures than one sample block holds, so the whole run completes many captures per block
// and spans two blocks; stepping one sample at a time completes at most one per call. Live mode's
// acquire and process count the same captures as advance.
TEST(StatisticsSteppingTest, HowTheClockIsSteppedNeverChangesTheStatistics) {
  constexpr std::size_t kSamples = (70 * kRecord) + 500;
  const std::optional<std::string> whole =
      stepped_statistics(kSamples, kSamples, &test::advance_in_steps);
  EXPECT_THAT(whole.value_or(""), testing::EndsWith(std::string(",70;") + kNoErrorReply));
  for (const Driver drive : {&test::advance_in_steps, &test::acquire_and_process}) {
    for (const std::size_t step : {std::size_t{1}, std::size_t{997}, kRecord - 1, kRecord,
                                   kRecord + 1, Instrument::kMaxBlockLength}) {
      EXPECT_EQ(stepped_statistics(kSamples, step, drive), whole) << "step " << step;
    }
  }
}

/// The reply of a line held on a Single sent part-way through a capture, then MEAS:STAT? once the
/// Single has stopped the instrument, on seed 7 with noise, with the clock stepped `step` samples
/// at a time.
std::string stepped_single(std::size_t step) {
  Instrument instrument(7);
  ScpiSession session(instrument);
  session.send(kUntriggeredCaptures);
  session.send(short_records());
  session.send("SOUR1:FREQ 1234;NOIS 0.2");
  test::advance_in_steps(instrument, (30 * kRecord) + 300, step);
  const auto held = session.start("SINGle;*OPC?;:MEAS:STAT? VPP,CHAN1");
  test::advance_in_steps(instrument, 3 * kRecord, step);
  EXPECT_TRUE(held->ended) << "step " << step;
  return held->text.value_or("") + " / " + session.send("MEAS:STAT? VPP,CHAN1").value_or("");
}

// The Single's capture ends a block early (ADR 0003): wherever the steps fall around it, the
// statistics count the same captures and the held line reads them.
TEST(StatisticsSteppingTest, HowTheClockIsSteppedAcrossASingleNeverChangesTheStatistics) {
  const std::string whole = stepped_single(Instrument::kMaxBlockLength);
  EXPECT_THAT(whole, testing::EndsWith(",31"));  // 30 captures, then the Single's.
  for (const std::size_t step :
       {std::size_t{1}, std::size_t{299}, kRecord - 1, kRecord, kRecord + 1}) {
    EXPECT_EQ(stepped_single(step), whole) << "step " << step;
  }
}

TEST_F(StatisticsTest, NoCaptureSinceTheResetIsStaleData) {
  session.send("SOUR1:FUNC DC;OFFS 0.5");
  capture(1);
  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1"), "0,0,0,0,0,1");

  session.send("MEAS:STAT:RES");

  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1;*IDN?"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "0.5");  // The latest capture is still there.
  capture(1);
  EXPECT_EQ(session.send("MEAS:STAT? VMAX,CHAN1"), "0.5,0.5,0.5,0.5,0,1");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(StatisticsTest, NoCaptureYetIsStaleData) {
  instrument.advance(kRecord - 1);

  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
}

TEST_F(StatisticsTest, ResetWithNothingCountedIsHarmless) {
  session.send("MEAS:STAT:RES;:MEASure:STATistics:RESet");
  capture(1);

  EXPECT_EQ(count_of(session.send("MEAS:STAT? VPP,CHAN1")), 1.0);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(StatisticsTest, StatisticsAreInstrumentWide) {
  ScpiSession other(instrument);
  capture(2);

  EXPECT_EQ(other.send("MEAS:STAT? VPP,CHAN1"), session.send("MEAS:STAT? VPP,CHAN1"));
  other.send("MEAS:STAT:RES");
  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
}

/// A command and whether it resets the statistics.
struct StatisticsCase {
  std::string_view command;
  bool resets;
};

class StatisticsResetTest : public StatisticsTest,
                            public testing::WithParamInterface<StatisticsCase> {};

// Spec #1, story 64: every instrument-wide setting that shapes captures resets them, even when it
// sets the value already in place. The timebase only zooms the display (ADR 0004), so it keeps
// them, like Session-owned settings (ADR 0002), run control, faults, queries and rejected
// commands, which change no such setting.
TEST_P(StatisticsResetTest, OnlySettingChangesResetTheStatistics) {
  capture(3);

  session.send(GetParam().command);
  session.send("*CLS");  // Drops the error of a rejected command.

  const bool resets = GetParam().resets;
  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1"),
            resets ? std::nullopt : std::optional<std::string>("2,2,2,2,0,3"));
  EXPECT_EQ(session.next_error(), resets ? kDataStaleReply : kNoErrorReply);
}

INSTANTIATE_TEST_SUITE_P(
    Commands, StatisticsResetTest,
    testing::Values(StatisticsCase{"SOUR1:FUNC SIN", true}, StatisticsCase{"SOUR1:FREQ 1000", true},
                    StatisticsCase{"SOUR1:AMPL 2", true}, StatisticsCase{"SOUR1:OFFS 0", true},
                    StatisticsCase{"SOUR1:NOIS 0", true}, StatisticsCase{"SOUR3:FREQ 5", true},
                    StatisticsCase{"CHAN1:DISP ON", true}, StatisticsCase{"CHAN1:SCAL 1", true},
                    StatisticsCase{"CHAN1:OFFS 0", true}, StatisticsCase{"CHAN2:DISP OFF", true},
                    StatisticsCase{"ACQ:SRAT 1E6", true}, StatisticsCase{"ACQ:POIN 1000", true},
                    StatisticsCase{"TIM:SCAL 0.001", false},
                    StatisticsCase{"TRIG:SOUR CHAN1", true}, StatisticsCase{"TRIG:LEV 100", true},
                    StatisticsCase{"TRIG:SLOP POS", true}, StatisticsCase{"TRIG:MODE AUTO", true},
                    StatisticsCase{"TRIG:HOLD 0", true}, StatisticsCase{"TRIG:POS 0", true},
                    StatisticsCase{"*RST", true}, StatisticsCase{"WAV:SOUR CHAN1", false},
                    StatisticsCase{"WAV:FORM REAL", false}, StatisticsCase{"FFT:SOUR CHAN1", false},
                    StatisticsCase{"FFT:WIND HANN", false},
                    StatisticsCase{"SOUR1:FAUL GLIT,0", false}, StatisticsCase{"RUN", false},
                    StatisticsCase{"STOP", false}, StatisticsCase{"SINGle", false},
                    StatisticsCase{"*CLS", false}, StatisticsCase{"*IDN?", false},
                    StatisticsCase{"MEAS:VPP? CHAN1", false}, StatisticsCase{"SOUR1:FREQ?", false},
                    StatisticsCase{"TIM:SCAL?", false}, StatisticsCase{"CHAN1:SCAL 100", false},
                    StatisticsCase{"TRIG:MODE SING", false}, StatisticsCase{"ACQ:POIN", false}));

// *RST drops the latest capture too, so the reset only shows once a capture completes after it:
// at the *RST settings, a sine's first capture needs its 5000 pre-trigger samples, a trigger event
// and 5000 more, and a second needs 10000 more again, so 15000 samples hold exactly one.
TEST_F(StatisticsTest, RstResetsTheStatistics) {
  capture(3);

  session.send("*RST");
  instrument.advance(test::kResetAutoTimeout);

  EXPECT_EQ(count_of(session.send("MEAS:STAT? VPP,CHAN1")), 1.0);
}

// A capture's settings are those of its first sample, so the capture a setting change interrupted
// never counts: the first capture after it starts after the change.
TEST_F(StatisticsTest, CountingRestartsWithTheFirstCaptureAfterAChange) {
  instrument.advance(kRecord / 2);
  session.send("SOUR1:AMPL 4");
  capture(2);

  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1"), "4,4,4,4,0,2");
}

TEST_F(StatisticsTest, ClippedValuesCountWithOneWarning) {
  session.send("CHAN1:SCAL 0.2");  // The 1 V peaks are clipped at 0.8 V.
  capture(3);

  EXPECT_EQ(session.send("MEAS:STAT? VMAX,CHAN1"), "0.8,0.8,0.8,0.8,0,3");
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// A 10 V glitch on a 0.5 V level is clipped at the 4 V limit in the first capture only. VMAX is 4,
// 0.5 and 0.5: the current value is OK, but the statistics hold a questionable value.
TEST_F(StatisticsTest, EarlierClippedValueWarnsAfterTheSignalRecovers) {
  session.send("SOUR1:FUNC DC;OFFS 0.5;FAUL GLIT,10");
  capture(3);

  expect_statistics(session.send("MEAS:STAT? VMAX,CHAN1"), expected_of({4.0, 0.5, 0.5}));
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
  EXPECT_EQ(session.send("MEAS:VMAX? CHAN1"), "0.5");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(StatisticsTest, StatusEntriesLetTheRestOfTheLineRun) {
  session.send("CHAN1:SCAL 0.2");
  capture(1);

  EXPECT_EQ(session.send("MEAS:STAT? VMAX,CHAN1;STAT? VMIN,CHAN1;*IDN?"),
            "0.8,0.8,0.8,0.8,0,1;-0.8,-0.8,-0.8,-0.8,0,1;" + session.send("*IDN?").value_or(""));
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kClippedReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(StatisticsTest, EveryEnabledChannelHasItsOwnStatistics) {
  session.send("CHAN2:DISP ON;:CHAN4:DISP ON;:SOUR2:AMPL 3;:SOUR4:AMPL 5");
  capture(2);

  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN"), "2,2,2,2,0,2");
  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN2"), "3,3,3,3,0,2");
  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN4"), "5,5,5,5,0,2");
  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN3;*IDN?"), std::nullopt);
  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(StatisticsTest, LongFormsAreAccepted) {
  session.send("SOUR1:OFFS 0.5");
  capture(1);

  EXPECT_EQ(session.send("MEASure:STATistics? VAVerage,CHANnel1"),
            session.send("meas:stat? vav,chan1"));
  EXPECT_EQ(session.send("MEASure:STATistics? VMAX,CHANnel1"), "1.5,1.5,1.5,1.5,0,1");
  session.send("measure:statistics:reset");
  EXPECT_EQ(session.send("MEAS:STAT? VMAX,CHAN1"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
}

/// A malformed statistics command and the error it reports.
struct MalformedCase {
  std::string_view line;
  std::string_view error;
};

class StatisticsMalformedTest : public StatisticsTest,
                                public testing::WithParamInterface<MalformedCase> {};

// The command runs nothing, so neither the rest of the line nor a reset.
TEST_P(StatisticsMalformedTest, ReportsItsErrorAndRunsNothing) {
  capture(1);

  EXPECT_EQ(session.send(std::format("{};*IDN?", GetParam().line)), std::nullopt);
  EXPECT_EQ(session.next_error(), GetParam().error);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
  EXPECT_EQ(count_of(session.send("MEAS:STAT? VPP,CHAN1")), 1.0);
}

INSTANTIATE_TEST_SUITE_P(
    Commands, StatisticsMalformedTest,
    testing::Values(MalformedCase{"MEAS:STAT?", kMissingParameterReply},
                    MalformedCase{"MEAS:STAT? VPP", kMissingParameterReply},
                    MalformedCase{"MEAS:STAT? FREQX,CHAN1", kIllegalParameterValueReply},
                    MalformedCase{"MEAS:STAT? VPP,CHAN5", kIllegalParameterValueReply},
                    MalformedCase{"MEAS:STAT? 1,CHAN1", kDataTypeErrorReply},
                    MalformedCase{"MEAS:STAT? VPP,CHAN1,1", kParameterNotAllowedReply},
                    MalformedCase{"MEAS:STAT? CHAN1,VPP", kIllegalParameterValueReply},
                    MalformedCase{"MEAS:STAT:RES 1", kParameterNotAllowedReply},
                    MalformedCase{"MEAS:STAT:RES?", kUndefinedHeaderReply}));

// The Single's capture is counted before the lines held on it carry on, so they read it.
TEST_F(StatisticsTest, LineHeldOnASingleReadsTheSinglesCapture) {
  session.send("SOUR1:FUNC DC;OFFS 0.25");
  capture(2);  // Counted until the reset on the line below.

  const auto reply = session.start("MEAS:STAT:RES;:SINGle;*OPC?;:MEAS:STAT? VAV,CHAN1");
  EXPECT_FALSE(reply->ended);
  capture(1);

  EXPECT_TRUE(reply->ended);
  EXPECT_EQ(reply->text, "1;0.25,0.25,0.25,0.25,0,1");
  capture(1);  // Stopped: nothing more is counted.
  EXPECT_EQ(count_of(session.send("MEAS:STAT? VAV,CHAN1")), 1.0);
}

// Live mode (#8): a block acquired before a setting change holds the old signal, so its capture is
// neither published nor counted.
TEST_F(StatisticsTest, BlockAcquiredBeforeASettingChangeIsNotCounted) {
  test::acquire_and_process(instrument, 2 * kRecord, kRecord);
  const AcquiredBlock before = test::acquired(instrument, kRecord);

  session.send("CHAN1:OFFS 0");
  instrument.process(before);

  EXPECT_EQ(session.send("MEAS:STAT? VPP,CHAN1"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
}

// STOP keeps the statistics but discards what was on its way, which is not counted either.
TEST_F(StatisticsTest, BlockAcquiredBeforeStopIsNotCounted) {
  test::acquire_and_process(instrument, 2 * kRecord, kRecord);
  const AcquiredBlock before = test::acquired(instrument, kRecord);

  session.send("STOP");
  instrument.process(before);

  EXPECT_EQ(count_of(session.send("MEAS:STAT? VPP,CHAN1")), 2.0);
}

// The processing thread counts the Single's capture before the lines held on it carry on.
TEST_F(StatisticsTest, LineHeldOnASingleInLiveModeReadsTheSinglesCapture) {
  session.send("SOUR1:FUNC DC;OFFS 0.25");

  const auto reply = session.start("SINGle;*OPC?;:MEAS:STAT? VAV,CHAN1");
  instrument.process(test::acquired(instrument, kRecord));

  EXPECT_TRUE(reply->ended);
  EXPECT_EQ(reply->text, "1;0.25,0.25,0.25,0.25,0,1");
}

}  // namespace
}  // namespace openscope
