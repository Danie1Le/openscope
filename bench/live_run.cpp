// Runs the instrument live with all four channels on for a number of seconds (the first argument,
// 10 by default) at a sample rate (the second, the *RST 1E6 by default). Once a second it prints
// how far captures have got against the wall clock, how many samples acquisition skipped for
// falling behind it, how many blocks were dropped and captures discarded, and the longest a
// `*IDN?`, sent every 5 ms, took that second. Results are recorded in docs/benchmarks.md (NFR-1:
// zero dropped blocks at 4 channels x 1 MS/s).

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/live_pipeline.hpp"
#include "openscope/sample_clock.hpp"

int main(int argc, char** argv) {
  using std::chrono::steady_clock;
  const std::span arguments(argv, static_cast<std::size_t>(argc));
  const int seconds = arguments.size() > 1 ? std::stoi(arguments[1]) : 10;
  const std::string sample_rate = arguments.size() > 2 ? arguments[2] : "1E6";
  openscope::Instrument instrument;
  const openscope::SessionId session = instrument.open_session();
  // Every channel on, each with noise.
  instrument.execute(session,
                     "ACQ:SRAT " + sample_rate +
                         ";:CHAN2:DISP ON;:CHAN3:DISP ON;:CHAN4:DISP ON;:SOUR1:NOIS 0.01;:SOUR2:"
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
    std::chrono::duration<double, std::milli> slowest{0};
    while (steady_clock::now() < start + std::chrono::seconds(second)) {
      const auto sent = steady_clock::now();
      instrument.execute(session, "*IDN?", [](const std::optional<std::string>& /*reply*/) {});
      slowest = std::max<std::chrono::duration<double, std::milli>>(slowest,
                                                                    steady_clock::now() - sent);
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
}
