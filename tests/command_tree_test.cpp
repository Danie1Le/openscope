#include "openscope/scpi/command_tree.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope::scpi {
namespace {

using testing::ElementsAre;

/// One handler call the tree made.
struct Call {
  std::string header;
  Arguments arguments;
};

/// A command tree shaped like the instrument's (suffixed nodes, optional nodes, every parameter
/// kind) whose handlers record each call, so tests see exactly what reached a handler.
class CommandTreeTest : public testing::Test {
 protected:
  CommandTreeTest() {
    record("SOURce<1-4>:FREQuency", {NumericParameter{.min = 1.0, .max = 1e6, .unit = "HZ"}});
    record("CHANnel<1-4>:OFFSet", {NumericParameter{.min = -5.0, .max = 5.0, .unit = "V"}});
    record("ACQuire:POINts", {NumericParameter{.min = 0.0, .max = 1000.0, .unit = ""}});
    record("CHANnel<1-4>:DISPlay", {BooleanParameter{}});
    record("TRIGger:MODE", {KeywordParameter{.choices = {"AUTO", "NORMal"}}});
    record("TRIGger:SOURce", {KeywordParameter{.choices = {"CHANnel<1-4>"}}});
    record("MEASure:FREQuency?", {KeywordParameter{.choices = {"CHANnel<1-4>"}}}, "1000");
    record("OUTPut[:STATe]", {BooleanParameter{}});
    record("CALCulate<1-2>[:MATH<1-3>]:CLEar", {});
    record("TEST:PAIR", {NumericParameter{.min = 0.0, .max = 10.0, .unit = ""},
                         NumericParameter{.min = 0.0, .max = 10.0, .unit = ""}});
    record("*TST?", {}, "0");
    record("TEST:WIDE", {NumericParameter{.min = -1e30, .max = 1e30, .unit = "HZ"}});
    // The spec's fault command: a kind, then an optional magnitude and an optional duration.
    record("SOURce<1-4>:FAULt",
           {KeywordParameter{.choices = {"GLITch", "DROPout", "FSTep"}},
            NumericParameter{.min = 0.0, .max = 10.0, .unit = "V"},
            NumericParameter{.min = 0.0, .max = 1.0, .unit = "S"}},
           std::nullopt, 2);
    // Replies as many bytes as its parameter asks for, as a bulk query such as WAV:DATA? does.
    tree.add({.header = "TEST:BULK?",
              .parameters = {NumericParameter{.min = 0.0, .max = 1e9, .unit = ""}},
              .handler = [](Session&, const Arguments& arguments) -> HandlerResult {
                return std::string(static_cast<std::size_t>(std::get<double>(arguments.values[0])),
                                   'x');
              }});
  }

  /// Adds `header`, whose handler records its call and returns `result`.
  void record(std::string_view header, std::vector<ParameterSpec> parameters,
              const HandlerResult& result = std::nullopt, std::size_t optional_parameters = 0) {
    tree.add({.header = header,
              .parameters = std::move(parameters),
              .optional_parameters = optional_parameters,
              .handler = [this, header, result](Session&, const Arguments& arguments) {
                calls.push_back({.header = std::string(header), .arguments = arguments});
                return result;
              }});
  }

  std::optional<std::string> send(std::string_view line) {
    return tree.execute(session, line).reply();
  }

  int next_error_code() { return session.pop_error().code; }

  /// Sends `line`, expects exactly one handler call without errors, and returns its arguments.
  Arguments single_call(std::string_view line) {
    calls.clear();
    send(line);
    EXPECT_EQ(next_error_code(), 0) << line;
    EXPECT_EQ(calls.size(), 1U) << line;
    return calls.empty() ? Arguments{} : calls.back().arguments;
  }

  /// Sends `line` and expects `code` as its only error, with no handler call.
  void expect_rejected(std::string_view line, int code) {
    calls.clear();
    EXPECT_EQ(send(line), std::nullopt) << line;
    EXPECT_EQ(next_error_code(), code) << line;
    EXPECT_EQ(next_error_code(), 0) << line;
    EXPECT_TRUE(calls.empty()) << line << " reached " << calls.front().header;
  }

  template <typename T>
  T value_of(std::string_view line) {
    const Arguments arguments = single_call(line);
    if (arguments.values.size() != 1 || !std::holds_alternative<T>(arguments.values[0])) {
      ADD_FAILURE() << line << " did not produce one value of the expected kind";
      return T{};
    }
    return std::get<T>(arguments.values[0]);
  }

