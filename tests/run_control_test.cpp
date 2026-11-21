// Run control: the Running, Stopped and Armed-single run states (RUN, STOP, SINGle), the pending
// operation *OPC? and *WAI wait on, and which setting changes discard the capture in progress,
// driven through SCPI with the sample clock stepped by hand.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/sample_clock.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::first_index;
using test::kNoErrorReply;
using test::kResetRecordLength;
using test::kUntriggeredDc;
using test::LineReply;
using test::ScpiSession;
using testing::Each;
using testing::ElementsAre;
using testing::Eq;

/// `WAV:PRE?` for a capture of the DC signal below: untriggered, time zero on the first sample.
constexpr auto kUntriggeredPreamble = "1E+06,10000,1,0,0,1";

class RunControlTest : public test::UntriggeredDcFixture {
 protected:
  /// Starts `line` on `client` and expects it to be held.
  static std::shared_ptr<const LineReply> start_held(ScpiSession& client, std::string_view line) {
    std::shared_ptr<const LineReply> reply = client.start(line);
    EXPECT_FALSE(reply->ended) << line;
    return reply;
  }

  /// Starts `line` on the fixture's Session and expects it to be held.
  std::shared_ptr<const LineReply> start_held(std::string_view line) {
    return start_held(session, line);
  }
};

TEST_F(RunControlTest, RunningTakesCapturesContinuously) {
  instrument.advance(3 * kResetRecordLength);

  EXPECT_EQ(first_index(instrument), 2 * kResetRecordLength);
}

TEST_F(RunControlTest, StopFreezesTheLatestCapture) {
  instrument.advance(kResetRecordLength);
  const auto frozen = instrument.latest_capture();

  session.send("STOP");
  instrument.advance(5 * kResetRecordLength);

  EXPECT_EQ(instrument.latest_capture(), frozen);
  EXPECT_EQ(session.send("WAV:PRE?"), kUntriggeredPreamble);
}

TEST_F(RunControlTest, StopWhileStoppedChangesNothing) {
  instrument.advance(kResetRecordLength);
  const auto frozen = instrument.latest_capture();
  session.send("STOP");

  session.send("STOP");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(instrument.latest_capture(), frozen);
  EXPECT_EQ(session.send("*OPC?"), "1");
}

TEST_F(RunControlTest, RunAfterStopStartsANewCapture) {
  instrument.advance(6000);
  session.send("STOP");
  instrument.advance(1000);

  session.send("RUN");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(first_index(instrument), 7000U);  // Nothing from before the STOP.
}

TEST_F(RunControlTest, RunWhileRunningKeepsTheCaptureInProgress) {
  instrument.advance(6000);

  session.send("RUN");
  instrument.advance(4000);

  EXPECT_EQ(first_index(instrument), 0U);
}

TEST_F(RunControlTest, SingleTakesOneCaptureThenStops) {
  session.send("SINGle");
  instrument.advance(kResetRecordLength);
  const auto single = instrument.latest_capture();
  ASSERT_NE(single, nullptr);

  instrument.advance(5 * kResetRecordLength);

  EXPECT_EQ(instrument.latest_capture(), single);
  EXPECT_EQ(single->first_index, 0U);
}

TEST_F(RunControlTest, SingleCaptureStartsAfterTheSingle) {
  instrument.advance(6000);

  session.send("SING");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(first_index(instrument), 6000U);
}

// A block that holds many records completes only the first after SINGle.
TEST_F(RunControlTest, SingleTakesOneCaptureEvenWhenABlockHoldsSeveral) {
  session.send("ACQ:POIN 10");
  instrument.advance(25);
  ASSERT_EQ(first_index(instrument), 10U);

  session.send("SING");
  instrument.advance(1000);

  EXPECT_EQ(first_index(instrument), 25U);
  instrument.advance(1000);
  EXPECT_EQ(first_index(instrument), 25U);
}

