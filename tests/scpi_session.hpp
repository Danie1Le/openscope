#pragma once

#include <optional>
#include <string>
#include <string_view>

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
inline constexpr auto kInvalidSuffixReply = R"(-131,"Invalid suffix")";
inline constexpr auto kSuffixOutOfRangeReply = R"(-114,"Header suffix out of range")";
inline constexpr auto kOutOfRangeReply = R"(-222,"Data out of range")";

/// One Session on an Instrument, driven with SCPI lines the way a client drives it, so facade
/// tests read as a SCPI conversation.
class ScpiSession {
 public:
  explicit ScpiSession(Instrument& instrument)
      : instrument_(&instrument), id_(instrument.open_session()) {}

  /// Executes one SCPI line and returns the reply, if any.
  std::optional<std::string> send(std::string_view line) { return instrument_->execute(id_, line); }

  /// Pops this Session's oldest error with `SYST:ERR?`.
  std::string next_error() { return send("SYST:ERR?").value_or("<no reply to SYST:ERR?>"); }

 private:
  Instrument* instrument_;
  SessionId id_;
};

}  // namespace openscope::test
