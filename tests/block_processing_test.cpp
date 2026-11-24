// Live mode's two halves, `acquire` and `process`, driven by hand on one thread: blocks carry the
// settings they were acquired under, a block acquired before a command never completes a capture
// after it, and a capture that would span a dropped block (one acquired and never processed) is
// discarded and counted (#8).

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <functional>
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
#include "openscope/completed_capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/measurement.hpp"
#include "reply_numbers.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::acquire_and_process;
using test::acquired;
using test::first_index;
using test::kResetRecordLength;
using test::kUntriggeredDc;
using test::ScpiSession;

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

// #67: the processing thread runs no held line. Each Session's line is handed to its own
// Resumer as a piece of work of its own, so held bulk lines never run back to back in one call.
TEST_F(BlockProcessingTest, SingleCaptureHandsEachHeldLineToItsResumerAsItsOwnWork) {
  std::vector<std::function<void()>> handed;
  const Resumer queue = test::keeping_in(handed);
  ScpiSession first(instrument, queue);
  ScpiSession second(instrument, queue);
  const auto first_reply = first.start("SINGle;*OPC?;:WAV:SOUR?");
  const auto second_reply = second.start("*OPC?;:WAV:SOUR CHAN2;SOUR?");

  instrument.process(acquired(instrument, kResetRecordLength));

  ASSERT_EQ(handed.size(), 2U);
  EXPECT_FALSE(first_reply->ended);
  EXPECT_FALSE(second_reply->ended);
  handed.at(1)();
  EXPECT_FALSE(first_reply->ended);
  EXPECT_EQ(second_reply->text, "1;CHAN2");
  handed.at(0)();
  EXPECT_EQ(first_reply->text, "1;CHAN1");
}

// A held line carries on as the operation left the instrument (#68 review): a setting another
// Session sends before the held line's work runs waits for it, so the statistics the held line
// reads are still those of the Single's capture alone. Queries of a capture do not wait.
TEST_F(BlockProcessingTest, CommandsOfOtherSessionsWaitUntilTheHeldLinesHaveCarriedOn) {
  std::vector<std::function<void()>> handed;
  ScpiSession waiting(instrument, test::keeping_in(handed));
  const auto reply = waiting.start("MEAS:STAT:RES;:SING;*OPC?;:MEAS:STAT? VAV,CHAN1");
  instrument.process(acquired(instrument, kResetRecordLength));
  ScpiSession other(instrument);

  const auto setting = other.start("SOUR1:OFFS 0.5;:SOUR1:OFFS?");
  EXPECT_FALSE(setting->ended);
  EXPECT_TRUE(ScpiSession(instrument).start("WAV:PRE?")->ended);  // Reads only the capture.

  ASSERT_EQ(handed.size(), 1U);
  handed.front()();
  EXPECT_THAT(reply->text, testing::Optional(testing::EndsWith(",1")));  // A count of one.
  EXPECT_EQ(setting->text, "0.5");  // Carried on once the held line had.
  EXPECT_EQ(waiting.next_error(), test::kNoErrorReply);
}

// Until a released line has carried on, no other Session's command that takes the Instrument's
// mutex can run: it waits. So the bulk queries replying now shows they take no lock, and one
// client's bulk data never stalls another's commands or acquisition (#68 review).
TEST_F(BlockProcessingTest, BulkDataQueriesTakeNoLock) {
  std::vector<std::function<void()>> handed;
  ScpiSession waiting(instrument, test::keeping_in(handed));
  const auto reply = waiting.start("SINGle;*OPC?");
  instrument.process(acquired(instrument, kResetRecordLength));
  ASSERT_EQ(handed.size(), 1U);
  std::vector<std::string> queries = {"WAV:DATA?", "WAV:PRE?", "FFT:DATA?",
                                      "FFT:SPAC?", "*IDN?",    "SYST:ERR?"};
  for (const Measurement& measurement : measurements()) {
    queries.push_back(std::format("MEAS:{}? CHAN1", measurement.mnemonic));
  }
  for (const std::string& query : queries) {
    // A Session each, so one that waits does not hold up the next.
    EXPECT_TRUE(ScpiSession(instrument).start(query)->ended) << query;
  }
  EXPECT_FALSE(ScpiSession(instrument).start("ACQ:POIN?")->ended);  // One that takes it waits.

  handed.front()();
  EXPECT_EQ(reply->text, "1");
}

