// What the generator, channel, acquisition and timebase settings do to captures, driven through
// SCPI with the sample clock stepped by hand.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::captured;
using test::kResetRecordLength;
using test::ScpiSession;
using testing::Each;
using testing::ElementsAre;
using testing::Eq;
using testing::FloatNear;

class AcquisitionTest : public test::InstrumentFixture {
 protected:
  /// The first sample index of the latest capture, or -1 when there is none.
  [[nodiscard]] std::int64_t latest_first_index() const {
    const std::shared_ptr<const Capture> capture = instrument.latest_capture();
    return capture == nullptr ? -1 : static_cast<std::int64_t>(capture->first_index);
  }
};

TEST_F(AcquisitionTest, ChannelNIsFedByGeneratorN) {
  session.send("CHAN2:DISP ON;:CHAN3:DISP ON;:CHAN4:DISP ON");
  session.send("SOUR1:FUNC DC;OFFS 0.5;:SOUR2:FUNC DC;OFFS 1;:SOUR3:FUNC DC;OFFS 1.5");
  session.send("SOUR4:FUNC DC;OFFS 2");

  instrument.advance(kResetRecordLength);

  EXPECT_THAT(captured(instrument, 1).samples, Each(Eq(0.5F)));
  EXPECT_THAT(captured(instrument, 2).samples, Each(Eq(1.0F)));
  EXPECT_THAT(captured(instrument, 3).samples, Each(Eq(1.5F)));
  EXPECT_THAT(captured(instrument, 4).samples, Each(Eq(2.0F)));
}

TEST_F(AcquisitionTest, FunctionFrequencyAmplitudeAndOffsetShapeTheCapturedSignal) {
  // 250 kHz at 1 MSa/s is four samples per cycle.
  session.send("SOUR1:FUNC SQU;FREQ 250KHZ;AMPL 2;OFFS 1");
  session.send(":ACQ:POIN 8");

  instrument.advance(8);

  EXPECT_THAT(captured(instrument, 1).samples,
              ElementsAre(3.0F, 3.0F, -1.0F, -1.0F, 3.0F, 3.0F, -1.0F, -1.0F));
}

TEST_F(AcquisitionTest, SampleRateSetsTheTimeBetweenSamples) {
  session.send("ACQ:SRAT 4000;POIN 4;:SOUR1:FUNC RAMP;FREQ 1000");

  instrument.advance(4);

  EXPECT_THAT(captured(instrument, 1).samples, ElementsAre(-1.0F, -0.5F, 0.0F, 0.5F));
  EXPECT_EQ(instrument.latest_capture()->sample_rate, 4000.0);
}

// The ramp is half way through its cycle (0 V) when its frequency halves, and keeps rising from
// there at the new rate. Restarting the phase at the new frequency would give -0.5 V at index 2.
TEST_F(AcquisitionTest, FrequencyChangeIsPhaseContinuous) {
  session.send("ACQ:POIN 5;:SOUR1:FUNC RAMP;FREQ 250KHZ");
  instrument.advance(2);

  session.send("SOUR1:FREQ 125KHZ");
  instrument.advance(3);

  EXPECT_THAT(captured(instrument, 1).samples, ElementsAre(-1.0F, -0.5F, 0.0F, 0.25F, 0.5F));
}

TEST_F(AcquisitionTest, SampleRateChangeIsPhaseContinuous) {
  session.send("ACQ:SRAT 4000;:SOUR1:FUNC RAMP;FREQ 1000");
  instrument.advance(3);  // The ramp reaches phase 0.75 at index 3.

  session.send("ACQ:SRAT 8000;POIN 3");
  instrument.advance(3);

  EXPECT_THAT(captured(instrument, 1).samples, ElementsAre(0.5F, 0.75F, -1.0F));
}

TEST_F(AcquisitionTest, SameSeedAndCommandsGiveIdenticalCapturesAndAnotherSeedDoesNot) {
  const auto run = [](std::uint64_t seed) {
    Instrument noisy{seed};
    ScpiSession client{noisy};
    client.send("SOUR1:NOIS 0.1;:SOUR2:FUNC NOIS;:CHAN2:DISP ON;:ACQ:POIN 1000");
    noisy.advance(1500);
    client.send("SOUR1:FREQ 3KHZ");
    noisy.advance(1500);
    return std::vector{captured(noisy, 1).samples, captured(noisy, 2).samples};
  };

  const auto first = run(9);

  EXPECT_EQ(run(9), first);
  EXPECT_NE(run(10), first);
  EXPECT_NE(first[0], first[1]);  // The two channels' noise differs.
}