  CommandTree tree;
  Session session;
  std::vector<Call> calls;
};

TEST_F(CommandTreeTest, NumericParameterAcceptsEveryDecimalForm) {
  const std::vector<std::pair<const char*, double>> cases = {
      {"CHAN:OFFS 2", 2.0},        {"CHAN:OFFS +2.5", 2.5},  {"CHAN:OFFS -2.5", -2.5},
      {"CHAN:OFFS 2.", 2.0},       {"CHAN:OFFS .5", 0.5},    {"CHAN:OFFS -.5", -0.5},
      {"CHAN:OFFS 0002.500", 2.5}, {"CHAN:OFFS 25E-1", 2.5}, {"CHAN:OFFS 0.25e+1", 2.5},
      {"CHAN:OFFS 2.5E0", 2.5},    {"ACQ:POIN 1E3", 1000.0}, {"ACQ:POIN 1e+3", 1000.0},
      {"CHAN:OFFS -0", 0.0},
  };
  for (const auto& [line, expected] : cases) {
    EXPECT_DOUBLE_EQ(value_of<double>(line), expected) << line;
  }
}

TEST_F(CommandTreeTest, NumericSuffixAppliesItsMultiplier) {
  const std::vector<std::pair<const char*, double>> cases = {
      {"TEST:WIDE 1HZ", 1.0},     {"TEST:WIDE 1EXHZ", 1e18},  {"TEST:WIDE 1PEHZ", 1e15},
      {"TEST:WIDE 1THZ", 1e12},   {"TEST:WIDE 1GHZ", 1e9},    {"TEST:WIDE 1MAHZ", 1e6},
      {"TEST:WIDE 1KHZ", 1e3},    {"TEST:WIDE 1UHZ", 1e-6},   {"TEST:WIDE 1NHZ", 1e-9},
      {"TEST:WIDE 1PHZ", 1e-12},  {"TEST:WIDE 1FHZ", 1e-15},  {"TEST:WIDE 1AHZ", 1e-18},
      {"SOUR:FREQ 1kHz", 1e3},    {"SOUR:FREQ 1 kHz", 1e3},   {"SOUR:FREQ 1E3HZ", 1e3},
      {"SOUR:FREQ 1e3 hz", 1e3},  {"SOUR:FREQ 1E9UHZ", 1e3},  {"CHAN:OFFS 1V", 1.0},
      {"CHAN:OFFS 10mV", 0.01},   {"CHAN:OFFS -500MV", -0.5}, {"CHAN:OFFS 250uV", 250e-6},
      {"CHAN:OFFS 1E12 PV", 1.0},
  };
  for (const auto& [line, expected] : cases) {
    EXPECT_EQ(value_of<double>(line), expected) << line;  // Exact: one rounding per value.
  }
}

// IEEE 488.2: suffixes are case-insensitive, so M before HZ is mega, never milli.
TEST_F(CommandTreeTest, MegahertzIsWrittenMhzInAnyCase) {
  for (const char* line : {"SOUR:FREQ 0.5MHZ", "SOUR:FREQ 0.5mhz", "SOUR:FREQ 0.5mHz"}) {
    EXPECT_EQ(value_of<double>(line), 5e5) << line;
  }
  EXPECT_EQ(value_of<double>("CHAN:OFFS 500MV"), 0.5);
}

TEST_F(CommandTreeTest, SuffixThatIsNotTheParametersUnitQueuesInvalidSuffix) {
  for (const char* line :
       {"SOUR:FREQ 1V", "SOUR:FREQ 1XHZ", "SOUR:FREQ 1HZZ", "SOUR:FREQ 1K", "SOUR:FREQ 1E",
        "SOUR:FREQ 1 Z", "ACQ:POIN 1V", "ACQ:POIN 1E3K", "CHAN:OFFS 1MMV"}) {
    expect_rejected(line, kInvalidSuffix.code);
  }
}

TEST_F(CommandTreeTest, NumericParameterAcceptsItsBoundsAndRejectsBeyondThem) {
  EXPECT_EQ(value_of<double>("SOUR:FREQ 1"), 1.0);
  EXPECT_EQ(value_of<double>("SOUR:FREQ 1E6"), 1e6);
  EXPECT_EQ(value_of<double>("SOUR:FREQ 1MHZ"), 1e6);
  EXPECT_EQ(value_of<double>("CHAN:OFFS -5"), -5.0);
  for (const char* line :
       {"SOUR:FREQ 0.999999", "SOUR:FREQ 1000001", "SOUR:FREQ 0", "SOUR:FREQ -1",
        "SOUR:FREQ 1.000001MHZ", "CHAN:OFFS 5.000001", "CHAN:OFFS -5.000001", "SOUR:FREQ 1E999",
        "SOUR:FREQ -1E999", "SOUR:FREQ 1E-999", "CHAN:OFFS 1E-999", "SOUR:FREQ 1EXHZ",
        "SOUR:FREQ "
        "1000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0"}) {
    expect_rejected(line, kDataOutOfRange.code);
  }
}

TEST_F(CommandTreeTest, KeywordWhereANumberBelongsQueuesDataTypeError) {
  expect_rejected("SOUR:FREQ ON", kDataTypeError.code);
  expect_rejected("SOUR:FREQ MAX", kDataTypeError.code);
}

TEST_F(CommandTreeTest, BooleanParameterAcceptsOnOffAndRoundedNumbers) {
  const std::vector<std::pair<const char*, bool>> cases = {
      {"CHAN:DISP ON", true},   {"CHAN:DISP OFF", false},  {"CHAN:DISP on", true},
      {"CHAN:DISP Off", false}, {"CHAN:DISP 1", true},     {"CHAN:DISP 0", false},
      {"CHAN:DISP 0.4", false}, {"CHAN:DISP 0.5", true},   {"CHAN:DISP -1", true},
      {"CHAN:DISP 2", true},    {"CHAN:DISP -0.4", false}, {"CHAN:DISP 1E9", true},
  };
  for (const auto& [line, expected] : cases) {
    EXPECT_EQ(value_of<bool>(line), expected) << line;
  }
}

TEST_F(CommandTreeTest, BooleanParameterRejectsOtherValues) {
  expect_rejected("CHAN:DISP MAYBE", kIllegalParameterValue.code);
  expect_rejected("CHAN:DISP ON1", kIllegalParameterValue.code);
  expect_rejected("CHAN:DISP ONN", kIllegalParameterValue.code);
  expect_rejected("CHAN:DISP 1V", kInvalidSuffix.code);
  expect_rejected("CHAN:DISP 1E999", kDataOutOfRange.code);
}

TEST_F(CommandTreeTest, KeywordParameterAcceptsShortAndLongFormsInAnyCase) {
  const std::vector<std::pair<const char*, std::size_t>> cases = {
      {"TRIG:MODE AUTO", 0}, {"TRIG:MODE auto", 0},   {"TRIG:MODE NORM", 1},
      {"TRIG:MODE norm", 1}, {"TRIG:MODE NORMAL", 1}, {"TRIG:MODE Normal", 1},
  };
  for (const auto& [line, expected] : cases) {
    const auto choice = value_of<Choice>(line);
    EXPECT_EQ(choice.index, expected) << line;
    EXPECT_EQ(choice.suffix, 0U) << line;
  }
}

TEST_F(CommandTreeTest, KeywordParameterRejectsAnythingOutsideItsChoices) {
  for (const char* line :
       {"TRIG:MODE NORMA", "TRIG:MODE NOR", "TRIG:MODE SINGLE", "TRIG:MODE AUTO1"}) {
    expect_rejected(line, kIllegalParameterValue.code);
  }
  expect_rejected("TRIG:MODE 1", kDataTypeError.code);
}

TEST_F(CommandTreeTest, KeywordChoiceCarriesItsSuffixDefaultingToOne) {
  const std::vector<std::pair<const char*, unsigned>> cases = {
      {"TRIG:SOUR CHAN2", 2}, {"TRIG:SOUR channel4", 4}, {"TRIG:SOUR CHAN", 1},
      {"TRIG:SOUR CHAN1", 1}, {"TRIG:SOUR CHAN01", 1},
  };
  for (const auto& [line, expected] : cases) {
    const auto choice = value_of<Choice>(line);
    EXPECT_EQ(choice.index, 0U) << line;
    EXPECT_EQ(choice.suffix, expected) << line;
  }
}

TEST_F(CommandTreeTest, KeywordChoiceSuffixOutsideItsRangeIsIllegal) {
  for (const char* line :
       {"TRIG:SOUR CHAN0", "TRIG:SOUR CHAN5", "TRIG:SOUR CHAN99999999999999999999"}) {
    expect_rejected(line, kIllegalParameterValue.code);
  }
}

TEST_F(CommandTreeTest, HeaderSuffixesReachTheHandlerDefaultingToOne) {
  EXPECT_EQ(single_call("SOUR2:FREQ 1").suffixes, std::vector<unsigned>{2});
  EXPECT_EQ(single_call("SOURCE4:FREQUENCY 1").suffixes, std::vector<unsigned>{4});
  EXPECT_EQ(single_call("SOUR:FREQ 1").suffixes, std::vector<unsigned>{1});
  EXPECT_EQ(single_call("CALC2:MATH3:CLE").suffixes, (std::vector<unsigned>{2, 3}));
  // An omitted optional node still gets its default suffix, so suffix positions are fixed.
  EXPECT_EQ(single_call("CALC2:CLE").suffixes, (std::vector<unsigned>{2, 1}));
}

TEST_F(CommandTreeTest, HeaderSuffixOutsideItsRangeQueuesHeaderSuffixOutOfRange) {
  for (const char* line :
       {"SOUR0:FREQ 1", "SOUR5:FREQ 1", "SOUR99999999999999999999:FREQ 1", "CALC3:CLE",
        "CALC:MATH4:CLE", "CALC0:MATH0:CLE", "source5:frequency 1"}) {
    expect_rejected(line, kHeaderSuffixOutOfRange.code);
  }
}

TEST_F(CommandTreeTest, HeaderSuffixOutOfRangeDiscardsTheRestOfTheLine) {
  send("SOUR2:FREQ 1;:SOUR5:FREQ 1;:SOUR3:FREQ 1");

  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls[0].arguments.suffixes, std::vector<unsigned>{2});
  EXPECT_EQ(next_error_code(), kHeaderSuffixOutOfRange.code);
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(CommandTreeTest, SuffixOnANodeThatTakesNoneIsUndefined) {
  for (const char* line : {"SOUR2:FREQ1 1", "TRIG1:MODE AUTO", "CALC3:CLE1", "SOUR5:BOGUS 1",
                           "SOUR5:FREQ:BOGUS 1", "*TST1?"}) {
    expect_rejected(line, kUndefinedHeader.code);
  }
}

TEST_F(CommandTreeTest, OptionalNodeMayBeOmitted) {
  for (const char* line : {"OUTP ON", "OUTP:STAT ON", "output:state on", ":OUTPut:STATe ON"}) {
    EXPECT_TRUE(value_of<bool>(line)) << line;
  }
  expect_rejected("OUTP:STAT:STAT ON", kUndefinedHeader.code);
  expect_rejected("STAT ON", kUndefinedHeader.code);
}

TEST_F(CommandTreeTest, ParameterCountIsCheckedBeforeValues) {
  EXPECT_EQ(single_call("TEST:PAIR 1,2").values.size(), 2U);
  expect_rejected("TEST:PAIR", kMissingParameter.code);
  expect_rejected("TEST:PAIR 1", kMissingParameter.code);
  expect_rejected("MEAS:FREQ?", kMissingParameter.code);
  expect_rejected("TEST:PAIR 1,2,3", kParameterNotAllowed.code);
  expect_rejected("TEST:PAIR 99,1,2", kParameterNotAllowed.code);
  expect_rejected("*TST? 1", kParameterNotAllowed.code);
}

TEST_F(CommandTreeTest, OptionalTrailingParametersMayBeLeftOut) {
  EXPECT_EQ(single_call("SOUR2:FAUL GLIT").values.size(), 1U);
  EXPECT_EQ(single_call("SOUR2:FAUL DROP,2.5").values.size(), 2U);
  const Arguments all = single_call("SOUR2:FAUL FST,2.5,100MS");
  ASSERT_EQ(all.values.size(), 3U);
  EXPECT_EQ(std::get<Choice>(all.values[0]).index, 2U);
  EXPECT_EQ(std::get<double>(all.values[1]), 2.5);
  EXPECT_EQ(std::get<double>(all.values[2]), 0.1);
}

TEST_F(CommandTreeTest, OptionalParametersAreCountedAndValidatedWhenSent) {
  expect_rejected("SOUR:FAUL", kMissingParameter.code);
  expect_rejected("SOUR:FAUL GLIT,1,0.1,1", kParameterNotAllowed.code);
  expect_rejected("SOUR:FAUL GLIT,11", kDataOutOfRange.code);
  expect_rejected("SOUR:FAUL GLIT,1,2", kDataOutOfRange.code);
  expect_rejected("SOUR:FAUL GLIT,1,1V", kInvalidSuffix.code);
  expect_rejected("SOUR:FAUL BOGUS", kIllegalParameterValue.code);
}

TEST_F(CommandTreeTest, QueryMayTakeParameters) {
  EXPECT_EQ(send("MEAS:FREQ? CHAN2"), "1000");
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(std::get<Choice>(calls[0].arguments.values[0]).suffix, 2U);
}

TEST_F(CommandTreeTest, WhitespaceMaySurroundParametersAndSeparators) {
  for (const char* line : {"TEST:PAIR 1,2", "TEST:PAIR\t1,2", "TEST:PAIR   1 , 2  ",
                           "TEST:PAIR 1,\t2", " \tTEST:PAIR 1,2"}) {
    EXPECT_EQ(single_call(line).values.size(), 2U) << line;
  }
}

// IEEE 488.2 7.4.1.2: white space is any byte from 0x00 to 0x20 except newline.
TEST_F(CommandTreeTest, EveryIeee4882WhiteSpaceByteSeparatesTokens) {
  for (const char byte : {'\0', '\x01', '\t', '\v', '\f', '\r', '\x1F', ' '}) {
    std::string line = "_TEST:PAIR_1_,_2_;_*TST?_";
    std::ranges::replace(line, '_', byte);
    calls.clear();
    EXPECT_EQ(send(line), "0") << static_cast<int>(byte);
    EXPECT_EQ(calls.size(), 2U) << static_cast<int>(byte);
    EXPECT_EQ(next_error_code(), 0) << static_cast<int>(byte);
  }
  EXPECT_EQ(value_of<double>("SOUR:FREQ 1\rKHZ"), 1e3);
}

// Strings, non-decimal numbers and blocks are well-formed IEEE 488.2 data that no command takes.
TEST_F(CommandTreeTest, StringNonDecimalAndBlockDataQueueDataTypeError) {
  for (const char* data : {"'a'", "\"a\"", "''", "\"\"", "'it''s'", R"("say ""hi""")", "'a;b,c'",
                           "#H1F", "#hff", "#B101", "#b0", "#Q17", "#q0", "#15hello", "#13a;b",
                           "#210abcdefghij", "#0", "#0anything;at all,"}) {
    for (const char* header : {"SOUR:FREQ ", "CHAN:DISP ", "TRIG:MODE "}) {
      expect_rejected(std::string(header) + data, kDataTypeError.code);
    }
  }
  expect_rejected("TEST:PAIR 1,'2'", kDataTypeError.code);
  expect_rejected("TEST:PAIR '1' , 2", kDataTypeError.code);
  expect_rejected(std::string("SOUR:FREQ #12\0\n", 15), kDataTypeError.code);  // Any bytes.
}

TEST_F(CommandTreeTest, DataTypeErrorRunsTheCommandsBeforeIt) {
  send("TRIG:MODE AUTO;:SOUR:FREQ \"1\";:CHAN:DISP ON");

  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls[0].header, "TRIGger:MODE");
  EXPECT_EQ(next_error_code(), kDataTypeError.code);
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(CommandTreeTest, MalformedLinesAreSyntaxErrors) {
  for (const char* line : {"SOUR:FREQ,1",
                           "SOUR:FREQ 1,",
                           "SOUR:FREQ 1,,2",
                           "SOUR:FREQ ,1",
                           "SOUR::FREQ 1",
                           "SOUR:FREQ: 1",
                           ":",
                           "?",
                           "*",
                           "*?",
                           "SOUR:FREQ 1.2.3",
                           "SOUR:FREQ 1x2",
                           "SOUR:FREQ ++1",
                           "SOUR:FREQ +",
                           "SOUR:FREQ .",
                           "SOUR:FREQ 1E+",
                           "SOUR:FREQ 'a",
                           "SOUR:FREQ \"a",
                           "SOUR:FREQ 'a''",
                           "SOUR:FREQ 'a'b",
                           "SOUR:FREQ #",
                           "SOUR:FREQ #H",
                           "SOUR:FREQ #HG",
                           "SOUR:FREQ #H1F HZ",
                           "SOUR:FREQ #B2",
                           "SOUR:FREQ #Q8",
                           "SOUR:FREQ #A1",
                           "SOUR:FREQ #1",
                           "SOUR:FREQ #15abc",
                           "SOUR:FREQ #2x1abc",
                           "SOUR:FREQ #13abcd",
                           "SOUR:FREQ #9999999999",
                           "TRIG:MODE AUTO;",
                           ";TRIG:MODE AUTO",
                           "TRIG:MODE AUTO;;TRIG:MODE AUTO",
                           ";",
                           "TEST:PAIR 1 2",
                           "SOUR :FREQ 1",
                           "SOUR: FREQ 1",
                           "SOUR:FREQ?x",
                           "SOUR-FREQ 1",
                           ":*TST?",
                           "*TST?:A",
                           "1SOUR:FREQ 1",
                           "_SOUR:FREQ 1",
                           "CHAN:DISP O N",
                           "SOUR:FREQ 1\n",
                           "SOUR:FREQ 1KHZ2",
                           "SOUR:FREQ 1;;",
                           "SOUR:FREQ \xC3\xA9"}) {
    expect_rejected(line, kSyntaxError.code);
  }
}

TEST_F(CommandTreeTest, SyntaxErrorAnywhereRunsNothingOnTheLine) {
  expect_rejected("TRIG:MODE AUTO;CHAN:DISP ON;SOUR:FREQ 1,", kSyntaxError.code);
}

TEST_F(CommandTreeTest, CommandsOnALineRunInOrder) {
  send("TRIG:MODE AUTO;:CHAN2:DISP OFF;*TST?;:SOUR3:FREQ 1KHZ");

  ASSERT_EQ(calls.size(), 4U);
  EXPECT_EQ(calls[0].header, "TRIGger:MODE");
  EXPECT_EQ(calls[1].header, "CHANnel<1-4>:DISPlay");
  EXPECT_EQ(calls[2].header, "*TST?");
  EXPECT_EQ(calls[3].header, "SOURce<1-4>:FREQuency");
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(CommandTreeTest, ErrorDiscardsTheRestOfTheLineButNotWhatAlreadyRan) {
  for (const auto& [line, code] :
       {std::pair{"TRIG:MODE AUTO;:SOUR:FREQ 0;:CHAN:DISP ON", kDataOutOfRange.code},
        std::pair{"TRIG:MODE AUTO;:BOGUS;:CHAN:DISP ON", kUndefinedHeader.code},
        std::pair{"TRIG:MODE AUTO;:TEST:PAIR 1;:CHAN:DISP ON", kMissingParameter.code}}) {
    calls.clear();
    send(line);
    ASSERT_EQ(calls.size(), 1U) << line;
    EXPECT_EQ(calls[0].header, "TRIGger:MODE") << line;
    EXPECT_EQ(next_error_code(), code) << line;
    EXPECT_EQ(next_error_code(), 0) << line;
  }
}

TEST_F(CommandTreeTest, ExecutionErrorFromAHandlerDiscardsTheRestOfTheLine) {
  const ScpiError conflict{.code = -221, .message = "Settings conflict"};
  tree.add({.header = "TEST:FAIL",
            .parameters = {},
            .handler = [this, conflict](Session&, const Arguments& arguments) -> HandlerResult {
              calls.push_back({.header = "TEST:FAIL", .arguments = arguments});
              return conflict;
            }});

  EXPECT_EQ(send("*TST?;:TEST:FAIL;*TST?;:CHAN:DISP ON"), "0");

  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[1].header, "TEST:FAIL");
  const ScpiError queued = session.pop_error();
  EXPECT_EQ(queued.code, conflict.code);
  EXPECT_EQ(queued.message, conflict.message);
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(CommandTreeTest, RelativeHeadersContinueFromThePreviousPath) {
  send("TRIG:MODE AUTO;SOUR CHAN2;MODE NORM");

  ASSERT_EQ(calls.size(), 3U);
  EXPECT_EQ(calls[1].header, "TRIGger:SOURce");
  EXPECT_EQ(calls[2].header, "TRIGger:MODE");
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(CommandTreeTest, RelativeHeaderKeepsThePathsSuffixes) {
  EXPECT_EQ(send("MEAS:FREQ? CHAN1;FREQ? CHAN3"), "1000;1000");

  calls.clear();
  send("SOUR2:FREQ 1;FREQ 2");

  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[1].arguments.suffixes, std::vector<unsigned>{2});
}

TEST_F(CommandTreeTest, RelativeHeaderIsNotRetriedFromTheRoot) {
  calls.clear();
  send("TRIG:MODE AUTO;TRIG:SOUR CHAN1");  // Means TRIG:TRIG:SOUR.

  EXPECT_EQ(calls.size(), 1U);
  EXPECT_EQ(next_error_code(), kUndefinedHeader.code);
}

TEST_F(CommandTreeTest, CommonCommandsLeaveThePathAlone) {
  calls.clear();
  send("TRIG:MODE AUTO;*TST?;SOUR CHAN2");

  ASSERT_EQ(calls.size(), 3U);
  EXPECT_EQ(calls[2].header, "TRIGger:SOURce");
}

TEST_F(CommandTreeTest, RepliesOfSeveralQueriesAreJoinedWithSemicolons) {
  EXPECT_EQ(send("*TST?;TRIG:MODE AUTO;*TST?"), "0;0");
  EXPECT_EQ(send("TRIG:MODE AUTO"), std::nullopt);
}

TEST_F(CommandTreeTest, RepliesBeforeAnErrorAreKept) {
  EXPECT_EQ(send("*TST?;BOGUS;*TST?"), "0");
  EXPECT_EQ(next_error_code(), kUndefinedHeader.code);
}

/// `:TEST:BULK? <bytes>`, for the reply-limit tests: absolute, so it may follow any command.
std::string bulk(std::size_t bytes) { return ":TEST:BULK? " + std::to_string(bytes); }

// The Python client reads at most this much (transport.MAX_RESPONSE_BYTES): the two must agree.
TEST(ReplyLimitTest, IsSixtyFourMebibytes) { EXPECT_EQ(kMaxReplyLength, 64U * 1024U * 1024U); }

TEST_F(CommandTreeTest, RepliesUpToTheLimitAreReplied) {
  const std::size_t first = kMaxReplyLength / 2;
  const std::size_t second = kMaxReplyLength - first - 1;  // The joining ';' takes the last byte.

  const std::optional<std::string> reply = send(bulk(first) + ";" + bulk(second));

  EXPECT_EQ(reply.value_or("").size(), kMaxReplyLength);
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(CommandTreeTest, ReplyOneByteOverTheLimitQueuesTooMuchDataAndDiscardsTheRestOfTheLine) {
  const std::size_t first = kMaxReplyLength / 2;
  const std::size_t second = kMaxReplyLength - first;

  const std::optional<std::string> reply =
      send(bulk(first) + ";" + bulk(second) + ";*TST?;:TRIG:MODE AUTO");

  EXPECT_EQ(reply.value_or("").size(), first);  // The replies that fit are kept.
  EXPECT_EQ(next_error_code(), kTooMuchData.code);
  EXPECT_EQ(next_error_code(), 0);
  EXPECT_TRUE(calls.empty());  // Neither *TST? nor TRIG:MODE ran.
}

TEST_F(CommandTreeTest, SingleReplyOverTheLimitIsNotReplied) {
  EXPECT_EQ(send(bulk(kMaxReplyLength + 1)), std::nullopt);
  EXPECT_EQ(next_error_code(), kTooMuchData.code);

  EXPECT_EQ(send(bulk(kMaxReplyLength)).value_or("").size(), kMaxReplyLength);
  EXPECT_EQ(next_error_code(), 0);
}

// Its joining ';' alone would pass the limit.
TEST_F(CommandTreeTest, EmptyReplyAfterAFullOneIsTooMuchData) {
  EXPECT_EQ(send(bulk(kMaxReplyLength) + ";" + bulk(0)).value_or("").size(), kMaxReplyLength);
  EXPECT_EQ(next_error_code(), kTooMuchData.code);
}

TEST_F(CommandTreeTest, EmptyOrBlankLineDoesNothing) {
  for (const char* line : {"", " ", "\t \t"}) {
    calls.clear();
    EXPECT_EQ(send(line), std::nullopt);
    EXPECT_TRUE(calls.empty());
    EXPECT_EQ(next_error_code(), 0);
  }
}

/// A tree with `*HOLD?` and `TRIGger:HOLD`, which hold their line the way `*OPC?` and `*WAI` do
/// while an operation is pending. `*HOLD?` replies `done` once its line is resumed.
class HeldLineTest : public CommandTreeTest {
 protected:
  HeldLineTest() {
    record("*HOLD?", {}, Hold{.reply = "done"});
    record("TRIGger:HOLD", {}, Hold{});
  }

  /// The headers of the handler calls so far.
  [[nodiscard]] std::vector<std::string> headers() const {
    std::vector<std::string> called;
    called.reserve(calls.size());
    for (const Call& call : calls) {
      called.push_back(call.header);
    }
    return called;
  }
};

// A copy of a held line could be resumed twice and run its rest twice.
static_assert(!std::is_copy_constructible_v<Execution> && !std::is_copy_assignable_v<Execution>);
static_assert(std::is_nothrow_move_constructible_v<Execution>);

TEST_F(HeldLineTest, HoldStopsTheLineAfterTheCommandThatHeldIt) {
  const Execution execution = tree.execute(session, "*TST?;*HOLD?;*TST?;:TRIG:MODE AUTO");

  EXPECT_TRUE(execution.held());
  EXPECT_EQ(execution.reply(), "0");
  EXPECT_THAT(headers(), ElementsAre("*TST?", "*HOLD?"));
  EXPECT_EQ(next_error_code(), 0);
}

// The holding command is not run again: what it waited for is over, even if it has begun again.
TEST_F(HeldLineTest, ResumeAddsTheHoldReplyThenRunsTheRestOfTheLine) {
  Execution execution = tree.execute(session, "*TST?;*HOLD?;*TST?;:TRIG:MODE AUTO");

  tree.resume(session, execution);

  EXPECT_FALSE(execution.held());
  EXPECT_EQ(execution.reply(), "0;done;0");
  EXPECT_THAT(headers(), ElementsAre("*TST?", "*HOLD?", "*TST?", "TRIGger:MODE"));
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(HeldLineTest, LineMayHoldAgainWhenResumed) {
  Execution execution = tree.execute(session, "*HOLD?;*HOLD?;*TST?");

  tree.resume(session, execution);
  EXPECT_TRUE(execution.held());
  EXPECT_EQ(execution.reply(), "done");

  tree.resume(session, execution);
  EXPECT_FALSE(execution.held());
  EXPECT_EQ(execution.reply(), "done;done;0");
  EXPECT_THAT(headers(), ElementsAre("*HOLD?", "*HOLD?", "*TST?"));
}

TEST_F(HeldLineTest, ResumedLineKeepsItsPath) {
  Execution execution = tree.execute(session, "TRIG:MODE AUTO;HOLD;SOUR CHAN2");

  tree.resume(session, execution);

  EXPECT_THAT(headers(), ElementsAre("TRIGger:MODE", "TRIGger:HOLD", "TRIGger:SOURce"));
  EXPECT_EQ(execution.reply(), std::nullopt);
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(HeldLineTest, ErrorAfterTheResumeDiscardsTheRestOfTheLine) {
  Execution execution = tree.execute(session, "*TST?;*HOLD?;BOGUS;*TST?");

  tree.resume(session, execution);

  EXPECT_FALSE(execution.held());
  EXPECT_EQ(execution.reply(), "0;done");
  EXPECT_EQ(next_error_code(), kUndefinedHeader.code);
  EXPECT_EQ(next_error_code(), 0);
}

TEST_F(HeldLineTest, HoldReplyOverTheLimitQueuesTooMuchData) {
  // `done` and its ';' take five bytes: one more than is left.
  Execution execution = tree.execute(session, bulk(kMaxReplyLength - 4) + ";*HOLD?;*TST?");

  tree.resume(session, execution);

  EXPECT_FALSE(execution.held());
  EXPECT_EQ(execution.reply().value_or("").size(), kMaxReplyLength - 4);
  EXPECT_THAT(headers(), ElementsAre("*HOLD?"));  // *TST? never ran.
  EXPECT_EQ(next_error_code(), kTooMuchData.code);
  EXPECT_EQ(next_error_code(), 0);
}

std::optional<std::string> no_reply(Session& /*session*/, const Arguments& /*arguments*/) {
  return std::nullopt;
}

/// Whether `tree.add(command)` throws std::invalid_argument.
bool add_rejected(CommandTree& tree, Command command) {
  try {
    tree.add(std::move(command));
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

/// Whether adding `header` with `parameters` throws std::invalid_argument.
bool rejected(std::string_view header, std::vector<ParameterSpec> parameters = {},
              Handler handler = no_reply, std::size_t optional_parameters = 0) {
  CommandTree tree;
  return add_rejected(tree, {.header = header,
                             .parameters = std::move(parameters),
                             .optional_parameters = optional_parameters,
                             .handler = std::move(handler)});
}

TEST(CommandTreePatternTest, MalformedPatternsAreRejectedWhenAdded) {
  for (const char* header : {"",
                             "?",
                             "*",
                             "*?",
                             "*Idn?",
                             "*IDN<1-4>",
                             "sYSTem",
                             "SYSTem1",
                             "SYST_ERR",
                             "SyST",
                             "SYST<",
                             "SYST<1-",
                             "SYST<1->",
                             "SYST<1-0>",
                             "SYST<2-4>",
                             "SYST<1-x>",
                             "SYST<1-4>x",
                             "SYST<1-4",
                             "SYST:",
                             ":",
                             "SYST::ERR",
                             "SYST[:ERR",
                             "SYST[ERR]",
                             "SYST]ERR",
                             "SYST:[:ERR]",
                             "1ABC",
                             "SYST:ERR?:X",
                             "SYST<1-99999999999>"}) {
    EXPECT_TRUE(rejected(header)) << header;
  }
  EXPECT_TRUE(rejected("TRIG:MODE", {KeywordParameter{.choices = {"normal"}}}));
  EXPECT_TRUE(rejected("TRIG:MODE", {}, nullptr));
}

TEST(CommandTreePatternTest, WellFormedPatternsAreAccepted) {
  for (const char* header : {"*IDN?", "*RST", "SYSTem:ERRor[:NEXT]?", ":SYSTem:ERRor?",
                             "[:SENSe]:FREQuency", "CHANnel<1-4>:SCALe", "NEXT"}) {
    EXPECT_FALSE(rejected(header)) << header;
  }
  EXPECT_FALSE(rejected("TRIG:SOUR", {KeywordParameter{.choices = {"CHANnel<1-4>", "EXTernal"}}}));
}

TEST(CommandTreePatternTest, OptionalParameterCountIsCheckedWhenAdded) {
  const std::vector<ParameterSpec> pair = {BooleanParameter{}, BooleanParameter{}};
  for (const std::size_t optional : {0U, 1U, 2U}) {
    EXPECT_FALSE(rejected("PAIR", pair, no_reply, optional)) << optional;
  }
  EXPECT_TRUE(rejected("PAIR", pair, no_reply, 3));
  EXPECT_TRUE(rejected("NONE", {}, no_reply, 1));
}

TEST(CommandTreePatternTest, MalformedNumericParametersAreRejectedWhenAdded) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  // A unit is upper-case letters only: `Hz` or `mV` would make every suffixed value -131.
  for (const char* unit : {"Hz", "hz", "mV", "V1", "H Z", "_V", "\xC3\x89"}) {
    EXPECT_TRUE(rejected("FREQ", {NumericParameter{.min = 0.0, .max = 1.0, .unit = unit}})) << unit;
  }
  for (const auto& [min, max] : {std::pair{1.0, 0.0}, std::pair{kNan, 1.0}, std::pair{0.0, kNan},
                                 std::pair{-kInf, 0.0}, std::pair{0.0, kInf}}) {
    EXPECT_TRUE(rejected("FREQ", {NumericParameter{.min = min, .max = max, .unit = "HZ"}}))
        << min << ".." << max;
  }
  // Every parameter is checked, not only the first.
  EXPECT_TRUE(rejected("PAIR", {NumericParameter{.min = 0.0, .max = 1.0, .unit = ""},
                                NumericParameter{.min = 0.0, .max = 1.0, .unit = "v"}}));
}

TEST(CommandTreePatternTest, WellFormedNumericParametersAreAccepted) {
  for (const char* unit : {"", "V", "HZ", "S", "PCT"}) {
    EXPECT_FALSE(rejected("FREQ", {NumericParameter{.min = 0.0, .max = 1.0, .unit = unit}}))
        << unit;
  }
  EXPECT_FALSE(rejected("FREQ", {NumericParameter{.min = 2.0, .max = 2.0, .unit = ""}}));
  EXPECT_FALSE(rejected("FREQ", {NumericParameter{.min = -1e300, .max = 1e300, .unit = ""}}));
}

/// Whether adding `header` to a tree that already holds `existing` throws std::invalid_argument.
bool overlap_rejected(std::string_view existing, std::string_view header) {
  CommandTree tree;
  tree.add({.header = existing, .parameters = {}, .handler = no_reply});
  return add_rejected(tree, {.header = header, .parameters = {}, .handler = no_reply});
}

// The first matching command always runs, so a header that another one shadows could never run.
TEST(CommandTreePatternTest, HeadersThatCanMatchTheSameInputAreRejected) {
  for (const auto& [existing, header] : {
           std::pair{"SYSTem:ERRor?", "SYSTem:ERRor?"},
           std::pair{"SYSTem:ERRor?", "syst:err?"},
           std::pair{"*RST", "*RST"},
           std::pair{"MEASure:FREQuency?", "MEASure[:SCALar]:FREQuency?"},
           std::pair{"MEASure[:SCALar]:FREQuency?", "MEASure:FREQuency?"},
           std::pair{"MEASure[:SCALar]:FREQuency?", "MEASure:SCALar:FREQuency?"},
           std::pair{"[:SENSe]:FREQuency", "FREQuency"},
           std::pair{"CHANnel<1-4>:SCALe", "CHANnel<1-2>:SCALe"},
           std::pair{"CHANnel<1-4>:SCALe", "CHAN:SCAL"},
           std::pair{"FREQuency:VOLT", "FREQ:VOLTage"},
           std::pair{"FREQ:VOLTage", "FREQuency:VOLT"},
           std::pair{"SOURce[:FUNCtion]", "SOURce[:SHAPe]"},
       }) {
    EXPECT_TRUE(overlap_rejected(existing, header)) << existing << " then " << header;
  }
}

TEST(CommandTreePatternTest, HeadersThatNeverMatchTheSameInputAreAccepted) {
  for (const auto& [existing, header] : {
           std::pair{"SYSTem:ERRor", "SYSTem:ERRor?"},
           std::pair{"SYSTem:ERRor?", "SYSTem:ERRor:COUNt?"},
           std::pair{"*RST", "RST"},
           std::pair{"*RST", "RSTe"},
           std::pair{"CHANnel<1-4>:SCALe", "CHANnel<1-4>:OFFSet"},
           std::pair{"FREQuency", "FRE"},
           std::pair{"SOURce:FUNCtion", "SOURce[:FUNCtion]:SHAPe"},
       }) {
    EXPECT_FALSE(overlap_rejected(existing, header)) << existing << " then " << header;
  }
}

// The tree keeps its own copy of the unit, so a unit built at run time may go away after add().
TEST_F(CommandTreeTest, NumericUnitIsCopiedWhenAdded) {
  {
    std::string unit = "V";
    record("LEVel", {NumericParameter{.min = 0.0, .max = 10.0, .unit = unit}});
    unit = "Q";  // The caller changes its text after add(); the tree must keep "V".
  }

  EXPECT_EQ(value_of<double>("LEV 1V"), 1.0);
  expect_rejected("LEV 1Q", kInvalidSuffix.code);
}

TEST(ShortFormTest, IsTheLeadingUpperCaseLetters) {
  EXPECT_EQ(short_form("SINusoid"), "SIN");
  EXPECT_EQ(short_form("CHANnel<1-4>"), "CHAN");
  EXPECT_EQ(short_form("RAMP"), "RAMP");
  EXPECT_EQ(short_form("DC"), "DC");
  EXPECT_EQ(short_form("lower"), "");
  EXPECT_EQ(short_form(""), "");
}

}  // namespace
}  // namespace openscope::scpi
