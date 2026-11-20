#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace openscope::scpi {

/// A header node or keyword as typed, split into its upper-cased name and its trailing numeric
/// suffix: `chan2` is `{"CHAN", 2}`, `TRIGger` is `{"TRIGGER", nullopt}`.
struct Mnemonic {
  std::string name;
  /// The typed suffix; a suffix too large for `unsigned` is held as its maximum, which no command
  /// accepts.
  std::optional<unsigned> suffix;
};

/// A decimal number as typed, with its unit suffix (upper-cased, empty when there is none). The
/// suffix is checked against the command's unit later, when the parameter is converted.
struct Number {
  double value;
  std::string suffix;
};

/// One parameter (program data element): a decimal number or a keyword.
using ProgramData = std::variant<Number, Mnemonic>;

/// One command (program message unit) of a line, parsed but not yet matched to a command.
struct ParsedCommand {
  /// An IEEE 488.2 common command such as `*IDN?`; its path holds one mnemonic with no suffix.
  bool common = false;
  /// The header started with `:`, so it is resolved from the root, not the current path.
  bool absolute = false;
  std::vector<Mnemonic> path;
  bool query = false;
  std::vector<ProgramData> parameters;
};

/// Splits one line (terminator already stripped) into its commands, or returns nullopt when the
/// line breaks the grammar (written out at the top of parser.cpp). An empty or blank line has no
/// commands.
std::optional<std::vector<ParsedCommand>> parse_line(std::string_view line);

/// Upper-cases ASCII letters only, so the result never depends on the C locale.
std::string to_upper(std::string_view text);

}  // namespace openscope::scpi
