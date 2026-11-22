// The SCPI server over real loopback sockets (#13): framing, ordering, one Session per
// connection, the line and client limits, the idle timeout, `*OPC?` waits across connections,
// cleanup on disconnect, TCP keepalive, and garbage input. The server runs on its own I/O thread,
// as in the engine, so the TSan build checks every reply handed to it from another thread.

#include "openscope/server/scpi_server.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>  // IWYU pragma: keep (the definition of json)
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "eventually.hpp"
#include "instrument_fixture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/server/json_log.hpp"
#include "openscope/server/server_options.hpp"
#include "scpi_session.hpp"

namespace openscope {
namespace {

using namespace std::string_view_literals;
using std::chrono::milliseconds;
using test::kInputBufferOverrunReply;
using test::kNoErrorReply;
using test::kUndefinedHeaderReply;
using testing::StartsWith;

using test::kPatience;
using test::padded;

/// How long a test watches for something that should not happen.
constexpr milliseconds kQuiet{300};

/// The `events` named `name`, in order.
std::vector<nlohmann::json> events_named(const std::vector<nlohmann::json>& events,
                                         std::string_view name) {
  std::vector<nlohmann::json> named;
  std::ranges::copy_if(events, std::back_inserter(named),
                       [&](const nlohmann::json& event) { return event["event"] == name; });
  return named;
}

/// The first of `events` named `name`, or null.
nlohmann::json first_event(const std::vector<nlohmann::json>& events, std::string_view name) {
  std::vector<nlohmann::json> named = events_named(events, name);
  return named.empty() ? nlohmann::json() : named.front();
}

/// A ScpiServer on a free loopback port, run on its own I/O thread like the engine's.
class RunningServer {
 public:
  RunningServer(Instrument& instrument, const ServerOptions& options)
      : server_(io_, instrument, options, log_), io_thread_([this] { io_.run(); }) {}

  RunningServer(const RunningServer&) = delete;
  RunningServer& operator=(const RunningServer&) = delete;
  RunningServer(RunningServer&&) = delete;
  RunningServer& operator=(RunningServer&&) = delete;
  ~RunningServer() { stop(); }

  [[nodiscard]] std::uint16_t port() const { return server_.port(); }

  /// Every event logged so far, parsed. The log is read on the I/O thread, which writes it.
  std::vector<nlohmann::json> events() {
    std::promise<std::string> text;
    boost::asio::post(io_, [&] { text.set_value(log_text_.str()); });
    std::vector<nlohmann::json> parsed;
    std::istringstream lines(text.get_future().get());
    for (std::string line; std::getline(lines, line);) {
      parsed.push_back(nlohmann::json::parse(line));
    }
    return parsed;
  }

  /// Every event logged, once one named `name` has been; fails the test if none is within
  /// kPatience. A connection that the client closes is logged only once the server sees it.
  std::vector<nlohmann::json> events_once_logged(std::string_view name) {
    std::vector<nlohmann::json> logged;
    if (!test::eventually([&] {
          logged = events();
          return !first_event(logged, name).is_null();
        })) {
      ADD_FAILURE() << "no " << name << " event logged";
    }
    return logged;
  }

 private:
  void stop() {
    if (io_thread_.joinable()) {
      io_.stop();
      io_thread_.join();
    }
  }

  std::ostringstream log_text_;
  JsonLog log_{log_text_};
  boost::asio::io_context io_;
  ScpiServer server_;
  std::thread io_thread_;
};

/// A SCPI client on a raw socket, with a deadline on every read.
class Client {
 public:
  explicit Client(std::uint16_t port,
                  const boost::asio::ip::address& host = boost::asio::ip::address_v4::loopback()) {
    socket_.connect({host, port});
  }

  /// The port of this end of the connection.
  [[nodiscard]] std::uint16_t local_port() const { return socket_.local_endpoint().port(); }

  /// Sends `text` as it is: terminators are the caller's.
  void send(std::string_view text) { boost::asio::write(socket_, boost::asio::buffer(text)); }

