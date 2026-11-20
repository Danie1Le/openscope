#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "openscope/scpi/command_tree.hpp"
#include "openscope/session.hpp"

namespace openscope {

/// Identifies one Session opened on an Instrument.
enum class SessionId : std::uint32_t {};

/// The single entry point for instrument behaviour. It owns every Session and executes SCPI text
/// on a Session's behalf, so transports and tests all reach the instrument through the same path.
class Instrument {
 public:
  /// The longest line, in bytes without its terminator, that `execute` runs (4 KB).
  static constexpr std::size_t kMaxLineLength = 4096;

  /// Builds the instrument with its SCPI command tree.
  Instrument();

  /// Opens a new Session with an empty error queue.
  SessionId open_session();

  /// Executes one line of SCPI text for `session` and returns its reply, if it has one.
  /// `line` excludes its terminator: the transport strips the trailing `\n` and an optional `\r`
  /// before calling. A line longer than kMaxLineLength only queues -363. Errors are pushed onto
  /// that Session's error queue, not returned. Throws std::out_of_range for a SessionId this
  /// Instrument did not open.
  std::optional<std::string> execute(SessionId session, std::string_view line);

 private:
  scpi::CommandTree commands_;
  std::unordered_map<SessionId, Session> sessions_;
  std::uint32_t next_session_id_ = 0;
};

}  // namespace openscope
