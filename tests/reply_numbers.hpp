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

/// The fields of a MEAS:STAT? reply, in reply order.
struct Statistics {
  double current = 0.0;
  double mean = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
  double standard_deviation = 0.0;
  double count = 0.0;
};

/// The fields of a MEAS:STAT? reply. Fails the test and returns -1 in every field when the reply
/// is not six numbers.
inline Statistics statistics_of(const std::optional<std::string>& reply) {
  const std::vector<double> fields = numbers(reply);
  if (fields.size() != 6) {
    ADD_FAILURE() << "not a statistics reply: " << reply.value_or("<no reply>");
    return {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
  }
  return {.current = fields.at(0),
          .mean = fields.at(1),
          .minimum = fields.at(2),
          .maximum = fields.at(3),
          .standard_deviation = fields.at(4),
          .count = fields.at(5)};
}

}  // namespace openscope::test
