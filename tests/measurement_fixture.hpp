#pragma once

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "instrument_fixture.hpp"

namespace openscope::test {

/// The number a measurement query replied, as a client parses it. Fails the test and returns NaN
/// when the reply is missing or not one number.
inline double number(const std::optional<std::string>& reply) {
  const std::string text = reply.value_or("");
  double value = std::nan("");
  const char* const end = std::to_address(text.cend());
  if (text.empty() || std::from_chars(text.data(), end, value).ptr != end) {
    ADD_FAILURE() << "not a number: " << text;
  }
  return value;
}

/// `length` samples of a pulse train between 0 V and 1 V with a period of `period` samples: low for
/// `period - high` samples, then high for `high`, starting `delay` samples (under `period`) late,
/// so that a delay up to `high` starts the capture on the high level.
inline std::vector<float> pulse_train(std::size_t length, std::size_t period, std::size_t high,
                                      std::size_t delay = 0) {
  std::vector<float> samples(length);
  for (std::size_t index = 0; index < length; ++index) {
    samples[index] = (index + period - delay) % period >= period - high ? 1.0F : 0.0F;
  }
  return samples;
}

/// Trigger settings under which Auto takes untriggered captures back to back from sample 0: time
/// zero on the first sample and a level no signal reaches. Measurements read the whole capture, so
/// these tests need each capture to complete one record length after the last.
inline constexpr auto kUntriggeredCaptures = "TRIG:POS 0;LEV 100";

/// An Instrument at its `*RST` settings taking untriggered captures back to back: the starting
/// point of measurement facade tests.
class MeasurementTest : public InstrumentFixture {
 protected:
  void SetUp() override { session.send(kUntriggeredCaptures); }
};

}  // namespace openscope::test