  /// The next reply line without its `\n`; nullopt when none arrives within `patience` or the
  /// server has closed the connection.
  std::optional<std::string> read_line(milliseconds patience = kPatience) {
    std::optional<std::string> line;
    boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(received_), '\n',
                                  [&](const auto& error, std::size_t length) {
                                    if (error) {
                                      closed_ = error != boost::asio::error::operation_aborted;
                                      return;
                                    }
                                    line = received_.substr(0, length - 1);
                                    received_.erase(0, length);
                                  });
    run(patience);
    return line;
  }

  /// Sends `line` with a `\n` and returns its reply.
  std::optional<std::string> query(std::string_view line) {
    send(std::string(line) + '\n');
    return read_line();
  }

  /// Whether the server closes the connection within `patience`; reply lines before that are
  /// skipped.
  bool closed_by_server(milliseconds patience = kPatience) {
    while (!closed_ && read_line(patience)) {
    }
    return closed_;
  }

 private:
  void run(milliseconds patience) {
    io_.restart();
    io_.run_for(patience);
    if (!io_.stopped()) {  // Timed out: cancel the read and let its handler see that.
      socket_.cancel();
      io_.restart();
      io_.run();
    }
  }

  boost::asio::io_context io_;
  boost::asio::ip::tcp::socket socket_{io_};
  std::string received_;
  bool closed_ = false;
};

/// A connection the server has admitted: it answers `*IDN?`. Retries while the server is still
/// at its client limit, since a disconnect frees a slot only once the server has seen it.
std::unique_ptr<Client> admitted_client(std::uint16_t port) {
  std::unique_ptr<Client> client;
  if (!test::eventually([&] {
        client = std::make_unique<Client>(port);
        return client->query("*IDN?").has_value();
      })) {
    ADD_FAILURE() << "no connection admitted";
  }
  return client;
}

/// kMaxClients admitted connections, which fill the server.
std::vector<std::unique_ptr<Client>> all_clients(std::uint16_t port) {
  std::vector<std::unique_ptr<Client>> clients;
  for (std::size_t i = 0; i < ScpiServer::kMaxClients; ++i) {
    clients.push_back(admitted_client(port));
  }
  return clients;
}

/// Matches the reply to `*IDN?`, which shows that a line ran and the connection works.
auto idn_reply() { return testing::Optional(StartsWith("OpenScope,")); }

/// The descriptors this process could have open: more than any test opens.
constexpr int kMostDescriptors = 4096;

/// The server's socket for the connection from the client at `client_port`: the descriptor of
/// this process bound to `server_port` whose peer is at `client_port`, or -1.
int server_socket(std::uint16_t server_port, std::uint16_t client_port) {
  const auto port_of = [](int descriptor, auto name) -> std::uint16_t {
    boost::asio::ip::tcp::endpoint endpoint;
    socklen_t size = static_cast<socklen_t>(endpoint.capacity());
    return name(descriptor, endpoint.data(), &size) == 0 ? endpoint.port() : 0;
  };
  for (int descriptor = 0; descriptor < kMostDescriptors; ++descriptor) {
    if (port_of(descriptor, ::getsockname) == server_port &&
        port_of(descriptor, ::getpeername) == client_port) {
      return descriptor;
    }
  }
  return -1;
}

/// The int socket option `name` at `level` of `descriptor`, or -1.
int socket_option(int descriptor, int level, int name) {
  int value = -1;
  socklen_t size = sizeof value;
  return ::getsockopt(descriptor, level, name, &value, &size) == 0 ? value : -1;
}

class ScpiServerTest : public testing::Test {
 protected:
  /// Restarts the server with `idle_timeout`.
  void restart(milliseconds idle_timeout) {
    server.reset();
    server = std::make_unique<RunningServer>(
        instrument, ServerOptions{.port = 0, .idle_timeout = idle_timeout});
  }

  [[nodiscard]] std::uint16_t port() const { return server->port(); }

