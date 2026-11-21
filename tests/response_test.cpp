#include "openscope/scpi/response.hpp"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "binary_block.hpp"

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

TEST(DefiniteLengthHeaderTest, GivesTheDigitCountThenTheLength) {
  EXPECT_EQ(definite_length_header(0), "#10");
  EXPECT_EQ(definite_length_header(9), "#19");
  EXPECT_EQ(definite_length_header(10), "#210");
  EXPECT_EQ(definite_length_header(4'000'000), "#74000000");
  EXPECT_EQ(definite_length_header(kMaxBlockBytes), "#9999999999");
}

TEST(DefiniteLengthHeaderTest, RejectsALengthNineDigitsCannotHold) {
  EXPECT_THROW((void)definite_length_header(kMaxBlockBytes + 1), std::length_error);
}

TEST(Float32BlockTest, WritesEachValueLeastSignificantByteFirst) {
  const std::array values{1.0F, -2.5F, 0.0F};

  // 1.0F is 0x3F800000 and -2.5F is 0xC0200000.
  EXPECT_EQ(float32_block(values), std::string("#212"
                                               "\x00\x00\x80\x3F"
                                               "\x00\x00\x20\xC0"
                                               "\x00\x00\x00\x00",
                                               16));
}

TEST(Float32BlockTest, EmptyBlockHasALengthOfZero) { EXPECT_EQ(float32_block({}), "#10"); }

TEST(Float32BlockTest, LengthGrowsADigitAtTenBytes) {
  const std::array<float, 3> three{};
  EXPECT_EQ(float32_block(three).substr(0, 4), "#212");
  EXPECT_EQ(float32_block(std::span(three).first(2)).substr(0, 3), "#18");
}

TEST(Float32BlockTest, ReadsBackAsTheSameValues) {
  const std::vector<float> values{0.1F, -1e-30F, 3.4e38F, 1.0F / 3.0F};

  EXPECT_EQ(test::decode_float32_block(float32_block(values)), values);
}

// The test helper that decodes blocks must catch a malformed one, or tests could pass on garbage.
TEST(DecodeFloat32BlockTest, RejectsAnythingButOneWellFormedBlock) {
  EXPECT_NONFATAL_FAILURE((void)test::decode_float32_block(""), "not a definite-length block");
  EXPECT_NONFATAL_FAILURE((void)test::decode_float32_block("1,2"), "not a definite-length block");
  EXPECT_NONFATAL_FAILURE((void)test::decode_float32_block("#0abcd"), "not a definite-length");
  EXPECT_NONFATAL_FAILURE((void)test::decode_float32_block("#14abc"), "does not match");
  EXPECT_NONFATAL_FAILURE((void)test::decode_float32_block("#14abcde"), "does not match");
  EXPECT_NONFATAL_FAILURE((void)test::decode_float32_block("#13abc"), "does not match");
  EXPECT_NONFATAL_FAILURE((void)test::decode_float32_block("#2"), "does not match");
}

}  // namespace
}  // namespace openscope::scpi
