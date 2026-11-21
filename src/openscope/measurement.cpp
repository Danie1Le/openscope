#include "openscope/measurement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

#include "openscope/capture.hpp"

namespace openscope {
namespace {

/// Bins in each histogram that top and base come from.
constexpr std::size_t kHistogramBins = 256;

/// A level is settled when its histogram bin holds at least this many times the average bin's
/// share of the capture: 1/128 of all the samples. A level the signal dwells at (a square wave's
/// high or low, a sine's peak) stands far above the average; a signal that sweeps evenly through
/// its range (a triangle, a ramp) fills every bin about equally and has no such level. Samples cut
/// off from the rest that add up to less than a settled level are outliers: a spike or a short
/// glitch.
constexpr double kSettledLevelRatio = 2.0;

/// The fewest samples a settled level holds, so that in a short capture, where one sample is
/// already 1/128 of it, a single spike is still an outlier.
constexpr double kFewestSettledSamples = 2.0;

/// The counts and sums of samples in kHistogramBins equal bins from `low` volts up.
struct Histogram {
  double low;
  double bins_per_volt;
  std::array<std::size_t, kHistogramBins> counts{};
  std::array<double, kHistogramBins> sums{};
};

/// The bin of `histogram` that `sample`, which lies in the histogram's range, falls in. The top of
/// the range lands one past the last bin, so it joins the last bin.
std::size_t bin_of(const Histogram& histogram, float sample) {
  return std::min(static_cast<std::size_t>((static_cast<double>(sample) - histogram.low) *
                                           histogram.bins_per_volt),
                  kHistogramBins - 1);
}

/// The histogram of the `samples` from `low` to `high` volts (`low` under `high`); samples outside
/// that range are left out.
Histogram make_histogram(std::span<const float> samples, float low, float high) {
  Histogram histogram{
      .low = low,
      .bins_per_volt = static_cast<double>(kHistogramBins) / (static_cast<double>(high) - low)};
  for (const float sample : samples) {
    if (sample >= low && sample <= high) {
      const std::size_t bin = bin_of(histogram, sample);
      ++histogram.counts.at(bin);
      histogram.sums.at(bin) += sample;
    }
  }
  return histogram;
}

/// How many bins at the start of `counts` hold only outliers: every bin up to the last empty one
/// before `settled_samples` samples are reached, so what they hold is cut off from the rest and
/// adds up to fewer than `settled_samples` samples.
std::size_t outlier_bins(const std::ranges::input_range auto& counts, double settled_samples) {
  std::size_t outliers = 0;
  std::size_t bins = 0;
  std::size_t samples = 0;
  for (const std::size_t count : counts) {
    samples += count;
    if (static_cast<double>(samples) >= settled_samples) {
      break;
    }
    ++bins;
    if (count == 0) {
      outliers = bins;
    }
  }
  return outliers;
}

/// The mean of the samples in the fullest of `counts`' bins, whose sample sums are `sums`, when it
/// holds at least `settled_samples` samples; otherwise `fallback`. Ties go to the first bin.
double settled_level(std::span<const std::size_t> counts, std::span<const double> sums,
                     double settled_samples, double fallback) {
  const auto fullest = std::ranges::max_element(counts);
  if (static_cast<double>(*fullest) < settled_samples) {
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
  const double settled_samples = std::max(kSettledLevelRatio * static_cast<double>(samples.size()) /
                                              static_cast<double>(kHistogramBins),
                                          kFewestSettledSamples);
  // Set the outliers at either end aside. Each end gives up fewer than `settled_samples`, and
  // outliers are only outliers of a rest that could hold a settled level: when too few samples are
  // left (a capture of two or three), every sample is kept.
  const Histogram all = make_histogram(samples, lowest, highest);
  const std::size_t first = outlier_bins(all.counts, settled_samples);
  const std::size_t last =
      kHistogramBins - 1 - outlier_bins(std::views::reverse(all.counts), settled_samples);
  float low = highest;
  float high = lowest;
  std::size_t left = 0;
  for (const float sample : samples) {
    const std::size_t bin = bin_of(all, sample);
    if (bin >= first && bin <= last) {
      low = std::min(low, sample);
      high = std::max(high, sample);
      ++left;
    }
  }
  if (static_cast<double>(left) < settled_samples) {
    low = lowest;
    high = highest;
  }
  levels.base = low;
  levels.top = high;
  if (low == high) {
    return levels;
  }
  // The kept samples get a histogram of their own, so a far outlier costs the levels no resolution.
  const Histogram kept = make_histogram(samples, low, high);
  constexpr std::size_t kHalf = kHistogramBins / 2;
  levels.base = settled_level(std::span(kept.counts).first<kHalf>(),
                              std::span(kept.sums).first<kHalf>(), settled_samples, low);
  levels.top = settled_level(std::span(kept.counts).last<kHalf>(),
                             std::span(kept.sums).last<kHalf>(), settled_samples, high);
  return levels;
}

namespace {

/// The mean of `samples`, summed in order so the result never depends on the platform.
double mean(std::span<const float> samples) {
  return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
}

/// The root mean square of `samples`: DC included, as a true-RMS meter reads it.
double root_mean_square(std::span<const float> samples) {
  const double sum_of_squares = std::accumulate(
      samples.begin(), samples.end(), 0.0,
      [](double sum, float sample) { return sum + (static_cast<double>(sample) * sample); });
  return std::sqrt(sum_of_squares / static_cast<double>(samples.size()));
}

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
    Measurement{.mnemonic = "VAVerage",
                .measure = [](const MeasuredSignal& signal) -> std::optional<double> {
                  return mean(signal.samples);
                }},
    Measurement{.mnemonic = "VRMS",
                .measure = [](const MeasuredSignal& signal) -> std::optional<double> {
                  return root_mean_square(signal.samples);
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
