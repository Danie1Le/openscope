// CaptureAssembler on its own: how sample blocks become captures, and the blocks it rejects.
// Captures through the facade are in capture_test.cpp and trigger_test.cpp.

#include "openscope/capture_assembler.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>

#include "capture_blocks.hpp"
#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"

namespace openscope {
namespace {

using test::HandedOn;
using test::kChannelOne;
using test::kUntriggeredEveryRecord;
using test::rising_block;
using testing::ElementsAre;

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
  CaptureAssembler assembler;

  EXPECT_THROW(assembler.push(rising_block(0, 1), {.sample_rate = 1.0, .record_length = 1},
                              kChannelOne, {.source = 2}),
               std::logic_error);
}

TEST(CaptureAssemblerTest, RecordLengthOfZeroIsALogicError) {
  CaptureAssembler assembler;

  EXPECT_THROW(
      assembler.push(rising_block(0, 1), {.sample_rate = 1.0, .record_length = 0}, kChannelOne, {}),
      std::logic_error);
}

TEST(CaptureAssemblerTest, PushHandsOnEveryCaptureAsItCompletes) {
  CaptureAssembler assembler;
  HandedOn handed_on;
  const AcquisitionSettings acquisition{.sample_rate = 1.0, .record_length = 2};

  assembler.push(rising_block(0, 7), acquisition, kChannelOne, kUntriggeredEveryRecord,
                 CaptureLimit::kEvery, handed_on.sink());
  EXPECT_THAT(handed_on.first_indexes(), ElementsAre(0U, 2U, 4U));

  assembler.push(rising_block(7, 1), acquisition, kChannelOne, kUntriggeredEveryRecord,
                 CaptureLimit::kEvery, handed_on.sink());
  EXPECT_THAT(handed_on.first_indexes(), ElementsAre(0U, 2U, 4U, 6U));

  const std::optional<std::size_t> end =
      assembler.push(rising_block(8, 1), acquisition, kChannelOne, kUntriggeredEveryRecord,
                     CaptureLimit::kEvery, handed_on.sink());
  EXPECT_THAT(handed_on.first_indexes(), ElementsAre(0U, 2U, 4U, 6U));
  EXPECT_EQ(end, std::nullopt);
}

TEST(CaptureAssemblerTest, PushOfTheFirstCaptureOnlyHandsOnThatCapture) {
  CaptureAssembler assembler;
  HandedOn handed_on;

  const std::optional<std::size_t> end =
      assembler.push(rising_block(0, 7), {.sample_rate = 1.0, .record_length = 2}, kChannelOne,
                     kUntriggeredEveryRecord, CaptureLimit::kFirst, handed_on.sink());

  EXPECT_EQ(end, 2U);
  EXPECT_THAT(handed_on.first_indexes(), ElementsAre(0U));
}

}  // namespace
}  // namespace openscope
