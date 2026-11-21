// Live mode: the acquisition and processing threads run the instrument in real time while the
// test thread sends SCPI, so the TSan build checks every hand-over between them (#8).

#include "openscope/live_pipeline.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "reply_numbers.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using std::chrono::steady_clock;
using test::kResetRecordLength;
using testing::Each;
using testing::Eq;

/// How long a test waits for the live instrument before it fails: far longer than a capture
/// takes, even under TSan on a loaded machine.
constexpr std::chrono::seconds kPatience{20};

/// Polls `done` until it holds; false if it still does not after kPatience.
bool eventually(const std::function<bool()>& done) {
  const steady_clock::time_point deadline = steady_clock::now() + kPatience;
  while (!done()) {
    if (steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/// Waits for a latest capture other than `seen` and returns it, or null after kPatience.
std::shared_ptr<const Capture> next_capture(const Instrument& instrument,
                                            const std::shared_ptr<const Capture>& seen) {
  std::shared_ptr<const Capture> capture;
  const bool arrived = eventually([&] {
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

// Spec story 34 in live mode: the processing thread completes the Single and replies.
TEST_F(LivePipelineTest, SingleOpcRepliesFromTheProcessingThreadOnceTheCaptureIsComplete) {
  const LivePipeline live(instrument);
  std::promise<std::optional<std::string>> replied;
  std::thread::id replier;
  const SessionId id = instrument.open_session();

  instrument.execute(id, "SINGle;*OPC?", [&](std::optional<std::string> reply) {
    replier = std::this_thread::get_id();
    replied.set_value(std::move(reply));
  });

  std::future<std::optional<std::string>> reply = replied.get_future();
  ASSERT_EQ(reply.wait_for(kPatience), std::future_status::ready);
  EXPECT_EQ(reply.get(), "1");
  EXPECT_NE(replier, std::this_thread::get_id());
  EXPECT_NE(instrument.latest_capture(), nullptr);
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
    std::vector<double> fields;

    const bool counted = eventually([&] {
      const std::optional<std::string> reply = session.send("MEAS:STAT? VAV,CHAN1");
      fields = reply ? test::numbers(reply) : std::vector<double>{};
      return fields.size() == 6 && fields[5] >= 2;  // The count.
    });

    ASSERT_TRUE(counted) << offset;
    EXPECT_EQ(fields[2], offset);  // The minimum.
    EXPECT_EQ(fields[3], offset);  // The maximum.
  }
}

// The processing thread counts a capture before it publishes it, so MEAS:STAT? checks the channel
// against the statistics: right after a channel change, the latest capture may still have the old
// channels while the statistics already count a capture with the new ones.
TEST_F(LivePipelineTest, StatisticsHoldTheChannelsOfTheCapturesTheyCount) {
  session.send("ACQ:POIN 10");  // Many captures a block, so the race is often there to lose.
  const LivePipeline live(instrument);
  for (int round = 0; round < 50; ++round) {
    const bool enabled = round % 2 == 0;
    session.send(enabled ? "CHAN2:DISP ON" : "CHAN2:DISP OFF");
    std::string error;

    const bool counted = eventually([&] {
      session.send("MEAS:STAT? VPP,CHAN2");
      error = session.next_error();
      return error != test::kDataStaleReply;
    });

    ASSERT_TRUE(counted) << round;
    EXPECT_EQ(error, enabled ? test::kNoErrorReply : test::kSettingsConflictReply) << round;
  }
}

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
