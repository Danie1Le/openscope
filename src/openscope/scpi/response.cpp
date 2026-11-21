#include "openscope/scpi/response.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace openscope::scpi {

std::string definite_length_header(std::size_t length) {
  if (length > kMaxBlockBytes) {
    throw std::length_error("definite-length block over 999999999 bytes");
  }
  const std::string digits = std::to_string(length);
  return "#" + std::to_string(digits.size()) + digits;
}

std::string float32_block(std::span<const float> values) {
  constexpr std::size_t kBytesPerValue = 4;
  static_assert(sizeof(float) == kBytesPerValue && std::numeric_limits<float>::is_iec559);
  std::string block = definite_length_header(values.size() * kBytesPerValue);
  std::size_t at = block.size();
  block.resize(at + (values.size() * kBytesPerValue));
  for (const float value : values) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (std::size_t byte = 0; byte < kBytesPerValue; ++byte) {
      block[at++] = static_cast<char>((bits >> (8U * byte)) & 0xFFU);
    }
  }
  return block;
}

}  // namespace openscope::scpi