TEST_F(RunControlTest, LatestCaptureStaysReadableWhileTheSingleIsArmed) {
  instrument.advance(kResetRecordLength);
  const auto earlier = instrument.latest_capture();

  session.send("SINGle");
  instrument.advance(kResetRecordLength - 1);

  EXPECT_EQ(instrument.latest_capture(), earlier);
  EXPECT_EQ(session.send("WAV:PRE?"), kUntriggeredPreamble);
}

TEST_F(RunControlTest, SingleWhileArmedRestartsTheCapture) {
  session.send("SINGle");
  instrument.advance(6000);

  session.send("SINGle");
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(first_index(instrument), 6000U);
}

TEST_F(RunControlTest, SingleWhileStoppedArmsOneCapture) {
  session.send("STOP");
  instrument.advance(3000);

  session.send("SINGle");
  instrument.advance(3 * kResetRecordLength);

  EXPECT_EQ(first_index(instrument), 3000U);
}

TEST_F(RunControlTest, RunWhileArmedKeepsTheCaptureInProgressAndCarriesOn) {
  session.send("SINGle");
  instrument.advance(6000);

  session.send("RUN");
  instrument.advance(4000);
  EXPECT_EQ(first_index(instrument), 0U);

  instrument.advance(kResetRecordLength);
  EXPECT_EQ(first_index(instrument), kResetRecordLength);
}

TEST_F(RunControlTest, RunAfterTheSingleCaptureResumesContinuousCaptures) {
  session.send("SINGle");
  instrument.advance(kResetRecordLength + 500);

  session.send("RUN");
  instrument.advance(2 * kResetRecordLength);

  EXPECT_EQ(first_index(instrument), 2 * kResetRecordLength + 500);
}

TEST_F(RunControlTest, StopWhileArmedEndsTheSingleWithoutACapture) {
  session.send("SINGle");
  instrument.advance(6000);

  session.send("STOP");
  instrument.advance(3 * kResetRecordLength);

  EXPECT_EQ(instrument.latest_capture(), nullptr);
}

TEST_F(RunControlTest, ResetReturnsToRunning) {
  session.send("STOP");

  session.send(std::string("*RST;:") + kUntriggeredDc);
  instrument.advance(2 * kResetRecordLength);

  EXPECT_EQ(first_index(instrument), kResetRecordLength);
}

// Settings sent while Stopped apply to the captures after RUN.
TEST_F(RunControlTest, SettingsChangedWhileStoppedApplyAfterRun) {
  session.send("STOP;:ACQ:POIN 100");
  instrument.advance(1000);

  session.send("RUN");
  instrument.advance(100);

  EXPECT_EQ(test::latest(instrument)->record_length, 100U);
  EXPECT_EQ(first_index(instrument), 1000U);
}

