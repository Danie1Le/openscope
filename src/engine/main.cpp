// openscope-engine: runs the instrument live and serves SCPI over TCP until SIGINT or SIGTERM
// (#13). `openscope-engine --help` lists the options.

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <nlohmann/json.hpp>  // IWYU pragma: keep (the definition of ordered_json)
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
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

/// How often the engine looks at the live counters, and so the most often it logs them.
constexpr std::chrono::seconds kCounterInterval{1};

/// The live counters as log fields, in the `live` and `stopping` events.
nlohmann::ordered_json counter_fields(const openscope::LiveCounters& counters) {
  return {{"dropped_blocks", counters.dropped_blocks},
          {"skipped_samples", counters.skipped_samples},
          {"discarded_captures", counters.discarded_captures}};
}

/// Logs a `live` event with the live pipeline's counters after each kCounterInterval in which they
/// changed, so a dropped block, skipped samples or a discarded capture is never silent (spec #1
/// story 41, NFR-1), and an engine that keeps losing samples logs once a second, not once a block.
class CounterLog {
 public:
  /// Looks at the counters of `live` and writes to `log`, which must both outlive it, on `io`'s
  /// thread.
  CounterLog(boost::asio::io_context& io, const openscope::LivePipeline& live,
             openscope::JsonLog& log)
      : timer_(io), live_(&live), log_(&log) {}

  /// Looks every kCounterInterval from now on, until it is destroyed or `io` stops.
  void start() {
    timer_.expires_after(kCounterInterval);
    timer_.async_wait([this](const boost::system::error_code& error) {
      if (error) {
        return;  // Cancelled: the timer is being destroyed.
      }
      const openscope::LiveCounters now = live_->counters();
      if (now != logged_) {
        log_->write("live", counter_fields(now));
        logged_ = now;
      }
      start();
    });
  }

 private:
  boost::asio::steady_timer timer_;
  const openscope::LivePipeline* live_;
  openscope::JsonLog* log_;
  /// The counters last logged; all zero until something is lost.
  openscope::LiveCounters logged_;
};

/// Runs the engine with the command-line `arguments` that follow the program name.
int run(std::span<char*> arguments, openscope::JsonLog& log) {
  const std::vector<std::string_view> options_text(arguments.begin(), arguments.end());
  openscope::EngineOptions options;
  try {
    options = openscope::parse_engine_options(options_text);
  } catch (const std::invalid_argument& error) {
    std::cerr << "openscope-engine: " << error.what() << "\n\n" << openscope::engine_usage();
    return kUsageError;
  }
  if (options.help) {
    std::cout << openscope::engine_usage();
    return 0;
  }

  // Destroyed in reverse: the pipeline's threads stop first, so no reply is posted while the server
  // ends its Sessions, and both go before the io_context replies are posted to.
  boost::asio::io_context io;
  openscope::Instrument instrument(options.seed);
  std::optional<openscope::ScpiServer> server;
  std::optional<openscope::LivePipeline> live;
  // Caught from here on, so a signal while the engine starts still stops it cleanly: io.run()
  // handles it at once, and only once the pipeline below runs, so `stopping` always has its counts.
  boost::asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](const auto& error, int signal) {
    if (!error) {
      nlohmann::ordered_json fields = {{"signal", signal}};
      if (live) {
        fields.update(counter_fields(live->counters()));
      }
      log.write("stopping", fields);
      io.stop();
    }
  });
  try {
    server.emplace(io, instrument, options.server, log);
  } catch (const boost::system::system_error& error) {
    const std::string message =
        std::format("cannot listen on {} port {}: {}", options.server.address, options.server.port,
                    error.code().message());
    log.write("error", {{"error", message}});
    return 1;
  }
  live.emplace(instrument);
  CounterLog counter_log(io, *live, log);
  counter_log.start();
  io.run();
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  openscope::JsonLog log(std::cerr);
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)).subspan(argc > 0 ? 1 : 0), log);
  } catch (const std::exception& error) {  // Anything else that stops the engine starting.
    log.write("error", {{"error", error.what()}});
    return 1;
  }
}
