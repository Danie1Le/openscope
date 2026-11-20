#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "openscope/instrument.hpp"

namespace openscope::test {

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