TEST_F(RunControlTest, RunControlCommandsTakeNoParametersAndHaveNoQuery) {
  for (const char* line : {"RUN 1", "STOP 1", "SING 1", "*WAI 1", "*OPC? 1"}) {
    session.send(line);
    EXPECT_EQ(session.next_error(), test::kParameterNotAllowedReply) << line;
  }
  for (const char* line : {"RUN?", "STOP?", "SING?", "*WAI?", "*OPC"}) {
    session.send(line);
    EXPECT_EQ(session.next_error(), test::kUndefinedHeaderReply) << line;
  }
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(RunControlTest, OpcRepliesAtOnceWhenNoOperationIsPending) {
  EXPECT_EQ(session.send("*OPC?"), "1");
  EXPECT_EQ(session.send("STOP;*OPC?"), "1");
  EXPECT_EQ(session.send("SINGle;STOP;*OPC?"), "1");
}

// Spec story 34.
TEST_F(RunControlTest, SingleOpcRepliesOnceTheCaptureIsComplete) {
  const auto reply = start_held("SINGle;*OPC?");

  instrument.advance(kResetRecordLength - 1);
  EXPECT_FALSE(reply->ended);

  instrument.advance(1);
  EXPECT_TRUE(reply->ended);
  EXPECT_EQ(reply->text, "1");
  EXPECT_EQ(first_index(instrument), 0U);
}

// Spec story 35: no trigger arrives in Normal mode, and the server never gives up on its own.
TEST_F(RunControlTest, OpcHasNoTimeout) {
  session.send("TRIG:MODE NORM");
  const auto reply = start_held("SINGle;*OPC?");

  instrument.advance(100 * kResetRecordLength);

  EXPECT_FALSE(reply->ended);
}

TEST_F(RunControlTest, OpcRepliesWhenAnotherSessionSendsStop) {
  session.send("TRIG:MODE NORM");
  const auto reply = start_held("SINGle;*OPC?");
  ScpiSession other{instrument};

  other.send("STOP");

  EXPECT_TRUE(reply->ended);
  EXPECT_EQ(reply->text, "1");
}

// RUN and *RST also leave Armed-single, so nothing is pending any more.
TEST_F(RunControlTest, OpcRepliesWhenAnotherSessionSendsRunOrReset) {
  session.send("TRIG:MODE NORM");
  for (const char* line : {"RUN", "*RST"}) {
    const auto reply = start_held("SINGle;*OPC?");
    ScpiSession other{instrument};

    other.send(line);

    EXPECT_TRUE(reply->ended) << line;
    EXPECT_EQ(reply->text, "1") << line;
    session.send("TRIG:MODE NORM");
  }
}

// The reply comes from the call that ended the operation, after that Session's own reply.
TEST_F(RunControlTest, StopFromAnotherSessionRepliesToItBeforeTheWaitingSession) {
  session.send("TRIG:MODE NORM");
  std::vector<std::string> order;
  const SessionId waiting = instrument.open_session();
  const SessionId stopper = instrument.open_session();
  instrument.execute(waiting, "SINGle;*OPC?", [&](const auto&) { order.emplace_back("waiting"); });

  instrument.execute(stopper, "STOP", [&](const auto&) { order.emplace_back("stopper"); });

  EXPECT_THAT(order, ElementsAre("stopper", "waiting"));
}

TEST_F(RunControlTest, OpcInAnotherSessionWaitsForTheSameSingle) {
  session.send("SINGle");
  ScpiSession other{instrument};
  const auto reply = start_held(other, "*OPC?");
  EXPECT_FALSE(reply->ended);

  instrument.advance(kResetRecordLength);

  EXPECT_EQ(reply->text, "1");
}

TEST_F(RunControlTest, EverySessionWaitingOnTheSingleReplies) {
  ScpiSession second{instrument};
  ScpiSession third{instrument};
  const auto first_reply = start_held("SINGle;*OPC?");
  const auto second_reply = start_held(second, "*OPC?");
  const auto third_reply = start_held(third, "*WAI;*IDN?");

  instrument.advance(kResetRecordLength);

  EXPECT_EQ(first_reply->text, "1");
  EXPECT_EQ(second_reply->text, "1");
  EXPECT_TRUE(third_reply->text.has_value());
}

// Spec story 34: the queries after *OPC? on the line read the completed capture.
TEST_F(RunControlTest, CommandsAfterOpcOnTheLineRunOnceTheCaptureIsComplete) {
  const auto reply = start_held("SINGle;*OPC?;:WAV:PRE?");

  instrument.advance(kResetRecordLength);

  EXPECT_EQ(reply->text, std::string("1;") + kUntriggeredPreamble);
  EXPECT_EQ(session.next_error(), kNoErrorReply);  // WAV:PRE? did not run before the capture.
}

// Spec story 37.
TEST_F(RunControlTest, WaiHoldsTheRestOfTheLineUntilTheCaptureIsComplete) {
  const auto reply = start_held("SINGle;*WAI;:WAV:PRE?");

  instrument.advance(kResetRecordLength);

  EXPECT_EQ(reply->text, kUntriggeredPreamble);
}

TEST_F(RunControlTest, WaiDoesNothingWhenNoOperationIsPending) {
  EXPECT_EQ(session.send("*WAI"), std::nullopt);
  EXPECT_EQ(session.send("*WAI;*OPC?"), "1");
}

TEST_F(RunControlTest, WaiLineWithoutQueriesEndsWithNoReplyOnceTheCaptureIsComplete) {
  const auto reply = start_held("SINGle;*WAI");

  instrument.advance(kResetRecordLength);

  EXPECT_TRUE(reply->ended);
  EXPECT_EQ(reply->text, std::nullopt);
}

// The resumed line still runs for its own Session, although another Session's STOP resumes it.
TEST_F(RunControlTest, ResumedLineReportsErrorsToItsOwnSession) {
  start_held("SINGle;*WAI;BOGUS");
  ScpiSession other{instrument};

  other.send("STOP");

  EXPECT_EQ(session.next_error(), test::kUndefinedHeaderReply);
  EXPECT_EQ(other.next_error(), kNoErrorReply);
}

// Spec story 37: a Session's later lines wait for its held line; the transport sends the next line
// only once the reply arrives, and the facade refuses one sent sooner.
TEST_F(RunControlTest, SessionWithAHeldLineRefusesItsNextLine) {
  start_held("SINGle;*WAI");

  EXPECT_THROW(session.start("*IDN?"), std::logic_error);

  instrument.advance(kResetRecordLength);
  EXPECT_EQ(session.send("*OPC?"), "1");
}

// A resumed line that arms another Single waits again, for that one.
TEST_F(RunControlTest, ResumedLineMayWaitForTheNextSingle) {
  const auto reply = start_held("SINGle;*WAI;SINGle;*OPC?");

  instrument.advance(kResetRecordLength);
  EXPECT_FALSE(reply->ended);

  instrument.advance(kResetRecordLength);
  EXPECT_EQ(reply->text, "1");
  EXPECT_EQ(first_index(instrument), kResetRecordLength);
}

// Every line waiting when the Single's capture completes carries on, although the first line
// resumed arms another Single: each waited for that capture, and it is complete.
TEST_F(RunControlTest, LinesHeldOnACompletedSingleAllCarryOnWhenOneArmsTheNext) {
  ScpiSession other{instrument};
  const auto rearming = start_held("SINGle;*WAI;SINGle;*OPC?");
  const auto waiting = start_held(other, "*OPC?;:WAV:PRE?");

  instrument.advance(kResetRecordLength);

  EXPECT_FALSE(rearming->ended);
  EXPECT_EQ(waiting->text, std::string("1;") + kUntriggeredPreamble);
}

// A line resumed later in the same round may end the new Single, which ends the wait again.
TEST_F(RunControlTest, LineResumedAfterOneThatArmsTheNextSingleMayStopIt) {
  ScpiSession stopper{instrument};
  const auto rearming = start_held("SINGle;*WAI;SINGle;*OPC?");
  const auto stopping = start_held(stopper, "*WAI;STOP");

  instrument.advance(kResetRecordLength);

  EXPECT_EQ(rearming->text, "1");
  EXPECT_TRUE(stopping->ended);
}

// Spec story 35: disconnecting ends the wait; there is no one left to reply to.
TEST_F(RunControlTest, ClosingTheSessionDropsItsHeldLine) {
  ScpiSession leaving{instrument};
  const auto dropped = start_held(leaving, "SINGle;*OPC?");
  const auto kept = start_held("*OPC?");

  leaving.close();
  instrument.advance(kResetRecordLength);

  EXPECT_FALSE(dropped->ended);
  EXPECT_EQ(kept->text, "1");
}

// A Single that completes inside a block leaves the rest of the block to what the lines it
// resumes start, as stepping one record at a time would.
TEST_F(RunControlTest, SingleArmedByAResumedLineStartsAfterTheCaptureWithinOneAdvance) {
  const auto reply = start_held("SINGle;*WAI;SINGle;*OPC?");

  instrument.advance(2 * kResetRecordLength);

  EXPECT_EQ(reply->text, "1");
  EXPECT_EQ(first_index(instrument), kResetRecordLength);
}

TEST_F(RunControlTest, RunSentByAResumedLineStartsAfterTheCaptureWithinOneAdvance) {
  start_held("SINGle;*WAI;RUN");

  instrument.advance(3 * kResetRecordLength);

  EXPECT_EQ(first_index(instrument), 2 * kResetRecordLength);
}

/// What a line and the captures leave once the sample clock has run.
struct SteppedRun {
  /// The line's reply, or `<held>` while it is held.
  std::string reply;
  SampleIndex latest_first_index = 0;
  /// The latest capture as another Session's `WAV:PRE?;DATA?` reads it.
  std::optional<std::string> latest;

  bool operator==(const SteppedRun&) const = default;

  // The capture text runs to many kilobytes, so a failure prints only its length.
  friend void PrintTo(const SteppedRun& run, std::ostream* out) {
    *out << '"' << run.reply << "\", first index " << run.latest_first_index << ", "
         << run.latest.value_or("").size() << " bytes of capture";
  }
};

/// Runs `line` on a fresh instrument set up with kUntriggeredDc and steps the sample clock
/// `samples` sample indexes in `advance` calls of at most `step`.
SteppedRun run_stepped(std::string_view line, std::size_t samples, std::size_t step) {
  Instrument instrument;
  ScpiSession session{instrument};
  session.send(kUntriggeredDc);
  const auto reply = session.start(line);
  for (std::size_t done = 0; done < samples; done += step) {
    instrument.advance(std::min(step, samples - done));
  }
  ScpiSession reader{instrument};
  return {.reply = reply->ended ? reply->text.value_or("<no reply>") : "<held>",
          .latest_first_index = first_index(instrument),
          .latest = reader.send("WAV:PRE?;DATA?")};
}

/// A line SteppingInvarianceTest runs, how many sample indexes it steps the clock, and the line's
/// reply when the clock is stepped one record length at a time.
struct ResumedLine {
  const char* line;
  std::size_t samples;
  const char* reply;

  friend void PrintTo(const ResumedLine& resumed, std::ostream* out) { *out << resumed.line; }
};

// ADR 0003: captures and replies do not depend on how `advance` is chunked, even when a line
// resumed inside it arms a Single, runs, or changes a setting at the sample the Single ended.
class SteppingInvarianceTest : public testing::TestWithParam<ResumedLine> {};

TEST_P(SteppingInvarianceTest, OneAdvanceGivesWhatManySmallOnesGive) {
  const auto [line, samples, reply] = GetParam();
  const SteppedRun by_record = run_stepped(line, samples, kResetRecordLength);
  ASSERT_EQ(by_record.reply, reply);

  for (const std::size_t step : {samples, std::size_t{1}, std::size_t{3333}, kResetRecordLength - 1,
                                 kResetRecordLength + 1}) {
    EXPECT_EQ(run_stepped(line, samples, step), by_record) << "step " << step;
  }
}

INSTANTIATE_TEST_SUITE_P(
    ResumedLines, SteppingInvarianceTest,
    testing::Values(
        ResumedLine{"SINGle;*WAI;SINGle;*OPC?", 3 * kResetRecordLength, "1"},
        ResumedLine{"SINGle;*WAI;RUN;SYST:ERR?", 3 * kResetRecordLength, kNoErrorReply},
        ResumedLine{"SINGle;*WAI;SOUR1:OFFS 0.5;:RUN;SYST:ERR?", 3 * kResetRecordLength,
                    kNoErrorReply},
        ResumedLine{"SINGle;*WAI;SINGle;*WAI;SINGle;*OPC?", 3 * kResetRecordLength, "1"},
        // Triggered captures: pre-trigger samples, Normal mode and a holdoff running across the
        // sample the Single ends at.
        ResumedLine{"SOUR1:FUNC SIN;NOIS 0.05;:TRIG:MODE NORM;POS 50;HOLD 3E-3;:SINGle;*WAI;"
                    "SINGle;*WAI;:RUN;SYST:ERR?",
                    5 * kResetRecordLength, kNoErrorReply},
        // Singles that end inside the sample blocks `advance` splits a long call into.
        ResumedLine{"ACQ:POIN 40000;:SINGle;*WAI;SINGle;*WAI;:RUN;SYST:ERR?",
                    15 * kResetRecordLength, kNoErrorReply}));

// Any setting change discards the capture in progress, even one that sets the value already in
// place, and the capture restarts at the next sample index.
class SettingChangeTest : public RunControlTest, public testing::WithParamInterface<const char*> {};

TEST_P(SettingChangeTest, DiscardsTheCaptureInProgressWhileRunning) {
  instrument.advance(kResetRecordLength);
  const auto published = instrument.latest_capture();
  instrument.advance(6000);

  session.send(GetParam());

  EXPECT_EQ(instrument.latest_capture(), published);  // The latest capture stays readable.
  instrument.advance(kResetRecordLength - 6000);
  EXPECT_EQ(instrument.latest_capture(), published);
  instrument.advance(6000);
  EXPECT_EQ(first_index(instrument), kResetRecordLength + 6000);
}

TEST_P(SettingChangeTest, RestartsTheArmedSingle) {
  const auto reply = start_held("SINGle;*OPC?");
  instrument.advance(6000);
  ScpiSession other{instrument};

  other.send(GetParam());
  instrument.advance(kResetRecordLength - 1);
  EXPECT_FALSE(reply->ended);

  instrument.advance(1);
  EXPECT_EQ(reply->text, "1");
  EXPECT_EQ(first_index(instrument), 6000U);
}

INSTANTIATE_TEST_SUITE_P(EverySetting, SettingChangeTest,
                         testing::Values("SOUR1:FUNC DC", "SOUR1:FREQ 1000", "SOUR1:AMPL 2",
                                         "SOUR1:OFFS 0", "SOUR1:NOIS 0", "SOUR2:FREQ 50",
                                         "CHAN1:DISP ON", "CHAN2:DISP OFF", "CHAN1:SCAL 1",
                                         "CHAN1:OFFS 0", "ACQ:SRAT 1E6", "ACQ:POIN 10000",
                                         "TRIG:SOUR CHAN1", "TRIG:LEV 0", "TRIG:SLOP POS",
                                         "TRIG:MODE AUTO", "TRIG:HOLD 0", "TRIG:POS 0"));

// The timebase only zooms the display (ADR 0004), and WAV and FFT settings belong to one Session
// (ADR 0002). A Fault is an event in the signal, which the capture is there to show.
TEST_F(RunControlTest, DisplayAndSessionSettingsAndFaultsKeepTheCaptureInProgress) {
  instrument.advance(6000);

  session.send("TIM:SCAL 1;:WAV:SOUR CHAN1;FORM REAL;:FFT:SOUR CHAN1;WIND RECT;:SOUR1:FAUL GLIT");
  instrument.advance(4000);

  EXPECT_EQ(session.next_error(), kNoErrorReply);
  EXPECT_EQ(first_index(instrument), 0U);
}

// A Generator change mid-capture would otherwise mix two signals in one capture.
TEST_F(RunControlTest, CaptureAfterAGeneratorChangeHoldsOnlyTheNewSignal) {
  instrument.advance(6000);

  session.send("SOUR1:OFFS 0.5");
  instrument.advance(kResetRecordLength);

  EXPECT_THAT(test::captured(instrument, 1).samples, Each(Eq(0.5F)));
}

// Spec story 38: in the Running run state each query reads the capture that is latest when it
// runs.
TEST_F(RunControlTest, QueriesReadTheLatestCaptureWhenTheyRun) {
  session.send("ACQ:POIN 2;:SOUR1:FUNC RAMP;FREQ 250KHZ");
  instrument.advance(2);
  const auto first = session.send("WAV:DATA?");

  instrument.advance(2);

  EXPECT_EQ(first, "-1,-0.5");
  EXPECT_EQ(session.send("WAV:DATA?"), "0,0.5");
}

}  // namespace
}  // namespace openscope
