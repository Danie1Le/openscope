// SOURce<n>:FAULt through the facade: each fault is found in WAV:DATA? at its sample index and
// shape, defaults, errors, one-shot behaviour, determinism and *RST. The Fault type and its place
// in a Generator's output are tested in fault_test.cpp and generator_test.cpp.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "binary_block.hpp"
#include "instrument_fixture.hpp"
#include "openscope/generator.hpp"
#include "openscope/instrument.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using test::float32_reply;
using test::kDataTypeErrorReply;
using test::kIllegalParameterValueReply;
using test::kInvalidSuffixReply;
using test::kMissingParameterReply;
using test::kNoErrorReply;
using test::kOutOfRangeReply;
using test::kParameterNotAllowedReply;
using test::kSettingsConflictReply;
using test::kSuffixOutOfRangeReply;
using test::kUndefinedHeaderReply;
using test::kUntriggeredCaptures;
using test::ScpiSession;
using testing::Each;
using testing::ElementsAre;
using testing::Eq;

/// The latest capture of `channel` (1 to 4) as `WAV:DATA?` reports it.
std::vector<float> wave(ScpiSession& session, unsigned channel = 1) {
  session.send("WAV:SOUR CHAN" + std::to_string(channel) + ";FORM REAL");
  return float32_reply(session, "WAV:DATA?");
}

/// `count` copies of `value`.
std::vector<float> flat(std::size_t count, float value) { return std::vector<float>(count, value); }

// Every test runs under kUntriggeredCaptures: Auto takes each record as it fills, so these tests do
// not depend on trigger events (#6).
class FaultCommandTest : public test::InstrumentFixture {
 protected:
  void SetUp() override { session.send(kUntriggeredCaptures); }

  /// CH1 is a 0.5 V DC level, captured 8 samples at a time.
  void use_dc_level() { session.send("SOUR1:FUNC DC;OFFS 0.5;:ACQ:POIN 8"); }

  /// CH1 is a 250 Hz square wave of +-1 V sampled at 1 kSa/s, so it runs 1, 1, -1, -1 in a
  /// repeat of 4 samples, captured 12 samples at a time.
  void use_square_wave() { session.send("ACQ:SRAT 1000;POIN 12;:SOUR1:FUNC SQU;FREQ 250;AMPL 2"); }
};

