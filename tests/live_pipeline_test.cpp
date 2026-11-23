// Live mode: the acquisition and processing threads run the instrument in real time while the
// test thread sends SCPI, so the TSan build checks every hand-over between them (#8).

#include "openscope/live_pipeline.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "eventually.hpp"
#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/sample_clock.hpp"
#include "reply_numbers.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using std::chrono::steady_clock;
using test::eventually;
using test::kPatience;
using test::kResetRecordLength;
using testing::Each;
using testing::Eq;

/// Waits for a latest capture other than `seen` and returns it, or null after kPatience. Runs
/// `meanwhile`, if given, before each look.
std::shared_ptr<const Capture> next_capture(const Instrument& instrument,
                                            const std::shared_ptr<const Capture>& seen,
                                            const std::function<void()>& meanwhile = {}) {
  std::shared_ptr<const Capture> capture;
  const bool arrived = eventually([&] {
    if (meanwhile) {
      meanwhile();
    }
    capture = instrument.latest_capture();
    return capture != seen;
  });
  return arrived ? capture : nullptr;
}

using LivePipelineTest = test::UntriggeredDcFixture;

// The pacer never lets the sample clock run ahead of the wall clock.
TEST_F(LivePipelineTest, TakesCapturesNoFasterThanRealTime) {
  const steady_clock::time_point start = steady_clock::now();
  const LivePipeline live(instrument);

  const std::shared_ptr<const Capture> capture = next_capture(instrument, nullptr);

  const std::chrono::duration<double> elapsed = steady_clock::now() - start;
  ASSERT_NE(capture, nullptr);
  EXPECT_LE(static_cast<double>(capture->first_index + capture->record_length),
            elapsed.count() * instrument.sample_rate());
  EXPECT_THAT(test::captured(instrument, 1).samples.size(), Eq(kResetRecordLength));
}

TEST_F(LivePipelineTest, KeepsTakingCaptures) {
  const LivePipeline live(instrument);
  const std::shared_ptr<const Capture> first = next_capture(instrument, nullptr);
  ASSERT_NE(first, nullptr);

  const std::shared_ptr<const Capture> later = next_capture(instrument, first);

  ASSERT_NE(later, nullptr);
  EXPECT_GT(later->first_index, first->first_index);
}

// Spec story 39 in live mode: once a setting command has returned, every capture published holds
// the new setting, although blocks acquired before it were still on their way.
TEST_F(LivePipelineTest, CapturesAfterASettingChangeHoldOnlyTheNewSignal) {
  const LivePipeline live(instrument);
  for (const float offset : {0.25F, -0.5F, 1.0F, 0.0F}) {
    session.send("SOUR1:OFFS " + std::to_string(offset));
    const std::shared_ptr<const Capture> seen = instrument.latest_capture();

    const std::shared_ptr<const Capture> capture = next_capture(instrument, seen);

    ASSERT_NE(capture, nullptr) << offset;
    ASSERT_TRUE(capture->channels[0].has_value());
    EXPECT_THAT(capture->channels[0].value_or(CapturedChannel{}).samples, Each(Eq(offset)));
  }
}

// Spec story 34 in live mode: the processing thread completes the Single, but never runs the held
// line (#67). It hands the rest of the line to the Session's Resumer, and the line ends, and
// replies, where that work runs: here, on the test thread.
TEST_F(LivePipelineTest, SingleOpcIsCarriedOnByTheResumerNotTheProcessingThread) {
  std::promise<std::function<void()>> handed;
  std::thread::id handed_from;
  test::ScpiSession waiting(instrument, [&](std::function<void()> work) {
    handed_from = std::this_thread::get_id();
    handed.set_value(std::move(work));
  });
  std::shared_ptr<const test::LineReply> reply;
  std::future<std::function<void()>> work = handed.get_future();
  {
    const LivePipeline live(instrument);
    reply = waiting.start("SINGle;*OPC?");
    ASSERT_EQ(work.wait_for(kPatience), std::future_status::ready);
  }
  EXPECT_NE(handed_from, std::this_thread::get_id());  // From the processing thread.
  EXPECT_NE(instrument.latest_capture(), nullptr);
  EXPECT_FALSE(reply->ended);

  work.get()();

  EXPECT_EQ(reply->text, "1");
}

