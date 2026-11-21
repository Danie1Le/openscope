// The FFT over SCPI: FFT:SOUR and FFT:WIND per Session, FFT:DATA? as a float32 block of dBV RMS
// levels, and FFT:SPAC?.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "binary_block.hpp"
#include "decibels.hpp"
#include "instrument_fixture.hpp"
#include "openscope/capture.hpp"
#include "openscope/fft.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::dbv;
using test::decode_float32_block;
using test::kDataStaleReply;
using test::kDataTypeErrorReply;
using test::kIllegalParameterValueReply;
using test::kNoErrorReply;
using test::kOneVoltPeakSineDb;
using test::kParameterNotAllowedReply;
using test::kResetRecordLength;
using test::kSettingsConflictReply;
using test::ScpiSession;
using testing::StartsWith;

/// The bin holding the highest level.
std::size_t peak_bin(const std::vector<float>& spectrum) {
  return static_cast<std::size_t>(
      std::distance(spectrum.begin(), std::ranges::max_element(spectrum)));
}

/// Facade tests of the FFT commands, each setting up the capture it reads.
class FftCommandsTest : public test::InstrumentFixture {
 protected:
  static constexpr std::size_t kRecordLength = 1024;

  /// Sets CH1's sine to `frequency`, takes one capture of kRecordLength samples at 1.024 MS/s (so
  /// the bins are exactly 1 kHz apart and a tone at a whole number of kilohertz sits on a bin) and
  /// returns the spectrum of the `FFT:SOUR` channel that `FFT:DATA?` replies under `window`. The
  /// trigger level is out of the sine's reach, so Auto completes the capture one record length in.
  std::vector<float> spectrum_of_sine(std::string_view frequency, std::string_view window) {
    session.send(
        std::format("ACQ:SRAT 1.024E6;POIN {};:TRIG:LEV 2;POS 0;:SOUR1:FREQ {};:FFT:WIND {}",
                    kRecordLength, frequency, window));
    instrument.advance(kRecordLength);
    return decode_float32_block(session.send("FFT:DATA?").value_or(""));
  }
};

TEST_F(FftCommandsTest, NewSessionReadsChannelOneThroughAHannWindow) {
  EXPECT_EQ(session.send("FFT:SOUR?"), "CHAN1");
  EXPECT_EQ(session.send("FFT:WIND?"), "HANN");
}

TEST_F(FftCommandsTest, WindowAcceptsEachChoiceInShortOrLongForm) {
  session.send("FFT:WINDow RECTangular");
  EXPECT_EQ(session.send("FFT:WIND?"), "RECT");
  session.send("fft:wind bhar");
  EXPECT_EQ(session.send("FFT:WIND?"), "BHAR");
  session.send("FFT:WIND HANNING");
  EXPECT_EQ(session.send("FFT:WINDow?"), "HANN");
  session.send("FFT:WIND BLACKMANHARRIS");
  session.send("FFT:WIND 1");
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.send("FFT:WIND?"), "HANN");
}

TEST_F(FftCommandsTest, SourceAcceptsOnlyAChannel) {
  session.send("FFT:SOURce CHANNEL3");
  EXPECT_EQ(session.send("FFT:SOURce?"), "CHAN3");
  session.send("FFT:SOUR CHAN5");
  session.send("FFT:SOUR SOUR1");
  session.send("FFT:SOUR 2");
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.send("FFT:SOUR?"), "CHAN3");
}

// Spec #1 and ADR 0002: the FFT source and window are the calling Session's, like WAV:SOUR.
TEST_F(FftCommandsTest, SourceAndWindowBelongToEachSessionAndSurviveRst) {
  ScpiSession other{instrument};
  other.send("FFT:SOUR CHAN2;WIND RECT;:WAV:SOUR CHAN4");
  session.send("FFT:SOUR CHAN3;WIND BHAR");

  session.send("*RST");

  EXPECT_EQ(session.send("FFT:SOUR?;WIND?"), "CHAN3;BHAR");
  EXPECT_EQ(other.send("FFT:SOUR?;WIND?"), "CHAN2;RECT");
  EXPECT_EQ(other.send("WAV:SOUR?"), "CHAN4");  // The two sources are separate settings.
}

class FftWindowTest : public FftCommandsTest, public testing::WithParamInterface<const char*> {};

TEST_P(FftWindowTest, ABinCentredSineReadsItsRmsLevelInItsBin) {
  const std::vector<float> spectrum = spectrum_of_sine("100KHZ", GetParam());

  ASSERT_EQ(spectrum.size(), (kRecordLength / 2) + 1);
  EXPECT_EQ(peak_bin(spectrum), 100);
  EXPECT_NEAR(spectrum[100], kOneVoltPeakSineDb, 1e-3);
  EXPECT_EQ(session.send("FFT:SPAC?"), "1000");
}

INSTANTIATE_TEST_SUITE_P(Windows, FftWindowTest, testing::Values("RECT", "HANN", "BHAR"));