TEST_F(AcquisitionTest, SamplesOutsideTheInputRangeAreClippedToItsLimits) {
  // 0.25 V/div around 0.5 V: the input range is -0.5 V to 1.5 V, and the 1 V sine dips below it.
  session.send("CHAN1:SCAL 0.25;OFFS 0.5");

  instrument.advance(kResetRecordLength);

  const std::vector<float> samples = captured(instrument, 1).samples;
  const auto [low, high] = std::ranges::minmax(samples);
  EXPECT_EQ(low, -0.5F);
  EXPECT_EQ(high, 1.0F);
  EXPECT_GT(std::ranges::count(samples, -0.5F), 1000);
}

TEST_F(AcquisitionTest, SamplesAboveTheInputRangeAreClippedToItsUpperLimit) {
  session.send("CHAN1:SCAL 0.25;OFFS -0.5");  // -1.5 V to 0.5 V.

  instrument.advance(kResetRecordLength);

  const auto [low, high] = std::ranges::minmax(captured(instrument, 1).samples);
  EXPECT_EQ(low, -1.0F);
  EXPECT_EQ(high, 0.5F);
}

TEST_F(AcquisitionTest, SampleExactlyAtTheLimitIsKeptAndOneBeyondIsHeldThere) {
  session.send("SOUR1:FUNC DC;OFFS 4;:SOUR2:FUNC DC;OFFS -4.5;:CHAN2:DISP ON");

  instrument.advance(kResetRecordLength);

  EXPECT_THAT(captured(instrument, 1).samples, Each(Eq(4.0F)));
  EXPECT_THAT(captured(instrument, 2).samples, Each(Eq(-4.0F)));
}

TEST_F(AcquisitionTest, NoiseIsClippedLikeTheSignal) {
  session.send("SOUR1:FUNC DC;NOIS 1;:CHAN1:SCAL 0.1");

  instrument.advance(kResetRecordLength);

  const auto [low, high] = std::ranges::minmax(captured(instrument, 1).samples);
  EXPECT_EQ(low, -0.4F);
  EXPECT_EQ(high, 0.4F);
}

TEST_F(AcquisitionTest, OnlyEnabledChannelsAreKeptInACapture) {
  session.send("CHAN1:DISP OFF;:CHAN3:DISP ON");

  instrument.advance(kResetRecordLength);

  const std::shared_ptr<const Capture> capture = instrument.latest_capture();
  ASSERT_NE(capture, nullptr);
  EXPECT_FALSE(capture->channels[0].has_value());
  EXPECT_FALSE(capture->channels[1].has_value());
  EXPECT_EQ(captured(instrument, 3).samples.size(), kResetRecordLength);
  EXPECT_FALSE(capture->channels[3].has_value());
}

TEST_F(AcquisitionTest, WithNoChannelEnabledCapturesStillCompleteButHoldNoSamples) {
  session.send("CHAN1:DISP OFF");

  instrument.advance(kResetRecordLength);

  const std::shared_ptr<const Capture> capture = instrument.latest_capture();
  ASSERT_NE(capture, nullptr);
  for (const auto& channel : capture->channels) {
    EXPECT_FALSE(channel.has_value());
  }
}

TEST_F(AcquisitionTest, RecordLengthSetsTheSpanOfACapture) {
  session.send("ACQ:POIN 100");

  instrument.advance(250);

  EXPECT_EQ(instrument.latest_capture()->record_length, 100U);
  EXPECT_EQ(instrument.latest_capture()->first_index, 100U);
  EXPECT_EQ(captured(instrument, 1).samples.size(), 100U);
}

TEST_F(AcquisitionTest, RecordLengthOfOneCompletesACaptureOnEverySample) {
  session.send("ACQ:POIN 1");

  instrument.advance(3);

  EXPECT_EQ(instrument.latest_capture()->first_index, 2U);
  EXPECT_EQ(captured(instrument, 1).samples.size(), 1U);
}

