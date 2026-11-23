#include "openscope/scpi/command_tree.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "openscope/scpi/ascii.hpp"
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

/// Fails registration: a mistake in the command table must stop start-up.
[[noreturn]] void reject(std::string_view problem, std::string_view text) {
  throw std::invalid_argument(std::string(problem) + ": '" + std::string(text) + "'");
}

[[noreturn]] void reject_pattern(std::string_view pattern) {
  reject("malformed SCPI pattern", pattern);
}

/// Parses `CHANnel<1-4>`: upper-case short form, lower-case rest, optional suffix range.
MnemonicPattern parse_mnemonic_pattern(std::string_view text) {
  const std::size_t letters_end = std::min(text.find('<'), text.size());
  const std::string_view letters = text.substr(0, letters_end);
  const std::string_view short_letters = short_form(letters);
  if (short_letters.empty() ||
      !std::ranges::all_of(letters.substr(short_letters.size()), ascii::is_lower)) {
    reject_pattern(text);
  }
  MnemonicPattern pattern{.short_form = std::string(short_letters),
                          .long_form = ascii::to_upper(letters),
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

/// Parses and checks one parameter of the command with pattern `header`.
ParameterRule to_rule(const ParameterSpec& spec, std::string_view header) {
  return std::visit(Overloaded{
                        [&](const NumericParameter& numeric) -> ParameterRule {
                          // A unit such as `Hz` would never match the upper-cased suffix, and
                          // bounds that are NaN or reversed would reject every value.
                          if (!std::ranges::all_of(numeric.unit, ascii::is_upper) ||
                              !std::isfinite(numeric.min) || !std::isfinite(numeric.max) ||
                              numeric.min > numeric.max) {
                            reject("malformed numeric parameter", header);
                          }
                          return numeric;
                        },
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

/// Whether a typed suffix must lie in the pattern's range, or only be on a node that takes one.
/// The second tells -114 "Header suffix out of range" apart from -113 "Undefined header".
enum class SuffixRange : std::uint8_t { kChecked, kIgnored };

/// Whether `typed` is the short or long form of `pattern`, with a suffix in its range if typed.
bool matches(const MnemonicPattern& pattern, const Mnemonic& typed,
             SuffixRange range = SuffixRange::kChecked) {
  if (typed.name != pattern.short_form && typed.name != pattern.long_form) {
    return false;
  }
  if (!typed.suffix) {
    return true;
  }
  if (range == SuffixRange::kIgnored) {
    return pattern.max_suffix > 0;
  }
  return *typed.suffix >= 1 && *typed.suffix <= pattern.max_suffix;
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
                 SuffixRange range, std::vector<unsigned>& suffixes) {
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
    if (match_nodes(nodes.subspan(1), rest, range, suffixes)) {
      return true;
    }
    suffixes.resize(mark);
    return false;
  };
  if (!path.empty() && matches(node.mnemonic, path.front(), range) &&
      match_rest(suffix_of(node.mnemonic, path.front()), path.subspan(1))) {
    return true;
  }
  return node.optional && match_rest(1, path);  // Omitted: its suffix defaults to 1.
}
// NOLINTEND(misc-no-recursion)

bool matches_header(const HeaderPattern& header, const ParsedCommand& command, SuffixRange range,
                    std::vector<unsigned>& suffixes) {
  return header.common == command.common && header.query == command.query &&
         match_nodes(header.nodes, command.path, range, suffixes);
}

/// Whether the header of any of `entries` (CommandTree entries) matches `command`.
template <typename Entries>
bool any_header_matches(const Entries& entries, const ParsedCommand& command, SuffixRange range) {
  std::vector<unsigned> unused_suffixes;
  return std::ranges::any_of(entries, [&](const auto& entry) {
    return matches_header(entry.header, command, range, unused_suffixes);
  });
}

/// Every path `header` accepts with each node typed in its short or long form and no suffix,
/// optional nodes present and absent. A typed node without a suffix matches any suffix range, so
/// another header can match some input of `header` exactly when it matches one of these paths.
std::vector<ParsedCommand> spellings(const HeaderPattern& header) {
  std::vector<std::vector<Mnemonic>> paths{{}};
  for (const HeaderNode& node : header.nodes) {
    std::vector<std::vector<Mnemonic>> longer;
    for (const std::vector<Mnemonic>& path : paths) {
      if (node.optional) {
        longer.push_back(path);
      }
      for (const std::string* name : {&node.mnemonic.short_form, &node.mnemonic.long_form}) {
        longer.push_back(path);
        longer.back().push_back(Mnemonic{.name = *name, .suffix = std::nullopt});
      }
    }
    paths = std::move(longer);
  }
  std::vector<ParsedCommand> commands;
  commands.reserve(paths.size());
  for (std::vector<Mnemonic>& path : paths) {
    commands.push_back({.common = header.common,
                        .absolute = true,
                        .path = std::move(path),
                        .query = header.query,
                        .parameters = {}});
  }
  return commands;
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
  if (std::holds_alternative<UnsupportedData>(data)) {
    return kDataTypeError;  // No parameter kind takes a string, radix number or block.
  }
  return std::visit(Overloaded{
                        [&](const NumericParameter& numeric) { return to_number(numeric, data); },
                        [&](const BooleanParameter&) { return to_boolean(data); },
                        [&](const KeywordRule& keyword) { return to_choice(keyword, data); },
                    },
                    rule);
}

/// Converts every parameter sent into `values`, or returns the first error: the count (at least
/// `required`, at most one per rule) is checked before any value.
std::optional<ScpiError> convert_all(const std::vector<ParameterRule>& rules, std::size_t required,
                                     const std::vector<ProgramData>& parameters,
                                     std::vector<Argument>& values) {
  if (parameters.size() > rules.size()) {
    return kParameterNotAllowed;
  }
  if (parameters.size() < required) {
    return kMissingParameter;
  }
  values.reserve(parameters.size());
  for (std::size_t i = 0; i < parameters.size(); ++i) {
    Conversion converted = convert(rules[i], parameters[i]);
    if (auto* error = std::get_if<ScpiError>(&converted)) {
      return std::move(*error);
    }
    values.push_back(std::get<Argument>(converted));
  }
  return std::nullopt;
}

/// Joins `answer`, a query's reply if it has one, to `replies`, the line's replies so far. When the
/// joined replies would pass kMaxReplyLength it drops `answer`, puts `session` back to `before`
/// (the Session as the query found it, if given) and queues -223, and returns false.
bool join_reply(Session& session, std::optional<Session> before,
                std::optional<std::string>& replies, std::optional<std::string> answer) {
  if (!answer) {
    return true;
  }
  const std::size_t joined = replies ? replies->size() + 1 : 0;
  if (joined + answer->size() > kMaxReplyLength) {
    if (before) {
      session = std::move(*before);
    }
    session.push_error(kTooMuchData);
    return false;
  }
  if (replies) {
    replies->append(1, ';').append(*answer);
  } else {
    replies = std::move(answer);
  }
  return true;
}

}  // namespace

std::string_view short_form(std::string_view pattern) {
  return pattern.substr(
      0, static_cast<std::size_t>(std::ranges::find_if_not(pattern, ascii::is_upper) -
                                  pattern.begin()));
}

void CommandTree::add(Command command) {
  if (!command.handler) {
    reject("SCPI command without a handler", command.header);
  }
  if (command.optional_parameters > command.parameters.size()) {
    reject("more optional parameters than parameters", command.header);
  }
  Entry entry{.header = parse_header_pattern(command.header),
              .parameters = {},
              .required_parameters = command.parameters.size() - command.optional_parameters,
              .handler = std::move(command.handler)};
  for (const ParsedCommand& spelling : spellings(entry.header)) {
    if (any_header_matches(entries_, spelling, SuffixRange::kChecked)) {
      reject("SCPI header overlaps one already added", command.header);
    }
  }
  entry.parameters.reserve(command.parameters.size());
  for (const ParameterSpec& spec : command.parameters) {
    entry.parameters.push_back(to_rule(spec, command.header));
  }
  entries_.push_back(std::move(entry));
}

Execution CommandTree::execute(Session& session, std::string_view line) const {
  Execution execution;
  std::optional<std::vector<ParsedCommand>> commands = parse_line(line);
  if (!commands) {
    session.push_error(kSyntaxError);
    return execution;
  }
  execution.commands_ = std::move(*commands);
  resume(session, execution);
  return execution;
}

void CommandTree::resume(Session& session, Execution& execution) const {
  std::vector<ParsedCommand>& commands = execution.commands_;
  bool fits = true;
  if (execution.hold_ && !execution.hold_->again) {
    fits = join_reply(session, std::nullopt, execution.reply_, std::move(execution.hold_->reply));
    ++execution.next_;
  }
  execution.hold_.reset();
  for (; fits && execution.next_ < commands.size(); ++execution.next_) {
    ParsedCommand& command = commands[execution.next_];
    // SCPI-99 compound headers: a header without a leading ':' continues from the path of the
    // previous command (all but its last node). Common commands leave the path alone.
    if (!command.common && !command.absolute) {
      command.path.insert(command.path.begin(), execution.path_.begin(), execution.path_.end());
      command.absolute = true;  // Its whole path now, should it run again (Hold::again).
    }
    Arguments arguments;
    const auto entry = std::ranges::find_if(entries_, [&](const Entry& candidate) {
      return matches_header(candidate.header, command, SuffixRange::kChecked, arguments.suffixes);
    });
    if (entry == entries_.end()) {
      session.push_error(any_header_matches(entries_, command, SuffixRange::kIgnored)
                             ? kHeaderSuffixOutOfRange
                             : kUndefinedHeader);
      break;
    }
    if (!command.common) {
      execution.path_.assign(command.path.begin(), command.path.end() - 1);
    }
    if (std::optional<ScpiError> error = convert_all(entry->parameters, entry->required_parameters,
                                                     command.parameters, arguments.values)) {
      session.push_error(std::move(*error));
      break;
    }
    // A query whose reply is then dropped for -223 leaves the Session as it found it, so a
    // `SYST:ERR?` that crosses the limit does not lose the entry it took off the queue.
    std::optional<Session> before = command.query ? std::optional(session) : std::nullopt;
    HandlerResult result = entry->handler(session, arguments);
    if (auto* hold = std::get_if<Hold>(&result)) {
      execution.hold_ = std::move(*hold);
      return;
    }
    if (auto* error = std::get_if<ScpiError>(&result)) {
      session.push_error(std::move(*error));
      break;
    }
    if (!join_reply(session, std::move(before), execution.reply_,
                    std::move(std::get<std::optional<std::string>>(result)))) {
      break;
    }
  }
  // The line has ended; after an error this discards the rest of it.
  execution.next_ = commands.size();
}

}  // namespace openscope::scpi