// A client that disconnects after its line was handed on, before the work ran, has gone: the
// rest of its line does not run.
TEST_F(BlockProcessingTest, HandedLineOfASessionClosedSinceDoesNotRun) {
  std::vector<std::function<void()>> handed;
  ScpiSession leaving(instrument, test::keeping_in(handed));
  const auto reply = leaving.start("SINGle;*OPC?;:SOUR1:FREQ 2000");
  instrument.process(acquired(instrument, kResetRecordLength));
  ASSERT_EQ(handed.size(), 1U);

  leaving.close();
  handed.front()();

  EXPECT_FALSE(reply->ended);
  EXPECT_EQ(session.send("SOUR1:FREQ?"), "1000");
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

/// Every capture `process` publishes from `block` and hands off for counting, in order.
std::vector<CompletedCapture> handed_off(Instrument& instrument, const AcquiredBlock& block) {
  std::vector<CompletedCapture> completed;
  instrument.process(block,
                     [&](CompletedCapture capture) { completed.push_back(std::move(capture)); });
  return completed;
}

/// Counts every capture in `completed`, oldest first, as live mode's measuring thread does.
void count_all(Instrument& instrument, const std::vector<CompletedCapture>& completed) {
  for (const CompletedCapture& capture : completed) {
    instrument.count_measured(capture);
  }
}

// #71: live mode measures captures on another thread. A capture is the latest as soon as it
// completes and is counted once count_measured has measured it, in the order they completed.
TEST_F(BlockProcessingTest, HandedOffCapturesArePublishedAtOnceAndCountedOnceMeasured) {
  session.send("ACQ:POIN 100");

  const std::vector<CompletedCapture> completed = handed_off(instrument, acquired(instrument, 300));

  ASSERT_EQ(completed.size(), 3U);
  EXPECT_EQ(first_index(instrument), 200U);
  count_all(instrument, completed);
  const test::Statistics statistics = test::statistics_of(session.send("MEAS:STAT? VAV,CHAN1"));
  EXPECT_EQ(statistics.count, 3.0);
}

// MEAS:STAT? holds its line while captures already published wait to be counted, so it never
// replies statistics that leave out the latest capture; other commands do not wait.
TEST_F(BlockProcessingTest, StatisticsQueryWaitsForThePublishedCapturesToBeCounted) {
  session.send("ACQ:POIN 100");
  const std::vector<CompletedCapture> completed = handed_off(instrument, acquired(instrument, 200));
  ASSERT_EQ(completed.size(), 2U);

  const auto reply = session.start("MEAS:STAT? VAV,CHAN1");
  EXPECT_FALSE(reply->ended);
  EXPECT_EQ(ScpiSession(instrument).send("ACQ:POIN?"), "100");
  instrument.count_measured(completed.front());
  EXPECT_FALSE(reply->ended);
  instrument.count_measured(completed.back());

  EXPECT_EQ(test::statistics_of(reply->text).count, 2.0);
}

// Only the captures published before the query hold it up: while Running, more keep coming, and
// a query that waited for all of them might never reply (it did not under TSan).
TEST_F(BlockProcessingTest, StatisticsQueryDoesNotWaitForCapturesPublishedAfterIt) {
  session.send("ACQ:POIN 100");
  const std::vector<CompletedCapture> before = handed_off(instrument, acquired(instrument, 100));
  const auto reply = session.start("MEAS:STAT? VAV,CHAN1");
  const std::vector<CompletedCapture> after = handed_off(instrument, acquired(instrument, 100));

  count_all(instrument, before);

  EXPECT_EQ(test::statistics_of(reply->text).count, 1.0);
  count_all(instrument, after);
}

// A capture that completed before a statistics reset never counts after it (#71 review): the
// statistics start from the captures that complete after the reset.
TEST_F(BlockProcessingTest, CapturesCompletedBeforeAStatisticsResetAreNotCounted) {
  session.send("ACQ:POIN 100");
  const std::vector<CompletedCapture> before = handed_off(instrument, acquired(instrument, 200));

  session.send("MEAS:STAT:RES");
  const std::vector<CompletedCapture> after = handed_off(instrument, acquired(instrument, 100));
  count_all(instrument, before);
  count_all(instrument, after);

  EXPECT_EQ(test::statistics_of(session.send("MEAS:STAT? VAV,CHAN1")).count, 1.0);
}

// The same for a setting change, which resets the statistics too: the capture that completed
// before it stays the latest until the next one, but is not counted.
TEST_F(BlockProcessingTest, CapturesCompletedBeforeASettingChangeAreNotCounted) {
  const std::vector<CompletedCapture> before =
      handed_off(instrument, acquired(instrument, kResetRecordLength));
  ASSERT_EQ(before.size(), 1U);

  session.send("SOUR1:OFFS 0.5");
  count_all(instrument, before);

  EXPECT_EQ(first_index(instrument), 0U);
  session.send("MEAS:STAT? VAV,CHAN1");
  EXPECT_EQ(session.next_error(), test::kDataStaleReply);  // Nothing counted.
}

// A capture that completed before STOP or SINGle still counts and stays the latest (#71 review):
// run control keeps the statistics, and STOP freezes the latest capture where it is.
class RunControlWithCapturesQueuedTest : public test::UntriggeredDcFixture,
                                         public testing::WithParamInterface<const char*> {};

TEST_P(RunControlWithCapturesQueuedTest, CapturesCompletedBeforeItAreCountedAndKept) {
  session.send("ACQ:POIN 100");
  const std::vector<CompletedCapture> completed = handed_off(instrument, acquired(instrument, 300));

  session.send(GetParam());
  count_all(instrument, completed);

  EXPECT_EQ(first_index(instrument), 200U);
  EXPECT_EQ(test::statistics_of(session.send("MEAS:STAT? VAV,CHAN1")).count, 3.0);
}

INSTANTIATE_TEST_SUITE_P(StopAndSingle, RunControlWithCapturesQueuedTest,
                         testing::Values("STOP", "SINGle"));

// A Single's capture is published with the Single's end and handed off like any other: the lines
// held on it carry on at once, and their MEAS:STAT? waits for the count, while other Sessions'
// settings wait for those lines (#68), so the statistics are of the Single's capture alone.
TEST_F(BlockProcessingTest, LinesHeldOnASingleReadItsCountedStatistics) {
  std::vector<std::function<void()>> handed;
  ScpiSession waiting(instrument, test::keeping_in(handed));
  const auto reply = waiting.start("MEAS:STAT:RES;:SING;*OPC?;:MEAS:STAT? VAV,CHAN1");

  const std::vector<CompletedCapture> completed =
      handed_off(instrument, acquired(instrument, kResetRecordLength));
  ASSERT_EQ(completed.size(), 1U);
  EXPECT_EQ(first_index(instrument), 0U);
  ASSERT_EQ(handed.size(), 1U);
  handed.front()();
  const auto setting = ScpiSession(instrument).start("SOUR1:OFFS 0.5");

  EXPECT_FALSE(reply->ended);
  EXPECT_FALSE(setting->ended);
  instrument.count_measured(completed.front());
  ASSERT_EQ(handed.size(), 2U);  // The held line again, now its query can reply.
  handed.back()();
  EXPECT_THAT(reply->text, testing::Optional(testing::EndsWith(",1")));  // A count of one.
  EXPECT_TRUE(setting->ended);
}

// A line held for the count whose client disconnects frees the others it was holding up.
TEST_F(BlockProcessingTest, ClosingASessionWaitingForTheCountLetsTheOthersCarryOn) {
  std::vector<std::function<void()>> handed;
  ScpiSession waiting(instrument, test::keeping_in(handed));
  (void)waiting.start("SING;*OPC?;:MEAS:STAT? VAV,CHAN1");
  const std::vector<CompletedCapture> completed =
      handed_off(instrument, acquired(instrument, kResetRecordLength));
  ASSERT_EQ(handed.size(), 1U);
  handed.front()();
  const auto setting = ScpiSession(instrument).start("SOUR1:OFFS 0.5");
  EXPECT_FALSE(setting->ended);

  waiting.close();

  EXPECT_TRUE(setting->ended);
  count_all(instrument, completed);
}

// After RUN ends a Single inside its block, the rest of the block is captured as Running, and its
// captures are handed off after the Single's.
TEST_F(BlockProcessingTest, RunningCapturesAfterASinglesCaptureAreHandedOffInOrder) {
  session.send("ACQ:POIN 10;:SINGle");
  const AcquiredBlock armed = acquired(instrument, 25);
  session.send("RUN");

  const std::vector<CompletedCapture> completed = handed_off(instrument, armed);

  ASSERT_EQ(completed.size(), 2U);
  EXPECT_EQ(completed.front().capture->first_index, 0U);  // The Single's.
  EXPECT_EQ(completed.back().capture->first_index, 10U);
  EXPECT_EQ(first_index(instrument), 10U);
  count_all(instrument, completed);
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

// Every block acquired while the Single was armed, not only the one that ends it, is captured as
// Running once RUN has ended the Single: a later block's captures are the ones it would give had it
// been acquired after RUN. The 2.5 ms holdoff from the event at sample 2000 skips the one at 3000.
TEST(BlockProcessingTriggerTest, LaterBlocksOfASingleEndedByRunAreCapturedAsRunning) {
  const std::string setup = std::string(kNormalTenSamples) + ";:TRIG:HOLD 0.0025;:SINGle";
  Instrument reference;
  ScpiSession reference_session(reference);
  reference_session.send(setup);
  const AcquiredBlock reference_first = acquired(reference, 1500);
  reference_session.send("RUN");
  reference.process(reference_first);
  reference.process(acquired(reference, 2000));
  Instrument instrument;
  ScpiSession session(instrument);
  session.send(setup);
  const AcquiredBlock first = acquired(instrument, 1500);
  const AcquiredBlock second = acquired(instrument, 2000);
  session.send("RUN");

  instrument.process(first);
  instrument.process(second);

  EXPECT_EQ(first_index(reference), 2000U);
  EXPECT_EQ(first_index(instrument), first_index(reference));
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

/// Runs one Single on a fresh instrument while another thread acquires and processes 50-sample
/// blocks, and sends a setting change the moment the Single's capture is visible. Returns true
/// when that capture stayed the latest: the Single had ended, so nothing was captured after it.
bool single_capture_stays_latest() {
  Instrument instrument(7);
  ScpiSession session(instrument);
  session.send(test::kUntriggeredCaptures);
  session.send("ACQ:POIN 100");
  session.send("SINGle");
  std::thread processing([&] {
    for (std::optional<AcquiredBlock> block = instrument.acquire(50); block;
         block = instrument.acquire(50)) {
      instrument.process(*block);
    }
  });
  std::shared_ptr<const Capture> single;
  while (single == nullptr) {  // The Single always completes: Auto takes an untriggered capture.
    single = instrument.latest_capture();
  }
  session.send("CHAN1:OFFS 0");
  processing.join();
  return instrument.latest_capture() == single;
}

// The processing thread publishes a Single's capture and ends the Single in one hold of the
// mutex. With a gap between the two, a command that has already seen the capture could land in
// it and leave the Single armed for a second capture. Such a gap fails about 1 trial in 400, so
// kTrials misses it in under 1 run in 1000.
TEST(BlockProcessingRaceTest, CommandAfterASinglesCaptureFindsTheSingleEnded) {
  constexpr int kTrials = 3000;
  int second_captures = 0;
  for (int trial = 0; trial < kTrials; ++trial) {
    second_captures += single_capture_stays_latest() ? 0 : 1;
  }
  EXPECT_EQ(second_captures, 0);
}

}  // namespace
}  // namespace openscope
