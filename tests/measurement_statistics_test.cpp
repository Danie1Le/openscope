// measure_all and MeasurementStatistics on their own.

#include "openscope/measurement_statistics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/measurement.hpp"

namespace openscope {
namespace {

/// The position in measurements() of the measurement queried as `mnemonic`.
std::size_t position_of(std::string_view mnemonic) {
  const std::span<const Measurement> all = measurements();
  const auto found = std::ranges::find(all, mnemonic, &Measurement::mnemonic);
  if (found == all.end()) {
    ADD_FAILURE() << "no measurement " << mnemonic;
  }
  return static_cast<std::size_t>(found - all.begin());
}

MeasurementResult ok(double value) { return {.status = MeasurementStatus::kOk, .value = value}; }
MeasurementResult clipped(double value) {
  return {.status = MeasurementStatus::kClipped, .value = value};
}
MeasurementResult too_few_cycles() {
  return {.status = MeasurementStatus::kTooFewCycles, .value = std::nullopt};
}

/// Results for channel `channel` (1 to 4) only, every measurement giving `result`.
CaptureResults results_on(std::size_t channel, const MeasurementResult& result) {
  CaptureResults results;
  results.at(channel - 1).emplace(measurements().size(), result);
  return results;
}

/// The result in `results` of the measurement queried as `mnemonic` on channel `channel` (1 to 4).
/// Fails the test and returns an empty result when the channel has no results.
MeasurementResult result_of(const CaptureResults& results, std::size_t channel,
                            std::string_view mnemonic) {
  const std::vector<MeasurementResult> channel_results =
      results.at(channel - 1).value_or(std::vector<MeasurementResult>{});
  if (channel_results.size() != measurements().size()) {
    ADD_FAILURE() << "CH" << channel << " has " << channel_results.size() << " results";
    return {};
  }
  return channel_results.at(position_of(mnemonic));
}

TEST(MeasureAllTest, MeasuresEveryMeasurementOfEveryCapturedChannelOnly) {
  Capture capture;
  capture.channels.at(0).emplace(
      CapturedChannel{.settings = {.enabled = true}, .samples = {0.0F, 1.0F, 2.0F, 3.0F}});
  capture.channels.at(2).emplace(
      CapturedChannel{.settings = {.enabled = true}, .samples = {-1.0F, -1.0F}});

  const CaptureResults results = measure_all(capture);

  EXPECT_EQ(result_of(results, 1, "VPP").value, 3.0);
  EXPECT_EQ(result_of(results, 1, "VAVerage").value, 1.5);
  EXPECT_FALSE(results.at(1).has_value());
  EXPECT_EQ(result_of(results, 3, "VMAX").value, -1.0);
  EXPECT_FALSE(results.at(3).has_value());
}

TEST(MeasureAllTest, ResultsCarryTheirStatus) {
  Capture capture;
  // -4 V is the lower limit of the input range at 1 V/div.
  capture.channels.at(0).emplace(
      CapturedChannel{.settings = {.enabled = true}, .samples = {-4.0F, 1.0F}});

  const CaptureResults results = measure_all(capture);

  EXPECT_EQ(result_of(results, 1, "VPP").status, MeasurementStatus::kClipped);
  EXPECT_EQ(result_of(results, 1, "VPP").value, 5.0);
}

TEST(MeasurementStatisticsTest, StartsEmpty) {
  const MeasurementStatistics statistics;

  EXPECT_TRUE(statistics.empty());
  EXPECT_FALSE(statistics.holds(1));
  EXPECT_THROW((void)statistics.at(1, position_of("VPP")), std::out_of_range);
}

TEST(MeasurementStatisticsTest, CountsEveryValueAndKeepsTheMostRecentResult) {
  MeasurementStatistics statistics;
  statistics.add(results_on(2, ok(1.0)));
  statistics.add(results_on(2, ok(3.0)));

  EXPECT_FALSE(statistics.empty());
  const MeasurementSeries& series = statistics.at(2, position_of("VMAX"));
  EXPECT_EQ(series.current.value, 3.0);
  EXPECT_EQ(series.values.count(), 2U);
  EXPECT_EQ(series.values.mean(), 2.0);
  EXPECT_FALSE(series.clipped);
}

TEST(MeasurementStatisticsTest, ChannelsAndMeasurementsAreKeptApart) {
  MeasurementStatistics statistics;
  std::vector<MeasurementResult> channel_one(measurements().size(), ok(1.0));
  channel_one.at(position_of("VMAX")) = ok(2.0);
  CaptureResults results = results_on(4, ok(7.0));
  results.at(0) = channel_one;
  statistics.add(results);

  EXPECT_EQ(statistics.at(1, position_of("VPP")).values.mean(), 1.0);
  EXPECT_EQ(statistics.at(1, position_of("VMAX")).values.mean(), 2.0);
  EXPECT_EQ(statistics.at(4, position_of("VPP")).values.mean(), 7.0);
  EXPECT_TRUE(statistics.holds(1));
  EXPECT_FALSE(statistics.holds(2));
  EXPECT_TRUE(statistics.holds(4));
  EXPECT_THROW((void)statistics.at(2, position_of("VPP")), std::out_of_range);
  EXPECT_THROW((void)statistics.at(1, measurements().size()), std::out_of_range);
}

TEST(MeasurementStatisticsTest, ResultsWithoutAValueAreLeftOutButBecomeCurrent) {
  MeasurementStatistics statistics;
  statistics.add(results_on(1, ok(2.0)));
  statistics.add(results_on(1, too_few_cycles()));

  const MeasurementSeries& series = statistics.at(1, position_of("VPP"));
  EXPECT_EQ(series.current.status, MeasurementStatus::kTooFewCycles);
  EXPECT_EQ(series.current.value, std::nullopt);
  EXPECT_EQ(series.values.count(), 1U);
  EXPECT_EQ(series.values.mean(), 2.0);
}

TEST(MeasurementStatisticsTest, CaptureWithOnlyResultsWithoutAValueCountsNoValue) {
  MeasurementStatistics statistics;
  statistics.add(results_on(1, too_few_cycles()));

  EXPECT_FALSE(statistics.empty());
  EXPECT_EQ(statistics.at(1, position_of("VPP")).values.count(), 0U);
}

TEST(MeasurementStatisticsTest, ClippedValuesCountAndMarkTheSeries) {
  MeasurementStatistics statistics;
  statistics.add(results_on(1, clipped(4.0)));
  statistics.add(results_on(1, ok(1.0)));

  const MeasurementSeries& series = statistics.at(1, position_of("VMAX"));
  EXPECT_EQ(series.current.status, MeasurementStatus::kOk);
  EXPECT_EQ(series.values.count(), 2U);
  EXPECT_EQ(series.values.maximum(), 4.0);
  EXPECT_TRUE(series.clipped);
}

TEST(MeasurementStatisticsTest, ResetForgetsEverything) {
  MeasurementStatistics statistics;
  statistics.add(results_on(1, clipped(4.0)));

  statistics.reset();

  EXPECT_TRUE(statistics.empty());
  EXPECT_THROW((void)statistics.at(1, position_of("VPP")), std::out_of_range);
  statistics.add(results_on(1, ok(1.0)));
  EXPECT_EQ(statistics.at(1, position_of("VPP")).values.count(), 1U);
  EXPECT_FALSE(statistics.at(1, position_of("VPP")).clipped);
}

}  // namespace
}  // namespace openscope
