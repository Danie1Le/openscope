// Every setting command's setter and query: *RST value, range limits, units, suffixes and the
// errors the command tree reports for it. What the settings do to captures is tested elsewhere.

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "instrument_fixture.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::kDataTypeErrorReply;
using test::kIllegalParameterValueReply;
using test::kMissingParameterReply;
using test::kNoErrorReply;
using test::kParameterNotAllowedReply;
using test::kUndefinedHeaderReply;
using test::ScpiSession;

constexpr auto kOutOfRangeReply = R"(-222,"Data out of range")";

/// One numeric setting: its header, its *RST reply, and values just inside and just outside its
/// range, written as a client would send them and as the query replies.
struct NumericSetting {
  std::string_view header;
  std::string_view reset_reply;
  std::string_view min;
  std::string_view min_reply;
  std::string_view max;
  std::string_view max_reply;
  std::string_view below_min;
  std::string_view above_max;
};

void PrintTo(const NumericSetting& setting, std::ostream* out) { *out << setting.header; }

class NumericSettingTest : public test::InstrumentFixture,
                           public testing::WithParamInterface<NumericSetting> {
 protected:
  static std::string set(std::string_view value) {
    return std::string(GetParam().header) + " " + std::string(value);
  }
  static std::string query() { return std::string(GetParam().header) + "?"; }
};

TEST_P(NumericSettingTest, StartsAtItsResetValue) {
  EXPECT_EQ(session.send(query()), GetParam().reset_reply);
}

