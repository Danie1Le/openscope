#include "openscope/running_statistics.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>

namespace openscope {
namespace {

RunningStatistics of(std::initializer_list<double> values) {
  RunningStatistics statistics;
  for (const double value : values) {
    statistics.add(value);
  }
  return statistics;
}

TEST(RunningStatisticsTest, StartsEmpty) { EXPECT_EQ(RunningStatistics{}.count(), 0U); }

TEST(RunningStatisticsTest, OneValueIsItsOwnMeanMinimumAndMaximumWithNoSpread) {
  const RunningStatistics statistics = of({-2.5});

  EXPECT_EQ(statistics.count(), 1U);
  EXPECT_EQ(statistics.mean(), -2.5);
  EXPECT_EQ(statistics.minimum(), -2.5);
  EXPECT_EQ(statistics.maximum(), -2.5);
  EXPECT_EQ(statistics.standard_deviation(), 0.0);
}

// Worked example: the deviations from the mean 5 are -3, -1, -1, -1, 0, 0, 2, 4, whose squares sum
// to 32, so the population variance is 32 / 8 = 4.
TEST(RunningStatisticsTest, StandardDeviationIsThePopulationOne) {
  const RunningStatistics statistics = of({2, 4, 4, 4, 5, 5, 7, 9});

  EXPECT_EQ(statistics.count(), 8U);
  EXPECT_EQ(statistics.mean(), 5.0);
  EXPECT_EQ(statistics.minimum(), 2.0);
  EXPECT_EQ(statistics.maximum(), 9.0);
  EXPECT_EQ(statistics.standard_deviation(), 2.0);
}

TEST(RunningStatisticsTest, MinimumAndMaximumDoNotDependOnOrder) {
  const RunningStatistics statistics = of({3, -7, 12, 0, -7, 12});

  EXPECT_EQ(statistics.minimum(), -7.0);
  EXPECT_EQ(statistics.maximum(), 12.0);
}

TEST(RunningStatisticsTest, EqualValuesHaveNoSpread) {
  EXPECT_EQ(of({0.1, 0.1, 0.1, 0.1, 0.1}).standard_deviation(), 0.0);
}

// 4, 7, 13 and 16 have a population variance of 22.5. Offset by 1E9, their squares need more than
// a double's 53 bits, so a running sum of squares loses the spread entirely; Welford's update
// works on the deviations and keeps it.
TEST(RunningStatisticsTest, LargeCloseValuesKeepTheirSpread) {
  constexpr double kOffset = 1e9;
  const RunningStatistics statistics = of({kOffset + 4, kOffset + 7, kOffset + 13, kOffset + 16});

  EXPECT_EQ(statistics.mean(), kOffset + 10);
  EXPECT_DOUBLE_EQ(statistics.standard_deviation(), std::sqrt(22.5));
}

}  // namespace
}  // namespace openscope
