// Live mode's two halves, `acquire` and `process`, driven by hand on one thread: blocks carry the
// settings they were acquired under, a block acquired before a command never completes a capture
// after it, and a capture that would span a dropped block (one acquired and never processed) is
// discarded and counted (#8).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::first_index;
using test::kResetRecordLength;
using test::kUntriggeredDc;
using test::ScpiSession;

/// The next `count` sample indexes of `instrument`, acquired. Fails the test when the instrument
/// acquired nothing (it is Stopped).
AcquiredBlock acquired(Instrument& instrument, std::size_t count) {
  std::optional<AcquiredBlock> block = instrument.acquire(count);
  if (!block) {
    ADD_FAILURE() << "nothing acquired";
    return {};
  }
  return std::move(*block);
}

/// Acquires `samples` sample indexes in blocks of at most `step` and processes each.
void acquire_and_process(Instrument& instrument, std::size_t samples, std::size_t step) {
  for (std::size_t done = 0; done < samples; done += step) {
    instrument.process(acquired(instrument, std::min(step, samples - done)));
  }
}

using BlockProcessingTest = test::UntriggeredDcFixture;

/// Normal trigger mode, time zero at the first sample and ten-sample captures: each capture is
/// placed by one trigger event.
constexpr auto kNormalTenSamples = "TRIG:MODE NORM;POS 0;:ACQ:POIN 10";

// The two halves together make the same captures as stepping the clock, whatever the block size.
TEST(BlockProcessingStepTest, ProcessingAcquiredBlocksGivesTheCapturesAdvanceGives) {
  constexpr auto kSetup = "SOUR1:NOIS 0.05;:CHAN2:DISP ON;:TRIG:POS 30";
  constexpr std::size_t kSamples = (3 * Instrument::kMaxBlockLength) + 123;
  Instrument stepped(7);
  ScpiSession(stepped).send(kSetup);
  stepped.advance(kSamples);

  for (const std::size_t step : {std::size_t{999}, Instrument::kMaxBlockLength}) {
    Instrument live(7);
    ScpiSession(live).send(kSetup);

    acquire_and_process(live, kSamples, step);

    const std::shared_ptr<const Capture> expected = test::latest(stepped);
    const std::shared_ptr<const Capture> capture = test::latest(live);
    EXPECT_EQ(capture->first_index, expected->first_index) << "step " << step;
    EXPECT_EQ(capture->untriggered, expected->untriggered) << "step " << step;
    EXPECT_EQ(test::captured(live, 1).samples, test::captured(stepped, 1).samples);
    EXPECT_EQ(test::captured(live, 2).samples, test::captured(stepped, 2).samples);
  }
}

TEST_F(BlockProcessingTest, BlockAcquiredBeforeASettingChangeNeverCompletesACapture) {
  const AcquiredBlock before = acquired(instrument, kResetRecordLength);

  session.send("CHAN1:SCAL 2");
  instrument.process(before);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
  instrument.process(acquired(instrument, kResetRecordLength));
  EXPECT_EQ(first_index(instrument), kResetRecordLength);
  EXPECT_EQ(test::captured(instrument, 1).settings.scale, 2.0);
  EXPECT_EQ(instrument.discarded_captures(), 0U);  // Not a dropped block.
}

// The block is processed under the settings it was acquired with, not the ones in place by then.
TEST_F(BlockProcessingTest, BlockIsAssembledUnderTheSettingsItWasAcquiredWith) {
  session.send("ACQ:POIN 100");
  const AcquiredBlock block = acquired(instrument, 100);
  const AcquiredBlock next = acquired(instrument, 100);

  instrument.process(block);
  session.send("TIM:SCAL 1");  // Display only (ADR 0004): it discards nothing.
  instrument.process(next);

  EXPECT_EQ(test::latest(instrument)->record_length, 100U);
  EXPECT_EQ(first_index(instrument), 100U);
}

TEST_F(BlockProcessingTest, CaptureThatWouldSpanADroppedBlockIsDiscardedAndCounted) {
  instrument.process(acquired(instrument, 6000));
  (void)instrument.acquire(1000);  // Dropped: never processed.

  instrument.process(acquired(instrument, kResetRecordLength - 1));
  EXPECT_EQ(instrument.latest_capture(), nullptr);
  instrument.process(acquired(instrument, 1));

  EXPECT_EQ(first_index(instrument), 7000U);  // Starts after the gap.
  EXPECT_EQ(instrument.discarded_captures(), 1U);
}

TEST_F(BlockProcessingTest, DroppedBlocksInARowDiscardOneCapture) {
  instrument.process(acquired(instrument, 6000));
  for (int n = 0; n < 3; ++n) {
    (void)instrument.acquire(1000);
  }

  acquire_and_process(instrument, kResetRecordLength, 1000);

  EXPECT_EQ(first_index(instrument), 9000U);
  EXPECT_EQ(instrument.discarded_captures(), 1U);
}

