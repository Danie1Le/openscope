// The shared test helpers that are not tested where they are used: eventually, padded,
// advance_in_steps and float32_reply.

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "binary_block.hpp"
#include "eventually.hpp"
#include "instrument_fixture.hpp"
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

}  // namespace
}  // namespace openscope::test
