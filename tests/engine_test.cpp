// openscope-engine as a process (#13): its exit statuses, and a clean stop on SIGINT or SIGTERM.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "engine/engine_options.hpp"
#include "eventually.hpp"

namespace openscope {
namespace {

using testing::HasSubstr;

/// openscope-engine started with `arguments`; its standard output and error are read as one.
class EngineProcess {
 public:
  explicit EngineProcess(std::vector<std::string> arguments) {
    std::array<int, 2> pipe_ends{};
    if (::pipe(pipe_ends.data()) != 0) {
      throw std::system_error(errno, std::generic_category(), "pipe");
    }
    output_end_ = pipe_ends[0];
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
    std::string path = OPENSCOPE_ENGINE_PATH;
    std::vector<char*> argv{path.data()};
    for (std::string& argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const int error = posix_spawn(&pid_, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_ends[1]);
    if (error != 0) {
      ::close(output_end_);
      throw std::system_error(error, std::generic_category(), "posix_spawn " + path);
    }
  }

  EngineProcess(const EngineProcess&) = delete;
  EngineProcess& operator=(const EngineProcess&) = delete;
  EngineProcess(EngineProcess&&) = delete;
  EngineProcess& operator=(EngineProcess&&) = delete;

  /// Kills the engine if it is still running.
  ~EngineProcess() {
    if (!exited_) {
      // NOLINTNEXTLINE(misc-include-cleaner): <csignal> declares them, through <signal.h>.
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    ::close(output_end_);
  }

  /// Whether the output contains `text`, reading until it does, ends, or kPatience runs out.
  bool printed(std::string_view text) {
    const auto deadline = std::chrono::steady_clock::now() + test::kPatience;
    while (output_.find(text) == std::string::npos) {
      if (!read_some(deadline)) {
        return false;
      }
    }
    return true;
  }

  // NOLINTNEXTLINE(misc-include-cleaner): <csignal> declares kill, through <signal.h>.
  void signal(int signal) const { ::kill(pid_, signal); }

  /// The exit status once the engine exits, as a shell gives it: 128 plus the signal number if a
  /// signal ended it; -1 if its output has not ended within kPatience.
  int exit_status() {
    const auto deadline = std::chrono::steady_clock::now() + test::kPatience;
    while (read_some(deadline)) {
    }
    if (!output_ended_) {
      return -1;
    }
    int status = 0;
    ::waitpid(pid_, &status, 0);
    exited_ = true;
    // NOLINTNEXTLINE(misc-include-cleaner): <sys/wait.h> defines them; <cstdlib> may first.
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }

  /// Its standard output and error so far.
  [[nodiscard]] const std::string& output() const { return output_; }

 private:
  /// Appends what the engine writes next to output_; false once the output ends or `deadline`
  /// passes.
  bool read_some(std::chrono::steady_clock::time_point deadline) {
    if (output_ended_) {
      return false;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd readable{.fd = output_end_, .events = POLLIN, .revents = 0};
    if (left.count() <= 0 || ::poll(&readable, 1, static_cast<int>(left.count())) <= 0) {
      return false;
    }
    std::array<char, 4096> chunk{};
    const ssize_t count = ::read(output_end_, chunk.data(), chunk.size());
    if (count <= 0) {
      output_ended_ = true;
      return false;
    }
    output_.append(chunk.data(), static_cast<std::size_t>(count));
    return true;
  }

  pid_t pid_ = 0;
  int output_end_ = -1;
  std::string output_;
  bool output_ended_ = false;
  bool exited_ = false;
};

TEST(EngineProcessTest, HelpPrintsTheUsageAndExitsWithStatus0) {
  EngineProcess engine({"--help"});

  EXPECT_EQ(engine.exit_status(), 0);
  EXPECT_EQ(engine.output(), engine_usage());
}

class InvalidCommandLineTest : public testing::TestWithParam<std::vector<std::string>> {};

TEST_P(InvalidCommandLineTest, PrintsTheErrorAndTheUsageAndExitsWithStatus2) {
  EngineProcess engine(GetParam());

  EXPECT_EQ(engine.exit_status(), 2);
  EXPECT_THAT(engine.output(), testing::StartsWith("openscope-engine: "));
  EXPECT_THAT(engine.output(), HasSubstr(engine_usage()));
}

INSTANTIATE_TEST_SUITE_P(Options, InvalidCommandLineTest,
                         testing::Values(std::vector<std::string>{"--bogus"},
                                         std::vector<std::string>{"--port", "70000"},
                                         std::vector<std::string>{"--address", "localhost"}));

TEST(EngineProcessTest, PortInUseLogsAnErrorAndExitsWithStatus1) {
  boost::asio::io_context io;
  const boost::asio::ip::tcp::acceptor taken(io, {boost::asio::ip::address_v4::loopback(), 0});
  const std::uint16_t port = taken.local_endpoint().port();

  EngineProcess engine({"--port", std::to_string(port)});

  EXPECT_EQ(engine.exit_status(), 1);
  EXPECT_THAT(engine.output(), HasSubstr(R"("event":"error")"));
  EXPECT_THAT(engine.output(), HasSubstr(std::format("cannot listen on 127.0.0.1 port {}", port)));
}

class SignalTest : public testing::TestWithParam<int> {};

// The signal comes the moment the engine says it listens, before it has started the pipeline.
TEST_P(SignalTest, StopsTheEngineCleanlyFromTheMomentItListens) {
  EngineProcess engine({"--port", "0"});
  ASSERT_TRUE(engine.printed(R"("event":"listening")")) << engine.output();

  engine.signal(GetParam());

  EXPECT_EQ(engine.exit_status(), 0) << engine.output();
  EXPECT_THAT(engine.output(),
              HasSubstr(std::format(R"("event":"stopping","signal":{})", GetParam())));
}

INSTANTIATE_TEST_SUITE_P(Signals, SignalTest, testing::Values(SIGINT, SIGTERM));

}  // namespace
}  // namespace openscope
