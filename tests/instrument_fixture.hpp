#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "scpi_session.hpp"

namespace openscope::test {

/// The `*RST` record length, which facade tests advance by to complete one capture.
inline constexpr std::size_t kResetRecordLength = 10'000;

/// `channel` (1 to 4) of `instrument`'s latest capture. Fails the test and returns an empty
/// channel when there is no capture or the channel is not in it.
inline CapturedChannel captured(const Instrument& instrument, std::size_t channel) {
  const std::shared_ptr<const Capture> capture = instrument.latest_capture();
  if (capture == nullptr) {
    ADD_FAILURE() << "no capture";
    return {};
  }
  const auto& captured_channel = capture->channels.at(channel - 1);
  if (!captured_channel) {
    ADD_FAILURE() << "CH" << channel << " is not in the capture";
    return {};
  }
  return *captured_channel;
}

/// An Instrument at its `*RST` settings with one Session on it: the starting point of facade
/// tests.
class InstrumentFixture : public testing::Test {
 protected:
  Instrument instrument;
  ScpiSession session{instrument};
};

}  // namespace openscope::test
