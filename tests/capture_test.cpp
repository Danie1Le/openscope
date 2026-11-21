#include "openscope/capture.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/channel.hpp"
#include "openscope/instrument.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/trigger.hpp"

namespace openscope {
namespace {

using testing::ElementsAre;
using testing::FloatNear;

using CaptureTest = test::InstrumentFixture;

TEST_F(CaptureTest, NoCaptureUntilOneRecordLengthHasBeenSampled) {
  EXPECT_EQ(instrument.latest_capture(), nullptr);

  instrument.advance(test::kResetRecordLength - 1);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
}

TEST_F(CaptureTest, AdvancingByZeroSamplesDoesNothing) {
  instrument.advance(0);
  instrument.advance(test::kResetRecordLength - 1);
  instrument.advance(0);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
}

TEST_F(CaptureTest, CaptureSpansOneRecordLengthOfTheEnabledChannelsOnly) {
  instrument.advance(test::kResetRecordLength);

  const std::shared_ptr<const Capture> capture = instrument.latest_capture();
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(capture->first_index, 0U);
  EXPECT_EQ(capture->sample_rate, 1e6);
  EXPECT_EQ(capture->record_length, test::kResetRecordLength);
  const CapturedChannel channel = test::captured(instrument, 1);
  EXPECT_EQ(channel.settings.scale, 1.0);
  EXPECT_EQ(channel.settings.offset, 0.0);
  EXPECT_EQ(channel.samples.size(), test::kResetRecordLength);
}

TEST_F(CaptureTest, ChannelsDisabledAtRstAreNotInTheCapture) {
  instrument.advance(test::kResetRecordLength);

  const std::shared_ptr<const Capture> capture = test::latest(instrument);
  for (std::size_t n = 1; n < kChannelCount; ++n) {
    EXPECT_FALSE(capture->channels.at(n).has_value()) << "CH" << n + 1;
  }
}

TEST_F(CaptureTest, ChannelOneCarriesGeneratorOnesSine) {
  instrument.advance(test::kResetRecordLength);

  const std::vector<float> samples = test::captured(instrument, 1).samples;
  EXPECT_EQ(samples[0], 0.0F);
  EXPECT_EQ(samples[250], 1.0F);
  EXPECT_THAT(samples[500], FloatNear(0.0F, 1e-6F));
  EXPECT_EQ(samples[750], -1.0F);
  EXPECT_EQ(samples[1250], 1.0F);  // The next cycle.
}

TEST_F(CaptureTest, CapturesFollowEachOtherBackToBackAndOnlyTheLatestIsKept) {
  instrument.advance((2 * test::kResetRecordLength) + 5000);
  EXPECT_EQ(instrument.latest_capture()->first_index, test::kResetRecordLength);

  instrument.advance(4999);
  EXPECT_EQ(instrument.latest_capture()->first_index, test::kResetRecordLength);

  instrument.advance(1);
  EXPECT_EQ(instrument.latest_capture()->first_index, 2 * test::kResetRecordLength);
}

TEST_F(CaptureTest, APublishedCaptureNeverChanges) {
  instrument.advance(test::kResetRecordLength);
  const std::shared_ptr<const Capture> first = instrument.latest_capture();
  const std::vector<float> samples = test::captured(instrument, 1).samples;

  instrument.advance(3 * test::kResetRecordLength);

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
  test::advance_in_steps(stepped, kSamples, 999);

  const std::shared_ptr<const Capture> whole = instrument.latest_capture();
  const std::shared_ptr<const Capture> pieces = stepped.latest_capture();
  EXPECT_EQ(whole->first_index,
            ((kSamples / test::kResetRecordLength) - 1) * test::kResetRecordLength);
  EXPECT_EQ(pieces->first_index, whole->first_index);
  EXPECT_EQ(test::captured(stepped, 1).samples, test::captured(instrument, 1).samples);
}

TEST(CaptureAssemblerTest, BlockWithoutAChannelOfTheCaptureInProgressIsALogicError) {
  std::array<ChannelSettings, kChannelCount> channels{};
  channels[1].enabled = true;
  CaptureAssembler assembler;
  SampleBlock block{.first_index = 0, .count = 2, .channels = {}};
  block.channels[0] = {0.0F, 0.0F};
  block.channels[1] = {0.0F, 0.0F};
  assembler.push(block, {.sample_rate = 1.0, .record_length = 4}, channels, {});

  block.first_index = 2;
  block.channels[1].clear();

  EXPECT_THROW(assembler.push(block, {.sample_rate = 1.0, .record_length = 4}, channels, {}),
               std::logic_error);
}

TEST(CaptureAssemblerTest, BlockWithoutTheTriggerChannelIsALogicError) {
  std::array<ChannelSettings, kChannelCount> channels{};
  channels[0].enabled = true;
  CaptureAssembler assembler;
  SampleBlock block{.first_index = 0, .count = 1, .channels = {}};
  block.channels[0] = {0.0F};

  EXPECT_THROW(
      assembler.push(block, {.sample_rate = 1.0, .record_length = 1}, channels, {.source = 2}),
      std::logic_error);
}

TEST(CaptureAssemblerTest, RecordLengthOfZeroIsALogicError) {
  std::array<ChannelSettings, kChannelCount> channels{};
  channels[0].enabled = true;
  CaptureAssembler assembler;
  SampleBlock block{.first_index = 0, .count = 1, .channels = {}};
  block.channels[0] = {0.0F};

  EXPECT_THROW(assembler.push(block, {.sample_rate = 1.0, .record_length = 0}, channels, {}),
               std::logic_error);
}

/// CH1 enabled, triggering on itself.
constexpr std::array<ChannelSettings, kChannelCount> kChannelOne{{{.enabled = true}, {}, {}, {}}};

/// Auto with time zero on the first sample: a signal that never triggers gives an untriggered
/// capture every record length.
constexpr TriggerSettings kUntriggeredEveryRecord{.position = 0.0};

/// A block of CH1 samples from `first` on, rising from 0 V by 1 V a sample, so it never moves the
/// hysteresis band below the 0 V trigger level and never triggers.
SampleBlock rising_block(SampleIndex first, std::size_t count) {
  SampleBlock block{.first_index = first, .count = count, .channels = {}};
  for (std::size_t n = 0; n < count; ++n) {
    block.channels[0].push_back(static_cast<float>(first + n));
  }
  return block;
}

/// The captures an assembler hands on as they complete.
struct HandedOn {
  /// Their first sample indexes, oldest first.
  std::vector<SampleIndex> first_indexes;
  /// The last one.
  std::shared_ptr<const Capture> last;

