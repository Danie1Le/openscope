#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "openscope/instrument.hpp"

namespace openscope::test {

/// `SYST:ERR?` replies that more than one test file expects, written out as a client sees them.
inline constexpr auto kNoErrorReply = R"(0,"No error")";
inline constexpr auto kDataTypeErrorReply = R"(-104,"Data type error")";
inline constexpr auto kParameterNotAllowedReply = R"(-108,"Parameter not allowed")";
inline constexpr auto kMissingParameterReply = R"(-109,"Missing parameter")";
inline constexpr auto kUndefinedHeaderReply = R"(-113,"Undefined header")";
inline constexpr auto kSettingsConflictReply = R"(-221,"Settings conflict")";
inline constexpr auto kIllegalParameterValueReply = R"(-224,"Illegal parameter value")";
inline constexpr auto kDataStaleReply = R"(-230,"Data corrupt or stale")";
inline constexpr auto kClippedReply = R"(-231,"Data questionable;Clipped")";
inline constexpr auto kInvalidSuffixReply = R"(-131,"Invalid suffix")";
inline constexpr auto kSuffixOutOfRangeReply = R"(-114,"Header suffix out of range")";
inline constexpr auto kOutOfRangeReply = R"(-222,"Data out of range")";
inline constexpr auto kClippedReply = R"(-231,"Data questionable;Clipped")";
inline constexpr auto kNoSignalReply = R"(-200,"Execution error;No signal")";
inline constexpr auto kTooFewCyclesReply = R"(-200,"Execution error;Too few cycles")";

/// The reply to one line, as the Instrument delivers it.
struct LineReply {
  /// True once the line has ended and `text` holds its reply.
  bool ended = false;
  /// The replies of the line's queries joined with `;`, or nullopt when none replied.
  std::optional<std::string> text;
};

/// One Session on an Instrument, driven with SCPI lines the way a client drives it, so facade
/// tests read as a SCPI conversation.
class ScpiSession {
 public:
  explicit ScpiSession(Instrument& instrument)
      : instrument_(&instrument), id_(instrument.open_session()) {}

  /// Executes one SCPI line that ends at once and returns the reply, if any. Fails the test when
  /// the line has not ended.
  std::optional<std::string> send(std::string_view line) {
    const std::shared_ptr<const LineReply> reply = start(line);
    if (!reply->ended) {
      ADD_FAILURE() << "'" << line << "' has not ended";
    }
    return reply->text;
  }

  /// Executes one SCPI line and returns its reply, which the Instrument fills in when the line
  /// ends.
  std::shared_ptr<const LineReply> start(std::string_view line) {
    auto reply = std::make_shared<LineReply>();
    instrument_->execute(id_, line, [reply](std::optional<std::string> text) {
      reply->ended = true;
      reply->text = std::move(text);
    });
    return reply;
  }

  /// Pops this Session's oldest error with `SYST:ERR?`.
  std::string next_error() { return send("SYST:ERR?").value_or("<no reply to SYST:ERR?>"); }

  /// Closes the Session, as a client's disconnect does.
  void close() { instrument_->close_session(id_); }

 private:
  Instrument* instrument_;
  SessionId id_;
};

}  // namespace openscope::test
