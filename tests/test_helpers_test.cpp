// The shared test helpers that are not tested where they are used: eventually, padded, send_live,
// advance_in_steps, float32_reply and measurement_index.

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "binary_block.hpp"
#include "eventually.hpp"
#include "instrument_fixture.hpp"
#include "measurement_fixture.hpp"
#include "openscope/measurement.hpp"
#include "scpi_session.hpp"

namespace openscope::test {
namespace {

TEST(EventuallyTest, PollsUntilTheConditionHolds) {
  int polls = 0;

  EXPECT_TRUE(eventually([&] { return ++polls == 3; }));
  EXPECT_EQ(polls, 3);
}

TEST(EventuallyTest, GivesUpAfterItsPatience) {
  const auto started = std::chrono::steady_clock::now();

  EXPECT_FALSE(eventually([] { return false; }, std::chrono::milliseconds(20)));
  const auto waited = std::chrono::steady_clock::now() - started;
  EXPECT_GE(waited, std::chrono::milliseconds(20));
  EXPECT_LT(waited, kPatience / 2);  // Its own patience, not the default.
}

TEST(PaddedTest, PadsWithSpacesToTheLength) {
  EXPECT_EQ(padded("*IDN?", 8), "*IDN?   ");
  EXPECT_EQ(padded("*IDN?", 5), "*IDN?");
}

// Untriggered DC captures of 10 samples, one every 10 sample indexes from 0: the latest capture
// shows how far the clock went.
TEST_F(UntriggeredDcFixture, AdvanceInStepsTakesAsManySamplesAsOneAdvance) {
  session.send("ACQ:POIN 10");

  advance_in_steps(instrument, 25, 7);

  // The capture of samples 20 to 29 completes at sample 29, exactly 5 samples on from 25.
  EXPECT_EQ(first_index(instrument), 10U);
  instrument.advance(4);
  EXPECT_EQ(first_index(instrument), 10U);
  instrument.advance(1);
  EXPECT_EQ(first_index(instrument), 20U);
}

TEST_F(UntriggeredDcFixture, Float32ReplyIsTheDecodedBlock) {
  session.send("ACQ:POIN 4;:SOUR1:OFFS 0.5;:WAV:FORM REAL");
  instrument.advance(4);

  EXPECT_EQ(float32_reply(session, "WAV:DATA?"), std::vector<float>(4, 0.5F));
  EXPECT_NONFATAL_FAILURE((void)float32_reply(session, "*IDN?"), "not a definite-length block");
}

TEST_F(UntriggeredDcFixture, SendLiveRepliesALineThatEndsAtOnce) {
  EXPECT_EQ(session.send_live("ACQ:POIN 4;POIN?"), "4");
}

// The line ends on the thread that completes the Single, after send_live has begun to wait.
TEST_F(UntriggeredDcFixture, SendLiveWaitsForALineThatEndsOnAnotherThread) {
  session.send("ACQ:POIN 4");
  const std::jthread single_taker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    instrument.advance(4);
  });

  EXPECT_EQ(session.send_live("SING;*OPC?"), "1");
}

TEST_F(UntriggeredDcFixture, SendLiveFailsALineThatNeverEnds) {
  session.send("TRIG:MODE NORM");
  std::optional<std::string> reply;

  EXPECT_NONFATAL_FAILURE(reply = session.send_live("SING;*OPC?", std::chrono::milliseconds(50)),
                          "has not ended");
  EXPECT_EQ(reply, std::nullopt);
  session.close();  // Drops the held line.
}

TEST(MeasurementIndexTest, IsThePositionOfTheMnemonicInMeasurements) {
  const std::span<const Measurement> all = measurements();
  const std::size_t vpp = measurement_index("VPP");
  const std::size_t fall_time = measurement_index("FALLtime");
  ASSERT_LT(vpp, all.size());
  ASSERT_LT(fall_time, all.size());

  EXPECT_EQ(all[vpp].mnemonic, "VPP");
  EXPECT_EQ(all[fall_time].mnemonic, "FALLtime");
}

TEST(MeasurementIndexTest, FailsTheTestForAnUnknownMnemonic) {
  std::size_t index = 0;

  EXPECT_NONFATAL_FAILURE(index = measurement_index("FREQ"), "no measurement FREQ");
  EXPECT_EQ(index, measurements().size());
}

}  // namespace
}  // namespace openscope::test
