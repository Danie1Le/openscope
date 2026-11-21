// openscope-engine: runs the instrument live and serves SCPI over TCP until SIGINT or SIGTERM
// (#13). `openscope-engine --help` lists the options.

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/error_code.hpp>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "engine/engine_options.hpp"
#include "openscope/instrument.hpp"
#include "openscope/live_pipeline.hpp"
#include "openscope/server/json_log.hpp"
#include "openscope/server/scpi_server.hpp"

namespace {

/// Exit status for a command line that cannot be parsed.
constexpr int kUsageError = 2;

/// Runs the engine with the command-line `arguments` that follow the program name.
int run(std::span<char*> arguments, openscope::JsonLog& log) {
  const std::vector<std::string_view> options_text(arguments.begin(), arguments.end());
  openscope::EngineOptions options;
  try {
    options = openscope::parse_engine_options(options_text);
  } catch (const std::invalid_argument& error) {
    std::cerr << "openscope-engine: " << error.what() << "\n\n" << openscope::kEngineUsage;
    return kUsageError;
  }
  if (options.help) {
    std::cout << openscope::kEngineUsage;
    return 0;
  }

  // Destroyed in reverse: the server ends its Sessions before the pipeline's threads stop, and
  // both go before the io_context their replies are posted to.
  openscope::Instrument instrument(options.seed);
  boost::asio::io_context io;
  const openscope::LivePipeline live(instrument);
  const openscope::ScpiServer server(io, instrument, options.server, log);
  boost::asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code& error, int signal) {
    if (!error) {
      log.write("stopping", {{"signal", signal}});
      io.stop();
    }
  });
  io.run();
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  openscope::JsonLog log(std::cerr);
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)).subspan(argc > 0 ? 1 : 0), log);
  } catch (const std::exception& error) {  // Such as an address that cannot be bound.
    log.write("error", {{"error", error.what()}});
    return 1;
  }
}
