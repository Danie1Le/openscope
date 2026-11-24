#include "openscope/measurement_statistics.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/measurement.hpp"

namespace openscope {

void MeasurementSeries::add(const MeasurementResult& result) {
  current = result;
  if (result.value) {
    values.add(*result.value);
    clipped = clipped || result.status == MeasurementStatus::kClipped;
  }
}

void MeasurementStatistics::add(const CaptureResults& results) {
  empty_ = false;
  for (std::size_t n = 0; n < kChannelCount; ++n) {
    if (const std::optional<std::vector<MeasurementResult>>& channel_results = results.at(n)) {
      std::optional<std::vector<MeasurementSeries>>& series = channels_.at(n);
      if (!series) {
        series.emplace(channel_results->size());
      }
      for (std::size_t m = 0; m < channel_results->size(); ++m) {
        series->at(m).add(channel_results->at(m));
      }
    }
  }
}

const MeasurementSeries& MeasurementStatistics::at(std::size_t channel,
                                                   std::size_t measurement) const {
  const std::optional<std::vector<MeasurementSeries>>& series = channels_.at(channel - 1);
  if (!series) {
    throw std::out_of_range("channel has no statistics");
  }
  return series->at(measurement);
}

}  // namespace openscope
