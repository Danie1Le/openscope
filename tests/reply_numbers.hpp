#pragma once

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openscope::test {

/// The comma-separated numbers of a query reply, as a client parses them: `float` for samples
/// (`WAV:DATA?` in ASCII), `double` for anything else. Fails the test and
/// returns no numbers when the reply is missing or holds anything but numbers.
template <typename Number = double>
std::vector<Number> numbers(const std::optional<std::string>& reply) {
  if (!reply || reply->empty()) {
    ADD_FAILURE() << "no reply";
    return {};
  }
  std::vector<Number> values;
  std::string_view rest = *reply;
  for (bool more = true; more;) {
    const std::size_t comma = rest.find(',');
    const std::string_view field = rest.substr(0, comma);
    Number value = std::numeric_limits<Number>::quiet_NaN();
    const char* const end = std::to_address(field.cend());
    if (field.empty() || std::from_chars(field.data(), end, value).ptr != end) {
      ADD_FAILURE() << "not numbers: " << *reply;
      return {};
    }
    values.push_back(value);
    more = comma != std::string_view::npos;
    rest.remove_prefix(more ? comma + 1 : rest.size());
  }
  return values;
}

/// The one number a query replied. Fails the test and returns NaN when the reply is missing or not
/// one number.
inline double number(const std::optional<std::string>& reply) {
  const std::vector<double> values = numbers(reply);
  if (values.size() != 1) {
    ADD_FAILURE() << "not one number: " << reply.value_or("");
    return std::nan("");
  }
  return values.front();
}

}  // namespace openscope::test