  Instrument instrument;
  std::unique_ptr<RunningServer> server =
      std::make_unique<RunningServer>(instrument, ServerOptions{.port = 0});
};

TEST(ServerOptionsTest, DefaultsToLoopbackOnPort5025) {
  const ServerOptions options;
  EXPECT_EQ(options.address, "127.0.0.1");
  EXPECT_EQ(options.port, 5025);
}

TEST_F(ScpiServerTest, RepliesToAQueryWithALine) {
  Client client(port());

  EXPECT_THAT(client.query("*IDN?"), testing::Optional(StartsWith("OpenScope,OS-4,")));
}

TEST_F(ScpiServerTest, LineEndsAtNewlineWithAnOptionalCarriageReturnBeforeIt) {
  Client client(port());

  client.send("*IDN?\r\nSYST:ERR?\r\n");

  EXPECT_THAT(client.read_line(), idn_reply());
  EXPECT_EQ(client.read_line(), kNoErrorReply);  // The `\r` was not part of the line.
}

TEST_F(ScpiServerTest, LinesSplitAcrossWritesOrSharingOneRunInOrder) {
  Client client(port());

  client.send("CHAN1:OF");
  client.send("FS 0.1\nCHAN1:OFFS?\n\n\r\nCHAN1:OFFS 0.2\nCHAN1:OFFS?;");
  client.send(":SYST:ERR?\n");

  EXPECT_EQ(client.read_line(), "0.1");
  EXPECT_EQ(client.read_line(), "0.2;" + std::string(kNoErrorReply));  // Empty lines do nothing.
}

TEST_F(ScpiServerTest, ManyPipelinedLinesAllReplyInOrder) {
  Client client(port());
  std::string lines;
  for (int i = 0; i < 1000; ++i) {
    lines += "CHAN1:OFFS " + std::to_string(i % 50) + ";OFFS?\n";
  }

  client.send(lines);

  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(client.read_line(), std::to_string(i % 50));
  }
}

TEST_F(ScpiServerTest, EachConnectionIsItsOwnSession) {
  Client first(port());
  Client second(port());

  first.send("BOGUS\n");

  EXPECT_EQ(second.query("SYST:ERR?"), kNoErrorReply);
  EXPECT_EQ(first.query("SYST:ERR?"), kUndefinedHeaderReply);
}

TEST_F(ScpiServerTest, LineOfTheMaximumLengthRuns) {
  Client client(port());

  client.send(padded("*IDN?", Instrument::kMaxLineLength) + "\n");
  client.send(padded("*IDN?", Instrument::kMaxLineLength) + "\r\n");

  EXPECT_THAT(client.read_line(), idn_reply());
  EXPECT_THAT(client.read_line(), idn_reply());
  EXPECT_EQ(client.query("SYST:ERR?"), kNoErrorReply);
}

class OversizedLineTest : public ScpiServerTest, public testing::WithParamInterface<std::string> {};

// The line is rejected with -363 and runs nothing (its `*IDN?` does not reply); the connection
// carries on with the next line.
TEST_P(OversizedLineTest, IsRejectedWithInputBufferOverrunAndTheConnectionCarriesOn) {
  Client client(port());

  client.send(GetParam() + "SYST:ERR?;ERR?\n");

  EXPECT_EQ(client.read_line(), std::string(kInputBufferOverrunReply) + ";" + kNoErrorReply);
  EXPECT_EQ(client.query("CHAN1:OFFS 0.5;OFFS?"), "0.5");
}

INSTANTIATE_TEST_SUITE_P(
    Lines, OversizedLineTest,
    testing::Values(
        // One byte over.
        padded("*IDN?", Instrument::kMaxLineLength + 1) + "\n",
        padded("*IDN?", Instrument::kMaxLineLength + 1) + "\r\n",
        // A `\r` that is not just before the `\n` is part of the line.
        padded("*IDN?", Instrument::kMaxLineLength) + "\rX\n",
        // Far longer than any buffer the server keeps, with a NUL and bytes over 0x7F in it.
        padded("*IDN?\0\xff\x80"sv, 1 << 20) + "\n"));

TEST_F(ScpiServerTest, RefusesAConnectionBeyondTheClientLimit) {
  std::vector<std::unique_ptr<Client>> clients = all_clients(port());

  Client refused(port());

  EXPECT_TRUE(refused.closed_by_server());
  for (const auto& client : clients) {
    EXPECT_THAT(client->query("*IDN?"), idn_reply());
  }
  clients.pop_back();  // Disconnecting frees a slot.
  EXPECT_THAT(admitted_client(port())->query("*IDN?"), idn_reply());
}