TEST_P(NumericSettingTest, AcceptsBothEndsOfItsRange) {
  session.send(set(GetParam().min));
  EXPECT_EQ(session.send(query()), GetParam().min_reply);

  session.send(set(GetParam().max));
  EXPECT_EQ(session.send(query()), GetParam().max_reply);

  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_P(NumericSettingTest, RejectsValuesOutsideItsRangeAndKeepsTheOldValue) {
  session.send(set(GetParam().max));

  session.send(set(GetParam().below_min));
  session.send(set(GetParam().above_max));

  EXPECT_EQ(session.send(query()), GetParam().max_reply);
  EXPECT_EQ(session.next_error(), kOutOfRangeReply);
  EXPECT_EQ(session.next_error(), kOutOfRangeReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_P(NumericSettingTest, RejectsAKeywordAMissingValueAndASecondValue) {
  session.send(set("ON"));
  session.send(std::string(GetParam().header));
  session.send(set(std::string(GetParam().min) + "," + std::string(GetParam().min)));
  session.send(query() + " " + std::string(GetParam().min));

  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.next_error(), kMissingParameterReply);
  EXPECT_EQ(session.next_error(), kParameterNotAllowedReply);
  EXPECT_EQ(session.next_error(), kParameterNotAllowedReply);
  EXPECT_EQ(session.send(query()), GetParam().reset_reply);
}

INSTANTIATE_TEST_SUITE_P(
    Settings, NumericSettingTest,
    testing::Values(
        NumericSetting{"SOUR1:FREQ", "1000", "0.001", "0.001", "1E9", "1E+09", "0.00099",
                       "1.001E9"},
        NumericSetting{"SOUR1:AMPL", "2", "0", "0", "100", "100", "-0.001", "100.001"},
        NumericSetting{"SOUR1:OFFS", "0", "-50", "-50", "50", "50", "-50.001", "50.001"},
        NumericSetting{"SOUR1:NOIS", "0", "0", "0", "50", "50", "-0.001", "50.001"},
        NumericSetting{"CHAN1:SCAL", "1", "0.001", "0.001", "10", "10", "0.00099", "10.001"},
        NumericSetting{"CHAN1:OFFS", "0", "-50", "-50", "50", "50", "-50.001", "50.001"},
        NumericSetting{"ACQ:SRAT", "1E+06", "1", "1", "1E9", "1E+09", "0.999", "1.001E9"},
        NumericSetting{"ACQ:POIN", "10000", "1", "1", "1048576", "1048576", "0.999", "1048576.001"},
        NumericSetting{"TIM:SCAL", "0.001", "1E-9", "1E-09", "100", "100", "0.99E-9", "100.001"},
        NumericSetting{"TRIG:LEV", "0", "-100", "-100", "100", "100", "-100.001", "100.001"},
        NumericSetting{"TRIG:HOLD", "0", "0", "0", "10", "10", "-1E-9", "10.001"},
        NumericSetting{"TRIG:POS", "50", "0", "0", "100", "100", "-0.001", "100.001"}),
    [](const testing::TestParamInfo<NumericSetting>& setting) {
      std::string name;
      for (const char c : setting.param.header) {
        if (c != ':') {
          name += c;
        }
      }
      return name;
    });

using SettingsTest = test::InstrumentFixture;

TEST_F(SettingsTest, LongFormsAreAccepted) {
  session.send("SOURce2:FREQuency 50;AMPLitude 3;OFFSet -1;NOISe 0.5;FUNCtion SQUare");
  session.send(":CHANnel3:SCALe 2;OFFSet 0.5;DISPlay ON");
  session.send(":ACQuire:SRATe 2E6;POINts 500");
  session.send(":TIMebase:SCALe 0.5");
  session.send(":TRIGger:SOURce CHANnel2;LEVel 0.5;SLOPe NEGative;MODE NORMal;HOLDoff 1E-3");
  session.send(":TRIGger:POSition 25");

  EXPECT_EQ(session.send("SOUR2:FREQ?;AMPL?;OFFS?;NOIS?;FUNC?"), "50;3;-1;0.5;SQU");
  EXPECT_EQ(session.send("CHAN3:SCAL?;OFFS?;DISP?"), "2;0.5;1");
  EXPECT_EQ(session.send("ACQ:SRAT?;POIN?"), "2E+06;500");
  EXPECT_EQ(session.send("TIM:SCAL?"), "0.5");
  EXPECT_EQ(session.send("TRIG:SOUR?;LEV?;SLOP?;MODE?;HOLD?;POS?"), "CHAN2;0.5;NEG;NORM;0.001;25");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(SettingsTest, NumbersAcceptTheirUnitWithAMultiplier) {
  session.send("SOUR1:FREQ 2.5KHZ;AMPL 500MV;OFFS -250mV;NOIS 10MV");
  session.send(":CHAN1:SCAL 50MV;OFFS 1.5V");
  session.send(":TIM:SCAL 2US");
  session.send(":TRIG:LEV -250MV;HOLD 20US");

  EXPECT_EQ(session.send("SOUR1:FREQ?;AMPL?;OFFS?;NOIS?"), "2500;0.5;-0.25;0.01");
  EXPECT_EQ(session.send("CHAN1:SCAL?;OFFS?"), "0.05;1.5");
  EXPECT_EQ(session.send("TIM:SCAL?"), "2E-06");
  EXPECT_EQ(session.send("TRIG:LEV?;HOLD?"), "-0.25;2E-05");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(SettingsTest, CountsRatesAndPercentagesTakeNoUnit) {
  session.send("ACQ:SRAT 1KHZ");
  session.send("ACQ:POIN 5V");
  session.send("SOUR1:FREQ 5V");
  session.send("TRIG:POS 5PCT");

  EXPECT_EQ(session.next_error(), R"(-131,"Invalid suffix")");
  EXPECT_EQ(session.next_error(), R"(-131,"Invalid suffix")");
  EXPECT_EQ(session.next_error(), R"(-131,"Invalid suffix")");
  EXPECT_EQ(session.next_error(), R"(-131,"Invalid suffix")");
  EXPECT_EQ(session.send("ACQ:SRAT?;POIN?"), "1E+06;10000");
  EXPECT_EQ(session.send("SOUR1:FREQ?"), "1000");
  EXPECT_EQ(session.send("TRIG:POS?"), "50");
}

TEST_F(SettingsTest, RecordLengthRoundsToTheNearestWholeSample) {
  session.send("ACQ:POIN 100.4");
  EXPECT_EQ(session.send("ACQ:POIN?"), "100");

  session.send("ACQ:POIN 2.5");
  EXPECT_EQ(session.send("ACQ:POIN?"), "3");

  session.send("ACQ:POIN 1048575.6");
  EXPECT_EQ(session.send("ACQ:POIN?"), "1048576");
}

TEST_F(SettingsTest, RecordLengthRangeAppliesToTheValueAsSentBeforeRounding) {
  session.send("ACQ:POIN 0.6");
  session.send("ACQ:POIN 1048576.4");

  EXPECT_EQ(session.next_error(), kOutOfRangeReply);
  EXPECT_EQ(session.next_error(), kOutOfRangeReply);
  EXPECT_EQ(session.send("ACQ:POIN?"), "10000");
}

TEST_F(SettingsTest, EachGeneratorAndChannelHasItsOwnSettings) {
  session.send("SOUR3:AMPL 3;FUNC RAMP");
  session.send(":CHAN2:SCAL 0.5;DISP ON");

  EXPECT_EQ(session.send("SOUR1:AMPL?;:SOUR2:AMPL?;:SOUR3:AMPL?;:SOUR4:AMPL?"), "2;2;3;2");
  EXPECT_EQ(session.send("SOUR1:FUNC?;:SOUR3:FUNC?;:SOUR4:FUNC?"), "SIN;RAMP;SIN");
  EXPECT_EQ(session.send("CHAN1:SCAL?;:CHAN2:SCAL?;:CHAN3:SCAL?;:CHAN4:SCAL?"), "1;0.5;1;1");
  EXPECT_EQ(session.send("CHAN1:DISP?;:CHAN2:DISP?;:CHAN3:DISP?;:CHAN4:DISP?"), "1;1;0;0");
}

TEST_F(SettingsTest, AnOmittedSuffixMeansGeneratorAndChannelOne) {
  session.send("SOUR:FREQ 5;:CHAN:OFFS 2");

  EXPECT_EQ(session.send("SOUR1:FREQ?;:CHAN1:OFFS?"), "5;2");
  EXPECT_EQ(session.send("SOUR:FREQ?;:CHAN:OFFS?"), "5;2");
}

TEST_F(SettingsTest, SuffixOutOfRangeIsReportedAndChangesNothing) {
  for (const char* line : {"SOUR5:FREQ 5", "SOUR0:AMPL 2", "CHAN5:DISP ON", "CHAN0:SCAL 2",
                           "SOUR5:FUNC?", "CHAN5:OFFS?"}) {
    EXPECT_EQ(session.send(line), std::nullopt) << line;
    EXPECT_EQ(session.next_error(), R"(-114,"Header suffix out of range")") << line;
  }
  EXPECT_EQ(session.send("SOUR4:FREQ?;AMPL?;:CHAN4:DISP?;SCAL?"), "1000;2;0;1");
}

TEST_F(SettingsTest, NodesWithoutASuffixRejectOne) {
  session.send("ACQ2:SRAT 5");
  session.send("TIM2:SCAL?");

  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
}

TEST_F(SettingsTest, FunctionAcceptsEachShortAndLongFormAndRepliesTheShortForm) {
  struct Forms {
    std::string_view short_form;
    std::string_view long_form;
  };
  constexpr std::array<Forms, 6> kFunctions{{{.short_form = "SIN", .long_form = "SINUSOID"},
                                             {.short_form = "SQU", .long_form = "SQUARE"},
                                             {.short_form = "TRI", .long_form = "TRIANGLE"},
                                             {.short_form = "RAMP", .long_form = "RAMP"},
                                             {.short_form = "DC", .long_form = "DC"},
                                             {.short_form = "NOIS", .long_form = "NOISE"}}};
  for (const Forms& function : kFunctions) {
    session.send("SOUR2:FUNC " + std::string(function.long_form));
    EXPECT_EQ(session.send("SOUR2:FUNC?"), function.short_form);
    session.send("sour2:func " + std::string(function.short_form));
    EXPECT_EQ(session.send("SOUR2:FUNC?"), function.short_form);
  }
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(SettingsTest, FunctionRejectsOtherKeywordsAndNumbers) {
  session.send("SOUR1:FUNC SQU");

  session.send("SOUR1:FUNC PULSE");
  session.send("SOUR1:FUNC SINU");
  session.send("SOUR1:FUNC 1");

  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.send("SOUR1:FUNC?"), "SQU");
}

TEST_F(SettingsTest, DisplayIsABoolean) {
  session.send("CHAN2:DISP 1");
  EXPECT_EQ(session.send("CHAN2:DISP?"), "1");
  session.send("CHAN2:DISP OFF");
  EXPECT_EQ(session.send("CHAN2:DISP?"), "0");
  session.send("CHAN2:DISP ON");
  EXPECT_EQ(session.send("CHAN2:DISP?"), "1");
  session.send("CHAN2:DISP 0");
  EXPECT_EQ(session.send("CHAN2:DISP?"), "0");

  session.send("CHAN2:DISP MAYBE");
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.send("CHAN2:DISP?"), "0");
}

TEST_F(SettingsTest, TriggerStartsOnChannelOneRisingThroughZeroVoltsInAuto) {
  EXPECT_EQ(session.send("TRIG:SOUR?;SLOP?;MODE?"), "CHAN1;POS;AUTO");
}

TEST_F(SettingsTest, TriggerSourceIsAnyChannelEnabledOrNot) {
  for (const std::string_view channel : {"CHAN2", "CHANNEL3", "chan4", "Channel1"}) {
    session.send("TRIG:SOUR " + std::string(channel));
    EXPECT_EQ(session.send("TRIG:SOUR?"), "CHAN" + std::string(channel.substr(channel.size() - 1)))
        << channel;
  }
  session.send("TRIG:SOUR CHAN3");
  session.send("TRIG:SOUR CHAN");
  EXPECT_EQ(session.send("TRIG:SOUR?"), "CHAN1");
  EXPECT_EQ(session.send("CHAN3:DISP?"), "0");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(SettingsTest, TriggerSourceRejectsAnythingButAChannel) {
  session.send("TRIG:SOUR CHAN2");

  for (const char* line :
       {"TRIG:SOUR CHAN5", "TRIG:SOUR CHAN0", "TRIG:SOUR SOUR1", "TRIG:SOUR EXT"}) {
    session.send(line);
    EXPECT_EQ(session.next_error(), kIllegalParameterValueReply) << line;
  }
  session.send("TRIG:SOUR 1");
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.send("TRIG:SOUR?"), "CHAN2");
}

TEST_F(SettingsTest, TriggerSlopeAndModeAcceptTheirShortAndLongForms) {
  session.send("TRIG:SLOP NEGATIVE;MODE NORMAL");
  EXPECT_EQ(session.send("TRIG:SLOP?;MODE?"), "NEG;NORM");
  session.send("trig:slop pos;mode auto");
  EXPECT_EQ(session.send("TRIG:SLOP?;MODE?"), "POS;AUTO");
  session.send("TRIG:SLOP POSITIVE;MODE NORM");
  EXPECT_EQ(session.send("TRIG:SLOP?;MODE?"), "POS;NORM");
  session.send("TRIG:SLOP NEG");
  EXPECT_EQ(session.send("TRIG:SLOP?"), "NEG");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(SettingsTest, TriggerSlopeAndModeRejectOtherKeywordsAndNumbers) {
  session.send("TRIG:SLOP NEG;MODE NORM");

  session.send("TRIG:SLOP EITHER");
  session.send("TRIG:SLOP 1");
  session.send("TRIG:MODE AUT");
  session.send("TRIG:MODE 0");

  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kDataTypeErrorReply);
  EXPECT_EQ(session.send("TRIG:SLOP?;MODE?"), "NEG;NORM");
}

// Spec #1: Single is a run-control action (`SINGle`), not a trigger mode.
TEST_F(SettingsTest, TriggerModeSingleIsAnIllegalParameterValue) {
  session.send("TRIG:MODE NORM");

  session.send("TRIG:MODE SING");
  session.send("TRIGger:MODE SINGle");

  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.next_error(), kIllegalParameterValueReply);
  EXPECT_EQ(session.send("TRIG:MODE?"), "NORM");
}

TEST_F(SettingsTest, TriggerNodesTakeNoSuffix) {
  session.send("TRIG2:LEV 1");
  session.send("TRIG:MODE2?");

  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.next_error(), kUndefinedHeaderReply);
  EXPECT_EQ(session.send("TRIG:LEV?"), "0");
}

TEST_F(SettingsTest, SettingsAreSharedByEverySession) {
  ScpiSession other{instrument};

  other.send("SOUR2:FREQ 7;:CHAN2:DISP ON;:ACQ:POIN 64;:TIM:SCAL 0.25;:TRIG:SOUR CHAN3;LEV 1");

  EXPECT_EQ(session.send("SOUR2:FREQ?;:CHAN2:DISP?;:ACQ:POIN?;:TIM:SCAL?;:TRIG:SOUR?;LEV?"),
            "7;1;64;0.25;CHAN3;1");
}

TEST_F(SettingsTest, RstRestoresEveryInstrumentWideSetting) {
  session.send("SOUR4:FUNC NOIS;FREQ 5;AMPL 3;OFFS 2;NOIS 1");
  session.send(":CHAN1:DISP OFF;:CHAN4:DISP ON;SCAL 0.1;OFFS -3");
  session.send(":ACQ:SRAT 5000;POIN 77;:TIM:SCAL 3");
  session.send(":TRIG:SOUR CHAN4;LEV 1.5;SLOP NEG;MODE NORM;HOLD 0.5;POS 10");

  session.send("*RST");

  EXPECT_EQ(session.send("SOUR4:FUNC?;FREQ?;AMPL?;OFFS?;NOIS?"), "SIN;1000;2;0;0");
  EXPECT_EQ(session.send("CHAN1:DISP?;:CHAN4:DISP?;SCAL?;OFFS?"), "1;0;1;0");
  EXPECT_EQ(session.send("ACQ:SRAT?;POIN?;:TIM:SCAL?"), "1E+06;10000;0.001");
  EXPECT_EQ(session.send("TRIG:SOUR?;LEV?;SLOP?;MODE?;HOLD?;POS?"), "CHAN1;0;POS;AUTO;0;50");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

}  // namespace
}  // namespace openscope
