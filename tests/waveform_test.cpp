// Waveform transfer: WAV:SOUR and WAV:FORM per Session, WAV:DATA? as ASCII or a float32 block,
// and WAV:PRE?.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "binary_block.hpp"
#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::captured;
using test::decode_float32_block;
using test::kDataTypeErrorReply;
using test::kIllegalParameterValueReply;
using test::kMissingParameterReply;
using test::kNoErrorReply;
using test::kParameterNotAllowedReply;
using test::kResetRecordLength;
using test::ScpiSession;
using testing::Optional;
using testing::StartsWith;

constexpr auto kSettingsConflictReply = R"(-221,"Settings conflict")";
constexpr auto kDataStaleReply = R"(-230,"Data corrupt or stale")";

/// Parses comma-separated floats, as a client reading `WAV:FORM ASC` does.
std::vector<float> parse_csv(std::string_view text) {
  std::vector<float> values;
  for (std::size_t start = 0; start <= text.size();) {
    const std::size_t end = std::min(text.find(',', start), text.size());
    const std::string_view field = text.substr(start, end - start);
    const char* const field_end = field.data() + field.size();
    float value = 0.0F;
    EXPECT_EQ(std::from_chars(field.data(), field_end, value).ptr, field_end)
        << "not a number: " << field;
    values.push_back(value);
    start = end + 1;
  }
  return values;
}

using WaveformTest = test::InstrumentFixture;

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
  session.send("CHAN2:DISP ON;:SOUR2:FUNC DC;OFFS 0.5;:ACQ:POIN 2");
  instrument.advance(2);

  other.send("WAV:SOUR CHAN2;FORM REAL");

  EXPECT_EQ(session.send("WAV:SOUR?;FORM?"), "CHAN1;ASC");
  EXPECT_EQ(other.send("WAV:SOUR?;FORM?"), "CHAN2;REAL");
  EXPECT_EQ(parse_csv(session.send("WAV:DATA?").value_or("")), captured(instrument, 1).samples);
  EXPECT_EQ(decode_float32_block(other.send("WAV:DATA?").value_or("")),
            (std::vector<float>{0.5F, 0.5F}));
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
  session.send("ACQ:POIN 4;:SOUR1:FUNC SQU;FREQ 250KHZ");
  instrument.advance(4);

  session.send("CHAN1:DISP OFF");

  EXPECT_EQ(session.send("WAV:DATA?"), "1,1,-1,-1");
}

TEST_F(WaveformTest, AsciiDataIsTheCapturedSamplesSeparatedByCommas) {
  session.send("ACQ:POIN 4;:SOUR1:FUNC SQU;FREQ 250KHZ;AMPL 1;OFFS 0.25");
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
  instrument.advance(kResetRecordLength);

  const std::optional<std::string> reply = session.send("WAV:DATA?");

  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(parse_csv(reply.value_or("")), captured(instrument, 1).samples);
}

TEST_F(WaveformTest, RealDataIsAFloat32BlockOfTheCapturedSamples) {
  session.send("ACQ:POIN 100;:SOUR1:NOIS 0.1;:WAV:FORM REAL");
  instrument.advance(100);

  const std::string reply = session.send("WAV:DATA?").value_or("");

  EXPECT_THAT(reply, StartsWith("#3400"));
  EXPECT_EQ(decode_float32_block(reply), captured(instrument, 1).samples);
}

TEST_F(WaveformTest, RealDataOfTheLongestRecordFitsOneBlock) {
  session.send("ACQ:POIN 1048576;:WAV:FORM REAL");
  instrument.advance(1'048'576);

  const std::string reply = session.send("WAV:DATA?").value_or("");

  EXPECT_THAT(reply, StartsWith("#74194304"));
  EXPECT_EQ(decode_float32_block(reply), captured(instrument, 1).samples);
}

TEST_F(WaveformTest, DataReadsTheLatestCaptureWhenTheQueryRuns) {
  session.send("ACQ:POIN 2;:SOUR1:FUNC RAMP;FREQ 250KHZ");
  instrument.advance(2);
  EXPECT_EQ(session.send("WAV:DATA?"), "-1,-0.5");

  instrument.advance(2);
  EXPECT_EQ(session.send("WAV:DATA?"), "0,0.5");
}

TEST_F(WaveformTest, PreambleDescribesTheLatestCapture) {
  instrument.advance(kResetRecordLength);

  EXPECT_EQ(session.send("WAV:PRE?"), "1E+06,10000,1,0,0,1");
}

TEST_F(WaveformTest, PreambleReportsTheSettingsTheCaptureWasTakenWith) {
  session.send("CHAN3:DISP ON;SCAL 0.5;OFFS -1.25;:ACQ:SRAT 2E6;POIN 100;:WAV:SOUR CHAN3");
  instrument.advance(100);

  session.send("CHAN3:SCAL 2;OFFS 0;:ACQ:SRAT 5;POIN 7");

  EXPECT_EQ(session.send("WAVeform:PREamble?"), "2E+06,100,0.5,-1.25,0,1");
}

TEST_F(WaveformTest, PreambleAndDataShareOneLine) {
  session.send("ACQ:POIN 2;:SOUR1:FUNC DC;OFFS 1");
  instrument.advance(2);

  EXPECT_EQ(session.send("WAV:PRE?;DATA?"), "1E+06,2,1,0,0,1;1,1");
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