TEST_F(ScpiServerTest, ClosesAConnectionThatStaysIdle) {
  restart(milliseconds(100));
  Client client(port());

  EXPECT_TRUE(client.closed_by_server());
}

TEST_F(ScpiServerTest, IdleTimeoutCountsFromTheLastCommandOrReply) {
  restart(milliseconds(2000));
  Client client(port());

  // Each command comes within the timeout of the last reply, but all of them span longer than it.
  for (int i = 0; i < 2; ++i) {
    std::this_thread::sleep_for(milliseconds(1200));
    EXPECT_THAT(client.query("*IDN?"), idn_reply());
  }

  EXPECT_TRUE(client.closed_by_server());
}

// A client trickling bytes that never make a line is still idle.
TEST_F(ScpiServerTest, PartialLineDoesNotCountAsACommandForTheIdleTimeout) {
  restart(milliseconds(500));
  Client client(port());

  bool closed = false;
  for (int i = 0; i < 30 && !closed; ++i) {  // Bytes for six times the timeout.
    client.send("*");
    closed = client.closed_by_server(milliseconds(100));
  }

  EXPECT_TRUE(closed);
}

// A client that sends queries and never reads the replies cannot keep its slot: once the socket
// buffers are full the reply's write stalls, and the idle timeout still closes the connection.
TEST_F(ScpiServerTest, ClosesAConnectionWhoseRepliesAreNeverRead) {
  restart(milliseconds(200));
  instrument.advance(test::kResetAutoTimeout);  // A capture, so WAV:DATA? replies ~100 KB.
  Client client(port());
  std::string queries;
  for (int i = 0; i < 500; ++i) {
    queries += "WAV:DATA?\n";
  }

  client.send(queries);

  const std::vector<nlohmann::json> events = server->events_once_logged("disconnect");
  EXPECT_EQ(first_event(events, "disconnect")["reason"], "idle timeout");
}

TEST_F(ScpiServerTest, SessionWaitingOnOpcIsNotClosedByTheIdleTimeout) {
  restart(milliseconds(100));
  Client waiting(port());
  ASSERT_EQ(waiting.query("SING;:SYST:ERR?"), kNoErrorReply);

  waiting.send("*OPC?\n");

  EXPECT_FALSE(waiting.closed_by_server(milliseconds(10 * 100)));
  instrument.advance(test::kResetAutoTimeout);  // The Single's capture completes.
  EXPECT_EQ(waiting.read_line(), "1");
  EXPECT_TRUE(waiting.closed_by_server());  // Idle again, from the reply on.
}

TEST_F(ScpiServerTest, OpcWaitHoldsOnlyItsOwnConnection) {
  Client waiting(port());
  Client other(port());
  waiting.send("SING;*OPC?\n");

  EXPECT_EQ(waiting.read_line(kQuiet), std::nullopt);
  EXPECT_THAT(other.query("*IDN?"), idn_reply());
  EXPECT_EQ(other.query("STOP;*OPC?"), "1");  // STOP ends the Single, on the other connection.
  EXPECT_EQ(waiting.read_line(), "1");
}

TEST_F(ScpiServerTest, LinesAfterAHeldLineWaitForIt) {
  Client client(port());

  client.send("SING;*OPC?\n*IDN?\n");

  EXPECT_EQ(client.read_line(kQuiet), std::nullopt);
  instrument.advance(test::kResetAutoTimeout);
  EXPECT_EQ(client.read_line(), "1");
  EXPECT_THAT(client.read_line(), idn_reply());
}

// Bytes keep arriving while a line is held; every line after it, whenever it arrives, runs once.
TEST_F(ScpiServerTest, LinesSentWhileOneIsHeldAndAfterItRunOnceInOrder) {
  Client client(port());
  Client other(port());
  client.send("SING;*OPC?\n");
  EXPECT_EQ(client.read_line(kQuiet), std::nullopt);
  client.send("CHAN1:OFFS 0.1;OFFS?\n");
  EXPECT_EQ(client.read_line(kQuiet), std::nullopt);

  EXPECT_EQ(other.query("STOP;*OPC?"), "1");

  EXPECT_EQ(client.read_line(), "1");
  EXPECT_EQ(client.read_line(), "0.1");
  EXPECT_EQ(client.query("CHAN1:OFFS 0.2;OFFS?"), "0.2");
  EXPECT_EQ(client.query("CHAN1:OFFS 0.3;OFFS?"), "0.3");
}

