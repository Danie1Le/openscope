#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "openscope/instrument.hpp"
#include "openscope/server/json_log.hpp"
#include "openscope/server/server_options.hpp"

namespace openscope {

/// Serves SCPI over TCP, the way scripts (a raw socket, PyVISA) drive a bench instrument (#13).
/// Each connection is one Session (ADR 0002). A line ends at `\n`, and an optional `\r` before it
/// is dropped; lines run in the order they arrive, each only once the one before has replied, and
/// a reply goes back as one line ending in `\n`. A line longer than Instrument::kMaxLineLength is
/// passed on cut short, so the Instrument rejects it with -363, and the connection carries on.
///
/// Limits keep one client from starving the rest: at most kMaxClients connections, a further
/// one is closed as soon as it is accepted; a connection that goes ServerOptions::idle_timeout
/// without a command or a reply is closed, also when it never reads the reply being written, but
/// not while the Instrument has not replied yet (a line held by `*OPC?` waits for as long as the
/// Single takes). A client that disconnects ends its Session at once, dropping any line it has
/// waiting; so does one whose host vanishes (loses power or network, sending no FIN or RST), once
/// TCP keepalive gives up on it. Connects, refusals, disconnects and errors go to the JsonLog.
///
/// shortcut: clients can hold all kMaxClients slots on purpose (connections that keep sending
/// commands, or that wait on a Single that never ends: Normal mode, no trigger); new connections,
/// including one that would send `STOP`, are then refused until one of them disconnects or the
/// engine restarts, as on a bench instrument with a client limit.
///
/// All of its work runs on the io_context's thread, which must be one thread, so connections need
/// no locking. Replies that end on another thread (inside `Instrument::process` or `advance`) are
/// posted back to it, so it never waits on a held line.
class ScpiServer {
 public:
  /// Connections served at once (spec #1, story 82).
  static constexpr std::size_t kMaxClients = 8;

  /// TCP keepalive on every connection: after kKeepAliveIdle without traffic, kKeepAliveProbes
  /// probes kKeepAliveInterval apart. A peer that answers none fails the pending read and its
  /// connection closes. The idle timeout is off while a line is held, so this is what frees the
  /// slot of a client whose host vanished while it waited on `*OPC?` (spec #1, stories 35 and 83),
  /// unless it had sent a chunk's worth (4 KB) behind the held line: then no read is pending, and
  /// the connection closes when the line ends (see Connection::read).
  static constexpr std::chrono::seconds kKeepAliveIdle{30};
  /// See kKeepAliveIdle.
  static constexpr std::chrono::seconds kKeepAliveInterval{5};
  /// See kKeepAliveIdle.
  static constexpr int kKeepAliveProbes = 3;
  static_assert(kKeepAliveIdle + kKeepAliveProbes * kKeepAliveInterval <= std::chrono::minutes(1),
                "a vanished client is noticed within a minute of its last traffic");

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

  /// How long accepting pauses after it fails.
  static constexpr std::chrono::milliseconds kAcceptRetryDelay{100};

  /// Accepts the next connection; after a failure, tries again after kAcceptRetryDelay.
  void accept();
  /// Serves `socket`, or closes it when kMaxClients connections are open already.
  void admit(boost::asio::ip::tcp::socket socket);

  Instrument* instrument_;
  JsonLog* log_;
  std::chrono::milliseconds idle_timeout_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::steady_timer accept_retry_;
  std::uint16_t port_;
  /// The connections accepted; closed ones are pruned when the next one arrives.
  std::vector<std::weak_ptr<Connection>> connections_;
};

}  // namespace openscope
