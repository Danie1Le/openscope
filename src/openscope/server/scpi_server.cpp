#include "openscope/server/scpi_server.hpp"

#include <algorithm>
#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "openscope/instrument.hpp"
#include "openscope/server/json_log.hpp"

namespace openscope {

namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {

/// The most bytes of one line a connection keeps. Two more than the longest line the Instrument
/// runs: a kept line that ends in `\r` loses it and must still be over the limit when the real
/// line is, so the Instrument rejects every line that is too long, and nothing more is needed.
constexpr std::size_t kKeptLineLength = Instrument::kMaxLineLength + 2;

/// `host:port` of `endpoint`, as logs name a client.
std::string describe(const tcp::endpoint& endpoint) {
  return std::format("{}:{}", endpoint.address().to_string(), endpoint.port());
}

}  // namespace

/// One client: its socket, its Session and its idle timer. Lines run one at a time: the next is
/// taken from the received bytes only once the one before has replied and the reply is written,
/// so a client that sends faster than it reads is slowed by TCP instead of filling memory.
class ScpiServer::Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(ScpiServer& server, tcp::socket socket, std::string peer)
      : server_(&server),
        socket_(std::move(socket)),
        idle_timer_(socket_.get_executor()),
        peer_(std::move(peer)),
        session_(server.instrument_->open_session()) {}

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
  /// logged before the socket closes, so the client sees the close only after it is logged.
  void close(std::string_view reason) {
    if (closed_) {
      return;
    }
    closed_ = true;
    server_->instrument_->close_session(session_);
    log("disconnect", {{"reason", reason}});
    error_code ignored;
    socket_.close(ignored);
    idle_timer_.cancel();
  }

  [[nodiscard]] bool closed() const { return closed_; }

 private:
  /// Reads more bytes unless a chunk's worth is already waiting. It keeps reading while a line
  /// waits for its reply, so a disconnect is seen at once and ends the wait.
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
    if (!running_ && !closed_) {
      const std::size_t newline = received_.find('\n');
      const std::size_t taken = std::min(newline, received_.size());
      line_.append(received_, 0, std::min(taken, kKeptLineLength - line_.size()));
      received_.erase(0, newline == std::string::npos ? taken : taken + 1);
      if (newline != std::string::npos) {
        run(std::exchange(line_, {}));
      }
    }
    read();
  }

  /// Executes `line` on the Session. Its reply may come later, from another thread (a line held
  /// by `*OPC?`), so it is posted to this connection's executor and never waited for here.
  void run(std::string line) {
    if (line.ends_with('\r')) {
      line.pop_back();
    }
    if (line.size() > Instrument::kMaxLineLength) {
      log("error",
          {{"error", std::format("line longer than {} bytes", Instrument::kMaxLineLength)}});
    }
    running_ = true;
    idle_timer_.cancel();  // A Session waiting for a reply is not idle, however long it waits.
    // The handler keeps the connection alive while the line runs, even with no I/O pending (a
    // full input buffer). A held line's handler is dropped when `close` ends the Session.
    server_->instrument_->execute(
        session_, line,
        [self = shared_from_this(),
         executor = socket_.get_executor()](std::optional<std::string> reply) {
          asio::post(executor,
                     [self, reply = std::move(reply)]() mutable { self->reply(std::move(reply)); });
        });
  }

  /// Writes the reply to the line that ran, if it has one, then runs the next line.
  void reply(std::optional<std::string> text) {
    if (closed_) {
      return;
    }
    arm_idle_timer();  // Idle from the reply on, which also bounds a write the client never reads.
    if (!text) {
      running_ = false;
      run_next_line();
      return;
    }
    output_ = std::move(*text);
    output_ += '\n';
    asio::async_write(socket_, asio::buffer(output_),
                      [self = shared_from_this()](const error_code& error, std::size_t /*count*/) {
                        if (error) {
                          self->close(error.message());
                          return;
                        }
                        self->running_ = false;
                        self->run_next_line();
                      });
  }

  /// Closes the connection after the idle timeout unless a command or reply comes first.
  void arm_idle_timer() {
    idle_timer_.expires_after(server_->idle_timeout_);
    idle_timer_.async_wait([self = shared_from_this()](const error_code& error) {
      // A wait that had already finished when the timer was re-armed or cancelled still arrives
      // without an error; the expiry and the running flag tell it is stale.
      if (error || self->running_ ||
          self->idle_timer_.expiry() > std::chrono::steady_clock::now()) {
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
  SessionId session_;
  /// What one read receives into.
  std::array<char, 4096> chunk_{};
  /// Bytes received and not yet taken into a line: less than two chunks.
  std::string received_;
  /// The line being assembled, cut at kKeptLineLength bytes.
  std::string line_;
  /// The reply being written.
  std::string output_;
  bool reading_ = false;
  /// A line is executing or its reply is being written.
  bool running_ = false;
  bool closed_ = false;
};

ScpiServer::ScpiServer(asio::io_context& io, Instrument& instrument, const ServerOptions& options,
                       JsonLog& log)
    : instrument_(&instrument),
      log_(&log),
      idle_timeout_(options.idle_timeout),
      acceptor_(io, tcp::endpoint(asio::ip::make_address(options.address), options.port)),
      port_(acceptor_.local_endpoint().port()) {
  log_->write("listening", {{"address", options.address}, {"port", port_}});
  accept();
}

ScpiServer::~ScpiServer() {
  for (const std::weak_ptr<Connection>& connection : connections_) {
    if (const std::shared_ptr<Connection> open = connection.lock()) {
      open->close("server stopped");
    }
  }
}

void ScpiServer::accept() {
  acceptor_.async_accept([this](const error_code& error, tcp::socket socket) {
    if (error == asio::error::operation_aborted) {
      return;
    }
    if (error) {
      log_->write("error", {{"error", error.message()}});
    } else {
      admit(std::move(socket));
    }
    accept();
  });
}

void ScpiServer::admit(tcp::socket socket) {
  std::erase_if(connections_, [](const std::weak_ptr<Connection>& connection) {
    const std::shared_ptr<Connection> open = connection.lock();
    return open == nullptr || open->closed();
  });
  error_code error;
  const tcp::endpoint peer = socket.remote_endpoint(error);
  if (error) {  // Gone before it was accepted.
    return;
  }
  if (connections_.size() >= kMaxClients) {
    log_->write("refuse", {{"peer", describe(peer)}, {"reason", "too many clients"}});
    socket.close(error);
    return;
  }
  auto connection = std::make_shared<Connection>(*this, std::move(socket), describe(peer));
  connections_.push_back(connection);
  connection->start();
}

}  // namespace openscope
