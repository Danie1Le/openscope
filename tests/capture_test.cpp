#include "openscope/capture.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/channel.hpp"
#include "openscope/instrument.hpp"
#include "openscope/sample_block.hpp"

namespace openscope {
namespace {

using testing::FloatNear;

// The *RST acquisition: CH1 only, a 1 V 1 kHz sine, 1 MSa/s, 10000 points.
constexpr std::size_t kRecordLength = 10'000;

using CaptureTest = test::InstrumentFixture;

TEST_F(CaptureTest, NoCaptureUntilOneRecordLengthHasBeenSampled) {
  EXPECT_EQ(instrument.latest_capture(), nullptr);

  instrument.advance(kRecordLength - 1);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
}

TEST_F(CaptureTest, AdvancingByZeroSamplesDoesNothing) {
  instrument.advance(0);
  instrument.advance(kRecordLength - 1);
  instrument.advance(0);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
}

TEST_F(CaptureTest, CaptureSpansOneRecordLengthOfTheEnabledChannelsOnly) {
  instrument.advance(kRecordLength);

  const std::shared_ptr<const Capture> capture = instrument.latest_capture();
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(capture->first_index, 0U);
  EXPECT_EQ(capture->sample_rate, 1e6);
  EXPECT_EQ(capture->record_length, kRecordLength);
  EXPECT_TRUE(capture->untriggered);
  EXPECT_EQ(capture->trigger_position, 0U);
  const CapturedChannel channel = test::captured(instrument, 1);
  EXPECT_EQ(channel.scale, 1.0);
  EXPECT_EQ(channel.offset, 0.0);
  EXPECT_EQ(channel.samples.size(), kRecordLength);
  for (std::size_t n = 1; n < kChannelCount; ++n) {
    EXPECT_FALSE(capture->channels.at(n).has_value()) << "CH" << n + 1;
  }
}

TEST_F(CaptureTest, ChannelOneCarriesGeneratorOnesSine) {
  instrument.advance(kRecordLength);

  const std::vector<float> samples = test::captured(instrument, 1).samples;
  EXPECT_EQ(samples[0], 0.0F);
  EXPECT_EQ(samples[250], 1.0F);
  EXPECT_THAT(samples[500], FloatNear(0.0F, 1e-6F));
  EXPECT_EQ(samples[750], -1.0F);
  EXPECT_EQ(samples[1250], 1.0F);  // The next cycle.
}

TEST_F(CaptureTest, CapturesFollowEachOtherBackToBackAndOnlyTheLatestIsKept) {
  instrument.advance((2 * kRecordLength) + 5000);
  EXPECT_EQ(instrument.latest_capture()->first_index, kRecordLength);

  instrument.advance(4999);
  EXPECT_EQ(instrument.latest_capture()->first_index, kRecordLength);

  instrument.advance(1);
  EXPECT_EQ(instrument.latest_capture()->first_index, 2 * kRecordLength);
}

TEST_F(CaptureTest, APublishedCaptureNeverChanges) {
  instrument.advance(kRecordLength);
  const std::shared_ptr<const Capture> first = instrument.latest_capture();
  const std::vector<float> samples = test::captured(instrument, 1).samples;

  instrument.advance(3 * kRecordLength);

  EXPECT_NE(instrument.latest_capture(), first);
  EXPECT_EQ(first->first_index, 0U);
  EXPECT_EQ(first->channels[0].value_or(CapturedChannel{}).samples, samples);
}

// A capture is the same however the sample clock was stepped, including steps longer than one
// sample block and blocks that end part-way through a capture.
TEST_F(CaptureTest, CaptureDoesNotDependOnHowTheClockWasStepped) {
  constexpr std::size_t kSamples = (3 * Instrument::kMaxBlockLength) + 7;
  instrument.advance(kSamples);

  Instrument stepped;
  for (std::size_t done = 0; done < kSamples; done += 999) {
    stepped.advance(std::min<std::size_t>(999, kSamples - done));
  }

  const std::shared_ptr<const Capture> whole = instrument.latest_capture();
  const std::shared_ptr<const Capture> pieces = stepped.latest_capture();
  EXPECT_EQ(whole->first_index, ((kSamples / kRecordLength) - 1) * kRecordLength);
  EXPECT_EQ(pieces->first_index, whole->first_index);
  EXPECT_EQ(test::captured(stepped, 1).samples, test::captured(instrument, 1).samples);
}

TEST(CaptureAssemblerTest, BlockWithoutAChannelOfTheCaptureInProgressIsALogicError) {
  std::array<ChannelSettings, kChannelCount> channels{};
  channels[1].enabled = true;
  CaptureAssembler assembler;
  SampleBlock block{.first = 0, .count = 2, .channels = {}};
  block.channels[1] = {0.0F, 0.0F};
  assembler.push(block, {.sample_rate = 1.0, .record_length = 4}, channels);

  block.first = 2;
  block.channels[1].clear();

  EXPECT_THROW(assembler.push(block, {.sample_rate = 1.0, .record_length = 4}, channels),
               std::logic_error);
}

}  // namespace
}  // namespace openscope
