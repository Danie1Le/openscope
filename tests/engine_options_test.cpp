#include "engine/engine_options.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openscope {
namespace {

using testing::HasSubstr;

EngineOptions parse(const std::vector<std::string_view>& arguments) {
  return parse_engine_options(arguments);
}

TEST(EngineOptionsTest, WithoutOptionsListensOnLoopbackPort5025) {
  const EngineOptions options = parse({});

  EXPECT_EQ(options.server.address, "127.0.0.1");
  EXPECT_EQ(options.server.port, 5025);
  EXPECT_EQ(options.server.idle_timeout, std::chrono::minutes(5));
  EXPECT_EQ(options.seed, 0U);
  EXPECT_FALSE(options.help);
}

TEST(EngineOptionsTest, ReadsEveryOption) {
  const EngineOptions options = parse(
      {"--address", "0.0.0.0", "--port", "0", "--idle-timeout", "86400", "--seed", "42", "-h"});

  EXPECT_EQ(options.server.address, "0.0.0.0");
  EXPECT_EQ(options.server.port, 0);
  EXPECT_EQ(options.server.idle_timeout, std::chrono::hours(24));
  EXPECT_EQ(options.seed, 42U);
  EXPECT_TRUE(options.help);
}

TEST(EngineOptionsTest, ReadsTheLargestValues) {
  const EngineOptions options =
      parse({"--port", "65535", "--idle-timeout", "1", "--seed", "18446744073709551615"});

  EXPECT_EQ(options.server.port, 65535);
  EXPECT_EQ(options.server.idle_timeout, std::chrono::seconds(1));
  EXPECT_EQ(options.seed, std::numeric_limits<std::uint64_t>::max());
}

TEST(EngineOptionsTest, HelpHasALongForm) { EXPECT_TRUE(parse({"--help"}).help); }

TEST(EngineOptionsTest, ReadsAnIpv6Address) {
  EXPECT_EQ(parse({"--address", "::1"}).server.address, "::1");
}

// The usage text names the defaults a command line without options gets.
TEST(EngineOptionsTest, UsageNamesTheDefaults) {
  const EngineOptions defaults = parse({});
  const std::string usage = engine_usage();

  EXPECT_THAT(usage, HasSubstr(std::format("(default {})", defaults.server.address)));
  EXPECT_THAT(usage, HasSubstr(std::format("(default {})", defaults.server.port)));
  EXPECT_THAT(
      usage,
      HasSubstr(std::format(
          "(default {})",
          std::chrono::duration_cast<std::chrono::seconds>(defaults.server.idle_timeout).count())));
  EXPECT_THAT(usage, HasSubstr(std::format("(default {})", defaults.seed)));
  EXPECT_THAT(usage, HasSubstr("--port <0-65535>"));
  EXPECT_THAT(usage, HasSubstr("--idle-timeout <1-86400>"));
}

class BadEngineOptionsTest
    : public testing::TestWithParam<std::pair<std::vector<std::string_view>, std::string>> {};

TEST_P(BadEngineOptionsTest, AreRejectedWithAMessageNamingTheOption) {
  const auto& [arguments, message] = GetParam();

  EXPECT_THAT([&] { parse(arguments); },
              testing::ThrowsMessage<std::invalid_argument>(testing::StrEq(message)));
}

INSTANTIATE_TEST_SUITE_P(
    Options, BadEngineOptionsTest,
    testing::Values(std::pair{std::vector<std::string_view>{"--bogus"}, "unknown option '--bogus'"},
                    std::pair{std::vector<std::string_view>{"5025"}, "unknown option '5025'"},
                    std::pair{std::vector<std::string_view>{"--port"}, "--port needs a value"},
                    std::pair{std::vector<std::string_view>{"--address"},
                              "--address needs a value"},
                    std::pair{std::vector<std::string_view>{"--address", "localhost"},
                              "--address takes an IPv4 or IPv6 address, not 'localhost'"},
                    std::pair{std::vector<std::string_view>{"--address", "999.1.1.1"},
                              "--address takes an IPv4 or IPv6 address, not '999.1.1.1'"},
                    std::pair{std::vector<std::string_view>{"--address", ""},
                              "--address takes an IPv4 or IPv6 address, not ''"},
                    std::pair{std::vector<std::string_view>{"--port", "65536"},
                              "--port takes a whole number from 0 to 65535, not '65536'"},
                    std::pair{std::vector<std::string_view>{"--port", "-1"},
                              "--port takes a whole number from 0 to 65535, not '-1'"},
                    std::pair{std::vector<std::string_view>{"--port", "50x"},
                              "--port takes a whole number from 0 to 65535, not '50x'"},
                    std::pair{std::vector<std::string_view>{"--port", ""},
                              "--port takes a whole number from 0 to 65535, not ''"},
                    std::pair{std::vector<std::string_view>{"--idle-timeout", "0"},
                              "--idle-timeout takes a whole number from 1 to 86400, not '0'"},
                    std::pair{std::vector<std::string_view>{"--idle-timeout", "86401"},
                              "--idle-timeout takes a whole number from 1 to 86400, not '86401'"},
                    std::pair{std::vector<std::string_view>{"--idle-timeout", "1.5"},
                              "--idle-timeout takes a whole number from 1 to 86400, not '1.5'"},
                    std::pair{std::vector<std::string_view>{"--seed", "18446744073709551616"},
                              "--seed takes a whole number from 0 to 18446744073709551615, not "
                              "'18446744073709551616'"}));

}  // namespace
}  // namespace openscope
