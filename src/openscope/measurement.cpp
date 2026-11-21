#include "openscope/measurement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"

namespace openscope {
namespace {

/// Bins in each histogram that top and base come from.
constexpr std::size_t kHistogramBins = 256;

/// A level is settled when it holds at least this many times the average bin's share of the
/// capture (1/128 of all the samples) and this many times what as many bins of its half hold on
/// average. A level the signal dwells at (a square wave's high or low, a sine's peak) stands far
/// above the average; a signal that sweeps evenly through its range (a triangle, a ramp) fills
/// every bin about equally and has no such level. Samples cut off from the rest that add up to
/// less than a settled level are outliers: a spike or a short glitch.
constexpr double kSettledLevelRatio = 2.0;

/// The fewest samples a settled level holds, so that in a short capture, where one sample is
/// already 1/128 of it, a single spike is still an outlier.
constexpr double kFewestSettledSamples = 2.0;

/// The fewest samples a level holds before it can stand out from the rest: in a capture of a few
/// hundred samples a handful landing close together is chance, not a level.
constexpr double kFewestLevelSamples = 24.0;

/// How many bins either side of a level's fullest bin still count towards the level: noise of 1%
/// of the signal's span spreads a level over about this many bins of 256.
constexpr std::size_t kLevelBins = 4;

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

/// The mean of the samples in the fullest of `counts`' bins, whose sample sums are `sums`, when
/// that bin is a settled level; otherwise `fallback`. Ties go to the first bin. Noise spreads a
/// level over a few bins, so the level is judged by the bins within kLevelBins of the fullest one:
/// they must hold at least `settled_samples` samples (and kFewestLevelSamples), and
/// kSettledLevelRatio times what as many bins hold on average. Without the last test, a signal that
/// sweeps evenly but whose samples land in every second or third bin (a short triangle) would have
/// a level wherever its bins happen to be fuller.
double settled_level(std::span<const std::size_t> counts, std::span<const double> sums,
                     double settled_samples, double fallback) {
  const auto fullest = static_cast<std::size_t>(std::ranges::max_element(counts) - counts.begin());
  const std::size_t first = fullest - std::min(fullest, kLevelBins);
  const std::size_t last = std::min(fullest + kLevelBins, counts.size() - 1);
  const std::span<const std::size_t> around = counts.subspan(first, last - first + 1);
  const auto level = static_cast<double>(std::reduce(around.begin(), around.end()));
  const auto all = static_cast<double>(std::reduce(counts.begin(), counts.end()));
  const double as_many_on_average =
      all * static_cast<double>(around.size()) / static_cast<double>(counts.size());
  if (level < std::max(settled_samples, kFewestLevelSamples) ||
      level < kSettledLevelRatio * as_many_on_average) {
    return fallback;
  }
  return sums[fullest] / static_cast<double>(counts[fullest]);
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

/// The level `fraction` of the way from base to top: 0.5 is the 50% reference level.
double level_at(const SignalLevels& levels, double fraction) {
  return levels.base + (fraction * (levels.top - levels.base));
}

/// A hysteresis band between two levels, as fractions of the way from base to top (#10): the
/// signal is low under the lower level and high over the upper one, and keeps its side in between.
struct Band {
  double lower;
  double upper;
};

/// The band whole cycles are counted with. It is centred on the 50% level so that a sampled peak
/// falling short of 90% (a triangle or ramp at 10 samples per cycle with noise) still counts, and
/// wide enough that noise around 50% does not.
constexpr Band kCycleBand{.lower = 0.3, .upper = 0.7};

/// A band inside kCycleBand. A move across it that turns back before crossing kCycleBand, and lasts
/// at least kDoubtfulCycleShare of a cycle, may be a cycle that kCycleBand missed: whole cycles are
/// then not known (FR-13).
constexpr Band kDoubtBand{.lower = 0.4, .upper = 0.6};

/// The share of the mean cycle a move across kDoubtBand and back must last to be a doubtful cycle.
/// Noise on a slow edge crosses kDoubtBand and back only while the edge is within 10% of the span
/// from 50%, which is at most a tenth of a cycle (a triangle's edge; a sine's takes 6%).
constexpr double kDoubtfulCycleShare = 0.125;

/// The band rise and fall time are measured across: the 10% and 90% reference levels.
constexpr Band kTransitionBand{.lower = 0.1, .upper = 0.9};

/// One move of the signal from beyond one level of a band to beyond the other. Its times are in
/// sample periods.
struct Edge {
  enum class Direction : std::uint8_t { kRising, kFalling };
  Direction direction;
  /// When the edge last crossed the 50% level, from the first sample.
  double middle;
  /// How long the edge took from last leaving the band level it started from (the lower one when
  /// rising, the upper one when falling) to reaching the other one.
  double duration;
};

/// When `samples` last reached `level` going up (`sign` 1) or down (-1) at or before sample
/// `reached`, which is past `level`, in sample periods from the first sample: linear interpolation
/// between the last sample short of `level` and the next one. A sample short of `level` must come
/// before `reached`.
double crossing_before(std::span<const float> samples, std::size_t reached, double level,
                       double sign) {
  std::size_t before = reached;
  while (sign * samples[before] >= sign * level) {
    --before;
  }
  const double from = samples[before];
  const double to = samples[before + 1];
  return static_cast<double>(before) + ((level - from) / (to - from));
}

/// The edges of `signal` across `band`, in order, so rising and falling alternate: a rising edge is
/// the signal going from under the band's lower level to over its upper one, a falling one the
/// reverse, so noise or a glitch that crosses 50% without reaching the far level is no edge. A
/// capture that starts inside the band has no edge until the signal first passes one of its
/// levels: which side it came from is unknown.
std::vector<Edge> find_edges(const MeasuredSignal& signal, Band band) {
  const double lower = level_at(signal.levels, band.lower);
  const double upper = level_at(signal.levels, band.upper);
  const double middle = level_at(signal.levels, 0.5);
  enum class Side : std::uint8_t { kUnknown, kLow, kHigh };
  Side side = Side::kUnknown;
  std::vector<Edge> edges;
  for (std::size_t index = 0; index < signal.samples.size(); ++index) {
    const double sample = signal.samples[index];
    Side now = side;
    if (sample < lower) {
      now = Side::kLow;
    } else if (sample > upper) {
      now = Side::kHigh;
    }
    if (now != side && side != Side::kUnknown) {
      const bool rising = now == Side::kHigh;
      const auto [start, far] = rising ? std::pair{lower, upper} : std::pair{upper, lower};
      const auto crossing = [&](double level) {
        return crossing_before(signal.samples, index, level, rising ? 1.0 : -1.0);
      };
      edges.push_back({.direction = rising ? Edge::Direction::kRising : Edge::Direction::kFalling,
                       .middle = crossing(middle),
                       .duration = crossing(far) - crossing(start)});
    }
    side = now;
  }
  return edges;
}

/// The whole cycles of a signal, each from one edge's 50% crossing to the next crossing in the same
/// direction.
struct WholeCycles {
  /// The edges from the first one to the last that ends a whole cycle.
  std::vector<Edge> edges;
  /// How many whole cycles they bound.
  std::size_t count;
  /// Sample periods from the first edge's 50% crossing to the last one's.
  double length;
};

/// Whether `signal` crossed kDoubtBand and back, between the first and the last of `edges` (its
/// edges across kCycleBand), for at least `shortest` sample periods.
///
/// Every edge across kCycleBand crosses kDoubtBand first, with the same or an earlier 50% crossing,
/// after the edge before it. So the kDoubtBand edges after an edge across kCycleBand and up to the
/// next one, `closing`, run in `closing`'s direction, the other, and so on, ending in `closing`'s:
/// each one in the other direction ends a move across kDoubtBand and back that began at the edge
/// before it.
bool crossed_doubt_band_and_back(const MeasuredSignal& signal, std::span<const Edge> edges,
                                 double shortest) {
  const std::vector<Edge> doubt_edges = find_edges(signal, kDoubtBand);
  std::size_t closing = 1;
  for (std::size_t index = 1; index < doubt_edges.size(); ++index) {
    const Edge& edge = doubt_edges[index];
    if (edge.middle <= edges.front().middle) {
      continue;
    }
    while (closing < edges.size() && edges[closing].middle < edge.middle) {
      ++closing;
    }
    if (closing == edges.size()) {
      return false;
    }
    if (edge.direction != edges[closing].direction &&
        edge.middle - doubt_edges[index - 1].middle >= shortest) {
      return true;
    }
  }
  return false;
}

/// The whole cycles in `signal`, or nullopt when there is not one or they are not known: the signal
/// crossed kDoubtBand and back between two edges across kCycleBand for long enough to be a cycle.
std::optional<WholeCycles> whole_cycles(const MeasuredSignal& signal) {
  std::vector<Edge> edges = find_edges(signal, kCycleBand);
  if (edges.size() < 3) {
    return std::nullopt;
  }
  edges.resize(edges.size() - ((edges.size() - 1) % 2));
  const double length = edges.back().middle - edges.front().middle;
  const std::size_t count = edges.size() / 2;
  if (crossed_doubt_band_and_back(signal, edges,
                                  kDoubtfulCycleShare * length / static_cast<double>(count))) {
    return std::nullopt;
  }
  return WholeCycles{.edges = std::move(edges), .count = count, .length = length};
}

/// The mean period of the whole cycles in `signal`, in seconds, or nullopt under one whole cycle.
std::optional<double> period(const MeasuredSignal& signal) {
  const std::optional<WholeCycles> cycles = whole_cycles(signal);
  if (!cycles) {
    return std::nullopt;
  }
  return cycles->length / static_cast<double>(cycles->count) / signal.sample_rate;
}

/// The percentage of the whole cycles in `signal` spent above the 50% level, or nullopt under one
/// whole cycle.
std::optional<double> positive_duty_cycle(const MeasuredSignal& signal) {
  const std::optional<WholeCycles> cycles = whole_cycles(signal);
  if (!cycles) {
    return std::nullopt;
  }
  const std::vector<Edge>& edges = cycles->edges;
  double high = 0.0;
  for (std::size_t edge = 0; edge + 1 < edges.size(); ++edge) {
    if (edges[edge].direction == Edge::Direction::kRising) {
      high += edges[edge + 1].middle - edges[edge].middle;
    }
  }
  return 100.0 * high / cycles->length;
}

/// The mean time the edges in `kDirection` in `signal` take from one outer reference level to the
/// other, in seconds, or nullopt when there is no such edge.
template <Edge::Direction kDirection>
std::optional<double> transition_time(const MeasuredSignal& signal) {
  double total = 0.0;
  std::size_t count = 0;
  for (const Edge& edge : find_edges(signal, kTransitionBand)) {
    if (edge.direction == kDirection) {
      total += edge.duration;
      ++count;
    }
  }
  if (count == 0) {
    return std::nullopt;
  }
  return total / static_cast<double>(count) / signal.sample_rate;
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
    Measurement{.mnemonic = "FREQuency",
                .measure = [](const MeasuredSignal& signal) -> std::optional<double> {
                  const std::optional<double> seconds = period(signal);
                  return seconds ? std::optional(1.0 / *seconds) : std::nullopt;
                },
                .needs_signal = true},
    Measurement{.mnemonic = "PERiod", .measure = period, .needs_signal = true},
    Measurement{.mnemonic = "DUTYcycle", .measure = positive_duty_cycle, .needs_signal = true},
    Measurement{.mnemonic = "RISetime",
                .measure = transition_time<Edge::Direction::kRising>,
                .needs_signal = true},
    Measurement{.mnemonic = "FALLtime",
                .measure = transition_time<Edge::Direction::kFalling>,
                .needs_signal = true},
};

}  // namespace

// No signal comes first, since a flat signal has no edges or cycles even when it is clipped flat
// at a limit; Clipped only marks a value that exists.
MeasurementResult evaluate(const Measurement& measurement, const ChannelSettings& settings,
                           const MeasuredSignal& signal) {
  const SignalLevels& levels = signal.levels;
  if (measurement.needs_signal && levels.top - levels.base < kNoSignalDivisions * settings.scale) {
    return {.status = MeasurementStatus::kNoSignal, .value = std::nullopt};
  }
  const std::optional<double> value = measurement.measure(signal);
  if (!value) {
    return {.status = MeasurementStatus::kTooFewCycles, .value = std::nullopt};
  }
  // Clipped samples are held at the limit, so a sample at the limit is one that hit it.
  const bool clipped =
      levels.minimum <= settings.lower_limit() || levels.maximum >= settings.upper_limit();
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
    result = evaluate(kMeasurements.at(measurement), captured->settings,
                      {.samples = captured->samples,
                       .levels = cached->levels,
                       .sample_rate = capture->sample_rate});
  }
  return *result;
}

}  // namespace openscope