TEST_F(AcquisitionTest, ACaptureKeepsTheVerticalSettingsItWasTakenWith) {
  session.send("CHAN1:SCAL 2;OFFS 0.5");
  instrument.advance(kResetRecordLength);

  session.send("CHAN1:SCAL 3;OFFS 1");

  EXPECT_EQ(captured(instrument, 1).settings.scale, 2.0);
  EXPECT_EQ(captured(instrument, 1).settings.offset, 0.5);
}

// A capture takes its settings from its first sample, so a change part-way through one would mix
// old and new settings: the capture in progress is dropped and the next starts at the change.
TEST_F(AcquisitionTest, ChannelAndAcquisitionChangesRestartTheCaptureInProgress) {
  for (const char* change : {"CHAN2:DISP ON", "CHAN1:DISP ON", "CHAN1:SCAL 2", "CHAN3:OFFS 1",
                             "ACQ:SRAT 2E6", "ACQ:POIN 10000"}) {
    Instrument fresh;
    ScpiSession client{fresh};
    fresh.advance(5000);

    client.send(change);
    fresh.advance(kResetRecordLength - 1);
    EXPECT_EQ(fresh.latest_capture(), nullptr) << change;

    fresh.advance(1);
    ASSERT_NE(fresh.latest_capture(), nullptr) << change;
    EXPECT_EQ(fresh.latest_capture()->first_index, 5000U) << change;
  }
}

TEST_F(AcquisitionTest, EnablingAChannelMidCaptureAddsItToTheNextCapture) {
  instrument.advance(5000);

  session.send("CHAN2:DISP ON");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(captured(instrument, 1).samples.size(), kResetRecordLength);
  EXPECT_EQ(captured(instrument, 2).samples.size(), kResetRecordLength);
}

TEST_F(AcquisitionTest, GeneratorAndTimebaseChangesKeepTheCaptureInProgress) {
  instrument.advance(5000);

  session.send("SOUR1:AMPL 2;FUNC SQU;FREQ 2KHZ;OFFS 1;NOIS 0.1;:TIM:SCAL 1");
  instrument.advance(5000);

  EXPECT_EQ(latest_first_index(), 0);
}

TEST_F(AcquisitionTest, RejectedChangesKeepTheCaptureInProgress) {
  instrument.advance(5000);

  session.send("CHAN1:SCAL 100");
  session.send("ACQ:POIN 0");
  session.send("CHAN5:DISP ON");
  instrument.advance(5000);

  EXPECT_EQ(latest_first_index(), 0);
}

// ADR 0004: the timebase is a display viewport.
TEST_F(AcquisitionTest, TimebaseNeverChangesWhatIsCaptured) {
  session.send("TIM:SCAL 1E-9");
  instrument.advance(kResetRecordLength);
  const std::vector<float> fine = captured(instrument, 1).samples;

  Instrument other;
  ScpiSession client{other};
  client.send("TIM:SCAL 100");
  other.advance(kResetRecordLength);

  EXPECT_EQ(other.latest_capture()->record_length, kResetRecordLength);
  EXPECT_EQ(other.latest_capture()->sample_rate, 1e6);
  EXPECT_EQ(captured(other, 1).samples, fine);
}

TEST_F(AcquisitionTest, RstDropsTheLatestCaptureAndTheCaptureInProgress) {
  session.send("ACQ:POIN 100");
  instrument.advance(150);

  session.send("*RST");

  EXPECT_EQ(instrument.latest_capture(), nullptr);
  instrument.advance(kResetRecordLength - 1);
  EXPECT_EQ(instrument.latest_capture(), nullptr);
  instrument.advance(1);
  EXPECT_EQ(latest_first_index(), 150);
}

TEST_F(AcquisitionTest, RstRestoresTheDefaultSignalWithoutAPhaseJump) {
  session.send("ACQ:SRAT 4000;:SOUR1:FUNC SQU;AMPL 3;:CHAN2:DISP ON");
  instrument.advance(252);  // 63 whole cycles of 1 kHz at 4 kSa/s: the phase is back at 0.

  session.send("*RST");
  instrument.advance(kResetRecordLength);

  const std::vector<float> samples = captured(instrument, 1).samples;
  EXPECT_THAT(samples[0], FloatNear(0.0F, 1e-6F));
  EXPECT_EQ(samples[250], 1.0F);
  EXPECT_FALSE(instrument.latest_capture()->channels[1].has_value());
}

}  // namespace
}  // namespace openscope
