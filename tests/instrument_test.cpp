#include "openscope/instrument.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::ScpiSession;
using testing::MatchesRegex;
using testing::Optional;
using testing::StartsWith;

constexpr auto kNoErrorReply = R"(0,"No error")";
constexpr auto kUndefinedHeaderReply = R"(-113,"Undefined header")";

class InstrumentTest : public testing::Test {
 protected:
  Instrument instrument;
  ScpiSession session{instrument};
};

TEST_F(InstrumentTest, IdnReportsManufacturerModelSerialAndVersion) {
  EXPECT_THAT(session.send("*IDN?"),
              Optional(MatchesRegex(R"(OpenScope,OS-4,[^,]+,[0-9]+\.[0-9]+\.[0-9]+)")));
}

TEST_F(InstrumentTest, SystErrReportsNoErrorWhenQueueIsEmpty) {
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, UnknownHeaderQueuesUndefinedHeaderWithoutReply) {
  EXPECT_EQ(session.send("BOGUS:HEADER"), std::nullopt);

  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, EachUnknownHeaderQueuesItsOwnError) {
  session.send("BOGUS");
  session.send("BOGUS");

  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, EmptyLineIsIgnored) {
  EXPECT_EQ(session.send(""), std::nullopt);

  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, HeadersAreCaseInsensitive) {
  EXPECT_THAT(session.send("*idn?"), Optional(StartsWith("OpenScope,")));
  session.send("bogus");
  EXPECT_EQ(session.send("Syst:Err?"), kUndefinedHeaderReply);
}

TEST_F(InstrumentTest, ClsEmptiesTheErrorQueue) {
  session.send("BOGUS");
  session.send("BOGUS");

  EXPECT_EQ(session.send("*CLS"), std::nullopt);

  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, ClsOnAnEmptyQueueQueuesNothing) {
  EXPECT_EQ(session.send("*CLS"), std::nullopt);

  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, RstIsAcceptedAndLeavesTheErrorQueueAlone) {
  session.send("BOGUS");

  EXPECT_EQ(session.send("*RST"), std::nullopt);

  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// ADR 0002: each Session owns its error queue.
TEST_F(InstrumentTest, ErrorsGoOnlyToTheCallingSessionsQueue) {
  ScpiSession other{instrument};

  session.send("BOGUS");

  EXPECT_EQ(other.next_error(), kNoErrorReply);
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
}

TEST_F(InstrumentTest, ClsClearsOnlyTheCallingSessionsQueue) {
  ScpiSession other{instrument};
  session.send("BOGUS");
  other.send("BOGUS");

  other.send("*CLS");

  EXPECT_EQ(other.next_error(), kNoErrorReply);
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
}

TEST_F(InstrumentTest, ExecuteRejectsASessionItDidNotOpen) {
  EXPECT_THROW(instrument.execute(SessionId{42}, "*IDN?"), std::out_of_range);
}

}  // namespace
}  // namespace openscope
