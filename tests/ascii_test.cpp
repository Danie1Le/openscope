#include "openscope/scpi/ascii.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace openscope::scpi::ascii {
namespace {

TEST(AsciiTest, CharacterClassesCoverExactlyTheirRanges) {
  // The bytes either side of each range, and a non-ASCII byte, fall outside it.
  EXPECT_TRUE(is_upper('A') && is_upper('Z'));
  EXPECT_FALSE(is_upper('@') || is_upper('[') || is_upper('a') || is_upper('\xC3'));
  EXPECT_TRUE(is_lower('a') && is_lower('z'));
  EXPECT_FALSE(is_lower('`') || is_lower('{') || is_lower('A') || is_lower('\xC3'));
  EXPECT_TRUE(is_letter('A') && is_letter('z'));
  EXPECT_FALSE(is_letter('0') || is_letter('_') || is_letter('\0'));
  EXPECT_TRUE(is_digit('0') && is_digit('9'));
  EXPECT_FALSE(is_digit('/') || is_digit(':') || is_digit('\0'));
}

TEST(AsciiTest, ToUpperUpperCasesAsciiLettersOnly) {
  EXPECT_EQ(to_upper("Chan2:scal_x?"), "CHAN2:SCAL_X?");
  EXPECT_EQ(to_upper(""), "");
  EXPECT_EQ(to_upper("AZ az @[`{"), "AZ AZ @[`{");  // The bytes around each letter range.
  EXPECT_EQ(to_upper("\xC3\xA9"), "\xC3\xA9");      // Non-ASCII bytes pass through unchanged.
  EXPECT_EQ(to_upper(std::string_view("a\0b", 3)), std::string_view("A\0B", 3));
}

}  // namespace
}  // namespace openscope::scpi::ascii
