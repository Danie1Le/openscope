#include "engine/engine_options.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/system/detail/error_code.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>

namespace openscope {

namespace {

/// The largest `--port`.
constexpr std::uint16_t kMaxPort = std::numeric_limits<std::uint16_t>::max();
/// The `--idle-timeout` range, in seconds.
constexpr std::chrono::seconds::rep kMinIdleTimeout = 1;
/// See kMinIdleTimeout: one day.
constexpr std::chrono::seconds::rep kMaxIdleTimeout = 86400;

/// `text` as a whole number from `min` to `max`. Throws std::invalid_argument naming `option`.
template <typename Number>
Number parse_number(std::string_view option, std::string_view text, Number min, Number max) {
  Number value{};
  const auto [end, error] = std::from_chars(text.data(), std::to_address(text.end()), value);
  if (error != std::errc() || end != std::to_address(text.end()) || value < min || value > max) {
    throw std::invalid_argument(
        std::format("{} takes a whole number from {} to {}, not '{}'", option, min, max, text));
  }
  return value;
}

/// `text` if it is an IP address, as ScpiServer reads it. Throws std::invalid_argument naming
/// `option` otherwise.
std::string parse_address(std::string_view option, std::string_view text) {
  std::string address(text);
  boost::system::error_code error;
  std::ignore = boost::asio::ip::make_address(address, error);
  if (error) {
    throw std::invalid_argument(
        std::format("{} takes an IPv4 or IPv6 address, not '{}'", option, text));
  }
  return address;
}

}  // namespace

std::string engine_usage() {
  const EngineOptions defaults;
  return std::format(
      "Usage: openscope-engine [options]\n"
      "\n"
      "Runs the OpenScope instrument live and serves SCPI over TCP. Logs go to standard error as\n"
      "one JSON object per line. Stop it with Ctrl+C (SIGINT) or SIGTERM.\n"
      "\n"
      "Options:\n"
      "  --address <ip>          Local IP address to listen on (default {}). Any other\n"
      "                          address exposes the instrument, which has no authentication,\n"
      "                          to the network. Host names are not resolved.\n"
      "  --port <0-{}>        TCP port (default {}); 0 picks a free port, logged at start.\n"
      "  --idle-timeout <{}-{}>\n"
      "                          Seconds a client may stay silent before it is disconnected\n"
      "                          (default {}). A client waiting on *OPC? is never idle.\n"
      "  --seed <n>              Seed of the generators' noise (default {}).\n"
      "  -h, --help              Print this help and exit.\n",
      defaults.server.address, kMaxPort, defaults.server.port, kMinIdleTimeout, kMaxIdleTimeout,
      std::chrono::duration_cast<std::chrono::seconds>(defaults.server.idle_timeout).count(),
      defaults.seed);
}

EngineOptions parse_engine_options(std::span<const std::string_view> arguments) {
  EngineOptions options;
  for (auto argument = arguments.begin(); argument != arguments.end(); ++argument) {
    const std::string_view option = *argument;
    const auto value = [&] {
      if (++argument == arguments.end()) {
        throw std::invalid_argument(std::format("{} needs a value", option));
      }
      return *argument;
    };
    if (option == "-h" || option == "--help") {
      options.help = true;
    } else if (option == "--address") {
      options.server.address = parse_address(option, value());
    } else if (option == "--port") {
      options.server.port = parse_number<std::uint16_t>(option, value(), 0, kMaxPort);
    } else if (option == "--idle-timeout") {
      options.server.idle_timeout =
          std::chrono::seconds(parse_number(option, value(), kMinIdleTimeout, kMaxIdleTimeout));
    } else if (option == "--seed") {
      options.seed = parse_number<std::uint64_t>(option, value(), 0,
                                                 std::numeric_limits<std::uint64_t>::max());
    } else {
      throw std::invalid_argument(std::format("unknown option '{}'", option));
    }
  }
  return options;
}

}  // namespace openscope
