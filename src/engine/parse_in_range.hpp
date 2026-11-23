#pragma once

#include <charconv>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace openscope {

/// `text` as a number from `min` to `max`, all of it; nullopt for anything else (a sign the type
/// does not take, a suffix, a second value, white space, NaN). How the engine and the benchmarks
/// read numbers from the command line, so only numbers parsed this way reach the instrument.
template <typename Number>
std::optional<Number> parse_in_range(std::string_view text, Number min, Number max) {
  Number value{};
  const char* const end = std::to_address(text.cend());
  const auto [stop, error] = std::from_chars(text.data(), end, value);
  const bool whole = error == std::errc{} && stop == end;
  if (!whole || !(value >= min && value <= max)) {  // NaN fails both comparisons.
    return std::nullopt;
  }
  return value;
}

}  // namespace openscope