// Four sine channels at 1 GS/s are far more than any machine samples in real time: acquisition
// falls behind the wall clock every round, and the samples it skips are counted, never hidden.
TEST_F(LivePipelineTest, CountsTheSamplesItFallsBehindOn) {
  session.send("ACQ:SRAT 1E9;:CHAN2:DISP ON;:CHAN3:DISP ON;:CHAN4:DISP ON");
  const LivePipeline live(instrument);

  EXPECT_TRUE(eventually([&] { return live.skipped_samples() > 0; }));
}

// Spec story 64 in live mode: once a setting command has returned, the statistics count only
// captures of the new signal, although blocks acquired before it were still on their way.
TEST_F(LivePipelineTest, StatisticsCountOnlyTheNewSignalAfterASettingChange) {
  const LivePipeline live(instrument);
  for (const float offset : {0.25F, -0.5F, 1.0F}) {
    session.send("SOUR1:OFFS " + std::to_string(offset));
    test::Statistics statistics;

    const bool counted = eventually([&] {
      const std::optional<std::string> reply = session.send_live("MEAS:STAT? VAV,CHAN1");
      statistics = reply ? test::statistics_of(reply) : test::Statistics{};
      return statistics.count >= 2;
    });

    ASSERT_TRUE(counted) << offset;
    EXPECT_EQ(statistics.minimum, offset);
    EXPECT_EQ(statistics.maximum, offset);
  }
}

// The processing thread counts and publishes each capture under one hold of the mutex, so right
// after a channel change MEAS:STAT? never finds statistics of the new channels beside a latest
// capture of the old ones, or the reverse.
TEST_F(LivePipelineTest, StatisticsHoldTheChannelsOfTheCapturesTheyCount) {
  session.send("ACQ:POIN 100");  // Several captures a block, each counted and published apart.
  const LivePipeline live(instrument);
  for (int round = 0; round < 20; ++round) {
    const bool enabled = round % 2 == 0;
    session.send(enabled ? "CHAN2:DISP ON" : "CHAN2:DISP OFF");
    std::string error;

    const bool counted = eventually([&] {
      session.send_live("MEAS:STAT? VPP,CHAN2");
      error = session.next_error();
      return error != test::kDataStaleReply;
    });

    ASSERT_TRUE(counted) << round;
    EXPECT_EQ(error, enabled ? test::kNoErrorReply : test::kSettingsConflictReply) << round;
  }
}

// NFR-1 (#71): Running with all four channels at 1 MS/s and the longest record drops no block and
// discards no capture, and the statistics count every capture published. Measuring one such
// capture takes longer than the ring buffer holds blocks, so it must not stall processing. Release
// builds only: unoptimised and sanitized builds measure captures this long slower than they come.
// With a client (the parameter), one more Session reads each capture's waveform, a measurement
// and the statistics all the while.
class LiveLongestRecordTest : public test::InstrumentFixture,
                              public testing::WithParamInterface<bool> {
 protected:
  static constexpr std::size_t kRecordLength = AcquisitionSettings::kMaxRecordLength;

  void SetUp() override {
#ifndef NDEBUG
    GTEST_SKIP() << "release builds only";
#endif
    // Noisy sines on every channel, captured untriggered back to back from sample 0, so the
    // latest capture's first sample tells how many captures came before it.
    session.send(std::format(
        "{};:{};:SOUR1:NOIS 0.01;:SOUR2:NOIS 0.01;:SOUR3:NOIS 0.01;:SOUR4:NOIS 0.01;:ACQ:POIN {}",
        test::kUntriggeredCaptures, test::kEnableChannelsTwoToFour, kRecordLength));
    ASSERT_EQ(session.next_error(), test::kNoErrorReply);
  }

  /// What the client does before each look for a new capture, if the test has one.
  std::function<void()> client_queries() {
    if (!GetParam()) {
      return {};
    }
    return [this] {
      (void)client_.send_live("WAV:FORM REAL;:WAV:DATA?;:MEAS:FREQ? CHAN1;:MEAS:STAT? VPP,CHAN4");
    };
  }

 private:
  test::ScpiSession client_{instrument};
};

