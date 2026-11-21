// Runs the instrument live at 4 channels x 1 MS/s for a number of seconds (the first argument,
// 10 by default) and prints, once a second, how far captures have got against the wall clock and
// how many blocks were dropped and captures discarded. Results are recorded in
// docs/benchmarks.md (NFR-1: zero dropped blocks at 4 channels x 1 MS/s).

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
  const std::span arguments(argv, static_cast<std::size_t>(argc));
  const int seconds = arguments.size() > 1 ? std::stoi(arguments[1]) : 10;
  openscope::Instrument instrument;
  const openscope::SessionId session = instrument.open_session();
  // Every channel on, each with noise, at the *RST 1 MS/s.
  instrument.execute(session,
                     "CHAN2:DISP ON;:CHAN3:DISP ON;:CHAN4:DISP ON;:SOUR1:NOIS 0.01;:SOUR2:NOIS "
                     "0.01;:SOUR3:NOIS 0.01;:SOUR4:NOIS 0.01;:SYST:ERR?",
                     [](const std::optional<std::string>& reply) {
                       std::cout << "setup errors: " << reply.value_or("") << '\n';
                     });

  const auto start = std::chrono::steady_clock::now();
  const openscope::LivePipeline live(instrument);
  std::cout << std::format("{:>8} {:>14} {:>14} {:>8} {:>10}\n", "seconds", "wall samples",
                           "captured to", "dropped", "discarded");
  for (int second = 1; second <= seconds; ++second) {
    std::this_thread::sleep_until(start + std::chrono::seconds(second));
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    const std::shared_ptr<const openscope::Capture> capture = instrument.latest_capture();
    const openscope::SampleIndex captured_to =
        capture == nullptr ? 0 : capture->first_index + capture->record_length;
    std::cout << std::format("{:>8.3f} {:>14.0f} {:>14} {:>8} {:>10}\n", elapsed.count(),
                             elapsed.count() * instrument.sample_rate(), captured_to,
                             live.dropped_blocks(), instrument.discarded_captures())
              << std::flush;
  }
}
