// Waveform transfer: WAV:SOUR and WAV:FORM per Session, WAV:DATA? as ASCII or a float32 block,
// and WAV:PRE?.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "binary_block.hpp"
#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/scpi/response.hpp"
#include "reply_numbers.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::captured;
using test::decode_float32_block;
using test::float32_reply;
using test::kDataStaleReply;
using test::kDataTypeErrorReply;
using test::kIllegalParameterValueReply;
using test::kMissingParameterReply;
using test::kNoErrorReply;
using test::kParameterNotAllowedReply;
using test::kResetRecordLength;
using test::kSettingsConflictReply;
using test::kTooMuchDataReply;
using test::ScpiSession;
using testing::Optional;
using testing::StartsWith;

class WaveformTest : public test::InstrumentFixture {
 protected:
  /// Takes one untriggered capture of `points` points.
  void take_capture(std::size_t points) {
    session.send(std::format("ACQ:POIN {};:{}", points, test::kUntriggeredCaptures));
    instrument.advance(points);
  }
};

TEST_F(WaveformTest, NewSessionReadsChannelOneAsAscii) {
  EXPECT_EQ(session.send("WAV:SOUR?"), "CHAN1");
  EXPECT_EQ(session.send("WAV:FORM?"), "ASC");
}

TEST_F(WaveformTest, SourceAcceptsEveryChannelInShortOrLongForm) {
  for (const std::string_view channel : {"CHAN2", "CHANNEL3", "chan4", "Channel1"}) {
    session.send("WAV:SOUR " + std::string(channel));
    EXPECT_EQ(session.send("WAVeform:SOURce?"),
              "CHAN" + std::string(channel.substr(channel.size() - 1)))
        << channel;
  }
  session.send("WAV:SOUR CHAN3");
  session.send("WAV:SOUR CHAN");
  EXPECT_EQ(session.send("WAV:SOUR?"), "CHAN1");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(WaveformTest, SourceRejectsAnythingButAChannel) {
  session.send("WAV:SOUR CHAN2");

  session.send("WAV:SOUR CHAN5");
  session.send("WAV:SOUR CHAN0");
  session.send("WAV:SOUR SOUR1");
  session.send("WAV:SOUR 1");
  session.send("WAV:SOUR");

  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.next_error(), kMissingParameterReply);
  EXPECT_EQ(session.send("WAV:SOUR?"), "CHAN2");
}

TEST_F(WaveformTest, FormatAcceptsAsciiAndReal) {
  session.send("WAV:FORM REAL");
  EXPECT_EQ(session.send("WAV:FORM?"), "REAL");
  session.send("WAVeform:FORMat ASCII");
  EXPECT_EQ(session.send("WAV:FORM?"), "ASC");
  session.send("wav:form real");
  session.send("WAV:FORM ASC");
  EXPECT_EQ(session.send("WAV:FORM?"), "ASC");

  session.send("WAV:FORM BYTE");
  session.send("WAV:FORM ASCI");
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.send("WAV:FORM?"), "ASC");
}

// ADR 0002: one Session's WAV:SOUR and WAV:FORM never change what another Session reads.
TEST_F(WaveformTest, SourceAndFormatBelongToEachSession) {
  ScpiSession other{instrument};
  // Time zero on the first sample: Auto captures the record as it fills.
  session.send("CHAN2:DISP ON;:SOUR2:FUNC DC;OFFS 0.5;:ACQ:POIN 2;:TRIG:POS 0");
  instrument.advance(2);

  other.send("WAV:SOUR CHAN2;FORM REAL");

  EXPECT_EQ(session.send("WAV:SOUR?;FORM?"), "CHAN1;ASC");
  EXPECT_EQ(other.send("WAV:SOUR?;FORM?"), "CHAN2;REAL");
  EXPECT_EQ(test::numbers<float>(session.send("WAV:DATA?")), captured(instrument, 1).samples);
  EXPECT_EQ(float32_reply(other, "WAV:DATA?"), (std::vector<float>{0.5F, 0.5F}));
}

TEST_F(WaveformTest, RstLeavesTheSessionsWaveformSettingsAlone) {
  session.send("WAV:SOUR CHAN4;FORM REAL");

  session.send("*RST");

  EXPECT_EQ(session.send("WAV:SOUR?;FORM?"), "CHAN4;REAL");
}

