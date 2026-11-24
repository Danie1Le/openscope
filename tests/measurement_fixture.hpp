#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "instrument_fixture.hpp"
#include "openscope/measurement.hpp"

namespace openscope::test {

/// The position in measurements() of the measurement queried as `mnemonic`. Fails the test and
/// returns measurements().size() when there is none.
inline std::size_t measurement_index(std::string_view mnemonic) {
  const std::span<const Measurement> all = measurements();
  const auto found = std::ranges::find(all, mnemonic, &Measurement::mnemonic);
  if (found == all.end()) {
    ADD_FAILURE() << "no measurement " << mnemonic;
  }
  return static_cast<std::size_t>(found - all.begin());
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

/// An Instrument at its `*RST` settings taking untriggered captures back to back
/// (kUntriggeredCaptures): the starting point of measurement facade tests.
class MeasurementTest : public InstrumentFixture {
 protected:
  void SetUp() override { session.send(kUntriggeredCaptures); }
};

}  // namespace openscope::test
