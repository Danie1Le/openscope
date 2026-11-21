#pragma once

#include <concepts>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "openscope/scpi/ascii.hpp"

/// Response data: how query replies write values, so every query writes them the same way.
namespace openscope::scpi {

/// `value` in the shortest form that reads back as exactly the same number, with an upper-case
/// exponent as IEEE 488.2 writes it: `1000`, `0.001`, `1E+06`, `2.5E-07`. Floats get the shortest
/// form that reads back as the same float.
template <std::floating_point Real>
std::string format_real(Real value) {
  return ascii::to_upper(std::format("{}", value));
}

/// SCPI-99's not-a-number value, which a query replies in place of a value it does not have.
inline constexpr std::string_view kNotANumber = "9.91E37";

/// The most bytes a definite-length block can carry: its length is at most nine digits.
inline constexpr std::size_t kMaxBlockBytes = 999'999'999;

/// The header of an IEEE 488.2 definite-length block of `length` bytes: `#`, the number of digits
/// in the length, then the length (`#212` for 12 bytes, `#10` for none). Throws std::length_error
/// when `length` is over kMaxBlockBytes.
std::string definite_length_header(std::size_t length);

/// `values` as a definite-length block of IEEE 754 float32, least significant byte first, the
/// byte order PyVISA's `query_binary_values` reads by default. Throws std::length_error when the
/// bytes would not fit in one block (over 249,999,999 values).
std::string float32_block(std::span<const float> values);

}  // namespace openscope::scpi
