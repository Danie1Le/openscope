#include "openscope/instrument.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

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

TEST_F(InstrumentTest, SystErrAcceptsLongFormsAndTheOptionalNextNode) {
  for (const char* query : {"SYSTem:ERRor?", "SYSTEM:ERROR?", "syst:err:next?", "SYST:ERR:NEXT?",
                            ":SYSTem:ERRor:NEXT?"}) {
    session.send("BOGUS");
    EXPECT_EQ(session.send(query), kUndefinedHeaderReply) << query;
  }
}

TEST_F(InstrumentTest, OnlyTheShortAndLongFormsOfAMnemonicMatch) {
  for (const char* query : {"SYSTE:ERR?", "SYS:ERR?", "SYST:ERRO?", "SYST:ERR:NEX?"}) {
    EXPECT_EQ(session.send(query), std::nullopt) << query;
    EXPECT_EQ(session.next_error(), kUndefinedHeaderReply) << query;
  }
}

TEST_F(InstrumentTest, SetterAndQueryFormsAreSeparateCommands) {
  for (const char* header : {"*IDN", "*RST?", "*CLS?", "SYST:ERR"}) {
    EXPECT_EQ(session.send(header), std::nullopt) << header;
    EXPECT_EQ(session.next_error(), kUndefinedHeaderReply) << header;
  }
}

TEST_F(InstrumentTest, CommandsSeparatedBySemicolonsRunInOrderAndRepliesAreJoined) {
  session.send("BOGUS");

  const std::string idn = session.send("*IDN?").value_or("");

  EXPECT_EQ(session.send("SYST:ERR?;*CLS;*IDN?;:SYST:ERR?"),
            R"(-113,"Undefined header";)" + idn + R"(;0,"No error")");
}

// SCPI-99 compound headers: a header without a leading colon continues from the previous path.
TEST_F(InstrumentTest, RelativeHeaderContinuesFromThePreviousCommandsPath) {
  session.send("BOGUS");
  session.send("BOGUS");

  EXPECT_EQ(session.send("SYST:ERR?;ERR?"), R"(-113,"Undefined header";-113,"Undefined header")");
}

TEST_F(InstrumentTest, CommonCommandsKeepTheCurrentPath) {
  EXPECT_EQ(session.send("SYST:ERR?;*CLS;ERR:NEXT?"), R"(0,"No error";0,"No error")");
}

TEST_F(InstrumentTest, LeadingColonResolvesFromTheRoot) {
  EXPECT_EQ(session.send("SYST:ERR?;:SYST:ERR?"), R"(0,"No error";0,"No error")");
  EXPECT_EQ(session.send("SYST:ERR?;SYST:ERR?"), R"(0,"No error")");  // Then SYST:SYST:ERR?.
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
}

TEST_F(InstrumentTest, WhitespaceAroundCommandsAndSeparatorsIsIgnored) {
  const std::string idn = session.send("*IDN?").value_or("");

  EXPECT_EQ(session.send(" \t*IDN? ; *IDN?\t "), idn + ";" + idn);
  EXPECT_EQ(session.send("   "), std::nullopt);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, ParameterOnACommandThatTakesNoneQueuesParameterNotAllowed) {
  session.send("BOGUS");

  EXPECT_EQ(session.send("*CLS 1"), std::nullopt);
  EXPECT_EQ(session.send("SYST:ERR? ON"), std::nullopt);

  // *CLS did not run: the first error is still queued.
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), R"(-108,"Parameter not allowed")");
  EXPECT_EQ(session.next_error(), R"(-108,"Parameter not allowed")");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, SyntaxErrorAnywhereOnALineRunsNothingOnIt) {
  session.send("BOGUS");

  EXPECT_EQ(session.send("*CLS;*IDN?;SYST:ERR? 1,"), std::nullopt);

  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), R"(-102,"Syntax error")");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, ErrorStopsTheRestOfTheLineAfterEarlierCommandsRan) {
  session.send("BOGUS");

  EXPECT_EQ(session.send("SYST:ERR?;BOGUS;*CLS"), R"(-113,"Undefined header")");

  // *CLS after the bad header did not run, so the new error is still queued.
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, LineOfExactlyTheMaximumLengthRuns) {
  session.send("BOGUS");
  std::string line = "*CLS";
  line.resize(Instrument::kMaxLineLength, ' ');

  EXPECT_EQ(session.send(line), std::nullopt);

  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, LineOverTheMaximumLengthQueuesInputBufferOverrunAndRunsNothing) {
  session.send("BOGUS");
  std::string line = "*CLS;*IDN?";
  line.resize(Instrument::kMaxLineLength + 1, ' ');

  EXPECT_EQ(session.send(line), std::nullopt);

  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), R"(-363,"Input buffer overrun")");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, FullErrorQueueKeepsTheOldestErrorsAndEndsWithQueueOverflow) {
  session.send("*CLS 1");  // -108 first, so the test can see which end of the queue survives.
  for (std::size_t i = 1; i < Session::kErrorQueueCapacity + 5; ++i) {
    session.send("BOGUS");
  }

  EXPECT_EQ(session.next_error(), R"(-108,"Parameter not allowed")");
  for (std::size_t i = 2; i < Session::kErrorQueueCapacity; ++i) {
    EXPECT_EQ(session.next_error(), kUndefinedHeaderReply) << "entry " << i;
  }
  EXPECT_EQ(session.next_error(), R"(-350,"Queue overflow")");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, ErrorQueueHoldsItsCapacityWithoutOverflow) {
  for (std::size_t i = 0; i < Session::kErrorQueueCapacity; ++i) {
    session.send("BOGUS");
  }

  for (std::size_t i = 0; i < Session::kErrorQueueCapacity; ++i) {
    EXPECT_EQ(session.next_error(), kUndefinedHeaderReply) << "entry " << i;
  }
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, ErrorsAfterAnOverflowQueueBehindIt) {
  for (std::size_t i = 0; i < Session::kErrorQueueCapacity + 1; ++i) {
    session.send("BOGUS");
  }
  session.next_error();  // Frees one slot.

  session.send("*CLS 1");

  for (std::size_t i = 2; i < Session::kErrorQueueCapacity; ++i) {
    session.next_error();
  }
  EXPECT_EQ(session.next_error(), R"(-350,"Queue overflow")");
  EXPECT_EQ(session.next_error(), R"(-108,"Parameter not allowed")");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(InstrumentTest, ExecuteRejectsASessionItDidNotOpen) {
  EXPECT_THROW(instrument.execute(SessionId{42}, "*IDN?"), std::out_of_range);
}

}  // namespace
}  // namespace openscope
