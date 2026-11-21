#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "openscope/server/server_options.hpp"

namespace openscope {

/// What `openscope-engine` was asked to do on its command line.
struct EngineOptions {
  /// Where the SCPI server listens and its idle timeout.
  ServerOptions server;
  /// The seed of every Generator's noise (ADR 0003): the same seed gives the same signals.
  std::uint64_t seed = 0;
  /// `--help`: print kEngineUsage and exit.
  bool help = false;
};

/// The `--help` text, which documents every option.
inline constexpr std::string_view kEngineUsage =
    "Usage: openscope-engine [options]\n"
    "\n"
    "Runs the OpenScope instrument live and serves SCPI over TCP. Logs go to standard error as\n"
    "one JSON object per line. Stop it with Ctrl+C (SIGINT) or SIGTERM.\n"
    "\n"
    "Options:\n"
    "  --address <ip>          Local IP address to listen on (default 127.0.0.1). Any other\n"
    "                          address exposes the instrument, which has no authentication,\n"
    "                          to the network.\n"
    "  --port <0-65535>        TCP port (default 5025); 0 picks a free port, logged at start.\n"
    "  --idle-timeout <1-86400>\n"
    "                          Seconds a client may stay silent before it is disconnected\n"
    "                          (default 300). A client waiting on *OPC? is never idle.\n"
    "  --seed <n>              Seed of the generators' noise (default 0).\n"
    "  -h, --help              Print this help and exit.\n";

/// Parses the command-line `arguments`, without the program name. Throws std::invalid_argument,
/// naming the option, for an unknown option, a missing value or a value out of range.
EngineOptions parse_engine_options(std::span<const std::string_view> arguments);

}  // namespace openscope
