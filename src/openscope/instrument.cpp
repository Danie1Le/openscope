#include "openscope/instrument.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/capture_assembler.hpp"
#include "openscope/channel.hpp"
#include "openscope/completed_capture.hpp"
#include "openscope/generator.hpp"
#include "openscope/measurement.hpp"
#include "openscope/measurement_statistics.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/scpi_error.hpp"
#include "openscope/session.hpp"

namespace openscope {
namespace {

using scpi::Arguments;

/// The `*RST` channel settings: CH1 enabled, the others disabled.
std::array<ChannelSettings, kChannelCount> default_channels() {
  std::array<ChannelSettings, kChannelCount> channels{};
  channels.front().enabled = true;
  return channels;
}

/// One fresh Generator per channel, each with its own seed derived from the instrument's, so the
/// four channels never share noise.
std::array<Generator, kChannelCount> make_generators(std::uint64_t seed, double sample_rate) {
  return [&]<std::size_t... N>(std::index_sequence<N...> /*channels*/) {
    return std::array<Generator, kChannelCount>{
        Generator{(seed * kChannelCount) + N, sample_rate}...};
  }(std::make_index_sequence<kChannelCount>{});
}

}  // namespace

Instrument::Instrument(std::uint64_t seed)
    : seed_(seed),
      channels_(default_channels()),
      generators_(make_generators(seed, acquisition_.sample_rate)) {
  add_system_commands();
  add_run_control_commands();
  add_generator_commands();
  add_channel_commands();
  add_acquisition_commands();
  add_timebase_commands();
  add_trigger_commands();
  add_waveform_commands();
  add_fft_commands();
  add_measurement_commands();
}

void Instrument::add_command(scpi::Command command) {
  command.handler = [this, handler = std::move(command.handler)](
                        Session& caller, const Arguments& arguments) -> scpi::HandlerResult {
    std::vector<RunningLine> woken;
    scpi::HandlerResult result;
    {
      const std::scoped_lock lock(mutex_);
      // The lines an operation's end released carry on as it left the instrument: until they
      // have, this command waits, holding its line, and runs once they have (settle).
      if (settling_ > 0 && !released(caller)) {
        return scpi::Hold{.reply = std::nullopt, .again = true};
      }
      result = handler(caller, arguments);
      // A command that ended the operation (STOP, RUN, *RST) releases the lines held on it here,
      // in the hold of the mutex that ended it, not when its own line ends: no other Session's
      // command (a SINGle that arms a new Single) can come in between.
      woken = take_woken();
    }
    hand_on(std::move(woken));
    return result;
  };
  commands_.add(std::move(command));
}

Instrument::SessionState* Instrument::state_of(const Session& caller) const {
  const auto found = std::ranges::find_if(
      sessions_, [&](const auto& open) { return &open.second->session == &caller; });
  return found == sessions_.end() ? nullptr : found->second.get();
}

void Instrument::reset() {
  acquisition_ = {};
  channels_ = default_channels();
  timebase_scale_ = kDefaultTimebaseScale;
  trigger_ = {};
  // Fresh Generators rather than reconfigured ones: phase continuity would carry the phase built
  // up by earlier settings past *RST, so the output would depend on the commands sent before it.
  generators_ = make_generators(seed_, acquisition_.sample_rate);
  capture_setting_changed();
  latest_.store(nullptr);
  run_state_ = RunState::kRunning;
}

void Instrument::enter(RunState state) {
  run_state_ = state;
  if (state != RunState::kRunning) {
    discard_capture();
  }
}

std::optional<std::vector<Instrument::RunningLine>> Instrument::keep(RunningLine& line) {
  const std::scoped_lock lock(mutex_);
  // Checked again here, under the lock that operations end and lines settle under, so no end is
  // missed. The holding command has no effect of its own, so a line held here waits as if it had
  // run now: `*OPC?` after a `STOP;SINGle` of another Session in between waits for that Single.
  // A command held to run again waits for the released lines to settle (any command of another
  // Session) or for the published captures to be counted (MEAS:STAT?).
  const bool runs_again = line.execution.held_to_run_again();
  const bool waits_to_settle = runs_again && settling_ > 0 && !line.state->released;
  const bool counting = runs_again && !waits_to_settle && waits_for_count(*line.state);
  if (!(runs_again ? waits_to_settle || counting : pending())) {
    return std::nullopt;
  }
  line.state->holding = true;
  if (counting) {
    awaiting_count_.push_back(std::move(line));
    return std::vector<RunningLine>{};
  }
  std::vector<RunningLine> settled = settle(*line.state);  // A released line waiting again has.
  (runs_again ? waiting_ : held_).push_back(std::move(line));
  return settled;
}

void Instrument::finish(RunningLine line) {
  while (line.execution.held()) {
    if (std::optional<std::vector<RunningLine>> settled = keep(line)) {
      hand_on(std::move(*settled));
      return;
    }
    commands_.resume(line.state->session, line.execution);
  }
  std::vector<RunningLine> settled;
  {
    const std::scoped_lock lock(mutex_);
    line.state->holding = false;
    settled = settle(*line.state);
  }
  line.on_reply(line.execution.take_reply());
  hand_on(std::move(settled));
}

void Instrument::resume(RunningLine line) {
  std::vector<RunningLine> settled;
  bool open = false;
  {
    const std::scoped_lock lock(mutex_);
    open = line.state->open;
    if (!open) {
      settled = settle(*line.state);
    }
  }
  if (!open) {
    hand_on(std::move(settled));
    return;
  }
  commands_.resume(line.state->session, line.execution);
  finish(std::move(line));
}

std::vector<Instrument::RunningLine> Instrument::take_woken() {
  // Every line held when the operation ended carries on, even after an earlier one arms another
  // Single: what each waited for is over. One that waits again is held again (finish).
  if (pending()) {
    return {};
  }
  std::vector<RunningLine> woken = std::exchange(held_, {});
  for (const RunningLine& line : woken) {
    line.state->released = true;
  }
  settling_ += woken.size();
  return woken;
}

std::vector<Instrument::RunningLine> Instrument::settle(SessionState& state) {
  if (!state.released) {
    return {};
  }
  state.released = false;
  return --settling_ == 0 ? std::exchange(waiting_, {}) : std::vector<RunningLine>{};
}

void Instrument::hand_on(std::vector<RunningLine> lines) {
  for (RunningLine& line : lines) {
    // A Resumer takes copyable work, and a held line is move-only.
    auto held = std::make_shared<RunningLine>(std::move(line));
    const Resumer& resumer = held->state->resumer;
    resumer([this, held] { resume(std::move(*held)); });
  }
}

SessionId Instrument::open_session(Resumer resumer) {
  const std::scoped_lock lock(mutex_);
  const SessionId id{next_session_id_++};
  sessions_.emplace(id, std::make_shared<SessionState>(
                            SessionState{.session = {}, .resumer = std::move(resumer)}));
  return id;
}

void Instrument::close_session(SessionId session) {
  std::vector<RunningLine> settled;
  {
    const std::scoped_lock lock(mutex_);
    const auto found = sessions_.find(session);
    if (found == sessions_.end()) {
      throw std::out_of_range("unknown Session");
    }
    found->second->open = false;
    const auto of_this_session = [&](const RunningLine& line) {
      return line.state == found->second;
    };
    std::erase_if(held_, of_this_session);
    std::erase_if(waiting_, of_this_session);
    // A released line waiting for the count has not settled yet; dropped, it has.
    if (std::erase_if(awaiting_count_, of_this_session) > 0) {
      settled = settle(*found->second);
    }
    sessions_.erase(found);
  }
  hand_on(std::move(settled));
}

void Instrument::execute(SessionId session, std::string_view line, ReplyHandler on_reply) {
  std::shared_ptr<SessionState> state;
  {
    const std::scoped_lock lock(mutex_);
    state = sessions_.at(session);
    if (state->holding) {
      throw std::logic_error("Session sent a line while its previous line is held");
    }
  }
  if (line.size() > kMaxLineLength) {
    state->session.push_error(kInputBufferOverrun);
    on_reply(std::nullopt);
    return;
  }
  scpi::Execution execution = commands_.execute(state->session, line);
  finish({.state = std::move(state),
          .execution = std::move(execution),
          .on_reply = std::move(on_reply)});
}

void Instrument::advance(std::size_t samples) {
  while (samples > 0) {
    std::vector<RunningLine> woken;
    {
      const std::scoped_lock lock(mutex_);
      std::size_t count = std::min(samples, kMaxBlockLength);
      const std::optional<AcquiredBlock> block = sample(count);
      std::optional<std::size_t> single_end;
      if (block) {
        // Every capture counts, not only the last of the block, so the statistics do not depend
        // on how `advance` is chunked; and before held lines resume, so they see the Single's
        // capture.
        single_end = blocks_.assemble(*block, [&](const std::shared_ptr<const Capture>& capture) {
          (void)publish(*block, capture, measure_all(*capture));
        });
        // The Single's capture ends the operation at its last sample: the lines it resumes run
        // with the clock there, and the next block starts there under whatever they changed, so
        // how `advance` is chunked never changes a capture or a reply (ADR 0003).
        count = single_end.value_or(count);
      }
      clock_.take(count);
      samples -= count;
      if (block && single_end) {
        conclude(*block);
        woken = take_woken();
      }
    }
    hand_on(std::move(woken));
  }
}

std::optional<AcquiredBlock> Instrument::acquire(std::size_t count) {
  if (count > kMaxBlockLength) {
    throw std::invalid_argument("sample block longer than kMaxBlockLength");
  }
  std::unique_lock lock(mutex_);
  const SampleIndex first = clock_.take(count);
  std::optional<AcquiredBlock> block = snapshot();
  if (!block) {
    return std::nullopt;
  }
  const std::array<Generator, kChannelCount> generators = generators_;
  lock.unlock();
  // Sampling is most of the cost, so it runs without the mutex and commands never wait on it. The
  // copies sample what generators_ would: a command changes a Generator from the sample clock's
  // next index on, after this block (ADR 0003).
  block->samples =
      sample_channels(generators, block->channels, block->trigger.source, first, count);
  return block;
}

void Instrument::process(const AcquiredBlock& block, const CaptureHandOff& hand_off) {
  // Without a hand-off, a capture is measured here, before it is published and counted.
  const auto measured_here = [&](const Capture& capture) {
    return hand_off ? std::nullopt : std::optional(measure_all(capture));
  };
  const auto hand_off_published = [&](std::optional<CompletedCapture> published) {
    if (published) {
      hand_off(std::move(*published));
    }
  };
  const CaptureSink publish_running = [&](const std::shared_ptr<const Capture>& capture) {
    const std::optional<CaptureResults> results = measured_here(*capture);
    std::optional<CompletedCapture> published;
    {
      const std::scoped_lock lock(mutex_);
      published = publish(block, capture, results);
    }
    hand_off_published(std::move(published));
  };
  // A Single's capture, the only one its block completes, is kept until the Single ends and
  // published in the same hold of the mutex: no command falls between the two, so a command
  // that has seen the capture finds the instrument Stopped.
  std::shared_ptr<const Capture> single_capture;
  std::optional<CaptureResults> single_results;
  const CaptureSink keep_single = [&](const std::shared_ptr<const Capture>& capture) {
    single_results = measured_here(*capture);
    single_capture = capture;
  };
  const std::optional<std::size_t> single_end = blocks_.assemble(
      block, blocks_.capture_limit(block) == CaptureLimit::kFirst ? keep_single : publish_running);
  if (!single_end) {
    return;
  }
  bool rest_runs = false;
  std::vector<RunningLine> woken;
  std::optional<CompletedCapture> published;
  {
    const std::scoped_lock lock(mutex_);
    published = publish(block, single_capture, single_results);
    rest_runs = conclude(block);
    woken = take_woken();
  }
  // Before the Running captures after it: captures are counted in the order they completed.
  hand_off_published(std::move(published));
  // Handed to the Resumers, which in live mode run them elsewhere: held bulk queries must not
  // stop this thread draining the ring buffer, nor run back to back in one hold (#67).
  hand_on(std::move(woken));
  // RUN came after the block was acquired for a Single. The Single still ends at its capture, and
  // the trigger restarts there as after any Single, but the instrument runs on: the samples after
  // that capture are captured, not lost.
  if (rest_runs) {
    blocks_.run_rest(block, *single_end, publish_running);
  }
}

void Instrument::count_measured(const CompletedCapture& completed) {
  const CaptureResults results = measure_all(*completed.capture);
  std::vector<RunningLine> counted;
  {
    const std::scoped_lock lock(mutex_);
    // A reset since the capture completed leaves it out: the statistics start after the reset.
    if (completed.statistics_epoch == statistics_epoch_) {
      statistics_.add(results);
    }
    ++counted_;
    // Order kept: the lines whose captures are all counted now, in the order they began to wait.
    const auto still_waits = std::ranges::stable_partition(
        awaiting_count_, [&](const RunningLine& line) { return !waits_for_count(*line.state); });
    counted.assign(std::make_move_iterator(awaiting_count_.begin()),
                   std::make_move_iterator(still_waits.begin()));
    awaiting_count_.erase(awaiting_count_.begin(), still_waits.begin());
  }
  hand_on(std::move(counted));
}

double Instrument::sample_rate() const {
  const std::scoped_lock lock(mutex_);
  return acquisition_.sample_rate;
}

std::optional<AcquiredBlock> Instrument::sample(std::size_t count) {
  std::optional<AcquiredBlock> block = snapshot();
  if (block) {
    block->samples =
        sample_channels(generators_, block->channels, block->trigger.source, clock_.now(), count);
  }
  return block;
}

std::optional<AcquiredBlock> Instrument::snapshot() {
  // Stopped samples nothing: Generators are pure functions of the sample index (ADR 0003), and
  // the capture in progress was dropped when the instrument stopped.
  if (run_state_ == RunState::kStopped) {
    return std::nullopt;
  }
  return AcquiredBlock{.samples = {},
                       .acquisition = acquisition_,
                       .channels = channels_,
                       .trigger = trigger_,
                       .limit = pending() ? CaptureLimit::kFirst : CaptureLimit::kEvery,
                       .generation = generation_,
                       .sequence = next_sequence_++};
}

std::optional<CompletedCapture> Instrument::publish(
    const AcquiredBlock& block, const std::shared_ptr<const Capture>& capture,
    const std::optional<CaptureResults>& results) {
  // A command since the block was acquired has discarded what it belongs to: its capture would
  // show settings older than that command.
  if (block.generation != generation_) {
    return std::nullopt;
  }
  latest_.store(capture);
  if (results) {
    statistics_.add(*results);
    return std::nullopt;
  }
  ++handed_off_;
  return CompletedCapture{.capture = capture, .statistics_epoch = statistics_epoch_};
}

bool Instrument::conclude(const AcquiredBlock& block) {
  // A command since the block was acquired has restarted or ended the Single it would end.
  if (block.generation != generation_) {
    return false;
  }
  if (pending()) {
    enter(RunState::kStopped);
    return false;
  }
  // RUN leaves Armed-single without discarding, so a block acquired while the Single was armed
  // may complete a capture after it: that capture is kept, but there is no Single left to end,
  // and the instrument runs on through the rest of the block.
  return true;
}

}  // namespace openscope
