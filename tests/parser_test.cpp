#include "openscope/scpi/parser.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace openscope::scpi {
namespace {

// Shared by the parser and the command tree; everything else they do is tested through
// CommandTree in command_tree_test.cpp.
TEST(ToUpperTest, UpperCasesAsciiLettersOnly) {
  EXPECT_EQ(to_upper("Chan2:scal_x?"), "CHAN2:SCAL_X?");
  EXPECT_EQ(to_upper(""), "");
  EXPECT_EQ(to_upper("AZ az @[`{"), "AZ AZ @[`{");  // The bytes around each letter range.
  EXPECT_EQ(to_upper("\xC3\xA9"), "\xC3\xA9");      // Non-ASCII bytes pass through unchanged.
  EXPECT_EQ(to_upper(std::string_view("a\0b", 3)), std::string_view("A\0B", 3));
}

}  // namespace
}  // namespace openscope::scpi