TEST_P(LiveLongestRecordTest, RunningAtFourChannelsLosesNoBlockAndCountsEveryCapture) {
  constexpr int kCaptures = 5;  // About five seconds.
  std::shared_ptr<const Capture> latest;
  std::optional<std::string> reply;
  LiveCounters counters;
  {
    const LivePipeline live(instrument);
    for (int published = 0; published < kCaptures; ++published) {
      latest = next_capture(instrument, latest, client_queries());
      ASSERT_NE(latest, nullptr) << published;
    }
    // STOP freezes the latest capture and the statistics together.
    reply = session.send_live("STOP;:MEAS:STAT? VPP,CHAN4");
    latest = test::latest(instrument);
    counters = live.counters();
  }

  EXPECT_EQ(counters.dropped_blocks, 0U);
  EXPECT_EQ(counters.discarded_captures, 0U);
  const SampleIndex captures_from_sample_zero = (latest->first_index / kRecordLength) + 1;
  EXPECT_EQ(test::statistics_of(reply).count, static_cast<double>(captures_from_sample_zero));
}

// The same for Singles (#71 review): a Single's capture is measured off the processing thread
// too, so taking Singles of the longest record drops no block, and the statistics a held line
// reads after a reset count that Single's capture alone.
TEST_P(LiveLongestRecordTest, SinglesLoseNoBlockAndEachCountsItsCapture) {
  const SessionId single_taker = instrument.open_session(run_at_once);
  std::vector<std::optional<std::string>> replies;
  LiveCounters counters;
  {
    const LivePipeline live(instrument);
    for (int single = 0; single < 3; ++single) {
      // Shared, since the line ends on a pipeline thread.
      const auto replied = std::make_shared<std::promise<std::optional<std::string>>>();
      std::future<std::optional<std::string>> reply = replied->get_future();
      instrument.execute(
          single_taker, "MEAS:STAT:RES;:SING;*OPC?;:MEAS:STAT? VPP,CHAN4",
          [replied](std::optional<std::string> text) { replied->set_value(std::move(text)); });
      const std::function<void()> meanwhile = client_queries();
      const bool ended = eventually([&] {
        if (meanwhile) {
          meanwhile();
        }
        return reply.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
      });
      ASSERT_TRUE(ended) << single;
      replies.push_back(reply.get());
    }
    counters = live.counters();
  }

  EXPECT_EQ(counters.dropped_blocks, 0U);
  EXPECT_EQ(counters.discarded_captures, 0U);
  EXPECT_THAT(replies, Each(testing::Optional(testing::EndsWith(",1"))));  // A count of one.
}

INSTANTIATE_TEST_SUITE_P(Clients, LiveLongestRecordTest, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& parameter) {
                           return parameter.param ? "WithAClient" : "WithNoClient";
                         });

TEST_F(LivePipelineTest, StopFreezesTheLatestCapture) {
  const LivePipeline live(instrument);
  ASSERT_NE(next_capture(instrument, nullptr), nullptr);

  session.send("STOP");
  const std::shared_ptr<const Capture> frozen = instrument.latest_capture();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(instrument.latest_capture(), frozen);
}

// A line held on a Single that never completes stays held when the pipeline stops, and the
// instrument is usable again afterwards.
TEST_F(LivePipelineTest, StopsWithALineStillHeld) {
  session.send("TRIG:MODE NORM");
  std::shared_ptr<const test::LineReply> reply;
  {
    const LivePipeline live(instrument);
    reply = session.start("SINGle;*OPC?");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  EXPECT_FALSE(reply->ended);
  test::ScpiSession(instrument).send("STOP");
  EXPECT_EQ(reply->text, "1");
}

}  // namespace
}  // namespace openscope
