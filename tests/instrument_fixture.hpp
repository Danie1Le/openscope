#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

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

/// An Instrument at its `*RST` settings with one Session on it: the starting point of facade
/// tests.
class InstrumentFixture : public testing::Test {
 protected:
  Instrument instrument;
  ScpiSession session{instrument};
};

}  // namespace openscope::test
