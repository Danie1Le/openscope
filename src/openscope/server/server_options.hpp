#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace openscope {

/// Where a ScpiServer listens and when it gives up on a silent client. The defaults are the
/// engine's.
struct ServerOptions {
  /// The local address to listen on. Loopback by default, so the instrument is not reachable from
  /// the network unless asked for (spec #1, story 81).
  std::string address = "127.0.0.1";
  /// The TCP port; 0 picks a free one (ScpiServer::port says which).
  std::uint16_t port = 5025;
  /// How long a Session may go without a command or a reply before its connection is closed.
  std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);
};

}  // namespace openscope
