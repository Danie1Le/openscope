// Runs the instrument live with all four channels on for a number of seconds (the first argument,
// 1 to 86400, 10 by default) at a sample rate (the second, 1 to 1E9 samples per second, the *RST
// 1 MS/s by default) and a record length (the third, 1 to 1048576 points, the *RST 10 000 by
// default). Once a second it prints how far captures have got against the wall clock, how many
// samples acquisition skipped for falling behind it, how many blocks were dropped and captures
// discarded, and the longest a `*IDN?`, sent every 5 ms, took that second. Results are recorded in
// docs/benchmarks.md (NFR-1: zero dropped blocks at 4 channels x 1 MS/s). A malformed or
// out-of-range argument exits 2 with the usage; a setup the instrument rejects exits 1.

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

#include "engine/parse_in_range.hpp"
#include "openscope/capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/live_pipeline.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/scpi_error.hpp"

namespace {

constexpr auto kUsage =
    "usage: live_run [seconds 1-86400 [sample rate 1-1E9 [record length 1-1048576]]]\n";

/// What the command line asks for; the defaults are the *RST settings, for 10 s.
struct Options {
  int seconds = 10;
  double sample_rate = openscope::AcquisitionSettings{}.sample_rate;
  std::size_t record_length = openscope::AcquisitionSettings{}.record_length;
};

/// The options in `arguments`, or nullopt when there are too many or one is not valid. Only
/// numbers parsed with parse_in_range reach the SCPI line, never the argument's text.
std::optional<Options> options_from(std::span<char*> arguments) {
  Options options;
  // Reads argument `index`, when given, into `field` if it is a number from `min` to `max`;
  // false when it is given and is not.
  const auto read = [&](std::size_t index, auto& field, auto min, auto max) {
    if (index >= arguments.size()) {
      return true;
    }
    const auto value = openscope::parse_in_range(arguments[index], min, max);
    if (value) {
      field = *value;
    }
    return value.has_value();
  };
  const bool valid = arguments.size() <= 4 && read(1, options.seconds, 1, 86'400) &&
                     read(2, options.sample_rate, 1.0, 1e9) &&
                     read(3, options.record_length, std::size_t{1},
                          openscope::AcquisitionSettings::kMaxRecordLength);
  return valid ? std::optional(options) : std::nullopt;
}

/// Runs `line` and returns the error queue's first entry after it.
std::string first_error(openscope::Instrument& instrument, openscope::SessionId session,
                        const std::string& line) {
  instrument.execute(session, line, [](const std::optional<std::string>& /*reply*/) {});
  std::string error;
  instrument.execute(session, "SYST:ERR?",
                     [&](const std::optional<std::string>& reply) { error = reply.value_or(""); });
  return error;
}

int run(std::span<char*> arguments) {
  using std::chrono::steady_clock;
  using Milliseconds = std::chrono::duration<double, std::milli>;
  const std::optional<Options> parsed = options_from(arguments);
  if (!parsed) {
    std::cerr << kUsage;
    return 2;
  }
  const Options& options = *parsed;
  const int seconds = options.seconds;
  openscope::Instrument instrument;
  const openscope::SessionId session = instrument.open_session(openscope::run_at_once);
  // Every channel on, each with noise. An error ends the line, so the queue's first entry says
  // whether all of it ran.
  const std::string error = first_error(
      instrument, session,
      std::format("ACQ:SRAT {};POIN {};:CHAN2:DISP ON;:CHAN3:DISP ON;:CHAN4:DISP ON;:SOUR1:NOIS "
                  "0.01;:SOUR2:NOIS 0.01;:SOUR3:NOIS 0.01;:SOUR4:NOIS 0.01",
                  options.sample_rate, options.record_length));
  std::cout << "setup errors: " << error << '\n';
  if (error != openscope::error_reply(openscope::kNoError)) {
    std::cerr << "live_run: the instrument rejected the setup\n";
    return 1;
  }

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
