#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/sample_clock.hpp"
#include "scpi_session.hpp"

namespace openscope::test {

/// The `*RST` record length, which facade tests advance by to complete one capture.
inline constexpr std::size_t kResetRecordLength = 10'000;

/// Samples until Auto takes an untriggered capture at `*RST` settings of a signal that never
/// triggers: the 5000 pre-trigger samples, then one record length without a trigger event.
inline constexpr std::size_t kResetAutoTimeout = 15'000;

/// Trigger settings under which Auto takes untriggered captures back to back from sample 0: time
/// zero on the first sample and a level no signal reaches. Measurements read the whole capture, so
/// their tests need each capture to complete one record length after the last.
inline constexpr auto kUntriggeredCaptures = "TRIG:POS 0;LEV 100";

/// Makes CH1 a DC signal that never triggers, with time zero on the first sample, so Auto takes
/// an untriggered capture every record length from where a capture starts: the sample indexes of
/// each capture show exactly when it started.
inline constexpr auto kUntriggeredDc = "SOUR1:FUNC DC;:TRIG:POS 0";

/// `instrument`'s latest capture. Fails the test and returns an empty capture when there is none,
/// so a test can read its fields without crashing the test binary.
inline std::shared_ptr<const Capture> latest(const Instrument& instrument) {
  std::shared_ptr<const Capture> capture = instrument.latest_capture();
  if (capture == nullptr) {
    ADD_FAILURE() << "no capture";
    return std::make_shared<const Capture>();
  }
  return capture;
}

/// `channel` (1 to 4) of `instrument`'s latest capture. Fails the test and returns an empty
/// channel when there is no capture or the channel is not in it.
inline CapturedChannel captured(const Instrument& instrument, std::size_t channel) {
  const std::shared_ptr<const Capture> capture = latest(instrument);
  const auto& captured_channel = capture->channels.at(channel - 1);
  if (!captured_channel) {
    ADD_FAILURE() << "CH" << channel << " is not in the capture";
    return {};
  }
  return *captured_channel;
}

/// The first sample index of `instrument`'s latest capture (0 and a test failure when there is
/// none).
inline SampleIndex first_index(const Instrument& instrument) {
  return latest(instrument)->first_index;
}

/// Advances `instrument`'s sample clock by `samples` in calls of at most `step` (at least 1), for
/// tests that check how the clock is stepped never changes a capture or a reply.
inline void advance_in_steps(Instrument& instrument, std::size_t samples, std::size_t step) {
  for (std::size_t done = 0; done < samples; done += step) {
    instrument.advance(std::min(step, samples - done));
  }
}

/// The next `count` sample indexes of `instrument`, acquired as live mode's acquisition thread
/// does. Fails the test when the instrument acquired nothing (it is Stopped).
inline AcquiredBlock acquired(Instrument& instrument, std::size_t count) {
  std::optional<AcquiredBlock> block = instrument.acquire(count);
  if (!block) {
    ADD_FAILURE() << "nothing acquired";
    return {};
  }
  return std::move(*block);
}

/// Acquires `samples` sample indexes in blocks of at most `step` and processes each, as live
/// mode's two threads do, on the calling thread.
inline void acquire_and_process(Instrument& instrument, std::size_t samples, std::size_t step) {
  for (std::size_t done = 0; done < samples; done += step) {
    instrument.process(acquired(instrument, std::min(step, samples - done)));
  }
}

/// An Instrument at its `*RST` settings with one Session on it: the starting point of facade
/// tests.
class InstrumentFixture : public testing::Test {
 protected:
  Instrument instrument;
  ScpiSession session{instrument};
};

/// InstrumentFixture set up with kUntriggeredDc, for tests that check when captures start.
class UntriggeredDcFixture : public InstrumentFixture {
 protected:
  UntriggeredDcFixture() { session.send(kUntriggeredDc); }
};

}  // namespace openscope::test
