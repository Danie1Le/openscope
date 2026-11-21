// The SCPI server over real loopback sockets (#13): framing, ordering, one Session per
// connection, the line and client limits, the idle timeout, `*OPC?` waits across connections,
// cleanup on disconnect, and garbage input. The server runs on its own I/O thread, as in the
// engine, so the TSan build checks every reply handed to it from another thread.

#include "openscope/server/scpi_server.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
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

/// The first of `events` named `name`, or null.
nlohmann::json first_event(const std::vector<nlohmann::json>& events, std::string_view name) {
  const auto found = std::ranges::find_if(
      events, [&](const nlohmann::json& event) { return event["event"] == name; });
  return found == events.end() ? nlohmann::json() : *found;
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
  explicit Client(std::uint16_t port) {
    socket_.connect({boost::asio::ip::address_v4::loopback(), port});
  }

  /// Sends `text` as it is: terminators are the caller's.
  void send(std::string_view text) { boost::asio::write(socket_, boost::asio::buffer(text)); }

  /// The next reply line without its `\n`; nullopt when none arrives within `patience` or the
  /// server has closed the connection.
  std::optional<std::string> read_line(milliseconds patience = kPatience) {
    std::optional<std::string> line;
    boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(received_), '\n',
                                  [&](const boost::system::error_code& error, std::size_t length) {
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

class ScpiServerTest : public testing::Test {
 protected:
  /// Restarts the server with `idle_timeout`.
  void restart(milliseconds idle_timeout) {
    server.emplace(instrument, ServerOptions{.port = 0, .idle_timeout = idle_timeout});
  }

  [[nodiscard]] std::uint16_t port() const { return server->port(); }

  Instrument instrument;
  std::optional<RunningServer> server{std::in_place, instrument, ServerOptions{.port = 0}};
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
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(client.query("CHAN1:OFFS " + std::to_string(i) + ";OFFS?"), std::to_string(i));
  }
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

  std::optional<std::string> line;
  do {
    line = client.read_line();
  } while (line && !line->starts_with("OpenScope,"));
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
  std::size_t connects = 0;
  std::size_t disconnects = 0;
  std::size_t refusals = 0;
  for (const nlohmann::json& event : events) {
    if (event["event"] == "connect") {
      ++connects;
      EXPECT_TRUE(event["session"].is_number());
      EXPECT_THAT(event["peer"].get<std::string>(), StartsWith("127.0.0.1:"));
    } else if (event["event"] == "disconnect") {
      ++disconnects;
      EXPECT_EQ(event["reason"], "idle timeout");
    } else if (event["event"] == "refuse") {
      ++refusals;
      EXPECT_EQ(event["reason"], "too many clients");
    }
  }
  EXPECT_EQ(connects, ScpiServer::kMaxClients);
  EXPECT_EQ(disconnects, ScpiServer::kMaxClients);
  EXPECT_EQ(refusals, 1U);
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
