#include "reply_numbers.hpp"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

namespace openscope::test {
namespace {

TEST(ReplyNumbersTest, ParsesEveryCommaSeparatedNumber) {
  EXPECT_EQ(numbers("1.5,-2,9.91E37,0"), (std::vector<double>{1.5, -2.0, 9.91e37, 0.0}));
  EXPECT_EQ(number("-0.25"), -0.25);
}

TEST(ReplyNumbersTest, AnythingButNumbersFailsTheTest) {
  EXPECT_NONFATAL_FAILURE((void)numbers(std::nullopt), "no reply");
  EXPECT_NONFATAL_FAILURE((void)numbers(""), "no reply");
  EXPECT_NONFATAL_FAILURE((void)numbers("1,,2"), "not numbers");
  EXPECT_NONFATAL_FAILURE((void)numbers("1,2,"), "not numbers");
  EXPECT_NONFATAL_FAILURE((void)numbers("1;2"), "not numbers");
  EXPECT_NONFATAL_FAILURE((void)numbers("1 "), "not numbers");
  EXPECT_NONFATAL_FAILURE(EXPECT_TRUE(std::isnan(number("1,2"))), "not one number");
}

TEST(ReplyNumbersTest, StatisticsAreTheSixFieldsOfAStatisticsReply) {
  const Statistics statistics = statistics_of("1,2,0.5,3,0.25,4");

  EXPECT_EQ(statistics.current, 1.0);
  EXPECT_EQ(statistics.mean, 2.0);
  EXPECT_EQ(statistics.minimum, 0.5);
  EXPECT_EQ(statistics.maximum, 3.0);
  EXPECT_EQ(statistics.standard_deviation, 0.25);
  EXPECT_EQ(statistics.count, 4.0);
  EXPECT_NONFATAL_FAILURE((void)statistics_of("1,2,3,4,5"), "not a statistics reply");
}

}  // namespace
}  // namespace openscope::test