TEST_F(ScpiServerTest, DisconnectWhileWaitingOnOpcEndsTheSessionAndFreesItsSlot) {
  auto waiting = std::make_unique<Client>(port());
  waiting->send("SING;*OPC?\n");
  EXPECT_EQ(waiting->read_line(kQuiet), std::nullopt);

  waiting.reset();
  std::vector<std::unique_ptr<Client>> clients = all_clients(port());
  instrument.advance(test::kResetAutoTimeout);  // Ends the Single the dropped line waited on.

  EXPECT_THAT(clients.back()->query("*IDN?"), idn_reply());
}

// A client host that loses power or network sends no FIN or RST, and while its line waits on
// `*OPC?` the idle timeout is off, so only TCP keepalive notices it: its probes go unanswered, the
// pending read fails and the connection closes as on the disconnect above, freeing the slot. On
// loopback the client's kernel answers every probe, and making a peer go silent needs privileges
// tests do not have (a firewall rule or TCP_REPAIR), so this checks the probe settings on the
// server's end of a connection that waits.
TEST_F(ScpiServerTest, ProbesAClientThatWaitsOnOpcWithTcpKeepalive) {
  Client waiting(port());
  ASSERT_THAT(waiting.query("*IDN?"), idn_reply());  // Admitted.
  waiting.send("SING;*OPC?\n");
  EXPECT_EQ(waiting.read_line(kQuiet), std::nullopt);

  const int socket = server_socket(port(), waiting.local_port());

  ASSERT_NE(socket, -1);
  EXPECT_EQ(socket_option(socket, SOL_SOCKET, SO_KEEPALIVE), 1);
  EXPECT_EQ(socket_option(socket, IPPROTO_TCP, TCP_KEEPIDLE), ScpiServer::kKeepAliveIdle.count());
  EXPECT_EQ(socket_option(socket, IPPROTO_TCP, TCP_KEEPINTVL),
            ScpiServer::kKeepAliveInterval.count());
  EXPECT_EQ(socket_option(socket, IPPROTO_TCP, TCP_KEEPCNT), ScpiServer::kKeepAliveProbes);
  // A vanished client is noticed within a minute of its last traffic.
  EXPECT_LE(
      ScpiServer::kKeepAliveIdle + ScpiServer::kKeepAliveProbes * ScpiServer::kKeepAliveInterval,
      std::chrono::minutes(1));
}

TEST_F(ScpiServerTest, StoppingTheServerEndsItsSessionsEvenWhileOneWaits) {
  Client waiting(port());
  waiting.send("SING;*OPC?\n");
  EXPECT_EQ(waiting.read_line(kQuiet), std::nullopt);

  server.reset();

  EXPECT_TRUE(waiting.closed_by_server());
  instrument.advance(test::kResetAutoTimeout);  // The dropped line's reply goes nowhere.
  test::ScpiSession session(instrument);
  EXPECT_EQ(session.send("*OPC?"), "1");
}

TEST_F(ScpiServerTest, RandomBytesNeverStopTheServer) {
  Client client(port());
  std::mt19937 random(13);
  std::uniform_int_distribution<int> byte(0, 255);
  std::string garbage(1 << 16, '\0');
  for (char& c : garbage) {
    c = static_cast<char>(byte(random));
  }

  // In writes of random sizes, so bytes also arrive while a line runs.
  std::uniform_int_distribution<std::size_t> write_size(1, 8192);
  for (std::size_t sent = 0; sent < garbage.size();) {
    const std::size_t size = std::min(write_size(random), garbage.size() - sent);
    client.send(std::string_view(garbage).substr(sent, size));
    sent += size;
  }
  client.send("\n*CLS;*IDN?\n");

  // Lines of the garbage may reply too; the *IDN? reply comes after them.
  std::optional<std::string> line = client.read_line();
  while (line && !line->starts_with("OpenScope,")) {
    line = client.read_line();
  }
  EXPECT_TRUE(line.has_value());
  EXPECT_EQ(client.query("SYST:ERR?"), kNoErrorReply);
}

