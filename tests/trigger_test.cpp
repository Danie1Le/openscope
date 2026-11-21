// The edge trigger. EdgeTrigger's own tests: which samples are trigger events. Then the facade
// tests: trigger events on Generator signals with exact expected sample indexes, and how they
// place captures in Normal and Auto trigger mode, driven through SCPI with the sample clock
// stepped by hand.

#include "openscope/trigger.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {
namespace {

using test::captured;
using test::kResetRecordLength;
using testing::Each;
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

class TriggerTest : public test::InstrumentFixture {
 protected:
  /// The sample indexes of the trigger events over the next `samples` samples. Captures one
  /// sample long with time zero on it, in Normal mode, so every trigger event the holdoff lets
  /// through places a capture of its own; the clock is stepped one sample at a time to see each.
  std::vector<SampleIndex> trigger_events(std::size_t samples) {
    session.send("ACQ:POIN 1;:TRIG:POS 0;MODE NORM");
    std::vector<SampleIndex> events;
    for (std::size_t i = 0; i < samples; ++i) {
      const std::shared_ptr<const Capture> before = instrument.latest_capture();
      instrument.advance(1);
      if (instrument.latest_capture() != before) {
        events.push_back(instrument.latest_capture()->first_index);
      }
    }
    return events;
  }
};

// The *RST trigger: CH1 rising through 0 V. The 1 kHz sine rises through 0 V at every multiple of
// 1000 samples; it starts there, which is not an edge because it has not been below the band.
TEST_F(TriggerTest, DefaultTriggerFiresWhereTheSineRisesThroughZero) {
  EXPECT_THAT(trigger_events(3500), ElementsAre(1000, 2000, 3000));
}

// The sine starts inside neither band edge: 0 V is below 0.4 V (0.5 V less the band) and above
// -0.4 V, so each trigger is armed at once. It reaches 0.5 V at 1/12 of a cycle (83.3 samples)
// and falls to -0.5 V at 7/12 (583.3).
TEST_F(TriggerTest, LevelAndSlopeChooseTheEdge) {
  session.send("TRIG:LEV 0.5");
  EXPECT_THAT(trigger_events(2500), ElementsAre(84, 1084, 2084));

  session.send("TRIG:LEV -0.5;SLOP NEG");
  EXPECT_THAT(trigger_events(2500), ElementsAre(2584, 3584, 4584));
}

TEST_F(TriggerTest, LevelIsInVolts) {
  session.send("SOUR1:OFFS 2;:CHAN1:OFFS 2;:TRIG:LEV 2500MV");

  EXPECT_THAT(trigger_events(2500), ElementsAre(84, 1084, 2084));
}

// A square wave jumps through the level, so its events sit exactly on its rising edges.
TEST_F(TriggerTest, SquareWaveFiresOnItsRisingEdgesOnly) {
  session.send("SOUR1:FUNC SQU");

  EXPECT_THAT(trigger_events(3999), ElementsAre(1000, 2000, 3000));
}

// The band is 0.1 vertical divisions of the trigger channel: a square wave of +-0.1 V reaches it
// at 1 V/div, one of +-0.095 V does not, and at 0.5 V/div a band of 0.05 V lets +-0.05 V through.
TEST_F(TriggerTest, HysteresisIsATenthOfADivisionOfTheTriggerChannel) {
  session.send("SOUR1:FUNC SQU;AMPL 0.2");
  EXPECT_THAT(trigger_events(2500), ElementsAre(1000, 2000));

  session.send("SOUR1:AMPL 0.19");
  EXPECT_THAT(trigger_events(2500), IsEmpty());

  session.send("SOUR1:AMPL 0.1;:CHAN1:SCAL 0.5");
  EXPECT_THAT(trigger_events(2500), ElementsAre(6000, 7000));
}

// Spec story 48: 1% noise (0.02 V RMS on the 2 V sine) jitters the signal across 0 V several times
// at each crossing, but never 0.1 V back down, so each rising crossing gives exactly one event
// within a few samples of it, and falling crossings none.
TEST_F(TriggerTest, NoiseAroundTheLevelGivesNoFalseTriggerEvents) {
  session.send("SOUR1:NOIS 0.02");

  const std::vector<SampleIndex> events = trigger_events(10'500);

  ASSERT_EQ(events.size(), 10U);
  for (std::size_t n = 0; n < events.size(); ++n) {
    const auto crossing = static_cast<double>(1000 * (n + 1));
    EXPECT_NEAR(static_cast<double>(events[n]), crossing, 20.0) << n;
  }
}

// Holdoff counts from the trigger event that placed the last capture. 1 ms is 1000 samples, so
// the next edge, exactly 1000 samples later, is let through; 1.001 ms holds it off.
TEST_F(TriggerTest, HoldoffLetsTheNextEventThroughOnceItHasPassed) {
  session.send("SOUR1:FUNC SQU;:TRIG:HOLD 1MS");
  EXPECT_THAT(trigger_events(3500), ElementsAre(1000, 2000, 3000));

  session.send("TRIG:HOLD 1.001MS");
  EXPECT_THAT(trigger_events(5500), ElementsAre(4000, 6000, 8000));
}

// ADR 0003: the holdoff is counted on the sample clock, so the sample rate sets how many samples
// it spans: at 2 MSa/s the square wave's edges are 2000 samples apart, and 1.5 ms is 3000.
TEST_F(TriggerTest, HoldoffIsCountedInSamplesAtTheSampleRate) {
  session.send("SOUR1:FUNC SQU;:ACQ:SRAT 2E6;:TRIG:HOLD 1.5MS");

  EXPECT_THAT(trigger_events(9000), ElementsAre(2000, 6000));
}

// With whole records: the event at 5000 places the first capture, the pre-trigger samples are
// full again at 15000, but the 12 ms holdoff lasts until 17000.
TEST_F(TriggerTest, HoldoffDelaysTheNextCaptureBeyondItsPreTriggerSamples) {
  session.send("TRIG:HOLD 12MS");

  instrument.advance(21'999);
  EXPECT_EQ(test::first_index(instrument), 0U);

  instrument.advance(1);
  EXPECT_EQ(test::first_index(instrument), 12'000U);
}

// The *RST position, 50%, puts time zero at sample 5000 of 10000. The events before 5000 come
// before the pre-trigger samples have filled, so 5000 is the first that places a capture.
TEST_F(TriggerTest, DefaultPositionPutsTheTriggerEventHalfWayThroughTheCapture) {
  instrument.advance(kResetRecordLength);

  const std::shared_ptr<const Capture> capture = test::latest(instrument);
  EXPECT_FALSE(capture->untriggered);
  EXPECT_EQ(capture->first_index, 0U);
  EXPECT_EQ(capture->trigger_position, 5000U);
  const std::vector<float> samples = captured(instrument, 1).samples;
  EXPECT_LT(samples[4999], 0.0F);
  EXPECT_GE(samples[5000], 0.0F);
  EXPECT_EQ(session.send("WAV:PRE?"), "1E+06,10000,1,0,5000,0");
}

// Time zero sits `position` percent of the way from the first sample to the last: 25% of 9999 is
// 2499.75, which rounds to 2500, so the first event with 2500 samples before it is at 3000.
TEST_F(TriggerTest, PositionSetsHowMuchOfTheCaptureComesBeforeTheTriggerEvent) {
  session.send("TRIG:POS 25");

  instrument.advance(10'500);

  EXPECT_EQ(test::first_index(instrument), 500U);
  EXPECT_EQ(session.send("WAV:PRE?"), "1E+06,10000,1,0,2500,0");
}

// At 0% the first event, at 1000, is time zero and the first sample. At 100% time zero is the
// last sample, with 9999 before it: the change at 11000 restarts the capture, so the first event
// with that many samples before it is at 21000.
TEST_F(TriggerTest, PositionsAtEitherEndOfTheRecord) {
  session.send("TRIG:POS 0");
  instrument.advance(1000 + kResetRecordLength);
  EXPECT_EQ(test::first_index(instrument), 1000U);
  EXPECT_EQ(test::latest(instrument)->trigger_position, 0U);

  session.send("TRIG:POS 100");
  instrument.advance(10'001);  // The capture ends one sample after time zero.
  EXPECT_EQ(test::first_index(instrument), 21'000U - 9999U);
  EXPECT_EQ(test::latest(instrument)->trigger_position, 9999U);
}

// At 300 Hz the sine rises through 0 V at multiples of 3333.3 samples; 6667 is the first event
// with the 5000 pre-trigger samples before it.
TEST_F(TriggerTest, EventsAreIgnoredUntilThePreTriggerSamplesHaveFilled) {
  session.send("SOUR1:FREQ 300");

  instrument.advance(6667 + 5000 - 1);
  EXPECT_EQ(instrument.latest_capture(), nullptr);

  instrument.advance(1);
  EXPECT_EQ(test::first_index(instrument), 6667U - 5000U);
}

// At 400 Hz the sine rises through 0 V at multiples of 2500. After the capture placed at 5000
// completes, the pre-trigger samples fill again from index 10000 on: the event at 12500 has only
// 2500 of them and is ignored, although the capture before holds older samples.
TEST_F(TriggerTest, EachCaptureFillsItsOwnPreTriggerSamples) {
  session.send("SOUR1:FREQ 400");

  instrument.advance(17'500);
  EXPECT_EQ(test::first_index(instrument), 0U);

  instrument.advance(2500);
  EXPECT_EQ(test::first_index(instrument), kResetRecordLength);
}

// Spec story 52: in Normal mode a DC signal, which never crosses the level, never captures.
TEST_F(TriggerTest, NormalModeCapturesOnlyOnATriggerEvent) {
  session.send("SOUR1:FUNC DC;:TRIG:MODE NORM");

  instrument.advance(5 * kResetRecordLength);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
}

TEST_F(TriggerTest, NormalModeKeepsTheLatestCaptureWhileItWaits) {
  instrument.advance(kResetRecordLength);
  const std::shared_ptr<const Capture> before = instrument.latest_capture();

  session.send("SOUR1:FUNC DC;:TRIG:MODE NORM");
  instrument.advance(5 * kResetRecordLength);

  EXPECT_EQ(instrument.latest_capture(), before);
}

// An 8 Hz sine starts rising from 0 V and only dips below the band after index 62500, so its
// first trigger event is at 125000. Normal mode waits for it however many records that takes and
// keeps the samples just before it, wherever the position puts time zero: each captured sample is
// the sine at its own sample index.
class NormalModeTest : public TriggerTest, public testing::WithParamInterface<std::string_view> {};

TEST_P(NormalModeTest, WaitsAsLongAsItTakesForATriggerEvent) {
  session.send("SOUR1:FREQ 8;:TRIG:MODE NORM;POS " + std::string(GetParam()));

  instrument.advance(140'000);

  const std::shared_ptr<const Capture> capture = test::latest(instrument);
  EXPECT_FALSE(capture->untriggered);
  EXPECT_EQ(capture->first_index + capture->trigger_position, 125'000U);
  const std::vector<float> samples = captured(instrument, 1).samples;
  for (std::size_t k = 0; k < samples.size(); k += 997) {
    const double t = static_cast<double>(capture->first_index + k) / 1e6;
    EXPECT_NEAR(samples[k], std::sin(2.0 * std::numbers::pi * 8.0 * t), 1e-6) << k;
  }
}

INSTANTIATE_TEST_SUITE_P(Positions, NormalModeTest, testing::Values("0", "50", "100"));

// Spec story 53: Auto takes an untriggered capture once one record length of samples has passed
// without a trigger event after the 5000 pre-trigger samples have filled. A DC signal never
// crosses the level, so the capture is the record from 5000 to 14999.
TEST_F(TriggerTest, AutoTakesAnUntriggeredCaptureAfterOneRecordLengthWithoutATriggerEvent) {
  session.send("SOUR1:FUNC DC;OFFS 0.5");

  instrument.advance(test::kResetAutoTimeout - 1);
  EXPECT_EQ(instrument.latest_capture(), nullptr);
  instrument.advance(1);

  const std::shared_ptr<const Capture> capture = test::latest(instrument);
  EXPECT_TRUE(capture->untriggered);
  EXPECT_EQ(capture->first_index, 5000U);
  EXPECT_THAT(captured(instrument, 1).samples, Each(0.5F));
  EXPECT_EQ(session.send("WAV:PRE?"), "1E+06,10000,1,0,5000,1");
}

// The Auto timeout runs for one record length from when the pre-trigger samples are full, so a
// late pre-trigger position does not starve the trigger. At 100% of 10000 points they are full at
// 9999 and Auto waits to 19998: the 50 Hz square wave's rising edge at 20000 is too late. At 10001
// points they are full at 10000 and Auto waits to 20000, which takes that edge.
TEST_F(TriggerTest, AutoTimeoutIsOneRecordLengthAfterThePreTriggerSamplesFill) {
  session.send("SOUR1:FUNC SQU;FREQ 50;:TRIG:POS 100");
  instrument.advance(20'000);
  EXPECT_TRUE(test::latest(instrument)->untriggered);
  EXPECT_EQ(test::first_index(instrument), 9999U);

  Instrument longer;
  test::ScpiSession client{longer};
  client.send("SOUR1:FUNC SQU;FREQ 50;:TRIG:POS 100;:ACQ:POIN 10001");
  longer.advance(20'001);
  const std::shared_ptr<const Capture> capture = test::latest(longer);
  EXPECT_FALSE(capture->untriggered);
  EXPECT_EQ(capture->first_index, 10'000U);
  EXPECT_EQ(capture->trigger_position, 10'000U);
}

// The 8 Hz sine's first event is at 125000. Until then Auto takes an untriggered capture every
// 15000 samples (the last at 110000 to 119999), and that event, 5000 samples into the capture that
// starts collecting at 120000, places the next one.
TEST_F(TriggerTest, AutoGoesBackToTriggeredCapturesWhenTriggerEventsArrive) {
  session.send("SOUR1:FREQ 8");

  instrument.advance(120'000);
  EXPECT_TRUE(test::latest(instrument)->untriggered);
  EXPECT_EQ(test::first_index(instrument), 110'000U);

  instrument.advance(kResetRecordLength);
  EXPECT_FALSE(test::latest(instrument)->untriggered);
  EXPECT_EQ(test::first_index(instrument), 120'000U);
}

// The trigger channel need not be enabled: CH2's 300 Hz sine places a capture of CH1's DC.
TEST_F(TriggerTest, ADisabledTriggerChannelPlacesCapturesOfTheEnabledOnes) {
  session.send("TRIG:SOUR CHAN2;:SOUR2:FREQ 300;:SOUR1:FUNC DC");

  instrument.advance(6667 + 5000);

  const std::shared_ptr<const Capture> capture = test::latest(instrument);
  EXPECT_FALSE(capture->untriggered);
  EXPECT_EQ(capture->first_index, 6667U - 5000U);
  EXPECT_FALSE(capture->channels[1].has_value());
}

}  // namespace
}  // namespace openscope
