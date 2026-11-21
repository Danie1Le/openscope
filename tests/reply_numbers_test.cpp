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

}  // namespace
}  // namespace openscope::test
