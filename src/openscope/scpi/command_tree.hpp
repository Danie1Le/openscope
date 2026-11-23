#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "openscope/scpi/parser.hpp"
#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope::scpi {

/// A number accepted within [min, max] after its unit suffix is applied. `unit` is the
/// upper-case base unit the parameter may be written with (`"HZ"`, `"V"`, `"S"`), optionally
/// behind an SI multiplier (`KHZ`, `MV`); empty means no suffix is allowed. The bounds are finite
/// with min <= max. The unit is owned, so the tree never holds a view into the caller's text.
struct NumericParameter {
  double min;
  double max;
  std::string unit;
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
  /// One value per parameter the caller sent, in declaration order. Optional trailing parameters
  /// that were left out have no value, so the handler applies their defaults.
  std::vector<Argument> values;
};

/// What a handler returns to hold its line, as `*WAI` and `*OPC?` do while an operation is
/// pending: the line stops after this command. When `CommandTree::resume` carries it on, `reply`
/// becomes the command's reply and the rest of the line runs. The handler is not run again, so
/// whatever it waited for is over even if it has begun again since.
struct Hold {
  std::optional<std::string> reply;
  /// The command has not run but waits to (a command of another Session while held lines carry
  /// on, #68 review): when the line carries on, it runs again instead of replying `reply`.
  bool again = false;
};

/// What a handler returns: its reply (nullopt for a command without one), the execution error
/// (such as -221) that stopped it, which the tree queues before discarding the rest of the line,
/// or Hold. A handler that replies but must also report something, as a measurement reports its
/// status, puts that entry on the Session's queue itself, so the line carries on.
using HandlerResult = std::variant<std::optional<std::string>, ScpiError, Hold>;

/// The most bytes the replies of one line may hold, joined with `;` and without the terminator:
/// 64 MiB, the most the Python client reads (`MAX_RESPONSE_BYTES` in
/// python/src/openscope_client/transport.py). A 4 KB line holds hundreds of queries, and each
/// `WAV:DATA?` or `FFT:DATA?` may reply megabytes, so without it one line could build a reply of
/// gigabytes while every other client waits (#67).
inline constexpr std::size_t kMaxReplyLength = std::size_t{64} * 1024 * 1024;

/// Runs a matched command for the calling Session.
using Handler = std::function<HandlerResult(Session&, const Arguments&)>;

/// The short form of a mnemonic or keyword pattern: its leading upper-case letters (`SINusoid` is
/// `SIN`, `CHANnel<1-4>` is `CHAN`). Queries reply with keywords in this form.
std::string_view short_form(std::string_view pattern);

/// One command: its header pattern, its parameters and its handler. Patterns are written as in
/// instrument manuals: `*IDN?`, `SYSTem:ERRor[:NEXT]?`, `CHANnel<1-4>:SCALe`. A query is a
/// separate command from its setter.
struct Command {
  std::string_view header;
  std::vector<ParameterSpec> parameters;
  /// How many of the last `parameters` a caller may leave out, as in
  /// `SOURce<n>:FAULt GLITch|DROPout|FSTep[,<magnitude>[,<duration>]]` (2).
  std::size_t optional_parameters = 0;
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

/// One line run by a CommandTree: the replies of its queries so far and, when a handler held it,
/// where it stopped, so `CommandTree::resume` can carry it on.
class Execution {
 public:
  /// Move-only: a copy of a held line could be resumed twice and run its rest twice.
  Execution() = default;
  Execution(const Execution&) = delete;
  Execution& operator=(const Execution&) = delete;
  Execution(Execution&&) = default;
  Execution& operator=(Execution&&) = default;
  ~Execution() = default;

  /// True while a handler holds the line (it returned Hold); false once the line has ended.
  [[nodiscard]] bool held() const { return hold_.has_value(); }

  /// True while the line is held by a command that runs again when it carries on (Hold::again).
  [[nodiscard]] bool held_to_run_again() const { return hold_ && hold_->again; }

  /// The replies of the queries that have run, joined with `;`, or nullopt when none replied.
  [[nodiscard]] const std::optional<std::string>& reply() const { return reply_; }

  /// Moves the reply out, for a line that has ended, so a large one (`WAV:DATA?`) is not copied.
  [[nodiscard]] std::optional<std::string> take_reply() { return std::move(reply_); }

 private:
  friend class CommandTree;

  /// The line's commands; those before `next_` have run, and `next_` is the one that holds the
  /// line while it is held.
  std::vector<ParsedCommand> commands_;
  std::size_t next_ = 0;
  /// The path the next relative header continues from (SCPI-99 compound headers).
  std::vector<Mnemonic> path_;
  std::optional<std::string> reply_;
  /// What the holding command returned, while the line is held.
  std::optional<Hold> hold_;
};

/// The SCPI command tree: matches each command of a line against the registered headers (short
/// or long form, any case, numeric suffixes, optional nodes, SCPI-99 compound-header paths),
/// checks its parameters, and runs its handler. A command with an error never reaches its
/// handler, so a bad command never changes state.
class CommandTree {
 public:
  /// Registers `command`. Throws std::invalid_argument when a header or keyword pattern is
  /// malformed, a numeric parameter has a unit that is not upper-case letters or bounds that are
  /// not finite with min <= max, more parameters are optional than declared, the handler is
  /// empty, or some input could match both this header and one already added (the first added
  /// would always win). That way a mistake in the command table fails at start-up, not when a
  /// client first sends the command.
  void add(Command command);

  /// Executes one line for `session` until it ends or a handler holds it, and returns how far it
  /// got. A syntax error anywhere runs nothing. Any other error, including an execution error a
  /// handler returns and -223 for a reply that would take the line's replies past
  /// kMaxReplyLength, runs the commands before it, keeps their replies and discards the rest of
  /// the line. A query whose reply -223 drops leaves `session` as it found it (a `SYST:ERR?` keeps
  /// its entry on the queue). Errors go to `session`'s queue.
  [[nodiscard]] Execution execute(Session& session, std::string_view line) const;

  /// Carries on the held `execution` for `session`: the holding command's Hold reply joins the
  /// replies, then the rest of the line runs until it ends or a handler holds it again.
  void resume(Session& session, Execution& execution) const;

 private:
  /// A registered command with its header and keyword patterns parsed.
  struct Entry {
    detail::HeaderPattern header;
    std::vector<detail::ParameterRule> parameters;
    /// How many leading parameters a caller must send.
    std::size_t required_parameters;
    Handler handler;
  };

  std::vector<Entry> entries_;
};

}  // namespace openscope::scpi
