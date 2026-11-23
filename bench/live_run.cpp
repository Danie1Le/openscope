// Runs the instrument live with all four channels on for a number of seconds (the first argument,
// 10 by default) at a sample rate (the second, the *RST 1 MS/s by default). Once a second it prints
// how far captures have got against the wall clock, how many samples acquisition skipped for
// falling behind it, how many blocks were dropped and captures discarded, and the longest a
// `*IDN?`, sent every 5 ms, took that second. Results are recorded in docs/benchmarks.md (NFR-1:
// zero dropped blocks at 4 channels x 1 MS/s).

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <thread>

#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/live_pipeline.hpp"
#include "openscope/sample_clock.hpp"

namespace {

int run(std::span<char*> arguments) {
  using std::chrono::steady_clock;
  using Milliseconds = std::chrono::duration<double, std::milli>;
  const int seconds = arguments.size() > 1 ? std::stoi(arguments[1]) : 10;
  // The *RST sample rate unless one is given.
  const std::string rate_command =
      arguments.size() > 2 ? std::format("ACQ:SRAT {};:", arguments[2]) : std::string();
  openscope::Instrument instrument;
  const openscope::SessionId session = instrument.open_session(openscope::run_at_once);
  // Every channel on, each with noise.
  instrument.execute(session,
                     rate_command +
                         "CHAN2:DISP ON;:CHAN3:DISP ON;:CHAN4:DISP ON;:SOUR1:NOIS 0.01;:SOUR2:"
                         "NOIS 0.01;:SOUR3:NOIS 0.01;:SOUR4:NOIS 0.01;:SYST:ERR?",
                     [](const std::optional<std::string>& reply) {
                       std::cout << "setup errors: " << reply.value_or("") << '\n';
                     });

  const auto start = steady_clock::now();
  const openscope::LivePipeline live(instrument);
  std::cout << std::format("{:>8} {:>14} {:>14} {:>14} {:>8} {:>10} {:>10}\n", "seconds",
                           "wall samples", "captured to", "skipped", "dropped", "discarded",
                           "*IDN? ms");
  for (int second = 1; second <= seconds; ++second) {
    Milliseconds slowest{0};
    while (steady_clock::now() < start + std::chrono::seconds(second)) {
      const auto sent = steady_clock::now();
      instrument.execute(session, "*IDN?", [](const std::optional<std::string>& /*reply*/) {});
      slowest = std::max<Milliseconds>(slowest, steady_clock::now() - sent);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::chrono::duration<double> elapsed = steady_clock::now() - start;
    const std::shared_ptr<const openscope::Capture> capture = instrument.latest_capture();
    const openscope::SampleIndex captured_to =
        capture == nullptr ? 0 : capture->first_index + capture->record_length;
    std::cout << std::format("{:>8.3f} {:>14.0f} {:>14} {:>14} {:>8} {:>10} {:>10.2f}\n",
                             elapsed.count(), elapsed.count() * instrument.sample_rate(),
                             captured_to, live.skipped_samples(), live.dropped_blocks(),
                             instrument.discarded_captures(), slowest.count())
              << std::flush;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& error) {  // A bad argument, for one.
    std::cerr << "live_run: " << error.what() << '\n';
    return 1;
  }
}