TEST_F(FftCommandsTest, WindowChoiceChangesLeakageOfAnOffBinSine) {
  // 100.5 kHz sits half-way between bins 100 and 101; bin 121 is 20.5 bins above it.
  const float rectangular = spectrum_of_sine("100.5KHZ", "RECT")[121];
  const float hann = spectrum_of_sine("100.5KHZ", "HANN")[121];
  const float blackman_harris = spectrum_of_sine("100.5KHZ", "BHAR")[121];

  EXPECT_GT(rectangular - kOneVoltPeakSineDb, -40.0);
  EXPECT_LT(hann, rectangular - 40.0);
  EXPECT_LT(blackman_harris - kOneVoltPeakSineDb, -92.0);
}

TEST_F(FftCommandsTest, ReadsTheSourceChannelOnly) {
  session.send("CHAN2:DISP ON;:SOUR2:FREQ 300KHZ;:FFT:SOUR CHAN2");

  const std::vector<float> channel2 = spectrum_of_sine("100KHZ", "RECT");

  EXPECT_EQ(peak_bin(channel2), 300);
  EXPECT_NEAR(channel2[300], kOneVoltPeakSineDb, 1e-3);
  EXPECT_LT(channel2[100], -100.0);
}

TEST_F(FftCommandsTest, TheResetRecordLengthUsesTheFirst8192Samples) {
  instrument.advance(kResetRecordLength);

  const std::string reply = session.send("FFT:DATA?").value_or("");

  EXPECT_THAT(reply, StartsWith("#516388"));  // 4097 bins of 4 bytes.
  EXPECT_EQ(decode_float32_block(reply).size(), 4097);
  EXPECT_EQ(session.send("FFT:SPACing?"), "122.0703125");  // 1 MS/s over 8192 points.
}

// The default Hann window over one sample would be all zero; spectra this short are rectangular.
TEST_F(FftCommandsTest, ARecordLengthOfOneGivesOneDcBin) {
  session.send("ACQ:POIN 1;:SOUR1:FUNC DC;OFFS 0.5");
  instrument.advance(1);

  const std::vector<float> spectrum = decode_float32_block(session.send("FFT:DATA?").value_or(""));

  ASSERT_EQ(spectrum.size(), 1);
  EXPECT_NEAR(spectrum[0], dbv(0.5), 1e-4);
  EXPECT_EQ(session.send("FFT:SPAC?"), "1E+06");
}

TEST_F(FftCommandsTest, TheLongestRecordLengthGivesHalfAMillionBins) {
  session.send(std::format("ACQ:POIN {};:TRIG:LEV 2;POS 0", AcquisitionSettings::kMaxRecordLength));
  instrument.advance(AcquisitionSettings::kMaxRecordLength);

  EXPECT_EQ(decode_float32_block(session.send("FFT:DATA?").value_or("")).size(),
            (AcquisitionSettings::kMaxRecordLength / 2) + 1);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(FftCommandsTest, SilenceReadsTheFloorInEveryBin) {
  session.send("ACQ:POIN 8;:TRIG:POS 0;:SOUR1:FUNC DC");
  instrument.advance(8);

  EXPECT_EQ(decode_float32_block(session.send("FFT:DATA?").value_or("")),
            std::vector<float>(5, kSpectrumFloorDb));
}

TEST_F(FftCommandsTest, SpacingReportsTheSettingsTheCaptureWasTakenWith) {
  session.send("ACQ:SRAT 2E6;POIN 4096;:TRIG:LEV 2;POS 0");
  instrument.advance(4096);

  session.send("ACQ:SRAT 5;POIN 7");

  EXPECT_EQ(session.send("FFT:SPAC?"), "488.28125");
}

TEST_F(FftCommandsTest, QueriesBeforeTheFirstCaptureOrAfterRstReportStaleData) {
  EXPECT_EQ(session.send("FFT:DATA?"), std::nullopt);
  EXPECT_EQ(session.send("FFT:SPAC?"), std::nullopt);
  instrument.advance(kResetRecordLength);
  session.send("*RST");
  EXPECT_EQ(session.send("FFT:DATA?"), std::nullopt);

  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.next_error(), kDataStaleReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(FftCommandsTest, SourceNotInTheLatestCaptureIsASettingsConflict) {
  instrument.advance(kResetRecordLength);

  session.send("FFT:SOUR CHAN2");
  EXPECT_EQ(session.send("FFT:DATA?"), std::nullopt);
  EXPECT_EQ(session.send("FFT:SPAC?"), std::nullopt);

  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
}

TEST_F(FftCommandsTest, DataAndSpacingTakeNoParameters) {
  instrument.advance(kResetRecordLength);

  session.send("FFT:DATA? HANN");
  session.send("FFT:SPAC? 1");

  EXPECT_EQ(session.next_error(), kParameterNotAllowedReply);
  EXPECT_EQ(session.next_error(), kParameterNotAllowedReply);
  EXPECT_EQ(session.send("FFT:SPAC?"), "122.0703125");
}

}  // namespace
}  // namespace openscope
