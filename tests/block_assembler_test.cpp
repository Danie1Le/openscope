// BlockAssembler on its own, with hand-built blocks: captures across blocks, the discard at a new
// generation or after a dropped block, Armed-single blocks, and a Single that RUN ended. Through
// the facade (acquire and process) these rules are in block_processing_test.cpp.

#include "openscope/block_assembler.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {
namespace {

using testing::ElementsAre;

/// The record length of the captures blocks() makes.
constexpr std::size_t kRecordLength = 4;

/// What a block is acquired under, past its samples.
struct Acquired {
  std::uint64_t sequence = 0;
  std::uint64_t generation = 0;
  CaptureLimit limit = CaptureLimit::kEvery;
};

/// A block of CH1 samples over sample indexes [first, first + count), each sample its own index
/// in volts, so a capture's samples show where it came from. Under Auto with time zero at the
/// first sample and no trigger event (the samples never fall below the level), each record length
/// of samples from where capturing starts is an untriggered capture.
AcquiredBlock block_of(SampleIndex first, std::size_t count, Acquired acquired = {}) {
  AcquiredBlock block{.samples = {.first_index = first, .count = count, .channels = {}},
                      .acquisition = {.sample_rate = 1.0, .record_length = kRecordLength},
                      .channels = {},
                      .trigger = {.position = 0.0},
                      .limit = acquired.limit,
                      .generation = acquired.generation,
                      .sequence = acquired.sequence};
  block.channels.front().enabled = true;
  for (std::size_t offset = 0; offset < count; ++offset) {
    block.samples.channels.front().push_back(static_cast<float>(first + offset));
  }
  return block;
}

/// A BlockAssembler and the first sample index of every capture it has completed.
class BlockAssemblerTest : public testing::Test {
 protected:
  std::optional<std::size_t> assemble(const AcquiredBlock& block) {
    return blocks.assemble(block, sink);
  }

  BlockAssembler blocks;
  std::vector<SampleIndex> firsts;
  std::vector<std::shared_ptr<const Capture>> captures;
  CaptureSink sink = [this](const std::shared_ptr<const Capture>& capture) {
    firsts.push_back(capture->first_index);
    captures.push_back(capture);
  };
};

TEST_F(BlockAssemblerTest, CapturesSpanTheBlocksTheyFillFrom) {
  EXPECT_EQ(assemble(block_of(0, 6)), std::nullopt);
  EXPECT_EQ(assemble(block_of(6, 4, {.sequence = 1})), std::nullopt);

  EXPECT_THAT(firsts, ElementsAre(0U, 4U));
  EXPECT_THAT(captured_channel(*captures.back(), 1).samples, ElementsAre(4.0F, 5.0F, 6.0F, 7.0F));
  EXPECT_EQ(blocks.discarded_captures(), 0U);
}

TEST_F(BlockAssemblerTest, DroppedBlockDiscardsTheCaptureItWouldSplitAndCountsIt) {
  assemble(block_of(0, 6));
  assemble(block_of(10, 8, {.sequence = 2}));  // Sequence 1 never arrives.

  EXPECT_THAT(firsts, ElementsAre(0U, 10U, 14U));
  EXPECT_EQ(blocks.discarded_captures(), 1U);
}

TEST_F(BlockAssemblerTest, DroppedBlockBetweenCapturesDiscardsNothing) {
  assemble(block_of(0, 4));
  assemble(block_of(8, 4, {.sequence = 2}));

  EXPECT_THAT(firsts, ElementsAre(0U, 8U));
  EXPECT_EQ(blocks.discarded_captures(), 0U);
}

TEST_F(BlockAssemblerTest, BlocksDroppedBeforeTheFirstDiscardNothing) {
  assemble(block_of(12, 4, {.sequence = 3}));

  EXPECT_THAT(firsts, ElementsAre(12U));
  EXPECT_EQ(blocks.discarded_captures(), 0U);
}

TEST_F(BlockAssemblerTest, NewGenerationStartsAFreshCaptureWithoutCountingIt) {
  assemble(block_of(0, 6));
  assemble(block_of(6, 8, {.sequence = 1, .generation = 1}));

  EXPECT_THAT(firsts, ElementsAre(0U, 6U, 10U));
  EXPECT_EQ(blocks.discarded_captures(), 0U);
}

// A capture of the older generation would have been discarded anyway, so the gap discards none.
TEST_F(BlockAssemblerTest, NewGenerationAfterADroppedBlockCountsNothing) {
  assemble(block_of(0, 6));
  assemble(block_of(10, 4, {.sequence = 2, .generation = 1}));

  EXPECT_THAT(firsts, ElementsAre(0U, 10U));
  EXPECT_EQ(blocks.discarded_captures(), 0U);
}

TEST_F(BlockAssemblerTest, ArmedSingleBlockStopsAfterItsFirstCapture) {
  EXPECT_EQ(assemble(block_of(0, 10, {.limit = CaptureLimit::kFirst})), kRecordLength);

  EXPECT_THAT(firsts, ElementsAre(0U));
}

TEST_F(BlockAssemblerTest, ArmedSingleBlockWithoutACaptureEndsNothing) {
  EXPECT_EQ(assemble(block_of(0, 3, {.limit = CaptureLimit::kFirst})), std::nullopt);

  EXPECT_TRUE(firsts.empty());
}

TEST_F(BlockAssemblerTest, CaptureLimitIsTheBlocksOwnUntilRunEndsItsSingle) {
  const AcquiredBlock armed = block_of(0, 10, {.limit = CaptureLimit::kFirst});
  EXPECT_EQ(blocks.capture_limit(armed), CaptureLimit::kFirst);
  EXPECT_EQ(blocks.capture_limit(block_of(0, 10)), CaptureLimit::kEvery);

  blocks.run_rest(armed, kRecordLength, sink);

  EXPECT_EQ(blocks.capture_limit(block_of(10, 4, {.limit = CaptureLimit::kFirst})),
            CaptureLimit::kEvery);
  EXPECT_EQ(blocks.capture_limit(block_of(10, 4, {.generation = 1, .limit = CaptureLimit::kFirst})),
            CaptureLimit::kFirst);
}

// RUN came after the block was acquired for a Single: the samples after the Single's capture, and
// the generation's later Armed-single blocks, are captured as Running.
TEST_F(BlockAssemblerTest, RunRestCapturesTheRestAsRunning) {
  const AcquiredBlock armed = block_of(0, 10, {.limit = CaptureLimit::kFirst});
  const std::optional<std::size_t> single_end = assemble(armed);
  ASSERT_EQ(single_end, kRecordLength);

  blocks.run_rest(armed, *single_end, sink);
  EXPECT_EQ(assemble(block_of(10, 10, {.sequence = 1, .limit = CaptureLimit::kFirst})),
            std::nullopt);

  EXPECT_THAT(firsts, ElementsAre(0U, 4U, 8U, 12U, 16U));
  EXPECT_THAT(captured_channel(*captures.at(2), 1).samples, ElementsAre(8.0F, 9.0F, 10.0F, 11.0F));
}

TEST_F(BlockAssemblerTest, RunRestAtTheEndOfTheBlockCapturesFromTheNextBlock) {
  const AcquiredBlock armed = block_of(0, 4, {.limit = CaptureLimit::kFirst});
  ASSERT_EQ(assemble(armed), kRecordLength);

  blocks.run_rest(armed, kRecordLength, sink);
  assemble(block_of(4, 8, {.sequence = 1, .limit = CaptureLimit::kFirst}));

  EXPECT_THAT(firsts, ElementsAre(0U, 4U, 8U));
}

}  // namespace
}  // namespace openscope
