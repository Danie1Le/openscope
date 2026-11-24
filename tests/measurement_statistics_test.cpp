// MeasurementStatistics on its own. Statistics through SCPI (MEAS:STAT?) are in
// statistics_commands_test.cpp.

#include "openscope/measurement_statistics.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

#include "measurement_fixture.hpp"
#include "openscope/measurement.hpp"

namespace openscope {
namespace {

using test::measurement_index;

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

TEST(MeasurementStatisticsTest, StartsEmpty) {
  const MeasurementStatistics statistics;

  EXPECT_TRUE(statistics.empty());
  EXPECT_THROW((void)statistics.at(1, measurement_index("VPP")), std::out_of_range);
}

TEST(MeasurementStatisticsTest, CountsEveryValueAndKeepsTheMostRecentResult) {
  MeasurementStatistics statistics;
  statistics.add(results_on(2, ok(1.0)));
  statistics.add(results_on(2, ok(3.0)));

  EXPECT_FALSE(statistics.empty());
  const MeasurementSeries& series = statistics.at(2, measurement_index("VMAX"));
  EXPECT_EQ(series.current.value, 3.0);
  EXPECT_EQ(series.values.count(), 2U);
  EXPECT_EQ(series.values.mean(), 2.0);
  EXPECT_FALSE(series.clipped);
}

TEST(MeasurementStatisticsTest, ChannelsAndMeasurementsAreKeptApart) {
  MeasurementStatistics statistics;
  std::vector<MeasurementResult> channel_one(measurements().size(), ok(1.0));
  channel_one.at(measurement_index("VMAX")) = ok(2.0);
  CaptureResults results = results_on(4, ok(7.0));
  results.at(0) = channel_one;
  statistics.add(results);

  EXPECT_EQ(statistics.at(1, measurement_index("VPP")).values.mean(), 1.0);
  EXPECT_EQ(statistics.at(1, measurement_index("VMAX")).values.mean(), 2.0);
  EXPECT_EQ(statistics.at(4, measurement_index("VPP")).values.mean(), 7.0);
  EXPECT_THROW((void)statistics.at(2, measurement_index("VPP")), std::out_of_range);
  EXPECT_THROW((void)statistics.at(1, measurements().size()), std::out_of_range);
}

TEST(MeasurementStatisticsTest, ResultsWithoutAValueAreLeftOutButBecomeCurrent) {
  MeasurementStatistics statistics;
  statistics.add(results_on(1, ok(2.0)));
  statistics.add(results_on(1, too_few_cycles()));

  const MeasurementSeries& series = statistics.at(1, measurement_index("VPP"));
  EXPECT_EQ(series.current.status, MeasurementStatus::kTooFewCycles);
  EXPECT_EQ(series.current.value, std::nullopt);
  EXPECT_EQ(series.values.count(), 1U);
  EXPECT_EQ(series.values.mean(), 2.0);
}

TEST(MeasurementStatisticsTest, CaptureWithOnlyResultsWithoutAValueCountsNoValue) {
  MeasurementStatistics statistics;
  statistics.add(results_on(1, too_few_cycles()));

  EXPECT_FALSE(statistics.empty());
  EXPECT_EQ(statistics.at(1, measurement_index("VPP")).values.count(), 0U);
}

TEST(MeasurementStatisticsTest, ClippedValuesCountAndMarkTheSeries) {
  MeasurementStatistics statistics;
  statistics.add(results_on(1, clipped(4.0)));
  statistics.add(results_on(1, ok(1.0)));

  const MeasurementSeries& series = statistics.at(1, measurement_index("VMAX"));
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
  EXPECT_THROW((void)statistics.at(1, measurement_index("VPP")), std::out_of_range);
  statistics.add(results_on(1, ok(1.0)));
  EXPECT_EQ(statistics.at(1, measurement_index("VPP")).values.count(), 1U);
  EXPECT_FALSE(statistics.at(1, measurement_index("VPP")).clipped);
}

}  // namespace
}  // namespace openscope
