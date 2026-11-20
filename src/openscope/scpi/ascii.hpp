#pragma once

#include <string>
#include <string_view>

/// ASCII character classes and case mapping for SCPI text. They never consult the C locale, so a
/// header or keyword parses the same way on every host. The parser and the command tree share
/// them so each class is written once.
namespace openscope::scpi::ascii {

constexpr bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr bool is_letter(char c) { return is_upper(c) || is_lower(c); }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

/// Upper-cases ASCII letters only; every other byte passes through unchanged. Headers, keywords
/// and suffixes are compared upper-cased.
inline std::string to_upper(std::string_view text) {
  std::string upper(text);
  for (char& c : upper) {
    if (is_lower(c)) {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return upper;
}

}  // namespace openscope::scpi::ascii
