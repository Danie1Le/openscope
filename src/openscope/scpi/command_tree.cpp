#include "openscope/scpi/command_tree.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "openscope/scpi/parser.hpp"
#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope::scpi {
namespace {

using detail::HeaderNode;
using detail::HeaderPattern;
using detail::KeywordRule;
using detail::MnemonicPattern;
using detail::ParameterRule;

/// Visitor built from lambdas, so std::visit fails to compile when a ParameterSpec or
/// ParameterRule alternative has no case.
template <typename... Lambdas>
struct Overloaded : Lambdas... {
  using Lambdas::operator()...;
};

[[noreturn]] void reject_pattern(std::string_view pattern) {
  throw std::invalid_argument("malformed SCPI pattern: '" + std::string(pattern) + "'");
}

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

/// Parses `CHANnel<1-4>`: upper-case short form, lower-case rest, optional suffix range.
MnemonicPattern parse_mnemonic_pattern(std::string_view text) {
  const std::size_t letters_end = std::min(text.find('<'), text.size());
  const std::string_view letters = text.substr(0, letters_end);
  const auto short_end =
      static_cast<std::size_t>(std::ranges::find_if_not(letters, is_upper) - letters.begin());
  if (short_end == 0 || !std::ranges::all_of(letters.substr(short_end), is_lower)) {
    reject_pattern(text);
  }
  MnemonicPattern pattern{.short_form = std::string(letters.substr(0, short_end)),
                          .long_form = to_upper(letters),
                          .max_suffix = 0};

  const std::string_view range = text.substr(letters_end);
  if (!range.empty()) {
    constexpr std::string_view kRangeStart = "<1-";
    if (!range.starts_with(kRangeStart) || !range.ends_with('>')) {
      reject_pattern(text);
    }
    const std::string_view digits =
        range.substr(kRangeStart.size(), range.size() - kRangeStart.size() - 1);
    const char* const digits_end = digits.data() + digits.size();
    const auto result = std::from_chars(digits.data(), digits_end, pattern.max_suffix);
    if (result.ec != std::errc{} || result.ptr != digits_end || pattern.max_suffix == 0) {
      reject_pattern(text);
    }
  }
  return pattern;
}

/// Parses `*IDN?`, `SYSTem:ERRor[:NEXT]?` or `CHANnel<1-4>:SCALe`.
HeaderPattern parse_header_pattern(std::string_view text) {
  HeaderPattern header;
  std::string_view rest = text;
  header.query = rest.ends_with('?');
  if (header.query) {
    rest.remove_suffix(1);
  }
  if (rest.starts_with('*')) {
    header.common = true;
    header.nodes.push_back({.mnemonic = parse_mnemonic_pattern(rest.substr(1)), .optional = false});
    const MnemonicPattern& name = header.nodes.front().mnemonic;
    if (name.short_form != name.long_form || name.max_suffix != 0) {
      reject_pattern(text);  // Common commands have one form and no suffix.
    }
    return header;
  }
  for (bool first = true; first || !rest.empty(); first = false) {
    const bool optional = rest.starts_with("[:");
    if (optional) {
      rest.remove_prefix(2);
    } else if (rest.starts_with(':')) {
      rest.remove_prefix(1);
    } else if (!first) {
      reject_pattern(text);
    }
    const std::size_t end = std::min(rest.find_first_of(":[]"), rest.size());
    header.nodes.push_back(
        {.mnemonic = parse_mnemonic_pattern(rest.substr(0, end)), .optional = optional});
    rest.remove_prefix(end);
    if (optional) {
      if (!rest.starts_with(']')) {
        reject_pattern(text);
      }
      rest.remove_prefix(1);
    }
  }
  return header;
}

ParameterRule to_rule(const ParameterSpec& spec) {
  return std::visit(Overloaded{
                        [](const NumericParameter& numeric) -> ParameterRule { return numeric; },
                        [](const BooleanParameter& boolean) -> ParameterRule { return boolean; },
                        [](const KeywordParameter& keyword) -> ParameterRule {
                          KeywordRule rule;
                          rule.choices.reserve(keyword.choices.size());
                          for (const std::string_view choice : keyword.choices) {
                            rule.choices.push_back(parse_mnemonic_pattern(choice));
                          }
                          return rule;
                        },
                    },
                    spec);
}

/// Whether `typed` is the short or long form of `pattern`, with a suffix in its range if typed.
bool matches(const MnemonicPattern& pattern, const Mnemonic& typed) {
  if (typed.name != pattern.short_form && typed.name != pattern.long_form) {
    return false;
  }
  return !typed.suffix || (*typed.suffix >= 1 && *typed.suffix <= pattern.max_suffix);
}

/// The suffix `typed` carries for `pattern`: 1 when omitted, 0 when the pattern takes none.
unsigned suffix_of(const MnemonicPattern& pattern, const Mnemonic& typed) {
  return pattern.max_suffix == 0 ? 0 : typed.suffix.value_or(1);
}

/// Matches `path` against `nodes`, skipping optional nodes as needed, and appends the suffix of
/// every suffixed node to `suffixes`. On failure `suffixes` is left as it was. The recursion is
/// as deep as the pattern has nodes, which the command table fixes, never the input.
// NOLINTBEGIN(misc-no-recursion)
bool match_nodes(std::span<const HeaderNode> nodes, std::span<const Mnemonic> path,
                 std::vector<unsigned>& suffixes) {
  if (nodes.empty()) {
    return path.empty();
  }
  const HeaderNode& node = nodes.front();
  // Takes `suffix` for this node and matches the remaining nodes against `rest`.
  const auto match_rest = [&](unsigned suffix, std::span<const Mnemonic> rest) {
    const std::size_t mark = suffixes.size();
    if (node.mnemonic.max_suffix > 0) {
      suffixes.push_back(suffix);
    }
    if (match_nodes(nodes.subspan(1), rest, suffixes)) {
      return true;
    }
    suffixes.resize(mark);
    return false;
  };
  if (!path.empty() && matches(node.mnemonic, path.front()) &&
      match_rest(suffix_of(node.mnemonic, path.front()), path.subspan(1))) {
    return true;
  }
  return node.optional && match_rest(1, path);  // Omitted: its suffix defaults to 1.
}
// NOLINTEND(misc-no-recursion)

bool matches_header(const HeaderPattern& header, const ParsedCommand& command,
                    std::vector<unsigned>& suffixes) {
  return header.common == command.common && header.query == command.query &&
         match_nodes(header.nodes, command.path, suffixes);
}

/// IEEE 488.2 suffix multipliers, as powers of ten.
struct Multiplier {
  std::string_view prefix;
  int exponent;
};
constexpr std::array kMultipliers{
    Multiplier{.prefix = "EX", .exponent = 18}, Multiplier{.prefix = "PE", .exponent = 15},
    Multiplier{.prefix = "T", .exponent = 12},  Multiplier{.prefix = "G", .exponent = 9},
    Multiplier{.prefix = "MA", .exponent = 6},  Multiplier{.prefix = "K", .exponent = 3},
    Multiplier{.prefix = "M", .exponent = -3},  Multiplier{.prefix = "U", .exponent = -6},
    Multiplier{.prefix = "N", .exponent = -9},  Multiplier{.prefix = "P", .exponent = -12},
    Multiplier{.prefix = "F", .exponent = -15}, Multiplier{.prefix = "A", .exponent = -18},
};

/// The power of ten `suffix` applies for `unit`, or nullopt when `suffix` is not `unit` behind
/// an optional multiplier. No suffix means the base unit.
std::optional<int> unit_exponent(std::string_view suffix, std::string_view unit) {
  if (suffix.empty()) {
    return 0;
  }
  if (unit.empty() || !suffix.ends_with(unit)) {
    return std::nullopt;
  }
  const std::string_view prefix = suffix.substr(0, suffix.size() - unit.size());
  if (prefix.empty()) {
    return 0;
  }
  if (prefix == "M" && unit == "HZ") {
    return 6;  // IEEE 488.2: suffixes are case-insensitive, so MHZ means megahertz.
  }
  const auto* multiplier = std::ranges::find(kMultipliers, prefix, &Multiplier::prefix);
  if (multiplier == kMultipliers.end()) {
    return std::nullopt;
  }
  return multiplier->exponent;
}

/// `value` times 10^`exponent`. Powers of ten up to 10^22 are exact doubles, so one multiply or
/// divide rounds once: 10 mV is exactly 0.01, which 10 * 0.001 is not.
double scale(double value, int exponent) {
  constexpr std::array kPowersOfTen{1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8, 1e9,
                                    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
  const double power = kPowersOfTen.at(static_cast<std::size_t>(std::abs(exponent)));
  return exponent < 0 ? value / power : value * power;
}

using Conversion = std::variant<Argument, ScpiError>;

Conversion to_number(const NumericParameter& spec, const ProgramData& data) {
  const auto* number = std::get_if<Number>(&data);
  if (number == nullptr) {
    return kDataTypeError;
  }
  const std::optional<int> exponent = unit_exponent(number->suffix, spec.unit);
  if (!exponent) {
    return kInvalidSuffix;
  }
  const double value = scale(number->value, *exponent);
  if (std::isnan(value) || value < spec.min || value > spec.max) {
    return kDataOutOfRange;
  }
  return Argument{value};
}

Conversion to_boolean(const ProgramData& data) {
  if (const auto* keyword = std::get_if<Mnemonic>(&data)) {
    if (!keyword->suffix && (keyword->name == "ON" || keyword->name == "OFF")) {
      return Argument{keyword->name == "ON"};
    }
    return kIllegalParameterValue;
  }
  const auto& number = std::get<Number>(data);
  if (!number.suffix.empty()) {
    return kInvalidSuffix;
  }
  if (std::isnan(number.value)) {
    return kDataOutOfRange;
  }
  return Argument{std::round(number.value) != 0.0};
}

Conversion to_choice(const KeywordRule& rule, const ProgramData& data) {
  const auto* keyword = std::get_if<Mnemonic>(&data);
  if (keyword == nullptr) {
    return kDataTypeError;
  }
  for (std::size_t i = 0; i < rule.choices.size(); ++i) {
    if (matches(rule.choices[i], *keyword)) {
      return Argument{Choice{.index = i, .suffix = suffix_of(rule.choices[i], *keyword)}};
    }
  }
  return kIllegalParameterValue;
}

Conversion convert(const ParameterRule& rule, const ProgramData& data) {
  return std::visit(Overloaded{
                        [&](const NumericParameter& numeric) { return to_number(numeric, data); },
                        [&](const BooleanParameter&) { return to_boolean(data); },
                        [&](const KeywordRule& keyword) { return to_choice(keyword, data); },
                    },
                    rule);
}

/// Converts every parameter into `values`, or returns the first error: the count is checked
/// before any value.
std::optional<ScpiError> convert_all(const std::vector<ParameterRule>& rules,
                                     const std::vector<ProgramData>& parameters,
                                     std::vector<Argument>& values) {
  if (parameters.size() > rules.size()) {
    return kParameterNotAllowed;
  }
  if (parameters.size() < rules.size()) {
    return kMissingParameter;
  }
  values.reserve(rules.size());
  for (std::size_t i = 0; i < rules.size(); ++i) {
    Conversion converted = convert(rules[i], parameters[i]);
    if (auto* error = std::get_if<ScpiError>(&converted)) {
      return std::move(*error);
    }
    values.push_back(std::get<Argument>(converted));
  }
  return std::nullopt;
}

}  // namespace

void CommandTree::add(Command command) {
  if (!command.handler) {
    throw std::invalid_argument("SCPI command without a handler: '" + std::string(command.header) +
                                "'");
  }
  Entry entry{.header = parse_header_pattern(command.header),
              .parameters = {},
              .handler = std::move(command.handler)};
  entry.parameters.reserve(command.parameters.size());
  for (const ParameterSpec& spec : command.parameters) {
    entry.parameters.push_back(to_rule(spec));
  }
  entries_.push_back(std::move(entry));
}

std::optional<std::string> CommandTree::execute(Session& session, std::string_view line) const {
  std::optional<std::vector<ParsedCommand>> commands = parse_line(line);
  if (!commands) {
    session.push_error(kSyntaxError);
    return std::nullopt;
  }

  std::optional<std::string> reply;
  // SCPI-99 compound headers: a header without a leading ':' continues from the path of the
  // previous command (all but its last node). Common commands leave the path alone.
  std::vector<Mnemonic> current_path;
  for (ParsedCommand& command : *commands) {
    if (!command.common && !command.absolute) {
      command.path.insert(command.path.begin(), current_path.begin(), current_path.end());
    }
    Arguments arguments;
    const auto entry = std::ranges::find_if(entries_, [&](const Entry& candidate) {
      return matches_header(candidate.header, command, arguments.suffixes);
    });
    if (entry == entries_.end()) {
      session.push_error(kUndefinedHeader);
      break;
    }
    if (!command.common) {
      current_path.assign(command.path.begin(), command.path.end() - 1);
    }
    if (std::optional<ScpiError> error =
            convert_all(entry->parameters, command.parameters, arguments.values)) {
      session.push_error(std::move(*error));
      break;
    }
    if (std::optional<std::string> answer = entry->handler(session, arguments)) {
      if (reply) {
        reply->append(1, ';').append(*answer);
      } else {
        reply = std::move(answer);
      }
    }
  }
  return reply;
}

}  // namespace openscope::scpi