TEST_F(BlockProcessingTest, EachGapDiscardsTheCaptureThatWouldSpanIt) {
  for (int gap = 0; gap < 2; ++gap) {
    instrument.process(acquired(instrument, 6000));
    (void)instrument.acquire(1000);
  }

  instrument.process(acquired(instrument, kResetRecordLength));

  EXPECT_EQ(first_index(instrument), 14'000U);
  EXPECT_EQ(instrument.discarded_captures(), 2U);
}

// A gap that falls where one capture has completed and the next has not started splits nothing.
TEST_F(BlockProcessingTest, DroppedBlockBetweenCapturesDiscardsNothing) {
  instrument.process(acquired(instrument, kResetRecordLength));
  (void)instrument.acquire(500);

  instrument.process(acquired(instrument, kResetRecordLength));

  EXPECT_EQ(first_index(instrument), kResetRecordLength + 500);
  EXPECT_EQ(instrument.discarded_captures(), 0U);
}

// The trigger restarts after a gap: an edge made of samples on both sides of it is no trigger
// event. CH1's 1 kHz sine arms the trigger below -0.1 V before sample 990 and crosses 0 V rising
// at sample 1000, inside the dropped block.
TEST(BlockProcessingTriggerTest, TriggerSeesNoEdgeAcrossADroppedBlock) {
  Instrument reference;
  ScpiSession(reference).send(kNormalTenSamples);
  reference.advance(2100);
  const std::size_t second_event = first_index(reference);
  ASSERT_GT(second_event, 1900U);
  Instrument instrument;
  ScpiSession(instrument).send(kNormalTenSamples);

  instrument.process(acquired(instrument, 990));
  (void)instrument.acquire(20);
  instrument.process(acquired(instrument, 90));

  EXPECT_EQ(instrument.latest_capture(), nullptr);
  instrument.process(acquired(instrument, 1000));
  EXPECT_EQ(first_index(instrument), second_event);
  EXPECT_EQ(instrument.discarded_captures(), 1U);  // Normal mode was collecting, waiting.
}

TEST_F(BlockProcessingTest, StoppedInstrumentTakesTheSamplesButAcquiresNothing) {
  session.send("STOP");

  EXPECT_EQ(instrument.acquire(1000), std::nullopt);

  session.send("RUN");
  instrument.process(acquired(instrument, kResetRecordLength));
  EXPECT_EQ(first_index(instrument), 1000U);
  EXPECT_EQ(instrument.discarded_captures(), 0U);  // Samples not acquired are no dropped block.
}

TEST_F(BlockProcessingTest, BlocksAreAtMostTheLongestSampleBlock) {
  EXPECT_THROW((void)instrument.acquire(Instrument::kMaxBlockLength + 1), std::invalid_argument);

  EXPECT_EQ(acquired(instrument, Instrument::kMaxBlockLength).samples.count,
            Instrument::kMaxBlockLength);
}

TEST_F(BlockProcessingTest, SingleCaptureEndsTheHeldLineAndStops) {
  const auto reply = session.start("SINGle;*OPC?");

  instrument.process(acquired(instrument, kResetRecordLength));

  EXPECT_EQ(reply->text, "1");
  EXPECT_EQ(first_index(instrument), 0U);
  EXPECT_EQ(instrument.acquire(1), std::nullopt);
}

// The Single's capture ends inside the block: the rest of the block is not captured, since the
// instrument has stopped.
TEST_F(BlockProcessingTest, SingleCaptureEndingInsideABlockStopsThere) {
  session.send("ACQ:POIN 10;:SINGle");

  instrument.process(acquired(instrument, 25));

  EXPECT_EQ(first_index(instrument), 0U);
  EXPECT_EQ(test::latest(instrument)->record_length, 10U);
}

TEST_F(BlockProcessingTest, StaleBlockDoesNotEndASingleArmedAgainSinceIt) {
  const auto reply = session.start("SINGle;*OPC?");
  const AcquiredBlock stale = acquired(instrument, kResetRecordLength);
  ScpiSession(instrument).send("SINGle");

  instrument.process(stale);

  EXPECT_FALSE(reply->ended);
  EXPECT_EQ(instrument.latest_capture(), nullptr);
  instrument.process(acquired(instrument, kResetRecordLength));
  EXPECT_EQ(reply->text, "1");
  EXPECT_EQ(first_index(instrument), kResetRecordLength);
}

// RUN leaves Armed-single without discarding: the capture a block from the Single completes is
// kept, and the instrument carries on Running.
TEST_F(BlockProcessingTest, BlockFromASingleCompletedAfterRunKeepsTheInstrumentRunning) {
  session.send("SINGle");
  const AcquiredBlock armed = acquired(instrument, kResetRecordLength);
  session.send("RUN");

  instrument.process(armed);

  EXPECT_EQ(first_index(instrument), 0U);
  instrument.process(acquired(instrument, kResetRecordLength));
  EXPECT_EQ(first_index(instrument), kResetRecordLength);
}

// The rest of that block, after the Single's capture, is captured too: the next capture starts
// where the Single's ended, with no gap.
TEST_F(BlockProcessingTest, RunDuringASinglesBlockCapturesTheRestOfTheBlock) {
  session.send("ACQ:POIN 10;:SINGle");
  const AcquiredBlock armed = acquired(instrument, 25);
  session.send("RUN");

  instrument.process(armed);
  EXPECT_EQ(first_index(instrument), 10U);  // Captures 0..9 and 10..19; 20..24 in progress.
  instrument.process(acquired(instrument, 25));

  EXPECT_EQ(first_index(instrument), 40U);
  EXPECT_EQ(instrument.discarded_captures(), 0U);
}

// The Single still ends at its capture, so the trigger restarts there as after any Single: the
// holdoff from the Single's trigger event does not hold off the rest of the block. CH1's 1 kHz
// sine rises through 0 V at samples 1000 and 2000; a 2.5 ms holdoff would skip the second.
TEST(BlockProcessingTriggerTest, RestOfASinglesBlockStartsWithTheTriggerRestarted) {
  const std::string setup = std::string(kNormalTenSamples) + ";:TRIG:HOLD 0.0025";
  Instrument reference;
  ScpiSession(reference).send(setup);
  reference.advance(1500);
  const std::size_t first_event = first_index(reference);
  ScpiSession(reference).send("SINGle");
  reference.advance(600);
  const std::size_t second_event = first_index(reference);
  ASSERT_LT(first_event, second_event);
  Instrument instrument;
  ScpiSession session(instrument);
  session.send(setup + ";:SINGle");
  const AcquiredBlock armed = acquired(instrument, 2100);
  session.send("RUN");

  instrument.process(armed);

  EXPECT_EQ(first_index(instrument), second_event);
}

TEST_F(BlockProcessingTest, ResetDropsTheLatestCaptureAndTheBlocksAcquiredBeforeIt) {
  instrument.process(acquired(instrument, kResetRecordLength));
  const AcquiredBlock before = acquired(instrument, kResetRecordLength);

  session.send(std::string("*RST;:") + kUntriggeredDc);
  instrument.process(before);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
  EXPECT_EQ(session.send("WAV:DATA?"), std::nullopt);
  EXPECT_EQ(session.next_error(), test::kDataStaleReply);
}

// The processing half on its own thread while this thread sends commands, as in live mode, with
// no other thread in between to order them: TSan checks that what they share is locked or atomic.
TEST_F(BlockProcessingTest, ProcessingOnAnotherThreadWhileCommandsRun) {
  constexpr int kBlocks = 200;
  session.send("ACQ:POIN 100");
  std::vector<AcquiredBlock> blocks;
  blocks.reserve(kBlocks);
  for (int n = 0; n < kBlocks; ++n) {
    blocks.push_back(acquired(instrument, 100));  // Each completes one capture.
  }

  std::thread processing([&] {
    for (const AcquiredBlock& block : blocks) {
      instrument.process(block);
    }
  });
  for (int n = 0; n < kBlocks; ++n) {
    session.send(n % 2 == 0 ? "SOUR1:OFFS 0.5;:WAV:SOUR CHAN1" : "SOUR1:OFFS 0;*OPC?");
  }
  processing.join();

  EXPECT_EQ(session.next_error(), test::kNoErrorReply);
}

// The acquisition half samples without the mutex, on copies of the Generators, while this thread
// changes them: TSan checks that sampling shares nothing with commands unlocked.
TEST_F(BlockProcessingTest, AcquiringOnAnotherThreadWhileCommandsChangeTheGenerators) {
  constexpr int kBlocks = 200;
  session.send("ACQ:POIN 100");
  std::vector<AcquiredBlock> blocks;
  blocks.reserve(kBlocks);

  std::thread acquisition([&] {
    for (int n = 0; n < kBlocks; ++n) {
      blocks.push_back(acquired(instrument, 100));
    }
  });
  for (int n = 0; n < kBlocks; ++n) {
    session.send(n % 2 == 0 ? "SOUR1:OFFS 0.5;FREQ 2000" : "SOUR1:OFFS 0;FREQ 1000");
  }
  acquisition.join();
  for (const AcquiredBlock& block : blocks) {
    instrument.process(block);
  }

  EXPECT_EQ(session.next_error(), test::kNoErrorReply);
}

}  // namespace
}  // namespace openscope
