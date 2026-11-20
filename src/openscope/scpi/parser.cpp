#include "openscope/scpi/parser.hpp"

#include <charconv>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The grammar, after IEEE 488.2 and SCPI-99, limited to the data types OpenScope commands take:
//
//   line      = ws* [ command { ws* ";" ws* command } ] ws*
//   command   = header [ ws+ parameter { ws* "," ws* parameter } ]
//   header    = "*" mnemonic [ "?" ]
//             | [ ":" ] mnemonic { ":" mnemonic } [ "?" ]
//   mnemonic  = letter { letter | digit | "_" }      trailing digits are the numeric suffix
//   parameter = number | mnemonic
//   number    = [ "+" | "-" ] ( digit+ [ "." digit* ] | "." digit+ )
//               [ ( "E" | "e" ) [ "+" | "-" ] digit+ ] [ ws* letter+ ]
//   ws        = " " | "\t"
//
// Strings, block data and non-decimal numbers are not part of it, so they are syntax errors.

namespace openscope::scpi {
namespace {

bool is_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || c == '\t'; }
bool is_mnemonic_char(char c) { return is_letter(c) || is_digit(c) || c == '_'; }

/// A read position in one line.
class Scanner {
 public:
  explicit Scanner(std::string_view text) : text_(text) {}

  [[nodiscard]] bool at_end() const { return pos_ == text_.size(); }
  [[nodiscard]] std::size_t position() const { return pos_; }
  [[nodiscard]] std::string_view since(std::size_t start) const {
    return text_.substr(start, pos_ - start);
  }

  /// The character `ahead` places past the read position, or '\0' past the end. Callers only
  /// test it with the character-class predicates above, which all reject '\0'.
  [[nodiscard]] char peek(std::size_t ahead = 0) const {
    return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
  }

  /// Consumes `c` if it is next.
  bool accept(char c) {
    if (at_end() || text_[pos_] != c) {
      return false;
    }
    ++pos_;
    return true;
  }

  template <typename Predicate>
  std::string_view take_while(Predicate predicate) {
    const std::size_t start = pos_;
    while (!at_end() && predicate(text_[pos_])) {
      ++pos_;
    }
    return since(start);
  }

  void skip_space() { take_while(is_space); }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

std::optional<std::string_view> scan_mnemonic(Scanner& scanner) {
  if (!is_letter(scanner.peek())) {
    return std::nullopt;
  }
  return scanner.take_while(is_mnemonic_char);
}

/// Splits trailing digits off `token`, which starts with a letter.
Mnemonic split_suffix(std::string_view token) {
  const std::size_t name_end = token.find_last_not_of("0123456789") + 1;
  Mnemonic mnemonic{.name = to_upper(token.substr(0, name_end)), .suffix = std::nullopt};
  if (name_end < token.size()) {
    const std::string_view digits = token.substr(name_end);
    unsigned suffix = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), suffix);
    mnemonic.suffix = result.ec == std::errc{} ? suffix : std::numeric_limits<unsigned>::max();
  }
  return mnemonic;
}

bool scan_header(Scanner& scanner, ParsedCommand& command) {
  if (scanner.accept('*')) {
    const auto name = scan_mnemonic(scanner);
    if (!name) {
      return false;
    }
    command.common = true;
    command.path.push_back(Mnemonic{.name = to_upper(*name), .suffix = std::nullopt});
  } else {
    command.absolute = scanner.accept(':');
    for (bool more = true; more; more = scanner.accept(':')) {
      const auto token = scan_mnemonic(scanner);
      if (!token) {
        return false;
      }
      command.path.push_back(split_suffix(*token));
    }
  }
  command.query = scanner.accept('?');
  return true;
}

void accept_sign(Scanner& scanner) {
  if (!scanner.accept('+')) {
    scanner.accept('-');
  }
}

std::optional<Number> scan_number(Scanner& scanner) {
  const std::size_t start = scanner.position();
  accept_sign(scanner);
  const std::string_view whole = scanner.take_while(is_digit);
  std::string_view fraction;
  if (scanner.accept('.')) {
    fraction = scanner.take_while(is_digit);
  }
  if (whole.empty() && fraction.empty()) {
    return std::nullopt;
  }
  // An `E` starts an exponent only when digits follow; otherwise it begins a suffix such as `EX`.
  const char after_e = scanner.peek(1);
  if ((scanner.peek() == 'E' || scanner.peek() == 'e') &&
      (is_digit(after_e) || ((after_e == '+' || after_e == '-') && is_digit(scanner.peek(2))))) {
    scanner.accept(scanner.peek());
    accept_sign(scanner);
    scanner.take_while(is_digit);
  }

  std::string_view text = scanner.since(start);
  if (text.front() == '+') {
    text.remove_prefix(1);  // from_chars takes no leading '+'.
  }
  Number number{.value = 0.0, .suffix = {}};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), number.value);
  if (result.ec != std::errc{}) {
    // Beyond a double's range, too large (1E999) or too small (1E-999): NaN, which no range
    // accepts, so the number is reported out of range.
    number.value = std::numeric_limits<double>::quiet_NaN();
  }
  scanner.skip_space();
  number.suffix = to_upper(scanner.take_while(is_letter));
  return number;
}

std::optional<ProgramData> scan_parameter(Scanner& scanner) {
  if (const auto token = scan_mnemonic(scanner)) {
    return split_suffix(*token);
  }
  if (auto number = scan_number(scanner)) {
    return std::move(*number);
  }
  return std::nullopt;
}

std::optional<ParsedCommand> scan_command(Scanner& scanner) {
  ParsedCommand command;
  if (!scan_header(scanner, command)) {
    return std::nullopt;
  }
  const std::string_view separator = scanner.take_while(is_space);
  if (scanner.at_end() || scanner.peek() == ';') {
    return command;
  }
  if (separator.empty()) {
    return std::nullopt;  // Parameters must be separated from the header by whitespace.
  }
  for (bool more = true; more; more = scanner.accept(',')) {
    scanner.skip_space();
    auto parameter = scan_parameter(scanner);
    if (!parameter) {
      return std::nullopt;
    }
    command.parameters.push_back(std::move(*parameter));
    scanner.skip_space();
  }
  return command;
}

}  // namespace

std::optional<std::vector<ParsedCommand>> parse_line(std::string_view line) {
  Scanner scanner{line};
  std::vector<ParsedCommand> commands;
  scanner.skip_space();
  if (scanner.at_end()) {
    return commands;
  }
  for (bool more = true; more; more = scanner.accept(';')) {
    scanner.skip_space();
    auto command = scan_command(scanner);
    if (!command) {
      return std::nullopt;
    }
    commands.push_back(std::move(*command));
    scanner.skip_space();
  }
  if (!scanner.at_end()) {
    return std::nullopt;
  }
  return commands;
}

std::string to_upper(std::string_view text) {
  std::string upper(text);
  for (char& c : upper) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return upper;
}

}  // namespace openscope::scpi