TEST_F(WaveformTest, QueriesBeforeTheFirstCaptureReportStaleData) {
  instrument.advance(kResetRecordLength - 1);

  EXPECT_EQ(session.send("WAV:DATA?"), std::nullopt);
  EXPECT_EQ(session.send("WAV:PRE?"), std::nullopt);

  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(WaveformTest, QueriesAfterRstReportStaleDataUntilTheNextCaptureCompletes) {
  instrument.advance(kResetRecordLength);
  session.send("*RST");
  instrument.advance(kResetRecordLength - 1);

  EXPECT_EQ(session.send("WAV:DATA?"), std::nullopt);
  EXPECT_EQ(session.next_error(), kDataStaleReply);

  instrument.advance(1);
  EXPECT_NE(session.send("WAV:PRE?"), std::nullopt);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(WaveformTest, AnErrorStopsTheRestOfTheLine) {
  EXPECT_EQ(session.send("*IDN?;WAV:DATA?;*IDN?"), session.send("*IDN?"));

  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(WaveformTest, ChannelNotInTheLatestCaptureIsASettingsConflict) {
  instrument.advance(kResetRecordLength);

  session.send("WAV:SOUR CHAN2");
  EXPECT_EQ(session.send("WAV:DATA?"), std::nullopt);
  EXPECT_EQ(session.send("WAV:PRE?"), std::nullopt);

  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
}

TEST_F(WaveformTest, EnablingAChannelDoesNotAddItToTheLatestCapture) {
  instrument.advance(kResetRecordLength);

  session.send("CHAN2:DISP ON;:WAV:SOUR CHAN2");

  EXPECT_EQ(session.send("WAV:DATA?"), std::nullopt);
  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
}

TEST_F(WaveformTest, DisablingAChannelKeepsItInTheLatestCapture) {
  // Time zero on the first sample: the first rising edge, at 4, comes after Auto's capture.
  session.send("ACQ:POIN 4;:SOUR1:FUNC SQU;FREQ 250KHZ;:TRIG:POS 0");
  instrument.advance(4);

  session.send("CHAN1:DISP OFF");

  EXPECT_EQ(session.send("WAV:DATA?"), "1,1,-1,-1");
}

TEST_F(WaveformTest, AsciiDataIsTheCapturedSamplesSeparatedByCommas) {
  // Time zero on the first sample: the first rising edge, at 4, comes after Auto's capture.
  session.send("ACQ:POIN 4;:SOUR1:FUNC SQU;FREQ 250KHZ;AMPL 1;OFFS 0.25;:TRIG:POS 0");
  instrument.advance(4);

  EXPECT_EQ(session.send("WAV:DATA?"), "0.75,0.75,-0.25,-0.25");
}

TEST_F(WaveformTest, AsciiDataOfAOnePointCaptureHasNoComma) {
  session.send("ACQ:POIN 1;:SOUR1:FUNC DC;OFFS -2");
  instrument.advance(1);

  EXPECT_EQ(session.send("WAVeform:DATA?"), "-2");
}

TEST_F(WaveformTest, AsciiDataReadsBackAsExactlyTheCapturedSamples) {
  session.send("SOUR1:NOIS 0.1");
  instrument.advance(2 * kResetRecordLength);  // Noise moves the trigger event off index 5000.

  const std::optional<std::string> reply = session.send("WAV:DATA?");

  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(test::numbers<float>(reply), captured(instrument, 1).samples);
}

TEST_F(WaveformTest, RealDataIsAFloat32BlockOfTheCapturedSamples) {
  session.send("ACQ:POIN 100;:SOUR1:NOIS 0.1;:WAV:FORM REAL");
  // Within 200 samples a capture has completed: triggered by the noise, or Auto's at 150.
  instrument.advance(200);

  const std::string reply = session.send("WAV:DATA?").value_or("");

  EXPECT_THAT(reply, StartsWith("#3400"));
  EXPECT_EQ(decode_float32_block(reply), captured(instrument, 1).samples);
}

TEST_F(WaveformTest, RealDataOfTheLongestRecordFitsOneBlock) {
  // The ramp never reaches the 2 V level, and time zero on the first sample means no pre-trigger
  // samples to wait for: Auto takes the first record as it fills.
  session.send("ACQ:POIN 1048576;:WAV:FORM REAL;:SOUR1:FUNC RAMP;FREQ 1;:TRIG:LEV 2;POS 0");
  instrument.advance(1'048'576);

  const std::string reply = session.send("WAV:DATA?").value_or("");

  EXPECT_THAT(reply, StartsWith("#74194304"));
  EXPECT_EQ(decode_float32_block(reply), captured(instrument, 1).samples);
}

/// The bytes of a `WAV:DATA?` reply in REAL at `points` points: the block header, then a float32
/// a point.
std::size_t real_block_bytes(std::size_t points) {
  const std::size_t data = points * sizeof(float);
  return scpi::definite_length_header(data).size() + data;
}

/// The bytes of `blocks` REAL `WAV:DATA?` replies at `points` points joined with `;`.
std::size_t joined_real_blocks(std::size_t blocks, std::size_t points) {
  return (blocks * real_block_bytes(points)) + blocks - 1;
}

/// A line that sets REAL and then asks `WAV:DATA?` `queries` times.
std::string real_data_queries(std::size_t queries) {
  std::string line = "WAV:FORM REAL;DATA?";
  for (std::size_t query = 1; query < queries; ++query) {
    line += ";DATA?";
  }
  return line;
}

// 157 blocks of 427 444 bytes and their 156 joining ';' make exactly kMaxReplyLength.
TEST_F(WaveformTest, LineOfDataQueriesUpToTheReplyLimitIsReplied) {
  constexpr std::size_t kPoints = 106'859;
  constexpr std::size_t kQueries = 157;
  ASSERT_EQ(joined_real_blocks(kQueries, kPoints), scpi::kMaxReplyLength);
  take_capture(kPoints);

  EXPECT_EQ(session.send(real_data_queries(kQueries)).value_or("").size(), scpi::kMaxReplyLength);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

// 33 blocks of 2 033 601 bytes and their 32 joining ';' are one byte over kMaxReplyLength: the
// blocks before the last are replied.
TEST_F(WaveformTest, LineOfDataQueriesOneByteOverTheReplyLimitIsTooMuchData) {
  constexpr std::size_t kPoints = 508'398;
  constexpr std::size_t kQueries = 33;
  ASSERT_EQ(joined_real_blocks(kQueries, kPoints), scpi::kMaxReplyLength + 1);
  take_capture(kPoints);

  EXPECT_EQ(session.send(real_data_queries(kQueries)).value_or("").size(),
            joined_real_blocks(kQueries - 1, kPoints));
  EXPECT_EQ(session.next_error(), kTooMuchDataReply);
}

// Time zero on the first sample: Auto takes the first 2 samples, and the ramp's trigger event at
// index 2 places the next capture.
TEST_F(WaveformTest, DataReadsTheLatestCaptureWhenTheQueryRuns) {
  session.send("ACQ:POIN 2;:SOUR1:FUNC RAMP;FREQ 250KHZ;:TRIG:POS 0");
  instrument.advance(2);
  EXPECT_EQ(session.send("WAV:DATA?"), "-1,-0.5");

  instrument.advance(2);
  EXPECT_EQ(session.send("WAV:DATA?"), "0,0.5");
}

TEST_F(WaveformTest, PreambleDescribesTheLatestCapture) {
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("WAV:PRE?"), "1E+06,10000,1,0,5000,0");
}

TEST_F(WaveformTest, PreambleReportsTheSettingsTheCaptureWasTakenWith) {
  session.send("CHAN3:DISP ON;SCAL 0.5;OFFS -1.25;:ACQ:SRAT 2E6;POIN 100;:WAV:SOUR CHAN3");
  instrument.advance(150);  // No trigger event: Auto's capture after 50 pre-trigger samples.

  session.send("CHAN3:SCAL 2;OFFS 0;:ACQ:SRAT 5;POIN 7");

  EXPECT_EQ(session.send("WAVeform:PREamble?"), "2E+06,100,0.5,-1.25,50,1");
}

TEST_F(WaveformTest, PreambleAndDataShareOneLine) {
  session.send("ACQ:POIN 2;:SOUR1:FUNC DC;OFFS 1");
  instrument.advance(3);  // One pre-trigger sample, then Auto's record.

  EXPECT_EQ(session.send("WAV:PRE?;DATA?"), "1E+06,2,1,0,1,1;1,1");
}

TEST_F(WaveformTest, DataAndPreambleTakeNoParameters) {
  instrument.advance(kResetRecordLength);

  session.send("WAV:DATA? 1");
  session.send("WAV:PRE? CHAN1");

  EXPECT_EQ(session.next_error(), kParameterNotAllowedReply);
  EXPECT_EQ(session.next_error(), kParameterNotAllowedReply);
  EXPECT_THAT(session.send("WAV:PRE?"), Optional(StartsWith("1E+06,")));
}

}  // namespace
}  // namespace openscope
