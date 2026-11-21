#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscope/instrument.hpp"
#include "openscope/server/json_log.hpp"

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

/// Serves SCPI over TCP, the way scripts (a raw socket, PyVISA) drive a bench instrument (#13).
/// Each connection is one Session (ADR 0002). A line ends at `\n`, and an optional `\r` before it
/// is dropped; lines run in the order they arrive, each only once the one before has replied, and
/// a reply goes back as one line ending in `\n`. A line longer than Instrument::kMaxLineLength is
/// passed on cut short, so the Instrument rejects it with -363, and the connection carries on.
///
/// Limits keep one client from starving the rest: at most kMaxClients connections, a further
/// one is closed as soon as it is accepted; a connection that goes ServerOptions::idle_timeout
/// without a command or a reply is closed, except while it waits for a reply (a line held by
/// `*OPC?` waits for as long as the Single takes). A client that disconnects ends its Session at
/// once, dropping any line it has waiting. Connects, refusals, disconnects and errors go to the
/// JsonLog.
///
/// All of its work runs on the io_context's thread, which must be one thread, so connections need
/// no locking. Replies that end on another thread (inside `Instrument::process` or `advance`) are
/// posted back to it, so it never waits on a held line.
class ScpiServer {
 public:
  /// Connections served at once (spec #1, story 82).
  static constexpr std::size_t kMaxClients = 8;

  /// Listens as `options` say and starts accepting on `io`. `instrument` and `log` must outlive the
  /// server. Throws boost::system::system_error when the address is not an IP address or cannot be
  /// bound.
  ScpiServer(boost::asio::io_context& io, Instrument& instrument, const ServerOptions& options,
             JsonLog& log);

  /// Closes every connection and ends its Session. Call it only once `io` has stopped running.
  ~ScpiServer();

  // Connections keep a pointer to the server.
  ScpiServer(const ScpiServer&) = delete;
  ScpiServer& operator=(const ScpiServer&) = delete;
  ScpiServer(ScpiServer&&) = delete;
  ScpiServer& operator=(ScpiServer&&) = delete;

  /// The port it listens on: ServerOptions::port, or the one picked for port 0.
  [[nodiscard]] std::uint16_t port() const { return port_; }

 private:
  class Connection;

  void accept();
  /// Serves `socket`, or closes it when kMaxClients connections are open already.
  void admit(boost::asio::ip::tcp::socket socket);

  Instrument* instrument_;
  JsonLog* log_;
  std::chrono::milliseconds idle_timeout_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::uint16_t port_;
  /// The connections accepted; closed ones are pruned when the next one arrives.
  std::vector<std::weak_ptr<Connection>> connections_;
};

}  // namespace openscope
