#pragma once

#include <concepts>
#include <format>
#include <string>

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

}  // namespace openscope::scpi
