#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "openscope/server/server_options.hpp"

namespace openscope {

/// What `openscope-engine` was asked to do on its command line. The defaults are those of a
/// command line with no options.
struct EngineOptions {
  /// Where the SCPI server listens and its idle timeout.
  ServerOptions server;
  /// The seed of every Generator's noise (ADR 0003): the same seed gives the same signals.
  std::uint64_t seed = 0;
  /// `--help`: print engine_usage() and exit.
  bool help = false;
};

/// The `--help` text, which documents every option; its defaults and ranges are those
/// parse_engine_options uses.
std::string engine_usage();

/// Parses the command-line `arguments`, without the program name. Throws std::invalid_argument,
/// naming the option, for an unknown option, a missing value, a value out of range or an address
/// that is not an IP address (host names are not resolved).
EngineOptions parse_engine_options(std::span<const std::string_view> arguments);

}  // namespace openscope
