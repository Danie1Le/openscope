#include "openscope/scpi/response.hpp"

#include <gtest/gtest.h>

namespace openscope::scpi {
namespace {

TEST(FormatRealTest, WritesTheShortestFormWithAnUpperCaseExponent) {
  EXPECT_EQ(format_real(0.0), "0");
  EXPECT_EQ(format_real(1000.0), "1000");
  EXPECT_EQ(format_real(-0.5), "-0.5");
  EXPECT_EQ(format_real(0.001), "0.001");
  EXPECT_EQ(format_real(1e6), "1E+06");
  EXPECT_EQ(format_real(2.5e-7), "2.5E-07");
  EXPECT_EQ(format_real(1e-300), "1E-300");
}

TEST(FormatRealTest, WritesEnoughDigitsToReadBackTheSameValue) {
  EXPECT_EQ(format_real(0.1), "0.1");
  EXPECT_EQ(format_real(1.0 / 3.0), "0.3333333333333333");
}

TEST(FormatRealTest, WritesFloatsWithTheDigitsAFloatNeeds) {
  EXPECT_EQ(format_real(0.1F), "0.1");
  EXPECT_EQ(format_real(1.0F / 3.0F), "0.33333334");
  EXPECT_EQ(format_real(-1e-5F), "-1E-05");
}

}  // namespace
}  // namespace openscope::scpi