TEST_F(ScpiServerTest, LogsListeningConnectsRefusalsAndDisconnectsAsJsonLines) {
  restart(milliseconds(100));
  {
    const std::vector<std::unique_ptr<Client>> clients = all_clients(port());
    EXPECT_TRUE(Client(port()).closed_by_server());
    for (const auto& client : clients) {
      EXPECT_TRUE(client->closed_by_server());  // Idle timeout.
    }
  }

  // The server logs an event before it closes the socket, so the clients have seen all of them.
  const std::vector<nlohmann::json> events = server->events();

  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front()["event"], "listening");
  EXPECT_EQ(events.front()["address"], "127.0.0.1");
  EXPECT_EQ(events.front()["port"], port());
  const std::vector<nlohmann::json> connects = events_named(events, "connect");
  EXPECT_EQ(connects.size(), ScpiServer::kMaxClients);
  for (const nlohmann::json& connect : connects) {
    EXPECT_TRUE(connect["session"].is_number());
    EXPECT_THAT(connect["peer"].get<std::string>(), StartsWith("127.0.0.1:"));
  }
  const std::vector<nlohmann::json> disconnects = events_named(events, "disconnect");
  EXPECT_EQ(disconnects.size(), ScpiServer::kMaxClients);
  for (const nlohmann::json& disconnect : disconnects) {
    EXPECT_EQ(disconnect["reason"], "idle timeout");
  }
  EXPECT_EQ(events_named(events, "refuse").size(), 1U);
  EXPECT_EQ(first_event(events, "refuse")["reason"], "too many clients");
}

TEST_F(ScpiServerTest, LogsAnOversizedLineAndAClientDisconnect) {
  {
    Client client(port());
    client.send(padded("", Instrument::kMaxLineLength + 1) + "\n");
    EXPECT_EQ(client.query("SYST:ERR?"), kInputBufferOverrunReply);
  }

  const std::vector<nlohmann::json> events = server->events_once_logged("disconnect");

  const nlohmann::json connect = first_event(events, "connect");
  const nlohmann::json error = first_event(events, "error");
  const nlohmann::json disconnect = first_event(events, "disconnect");
  EXPECT_EQ(error["error"], "line longer than 4096 bytes");
  EXPECT_EQ(error["session"], connect["session"]);
  EXPECT_EQ(disconnect["reason"], "closed by client");
  EXPECT_EQ(disconnect["session"], connect["session"]);
  EXPECT_EQ(disconnect["peer"], connect["peer"]);
}

// An IPv6 peer is logged as `[address]:port`, so the port can be split off.
TEST(ScpiServerIpv6Test, LogsAPeerInBrackets) {
  Instrument instrument;
  std::unique_ptr<RunningServer> server;
  try {
    server =
        std::make_unique<RunningServer>(instrument, ServerOptions{.address = "::1", .port = 0});
  } catch (const boost::system::system_error& error) {
    GTEST_SKIP() << "no IPv6 loopback: " << error.what();
  }
  EXPECT_THAT(Client(server->port(), boost::asio::ip::address_v6::loopback()).query("*IDN?"),
              idn_reply());

  const nlohmann::json connect = first_event(server->events_once_logged("connect"), "connect");

  EXPECT_THAT(connect["peer"].get<std::string>(), testing::MatchesRegex(R"(\[::1\]:[0-9]+)"));
}

/// What a ScpiServer needs, for tests that only construct one.
class ScpiServerSetupTest : public testing::Test {
 protected:
  Instrument instrument;
  std::ostringstream log_text;
  JsonLog log{log_text};
  boost::asio::io_context io;
};

// Host names are not resolved: the address is taken literally.
TEST_F(ScpiServerSetupTest, RejectsAnAddressThatIsNotAnIpAddress) {
  const ServerOptions options{.address = "localhost", .port = 0};

  EXPECT_THROW(const ScpiServer server(io, instrument, options, log), boost::system::system_error);
}

TEST_F(ScpiServerSetupTest, RejectsAPortAlreadyInUse) {
  const ScpiServer first(io, instrument, {.port = 0}, log);
  const ServerOptions options{.port = first.port()};

  EXPECT_THROW(const ScpiServer second(io, instrument, options, log), boost::system::system_error);
}

}  // namespace
}  // namespace openscope
