#include "openscope/server/json_log.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace openscope {
namespace {

TEST(JsonLogTest, WritesOneJsonObjectPerLineWithTimeAndEventFirst) {
  std::ostringstream out;
  JsonLog log(out);

  log.write("connect", {{"peer", "127.0.0.1:4000"}, {"session", 7}});
  log.write("listening");

  // POSIX extended regular expressions (gtest on Linux): no `\d`.
  EXPECT_THAT(out.str(),
              testing::MatchesRegex(
                  R"(\{"time":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z",)"
                  R"("event":"connect","peer":"127\.0\.0\.1:4000","session":7\})"
                  "\n"
                  R"(\{"time":"[^"]+","event":"listening"\})"
                  "\n"));
}

// Text from a client or the OS cannot break a line or the JSON around it.
TEST(JsonLogTest, EscapesTextAndReplacesInvalidUtf8) {
  std::ostringstream out;
  JsonLog log(out);

  log.write("error", {{"error", "a\"b\nc\xff"}});

  const std::string text = out.str();
  EXPECT_EQ(text.find('\n'), text.size() - 1);
  EXPECT_EQ(nlohmann::json::parse(text)["error"], "a\"b\nc\xef\xbf\xbd");  // U+FFFD
}

}  // namespace
}  // namespace openscope
