#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "openscope/session.hpp"

namespace openscope::scpi {

/// A number accepted within [min, max] after its unit suffix is applied. `unit` is the
/// upper-case base unit the parameter may be written with (`"HZ"`, `"V"`, `"S"`), optionally
/// behind an SI multiplier (`KHZ`, `MV`); empty means no suffix is allowed.
struct NumericParameter {
  double min;
  double max;
  std::string_view unit;
};

/// SCPI-99 boolean: `ON`, `OFF`, or a number that is rounded, with non-zero meaning ON.
struct BooleanParameter {};

/// One keyword out of a fixed set. Each choice is written like a header mnemonic: upper-case
/// short form, lower-case rest of the long form, and an optional suffix range (`NORMal`,
/// `CHANnel<1-4>`).
struct KeywordParameter {
  std::vector<std::string_view> choices;
};

/// What one parameter of a command accepts; the tree validates it before the handler runs.
using ParameterSpec = std::variant<NumericParameter, BooleanParameter, KeywordParameter>;

/// The keyword a caller picked: its position in KeywordParameter::choices and its numeric
/// suffix (1 when the choice takes a suffix and none was typed; 0 when it takes none).
struct Choice {
  std::size_t index;
  unsigned suffix;
};

/// A parameter after conversion: a number in base units, a boolean, or a keyword choice.
using Argument = std::variant<double, bool, Choice>;

/// What a handler receives once its command matched and every parameter passed validation.
struct Arguments {
  /// One value per header node that takes a suffix, in header order (1 when omitted).
  std::vector<unsigned> suffixes;
  /// One value per declared parameter, in order.
  std::vector<Argument> values;
};

/// Runs a matched command for the calling Session and returns its reply, if it has one. It may
/// push execution errors (such as -221) onto the Session's queue itself.
using Handler = std::function<std::optional<std::string>(Session&, const Arguments&)>;

/// One command: its header pattern, its parameters and its handler. Patterns are written as in
/// instrument manuals: `*IDN?`, `SYSTem:ERRor[:NEXT]?`, `CHANnel<1-4>:SCALe`. A query is a
/// separate command from its setter.
struct Command {
  std::string_view header;
  std::vector<ParameterSpec> parameters;
  Handler handler;
};

/// Parsed forms of the patterns in a Command, private to the command tree.
namespace detail {

/// `CHANnel<1-4>` accepts `CHAN` or `CHANNEL` with suffix 1 to 4.
struct MnemonicPattern {
  std::string short_form;
  std::string long_form;
  /// The largest suffix accepted; 0 when the mnemonic takes no suffix.
  unsigned max_suffix = 0;
};

/// One node of a header pattern; `[:NEXT]` is an optional node.
struct HeaderNode {
  MnemonicPattern mnemonic;
  bool optional = false;
};

/// `SYSTem:ERRor[:NEXT]?`: whether it is a common command and a query, and its nodes.
struct HeaderPattern {
  bool common = false;
  bool query = false;
  std::vector<HeaderNode> nodes;
};

/// A KeywordParameter with its choices parsed.
struct KeywordRule {
  std::vector<MnemonicPattern> choices;
};

/// A ParameterSpec with its keyword choices parsed.
using ParameterRule = std::variant<NumericParameter, BooleanParameter, KeywordRule>;

}  // namespace detail

/// The SCPI command tree: matches each command of a line against the registered headers (short
/// or long form, any case, numeric suffixes, optional nodes, SCPI-99 compound-header paths),
/// checks its parameters, and runs its handler. A command with an error never reaches its
/// handler, so a bad command never changes state.
class CommandTree {
 public:
  /// Registers `command`. Throws std::invalid_argument when a header or keyword pattern is
  /// malformed or the handler is empty, so a typo in the command table fails at start-up.
  void add(Command command);

  /// Executes one line for `session` and returns the replies of its queries, joined with `;`, or
  /// nullopt when no query replied. A syntax error anywhere runs nothing. Any other error runs
  /// the commands before it and discards the rest of the line. Errors go to `session`'s queue.
  std::optional<std::string> execute(Session& session, std::string_view line) const;

 private:
  /// A registered command with its header and keyword patterns parsed.
  struct Entry {
    detail::HeaderPattern header;
    std::vector<detail::ParameterRule> parameters;
    Handler handler;
  };

  std::vector<Entry> entries_;
};

}  // namespace openscope::scpi
