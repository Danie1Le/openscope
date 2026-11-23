// parse_in_range: how the engine (engine_options_test.cpp) and the benchmarks read numbers from the
// command line.

#include "engine/parse_in_range.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace openscope {
namespace {

TEST(ParseInRangeTest, ReadsAWholeNumberInTheRange) {
  EXPECT_EQ(parse_in_range("7", 1, 10), 7);
  EXPECT_EQ(parse_in_range<std::uint16_t>("65535", 0, 65535), 65535);
  EXPECT_EQ(parse_in_range("2.5e6", 1.0, 1e9), 2.5e6);
}

TEST(ParseInRangeTest, BoundsAreIncluded) {
  EXPECT_EQ(parse_in_range("1", 1, 10), 1);
  EXPECT_EQ(parse_in_range("10", 1, 10), 10);
  EXPECT_EQ(parse_in_range("0", 0.0, 1.0), 0.0);
}

TEST(ParseInRangeTest, OutsideTheRangeIsNothing) {
  EXPECT_EQ(parse_in_range("0", 1, 10), std::nullopt);
  EXPECT_EQ(parse_in_range("11", 1, 10), std::nullopt);
  EXPECT_EQ(parse_in_range("1e10", 1.0, 1e9), std::nullopt);
}

TEST(ParseInRangeTest, AnythingButOneWholeNumberIsNothing) {
  for (const std::string_view text : {"", " 5", "5 ", "5x", "+5", "5,6", "0x5", "five"}) {
    EXPECT_EQ(parse_in_range(text, 0, 10), std::nullopt) << '"' << text << '"';
  }
  EXPECT_EQ(parse_in_range<std::size_t>("-1", 0, 10), std::nullopt);
}

TEST(ParseInRangeTest, NumbersTooLargeForTheTypeAreNothing) {
  EXPECT_EQ(parse_in_range<std::uint16_t>("65536", 0, std::numeric_limits<std::uint16_t>::max()),
            std::nullopt);
}

TEST(ParseInRangeTest, NotANumberIsNothing) {
  EXPECT_EQ(parse_in_range("nan", 1.0, 1e9), std::nullopt);
  EXPECT_EQ(parse_in_range("inf", 1.0, 1e9), std::nullopt);
}

}  // namespace
}  // namespace openscope
