#include "openscope/server/scpi_server.hpp"

#include <netinet/tcp.h>

#include <algorithm>
#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/socket_option.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/detail/error_code.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>  // IWYU pragma: keep (the definition of ordered_json)
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "openscope/instrument.hpp"
#include "openscope/server/json_log.hpp"
#include "openscope/server/server_options.hpp"

namespace openscope {

namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {

/// The most bytes of one line a connection keeps. Two more than the longest line the Instrument
/// runs: a kept line that ends in `\r` loses it and must still be over the limit when the real
/// line is, so the Instrument rejects every line that is too long, and nothing more is needed.
constexpr std::size_t kKeptLineLength = Instrument::kMaxLineLength + 2;

/// Where one Session's lines run, one at a time, on the server's thread pool.
using Strand = asio::strand<asio::thread_pool::executor_type>;

/// What ends every reply.
constexpr std::string_view kNewline = "\n";

/// `host:port` of `endpoint`, as logs name a client; an IPv6 host is in brackets (`[::1]:5025`),
/// so the port can be split off.
std::string describe(const tcp::endpoint& endpoint) {
  std::ostringstream text;
  text << endpoint;
  return text.str();
}

/// A TCP socket option that is an int.
template <int Name>
using TcpOption = asio::detail::socket_option::integer<IPPROTO_TCP, Name>;

/// An acceptor listening as `options` say, with TCP keepalive on at ScpiServer's probe settings
/// and Nagle's algorithm off (each reply goes out when written, not when the one before is ACKed:
/// #58). They are set before it listens, and Linux gives every socket it accepts the same options.
/// Throws boost::system::system_error when the address is not an IP address or cannot be bound.
tcp::acceptor listening_acceptor(asio::io_context& io, const ServerOptions& options) {
  const tcp::endpoint endpoint(asio::ip::make_address(options.address), options.port);
  tcp::acceptor acceptor(io, endpoint.protocol());
  acceptor.set_option(tcp::acceptor::reuse_address(true));
  acceptor.set_option(tcp::acceptor::keep_alive(true));
  acceptor.set_option(tcp::no_delay(true));
  acceptor.set_option(
      TcpOption<TCP_KEEPIDLE>(static_cast<int>(ScpiServer::kKeepAliveIdle.count())));
  acceptor.set_option(
      TcpOption<TCP_KEEPINTVL>(static_cast<int>(ScpiServer::kKeepAliveInterval.count())));
  acceptor.set_option(TcpOption<TCP_KEEPCNT>(ScpiServer::kKeepAliveProbes));
  acceptor.bind(endpoint);
  acceptor.listen();
  return acceptor;
}

}  // namespace

/// The transport of one client: its socket, the Session it owns and its idle timer (the Session
/// is the state; this is only how it is reached). Lines run one at a time: the next is
/// taken from the received bytes only once the one before has replied and the reply is written,
/// so a client that sends faster than it reads is slowed by TCP instead of filling memory.
class ScpiServer::Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(ScpiServer& server, tcp::socket socket, std::string peer)
      : server_(&server),
        socket_(std::move(socket)),
        idle_timer_(socket_.get_executor()),
        peer_(std::move(peer)),
        strand_(std::make_unique<Strand>(asio::make_strand(server.execution_))),
        // A held line carries on on this Session's strand, never on the thread that ended its
        // operation (the live pipeline's processing thread).
        session_(server.instrument_->open_session([strand = *strand_](std::function<void()> work) {
          asio::post(strand, std::move(work));
        })) {}

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) = delete;
  Connection& operator=(Connection&&) = delete;
  ~Connection() = default;

  void start() {
    log("connect");
    arm_idle_timer();
    read();
  }

  /// Ends the Session (dropping a line it has held) and closes the socket, once. The event is
  /// logged after the socket closes (#72): a client or a tool that acts on the event must find the
  /// connection already closed.
  void close(std::string_view reason) {
    if (closed_) {
      return;
    }
    closed_ = true;
    // On the strand, after the line it may be running, which needs the Session open.
    asio::post(*strand_, [instrument = server_->instrument_, session = session_] {
      instrument->close_session(session);
    });
    // A strand must not outlive the server's thread pool, and the io_context may hold this
    // connection longer. The Session's Resumer keeps its own copy until close_session runs, which
    // `~ScpiServer` waits for when it joins the pool.
    strand_.reset();
    error_code ignored;
    std::ignore = socket_.close(ignored);
    idle_timer_.cancel();
    log("disconnect", {{"reason", reason}});
  }

  [[nodiscard]] bool closed() const { return closed_; }

 private:
  /// Where the connection is with its current line.
  enum class Stage : std::uint8_t {
    /// Assembling the next line from the bytes received.
    kAssembling,
    /// The line is in the Instrument and has not replied: the idle timer is off.
    kExecuting,
    /// The reply is being written; the next line waits for it, but the idle timer runs.
    kWriting,
  };

  /// Reads more bytes unless a chunk's worth is already waiting. It keeps reading while a line
  /// waits for its reply, so a disconnect ends the wait at once.
  ///
  /// shortcut: a client that sends a chunk's worth behind a held line and then disconnects is
  /// seen only when the line ends (the Single completes, or `STOP`, `RUN` or `*RST` from any
  /// client); peeking for the close without reading would need a platform-specific poll.
  void read() {
    if (reading_ || closed_ || received_.size() >= chunk_.size()) {
      return;
    }
    reading_ = true;
    // Into chunk_, which nothing else touches while the read is pending, then onto received_.
    socket_.async_read_some(asio::buffer(chunk_), [self = shared_from_this()](
                                                      const error_code& error, std::size_t count) {
      self->reading_ = false;
      if (error) {
        self->close(error == asio::error::eof ? "closed by client" : error.message());
        return;
      }
      self->received_.append(self->chunk_.data(), count);
      self->run_next_line();
    });
  }

  /// Unless a line is running, moves received bytes into the line being assembled and runs it
  /// once its `\n` has arrived. Then reads more.
  void run_next_line() {
    if (stage_ != Stage::kAssembling || closed_) {
      read();
      return;
    }
    const std::size_t newline = received_.find('\n');
    const std::size_t taken = std::min(newline, received_.size());
    line_.append(received_, 0, std::min(taken, kKeptLineLength - line_.size()));
    received_.erase(0, newline == std::string::npos ? taken : taken + 1);
    if (newline != std::string::npos) {
      run_line();
    }
    read();
  }

  /// Executes the assembled line on the Session's strand. Its reply comes later, from that strand,
  /// so it is posted to this connection's executor and never waited for.
  void run_line() {
    if (line_.ends_with('\r')) {
      line_.pop_back();
    }
    if (line_.size() > Instrument::kMaxLineLength) {
      log("error",
          {{"error", std::format("line longer than {} bytes", Instrument::kMaxLineLength)}});
    }
    stage_ = Stage::kExecuting;
    // A Session waiting for a reply is not idle, however long it waits. Moving the expiry out of
    // reach also tells a wait that already finished that it is stale.
    idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
    // The handler keeps the connection alive while the line runs, even with no I/O pending (a
    // full input buffer). A held line's handler is dropped when `close` ends the Session.
    asio::post(*strand_, [self = shared_from_this(), executor = socket_.get_executor(),
                          line = std::exchange(line_, {})] {
      self->server_->instrument_->execute(
          self->session_, line, [self, executor](std::optional<std::string> reply) {
            asio::post(executor, [self, reply = std::move(reply)]() mutable {
              self->reply(std::move(reply));
            });
          });
    });
  }

  /// Writes the reply to the line that ran, if it has one, then runs the next line.
  void reply(std::optional<std::string> text) {
    if (closed_) {
      return;
    }
    arm_idle_timer();  // Idle from the reply on, which also bounds a write the client never reads.
    if (!text) {
      stage_ = Stage::kAssembling;
      run_next_line();
      return;
    }
    output_ = std::move(*text);
    stage_ = Stage::kWriting;
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(output_), asio::buffer(kNewline)};
    asio::async_write(socket_, buffers,
                      [self = shared_from_this()](const error_code& error, std::size_t /*count*/) {
                        if (error) {
                          self->close(error.message());
                          return;
                        }
                        self->stage_ = Stage::kAssembling;
                        self->run_next_line();
                      });
  }

  /// Closes the connection after the idle timeout unless a command or reply comes first.
  void arm_idle_timer() {
    idle_timer_.expires_after(server_->idle_timeout_);
    idle_timer_.async_wait([self = shared_from_this()](const error_code& error) {
      // A wait that had already finished when the expiry moved still arrives without an error.
      if (error || self->idle_timer_.expiry() > std::chrono::steady_clock::now()) {
        return;
      }
      self->close("idle timeout");
    });
  }

  /// Logs `event` for this connection: its Session and peer, then `fields`.
  void log(std::string_view event,
           const nlohmann::ordered_json& fields = nlohmann::ordered_json::object()) {
    nlohmann::ordered_json named = {{"session", static_cast<std::uint64_t>(session_)},
                                    {"peer", peer_}};
    named.update(fields);
    server_->log_->write(event, named);
  }

  ScpiServer* server_;
  tcp::socket socket_;
  asio::steady_timer idle_timer_;
  std::string peer_;
  /// Where this Session's lines run, one at a time; empty once the connection is closed.
  std::unique_ptr<Strand> strand_;
  SessionId session_;
  /// What one read receives into.
  std::array<char, 4096> chunk_{};
  /// Bytes received and not yet taken into a line: less than two chunks.
  std::string received_;
  /// The line being assembled, cut at kKeptLineLength bytes.
  std::string line_;
  /// The reply being written, without its `\n`.
  std::string output_;
  Stage stage_ = Stage::kAssembling;
  bool reading_ = false;
  bool closed_ = false;
};

