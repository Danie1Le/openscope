#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "openscope/scpi/ascii.hpp"

namespace openscope::test {

/// Decodes an IEEE 488.2 definite-length block of little-endian float32 values, as a client such as
/// PyVISA's query_binary_values does. Fails the test and returns no values when `reply` is not
/// exactly one well-formed block.
inline std::vector<float> decode_float32_block(std::string_view reply) {
  if (reply.size() < 2 || reply[0] != '#' || !scpi::ascii::is_digit(reply[1]) || reply[1] == '0') {
    ADD_FAILURE() << "not a definite-length block";
    return {};
  }
  const auto digits = static_cast<std::size_t>(reply[1] - '0');
  std::size_t length = 0;
  const std::string_view length_text = reply.substr(2, digits);
  const auto parsed =
      std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
  const std::string_view bytes = reply.substr(std::min(reply.size(), 2 + digits));
  if (length_text.size() != digits || parsed.ptr != length_text.data() + length_text.size() ||
      bytes.size() != length || length % 4 != 0) {
    ADD_FAILURE() << "block length does not match its bytes";
    return {};
  }
  std::vector<float> values;
  for (std::size_t i = 0; i < bytes.size(); i += 4) {
    std::uint32_t bits = 0;
    for (std::size_t b = 0; b < 4; ++b) {
      bits |= std::uint32_t{static_cast<unsigned char>(bytes[i + b])} << (8U * b);
    }
    values.push_back(std::bit_cast<float>(bits));
  }
  return values;
}

}  // namespace openscope::test