  /// The sink to pass to `push`.
  CaptureSink sink() {
    return [this](const std::shared_ptr<const Capture>& capture) {
      first_indexes.push_back(capture->first_index);
      last = capture;
    };
  }
};

TEST(CaptureAssemblerTest, PushHandsOnEveryCaptureAsItCompletes) {
  CaptureAssembler assembler;
  HandedOn handed_on;
  const AcquisitionSettings acquisition{.sample_rate = 1.0, .record_length = 2};

  const Pushed three =
      assembler.push(rising_block(0, 7), acquisition, kChannelOne, kUntriggeredEveryRecord,
                     CaptureLimit::kEvery, handed_on.sink());
  EXPECT_THAT(handed_on.first_indexes, ElementsAre(0U, 2U, 4U));
  EXPECT_EQ(handed_on.last, three.capture);  // The last one handed on is returned.

  const Pushed one =
      assembler.push(rising_block(7, 1), acquisition, kChannelOne, kUntriggeredEveryRecord,
                     CaptureLimit::kEvery, handed_on.sink());
  EXPECT_THAT(handed_on.first_indexes, ElementsAre(0U, 2U, 4U, 6U));
  EXPECT_EQ(handed_on.last, one.capture);

  const Pushed none =
      assembler.push(rising_block(8, 1), acquisition, kChannelOne, kUntriggeredEveryRecord,
                     CaptureLimit::kEvery, handed_on.sink());
  EXPECT_THAT(handed_on.first_indexes, ElementsAre(0U, 2U, 4U, 6U));
  EXPECT_EQ(none.capture, nullptr);
}

TEST(CaptureAssemblerTest, PushOfTheFirstCaptureOnlyHandsOnThatCapture) {
  CaptureAssembler assembler;
  HandedOn handed_on;

  const Pushed pushed =
      assembler.push(rising_block(0, 7), {.sample_rate = 1.0, .record_length = 2}, kChannelOne,
                     kUntriggeredEveryRecord, CaptureLimit::kFirst, handed_on.sink());

  EXPECT_EQ(pushed.end, 2U);
  EXPECT_THAT(handed_on.first_indexes, ElementsAre(0U));
  EXPECT_EQ(handed_on.last, pushed.capture);
}

}  // namespace
}  // namespace openscope
