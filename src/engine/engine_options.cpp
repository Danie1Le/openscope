#include "engine/engine_options.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace openscope {

namespace {

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

}  // namespace

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
      options.server.address = value();
    } else if (option == "--port") {
      options.server.port = parse_number<std::uint16_t>(option, value(), 0, 65535);
    } else if (option == "--idle-timeout") {
      options.server.idle_timeout = std::chrono::seconds(parse_number(option, value(), 1, 86400));
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
