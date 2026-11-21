#include "openscope/measurement.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "openscope/capture.hpp"

namespace openscope {
namespace {

/// Bins in the histogram that top and base come from, over the samples' minimum to maximum.
constexpr std::size_t kHistogramBins = 256;

/// A half of the histogram has a settled level when its fullest bin holds at least this many times
/// the average count of that half's bins. A level the signal dwells at (a square wave's high or
/// low, a sine's peak) stands far above the average; a signal that sweeps evenly through its range
/// (a triangle, a ramp) fills every bin about equally and has no such level.
constexpr double kSettledLevelRatio = 2.0;

/// The mean of the samples in the fullest of `counts`' bins, whose sample sums are `sums`, when it
/// is a settled level; otherwise `fallback`. Ties go to the first bin.
double settled_level(std::span<const std::size_t> counts, std::span<const double> sums,
                     double fallback) {
  const auto fullest = std::ranges::max_element(counts);
  const std::size_t total = std::accumulate(counts.begin(), counts.end(), std::size_t{0});
  if (static_cast<double>(*fullest) * static_cast<double>(counts.size()) <
      kSettledLevelRatio * static_cast<double>(total)) {
    return fallback;
  }
  return sums[static_cast<std::size_t>(fullest - counts.begin())] / static_cast<double>(*fullest);
}

}  // namespace

SignalLevels find_levels(std::span<const float> samples) {
  const auto [lowest, highest] = std::ranges::minmax(samples);
  SignalLevels levels{.minimum = lowest, .maximum = highest, .top = highest, .base = lowest};
  if (lowest == highest) {
    return levels;  // A single level: top and base are both it.
  }
  std::array<std::size_t, kHistogramBins> counts{};
  std::array<double, kHistogramBins> sums{};
  const double bins_per_volt =
      static_cast<double>(kHistogramBins) / (static_cast<double>(highest) - lowest);
  for (const float sample : samples) {
    // The maximum lands one past the last bin, so it joins the last bin.
    const std::size_t bin =
        std::min(static_cast<std::size_t>((static_cast<double>(sample) - lowest) * bins_per_volt),
                 kHistogramBins - 1);
    ++counts.at(bin);
    sums.at(bin) += sample;
  }
  constexpr std::size_t kHalf = kHistogramBins / 2;
  levels.base = settled_level(std::span(counts).first<kHalf>(), std::span(sums).first<kHalf>(),
                              levels.minimum);
  levels.top =
      settled_level(std::span(counts).last<kHalf>(), std::span(sums).last<kHalf>(), levels.maximum);
  return levels;
}

namespace {

constexpr std::array kMeasurements{
    Measurement{.mnemonic = "VPP",
                .measure = [](const MeasuredSignal& signal) -> std::optional<double> {
                  return signal.levels.maximum - signal.levels.minimum;
                }},
    Measurement{.mnemonic = "VMIN",
                .measure = [](const MeasuredSignal& signal) -> std::optional<double> {
                  return signal.levels.minimum;
                }},
    Measurement{.mnemonic = "VMAX",
                .measure = [](const MeasuredSignal& signal) -> std::optional<double> {
                  return signal.levels.maximum;
                }},
};

}  // namespace

// No signal comes first, since a flat signal has no edges or cycles even when it is clipped flat
// at a limit; Clipped only marks a value that exists.
MeasurementResult evaluate(const Measurement& measurement, const CapturedChannel& channel,
                           const SignalLevels& levels) {
  if (measurement.needs_signal &&
      levels.top - levels.base < kNoSignalDivisions * channel.settings.scale) {
    return {.status = MeasurementStatus::kNoSignal, .value = std::nullopt};
  }
  const std::optional<double> value =
      measurement.measure({.samples = channel.samples, .levels = levels});
  if (!value) {
    return {.status = MeasurementStatus::kTooFewCycles, .value = std::nullopt};
  }
  // Clipped samples are held at the limit, so a sample at the limit is one that hit it.
  const bool clipped = levels.minimum <= channel.settings.lower_limit() ||
                       levels.maximum >= channel.settings.upper_limit();
  return {.status = clipped ? MeasurementStatus::kClipped : MeasurementStatus::kOk, .value = value};
}

std::span<const Measurement> measurements() { return kMeasurements; }

MeasurementResult MeasurementEngine::measure(const std::shared_ptr<const Capture>& capture,
                                             std::size_t channel, std::size_t measurement) {
  if (measurement >= kMeasurements.size()) {
    throw std::out_of_range("no such measurement");
  }
  const std::optional<CapturedChannel>& captured = capture->channels.at(channel - 1);
  if (!captured) {
    throw std::out_of_range("channel is not in the capture");
  }
  if (capture_.lock() != capture) {
    capture_ = capture;
    channels_ = {};
  }
  std::optional<ChannelResults>& cached = channels_.at(channel - 1);
  if (!cached) {
    cached.emplace(ChannelResults{
        .levels = find_levels(captured->samples),
        .results = std::vector<std::optional<MeasurementResult>>(kMeasurements.size())});
  }
  std::optional<MeasurementResult>& result = cached->results.at(measurement);
  if (!result) {
    result = evaluate(kMeasurements.at(measurement), *captured, cached->levels);
  }
  return *result;
}

}  // namespace openscope