ScpiServer::ScpiServer(asio::io_context& io, Instrument& instrument, const ServerOptions& options,
                       JsonLog& log)
    : instrument_(&instrument),
      log_(&log),
      idle_timeout_(options.idle_timeout),
      acceptor_(listening_acceptor(io, options)),
      accept_retry_(io),
      port_(acceptor_.local_endpoint().port()),
      execution_(kMaxExecutionThreads) {
  log_->write("listening", {{"address", options.address}, {"port", port_}});
  accept();
}

ScpiServer::~ScpiServer() {
  for (const std::weak_ptr<Connection>& connection : connections_) {
    if (const std::shared_ptr<Connection> open = connection.lock()) {
      // Closing ends a Session this server opened, which cannot fail; only writing the log line
      // can (out of memory), and a destructor has nowhere to report that, so the next one closes.
      try {
        open->close("server stopped");
      } catch (...) {  // NOLINT(bugprone-empty-catch): see above.
      }
    }
  }
  // Runs what is still queued, the Sessions' closes included, before the Instrument may go.
  execution_.join();
}

void ScpiServer::accept() {
  acceptor_.async_accept([this](const error_code& error, tcp::socket socket) {
    if (error == asio::error::operation_aborted) {
      return;
    }
    if (!error) {
      admit(std::move(socket));
      accept();
      return;
    }
    // Such as running out of file descriptors: retrying at once would spin and flood the log.
    log_->write("error", {{"error", error.message()}});
    accept_retry_.expires_after(kAcceptRetryDelay);
    accept_retry_.async_wait([this](const error_code& wait_error) {
      if (!wait_error) {
        accept();
      }
    });
  });
}

void ScpiServer::admit(tcp::socket socket) {
  std::erase_if(connections_, [](const std::weak_ptr<Connection>& connection) {
    const std::shared_ptr<Connection> open = connection.lock();
    return open == nullptr || open->closed();
  });
  error_code error;
  const tcp::endpoint endpoint = socket.remote_endpoint(error);
  if (error) {  // Gone before it was accepted.
    return;
  }
  std::string peer = describe(endpoint);
  if (connections_.size() >= kMaxClients) {
    std::ignore = socket.close(error);
    // Logged once the socket is closed, as `disconnect` is (#72): the client may act on the event.
    log_->write("refuse", {{"peer", peer}, {"reason", "too many clients"}});
    return;
  }
  auto connection = std::make_shared<Connection>(*this, std::move(socket), std::move(peer));
  connections_.push_back(connection);
  connection->start();
}

}  // namespace openscope