TEST_F(FaultCommandTest, GlitchIsASpikeOfTheGivenSizeAndLengthAtTheNextSampleIndex) {
  use_dc_level();
  instrument.advance(3);

  session.send("SOUR1:FAUL GLIT,1.5,2");
  instrument.advance(5);

  EXPECT_THAT(wave(session), ElementsAre(0.5F, 0.5F, 0.5F, 2.0F, 2.0F, 0.5F, 0.5F, 0.5F));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(FaultCommandTest, GlitchAddsToTheSignalAndCanBeNegative) {
  use_square_wave();
  instrument.advance(4);

  session.send("SOUR1:FAUL GLIT,-3,1");
  instrument.advance(8);

  // Without it: 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1.
  EXPECT_THAT(wave(session), ElementsAre(1.0F, 1.0F, -1.0F, -1.0F, -2.0F, 1.0F, -1.0F, -1.0F, 1.0F,
                                         1.0F, -1.0F, -1.0F));
}

TEST_F(FaultCommandTest, DropoutHoldsTheGivenLevelForTheGivenLength) {
  use_square_wave();
  instrument.advance(2);

  session.send("SOUR1:FAUL DROP,0.25,3");
  instrument.advance(10);

  EXPECT_THAT(wave(session), ElementsAre(1.0F, 1.0F, 0.25F, 0.25F, 0.25F, 1.0F, -1.0F, -1.0F, 1.0F,
                                         1.0F, -1.0F, -1.0F));
}

// +250 Hz for 2 samples from index 4 doubles the frequency of the square wave for those 2 samples
// and leaves it half a cycle late afterwards, with no jump.
TEST_F(FaultCommandTest, FrequencyStepChangesTheFrequencyPhaseContinuously) {
  use_square_wave();
  instrument.advance(4);

  session.send("SOUR1:FAUL FST,250,2");
  instrument.advance(8);

  EXPECT_THAT(wave(session), ElementsAre(1.0F, 1.0F, -1.0F, -1.0F, 1.0F, -1.0F, 1.0F, 1.0F, -1.0F,
                                         -1.0F, 1.0F, 1.0F));
}

TEST_F(FaultCommandTest, FrequencyStepKeepsItsShiftInTheCapturesAfterIt) {
  use_square_wave();
  instrument.advance(4);
  session.send("SOUR1:FAUL FST,250,2");
  instrument.advance(8);

  instrument.advance(12);

  // Phases of indexes 12 to 15 are 0.5, 0.75, 0.0 and 0.25 (half a cycle late): -1, -1, 1, 1.
  EXPECT_THAT(wave(session), ElementsAre(-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, -1.0F,
                                         -1.0F, 1.0F, 1.0F));
}

TEST_F(FaultCommandTest, GlitchAndDropoutHappenOnceAndTheNextCaptureIsClean) {
  use_square_wave();
  session.send("SOUR1:FAUL GLIT,1,1");
  session.send("SOUR1:FAUL DROP,0,1");
  instrument.advance(12);
  EXPECT_THAT(wave(session)[0], Eq(0.0F));

  instrument.advance(12);

  EXPECT_THAT(wave(session), ElementsAre(1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, -1.0F, -1.0F, 1.0F,
                                         1.0F, -1.0F, -1.0F));
}

TEST_F(FaultCommandTest, OmittedMagnitudeAndDurationTakeTheDefaultsOfTheirKind) {
  session.send("ACQ:POIN 400;:SOUR1:FUNC DC;OFFS 0.5");
  instrument.advance(100);
  session.send("SOUR1:FAUL GLIT");
  instrument.advance(100);
  session.send("SOUR1:FAUL DROP");
  instrument.advance(200);

  std::vector<float> expected = flat(400, 0.5F);
  expected[100] = 2.5F;                                             // GLIT: +2 V for 1 sample.
  std::fill(expected.begin() + 200, expected.begin() + 300, 0.0F);  // DROP: 0 V for 100 samples.
  EXPECT_EQ(wave(session), expected);
}

TEST_F(FaultCommandTest, FrequencyStepDefaultsAddOneKilohertzForOneThousandSamples) {
  session.send("ACQ:POIN 3000;:SOUR1:FUNC RAMP;FREQ 1000;AMPL 2");  // 1000 samples per cycle.
  instrument.advance(500);
  session.send("SOUR1:FAUL FST");
  instrument.advance(2500);

  const std::vector<float> samples = wave(session);

  // The ramp is at phase 0.5 (0 V) at index 500 and climbs 0.002 V per sample at 1 kHz. The step
  // doubles that to 0.004 V per sample until index 1500, which is a whole cycle later (0 V again),
  // and the ramp then climbs 0.002 V per sample as before.
  constexpr float kTolerance = 1e-6F;
  EXPECT_NEAR(samples[500], 0.0F, kTolerance);
  EXPECT_NEAR(samples[501], 0.004F, kTolerance);
  EXPECT_NEAR(samples[1500], 0.0F, kTolerance);
  EXPECT_NEAR(samples[1501], 0.002F, kTolerance);
}

TEST_F(FaultCommandTest, KeywordsTakeTheirShortAndLongFormsInAnyCase) {
  use_dc_level();

  for (const char* keyword :
       {"GLIT", "GLITCH", "glit", "Glitch", "DROP", "dropout", "FST", "FSTEP", "fstep"}) {
    session.send(std::string("SOUR1:FAUL ") + keyword);
    EXPECT_EQ(session.next_error(), kNoErrorReply) << keyword;
  }
}

TEST_F(FaultCommandTest, LongHeaderAndSuffixesAreAccepted) {
  session.send("CHAN2:DISP ON;:SOUR2:FUNC DC;OFFS 1;:SOUR1:FUNC DC;OFFS 0.5;:ACQ:POIN 4");

  session.send("SOURCE2:FAULT GLITCH,1,1");
  instrument.advance(4);

  EXPECT_THAT(wave(session, 2), ElementsAre(2.0F, 1.0F, 1.0F, 1.0F));
  EXPECT_THAT(wave(session, 1), Each(Eq(0.5F)));
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(FaultCommandTest, NoSuffixMeansGeneratorOne) {
  use_dc_level();

  session.send("SOUR:FAUL GLIT,1,1");
  instrument.advance(8);

  EXPECT_THAT(wave(session)[0], Eq(1.5F));
}

TEST_F(FaultCommandTest, AFaultOnOneGeneratorLeavesTheOthersAlone) {
  session.send("CHAN2:DISP ON;:CHAN3:DISP ON;:ACQ:POIN 4");
  session.send("SOUR1:FUNC DC;OFFS 0.5;:SOUR2:FUNC DC;OFFS 0.5;:SOUR3:FUNC DC;OFFS 0.5");

  session.send("SOUR2:FAUL DROP,0,4");
  instrument.advance(4);

  EXPECT_THAT(wave(session, 1), Each(Eq(0.5F)));
  EXPECT_THAT(wave(session, 2), Each(Eq(0.0F)));
  EXPECT_THAT(wave(session, 3), Each(Eq(0.5F)));
}

TEST_F(FaultCommandTest, AFaultOnADisabledChannelShowsOnceTheChannelIsEnabled) {
  session.send("SOUR2:FUNC DC;OFFS 1;:ACQ:POIN 4");

  session.send("SOUR2:FAUL DROP,0.25,100");
  instrument.advance(10);
  session.send("CHAN2:DISP ON");
  instrument.advance(4);

  EXPECT_THAT(wave(session, 2), Each(Eq(0.25F)));
}

TEST_F(FaultCommandTest, ASpikeBeyondTheInputRangeIsClippedLikeAnyOtherSample) {
  use_dc_level();

  session.send("SOUR1:FAUL GLIT,100,1");
  instrument.advance(8);

  // CH1's input range is +-4 V at the *RST scale of 1 V per division.
  EXPECT_THAT(wave(session)[0], Eq(4.0F));
}

TEST_F(FaultCommandTest, FaultsDoNotDiscardTheCaptureInProgress) {
  use_dc_level();
  instrument.advance(5);

  session.send("SOUR1:FAUL GLIT,1,1");
  instrument.advance(3);

  ASSERT_NE(instrument.latest_capture(), nullptr);
  EXPECT_EQ(instrument.latest_capture()->first_index, 0U);
  EXPECT_THAT(wave(session)[5], Eq(1.5F));
}

TEST_F(FaultCommandTest, AcceptsTheLimitsOfEachKind) {
  for (const char* command :
       {"GLIT,100,1", "GLIT,-100,1048576", "DROP,100,1", "DROP,-100,1", "FST,1E9,1", "FST,-1E9,1",
        "GLIT,0,1", "FST,0,1", "GLIT,2,1048575.5"}) {
    session.send(std::string("SOUR1:FAUL ") + command);
    EXPECT_EQ(session.next_error(), kNoErrorReply) << command;
  }
}

TEST_F(FaultCommandTest, DurationRoundsToTheNearestWholeSampleHalfAwayFromZero) {
  use_dc_level();

  session.send("SOUR1:FAUL GLIT,1,2.5");
  instrument.advance(8);

  EXPECT_THAT(wave(session), ElementsAre(1.5F, 1.5F, 1.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F));
}

// A rejected command queues its error, injects nothing and discards the rest of its line.
TEST_F(FaultCommandTest, RejectedCommandsInjectNothing) {
  use_dc_level();
  const std::vector<std::pair<std::string, const char*>> rejected{
      {"SOUR1:FAUL GLIT,100.1,1", kOutOfRangeReply},
      {"SOUR1:FAUL GLIT,-100.1,1", kOutOfRangeReply},
      {"SOUR1:FAUL DROP,100.1", kOutOfRangeReply},
      {"SOUR1:FAUL FST,1.1E9", kOutOfRangeReply},
      {"SOUR1:FAUL FST,-1.1E9", kOutOfRangeReply},
      {"SOUR1:FAUL GLIT,1E10", kOutOfRangeReply},
      {"SOUR1:FAUL GLIT,1,0", kOutOfRangeReply},
      {"SOUR1:FAUL GLIT,1,0.4", kOutOfRangeReply},
      {"SOUR1:FAUL GLIT,1,-5", kOutOfRangeReply},
      {"SOUR1:FAUL GLIT,1,1048576.5", kOutOfRangeReply},
      {"SOUR1:FAUL GLIT,1E999", kOutOfRangeReply},
      {"SOUR1:FAUL SPIKE", kIllegalParameterValueReply},
      {"SOUR1:FAUL GLI", kIllegalParameterValueReply},
      {"SOUR1:FAUL GLITCHY", kIllegalParameterValueReply},
      {"SOUR1:FAUL ON", kIllegalParameterValueReply},
      {"SOUR1:FAUL", kMissingParameterReply},
      {"SOUR1:FAUL 5", kDataTypeErrorReply},
      {"SOUR1:FAUL GLIT,ON", kDataTypeErrorReply},
      {"SOUR1:FAUL GLIT,1,DROP", kDataTypeErrorReply},
      {"SOUR1:FAUL GLIT,\"1\"", kDataTypeErrorReply},
      {"SOUR1:FAUL GLIT,1V", kInvalidSuffixReply},
      {"SOUR1:FAUL FST,1KHZ", kInvalidSuffixReply},
      {"SOUR1:FAUL GLIT,1,2,3", kParameterNotAllowedReply},
      {"SOUR5:FAUL GLIT", kSuffixOutOfRangeReply},
      {"SOUR0:FAUL GLIT", kSuffixOutOfRangeReply},
      {"SOUR1:FAUL? GLIT", kUndefinedHeaderReply},
  };

  for (const auto& [command, reply] : rejected) {
    session.send(command);
    EXPECT_EQ(session.next_error(), reply) << command;
    EXPECT_EQ(session.next_error(), kNoErrorReply) << command;
  }
  instrument.advance(8);

  EXPECT_THAT(wave(session), Each(Eq(0.5F)));
}

TEST_F(FaultCommandTest, ARejectedFaultStopsTheRestOfItsLine) {
  use_dc_level();

  session.send("SOUR1:FAUL GLIT,1,1;FAUL GLIT,500,1;FAUL DROP,0,1");
  instrument.advance(8);

  EXPECT_EQ(session.next_error(), kOutOfRangeReply);
  EXPECT_THAT(wave(session), ElementsAre(1.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F));
}

// A Generator keeps at most Generator::kMaxFaults faults in play, so a client cannot grow its
// per-sample work without bound. A fault that has ended no longer counts.
TEST_F(FaultCommandTest, AGeneratorHoldsAtMostMaxFaultsAtOnce) {
  use_dc_level();
  for (std::size_t i = 0; i < Generator::kMaxFaults; ++i) {
    session.send("SOUR1:FAUL GLIT,0.01,8");
    ASSERT_EQ(session.next_error(), kNoErrorReply) << i;
  }

  session.send("SOUR1:FAUL GLIT,0.01,8");
  EXPECT_EQ(session.next_error(), kSettingsConflictReply);
  EXPECT_EQ(session.next_error(), kNoErrorReply);

  instrument.advance(8);  // The faults end at index 8.
  session.send("SOUR1:FAUL GLIT,0.01,8");
  EXPECT_EQ(session.next_error(), kNoErrorReply);
  session.send("SOUR2:FAUL GLIT,0.01,8");  // Each Generator has its own.
  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(FaultCommandTest, SameSeedAndCommandsPutTheFaultAtTheSameIndexWithTheSameShape) {
  constexpr std::uint64_t kSeed = 7;
  const auto run = [](std::uint64_t seed) {
    Instrument device{seed};
    ScpiSession client{device};
    client.send(kUntriggeredCaptures);
    client.send("SOUR1:NOIS 0.1;:ACQ:POIN 1000");
    device.advance(123);
    client.send("SOUR1:FAUL GLIT,3,4");
    device.advance(77);
    client.send("SOUR1:FAUL FST,200,150;:SOUR1:FAUL DROP,0.2,10");
    device.advance(800);
    return wave(client);
  };

  const std::vector<float> first = run(kSeed);

  EXPECT_EQ(run(kSeed), first);
  EXPECT_NE(run(kSeed + 1), first);
  // The 4-sample glitch sits at 123 to 126 on top of a 2 Vpp, 1 kHz sine at 1 MSa/s plus noise.
  for (std::size_t i = 123; i < 127; ++i) {
    EXPECT_GT(first[i], 2.5F) << i;
  }
  EXPECT_LT(first[122], 2.0F);
  EXPECT_LT(first[127], 2.0F);
}

// *RST clears every fault: faults are generator state, and a reset returns generator state to
// what a freshly started instrument has (ADR 0003).
TEST_F(FaultCommandTest, RstClearsFaultsStillInPlay) {
  constexpr std::uint64_t kSeed = 7;
  constexpr auto kSetup = "ACQ:POIN 1000;:SOUR1:NOIS 0.1";

  Instrument faulted{kSeed};
  ScpiSession faulted_client{faulted};
  faulted_client.send(kSetup);
  faulted_client.send(kUntriggeredCaptures);
  faulted.advance(100);
  faulted_client.send(
      "SOUR1:FAUL DROP,0,100000;:SOUR1:FAUL FST,333,100000;:SOUR1:FAUL GLIT,5,100000");
  faulted.advance(50);
  faulted_client.send("*RST");
  faulted_client.send(kSetup);
  faulted_client.send(kUntriggeredCaptures);
  faulted.advance(1000);

  Instrument fresh{kSeed};
  ScpiSession fresh_client{fresh};
  fresh.advance(150);
  fresh_client.send(kSetup);
  fresh_client.send(kUntriggeredCaptures);
  fresh.advance(1000);

  EXPECT_EQ(wave(faulted_client), wave(fresh_client));
}

TEST_F(FaultCommandTest, RstFreesTheFaultSlots) {
  use_dc_level();
  for (std::size_t i = 0; i < Generator::kMaxFaults; ++i) {
    session.send("SOUR1:FAUL GLIT,0.01,8");
  }

  session.send("*RST");
  session.send("SOUR1:FAUL GLIT,0.01,8");

  EXPECT_EQ(session.next_error(), kNoErrorReply);
}

TEST_F(FaultCommandTest, FaultsSurviveSettingChangesThatDiscardTheCaptureInProgress) {
  use_dc_level();
  session.send("SOUR1:FAUL DROP,0,8");

  session.send("CHAN1:SCAL 2");
  instrument.advance(8);

  EXPECT_THAT(wave(session), Each(Eq(0.0F)));
}

}  // namespace
}  // namespace openscope
