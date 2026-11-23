#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/measurement.hpp"
#include "openscope/measurement_statistics.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/session.hpp"
#include "openscope/spectrum_cache.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

/// Identifies one Session opened on an Instrument. Ids count up and are never reused: 64 bits do
/// not wrap, so a closed Session's id never reaches a later client.
enum class SessionId : std::uint64_t {};

/// Receives the reply to one line when the line ends: the replies of its queries joined with `;`,
/// or nullopt when no query replied. It runs on the thread that ran the end of the line, without
/// the Instrument's mutex: inside `execute`, or, for a line `*OPC?` held, inside the work its
/// Session's Resumer was given.
using ReplyHandler = std::function<void(std::optional<std::string> reply)>;

/// Runs work for one Session in order with that Session's lines: how a transport carries on a
/// line `*OPC?` or `*WAI` held (#67). When the operation the line waits for ends, the Instrument
/// calls it, without its mutex, with the work that runs the rest of that one line; it calls it on
/// the thread that ended the operation, which in live mode is the processing thread, so a live
/// transport must post the work to where the Session's lines run rather than run it there. A
/// Resumer that runs the work at once suits test mode, which steps the sample clock by hand.
using Resumer = std::function<void(std::function<void()> work)>;

/// The Resumer for test mode, where one thread steps the sample clock with `advance`, and for a
/// Session that never holds a line: runs the work at once, on the calling thread.
inline void run_at_once(const std::function<void()>& work) { work(); }

/// The single entry point for instrument behaviour. It owns every Session and the instrument-wide
/// state (generators, channels, acquisition and trigger settings, the run state, the sample clock,
/// the latest capture and its measurements and spectra, the measurement statistics), and executes
/// SCPI text on a Session's behalf, so transports and tests all reach the instrument through the
/// same path.
///
/// The sample clock runs in one of two ways, never both on one Instrument. In test mode one thread
/// steps it with `advance`. In live mode an acquisition thread calls `acquire` and a processing
/// thread calls `process` on the blocks it acquired (#8). Either way any thread may call
/// `open_session`, `close_session`, `execute`, `sample_rate`, `latest_capture` and
/// `discarded_captures` at the same time, and lines of different Sessions run at the same time
/// (#67): each command that reads or changes instrument-wide state holds the mutex only while it
/// runs, and the commands registered with `add_lock_free_command` (bulk data queries among them)
/// hold none, so one client's bulk data never stalls another's commands. A LivePipeline must stop
/// before whatever its Sessions' Resumers post to (a ScpiServer's thread pool) goes away.
class Instrument {
 public:
  /// The longest line, in bytes without its terminator, that `execute` runs (4 KB).
  static constexpr std::size_t kMaxLineLength = 4096;

  /// The most sample indexes `advance` puts in one sample block, which bounds its memory.
  static constexpr std::size_t kMaxBlockLength = 65'536;

  /// Builds the instrument with its SCPI command tree and `*RST` settings. `seed` fixes every
  /// Generator's noise: instruments built with the same seed and driven the same way produce
  /// bit-identical samples (ADR 0003).
  explicit Instrument(std::uint64_t seed = 0);

  // The command handlers keep a pointer to the Instrument, so it never moves or copies.
  Instrument(const Instrument&) = delete;
  Instrument& operator=(const Instrument&) = delete;
  Instrument(Instrument&&) = delete;
  Instrument& operator=(Instrument&&) = delete;
  ~Instrument() = default;

  /// Opens a new Session with an empty error queue, whose held lines `resumer` carries on.
  SessionId open_session(Resumer resumer);

  /// Closes `session` when its client disconnects; its SessionId is never valid again. A line it
  /// has held is dropped without a reply. Throws std::out_of_range for a SessionId this Instrument
  /// did not open or already closed.
  void close_session(SessionId session);

  /// Executes one line of SCPI text for `session` and passes its reply to `on_reply`, once, when
  /// the line ends. `line` excludes its terminator: the transport strips the trailing `\n` and an
  /// optional `\r` before calling. A line longer than kMaxLineLength only queues -363. Errors are
  /// pushed onto that Session's error queue, not replied. A command of another Session may run
  /// between two commands of the line.
  ///
  /// While an operation is pending (a Single is armed), `*OPC?` and `*WAI` hold the line, and
  /// `execute` returns. Once nothing is pending, the Instrument hands the rest of the line to the
  /// Session's Resumer, as its own piece of work, as soon as the operation has ended: at the
  /// capture in `advance` or `process`, or right after the `STOP`, `RUN` or `*RST` command of any
  /// Session, before the rest of that command's line; the rest runs, and `on_reply` is called,
  /// when that work runs. Until every line that operation released has ended or waits
  /// again, a command of any other Session that reads or changes instrument-wide state waits too,
  /// and its line carries on through its Resumer once they have, so the released lines see the
  /// instrument as the operation left it. There is no timeout. A transport waits for `on_reply`
  /// without blocking, then sends the Session's next line (the Session's commands run in order).
  /// Throws std::logic_error when `session` still has a line held, and std::out_of_range for a
  /// SessionId this Instrument did not open or already closed.
  void execute(SessionId session, std::string_view line, ReplyHandler on_reply);

  /// Advances the sample clock by `samples` sample indexes with no wall-clock pacing, as tests do
  /// (ADR 0003), in sample blocks of at most kMaxBlockLength. Unless the instrument is Stopped,
  /// every enabled channel is sampled and each capture that fills becomes the latest capture. A
  /// Single's capture stops the instrument and hands the lines held on it to their Resumers at
  /// its last sample. With Resumers that run the work at once, as in test mode, those lines end
  /// there and the samples after it follow whatever they change, so one call gives the same
  /// captures and replies as any split of it into smaller calls.
  void advance(std::size_t samples);

  /// Live mode's acquisition half: takes the next `count` sample indexes on the sample clock and
  /// returns them sampled, with a snapshot of the settings `process` needs. While Stopped it
  /// still takes them, but samples nothing and returns nullopt. It holds the mutex that commands
  /// take only to take the indexes and the snapshot, not while it samples, so a command never
  /// waits for a block to be sampled. Throws std::invalid_argument when `count` exceeds
  /// kMaxBlockLength.
  [[nodiscard]] std::optional<AcquiredBlock> acquire(std::size_t count);

  /// A completed capture with the generation of the block it completed in: what publishing it
  /// checks, and what `publish_measured` needs to measure and publish it later, on another thread.
  struct CompletedCapture {
    std::shared_ptr<const Capture> capture;
    std::uint64_t generation = 0;
  };

  /// Takes each Running capture `process` completes, in order, for `publish_measured`.
  using CaptureHandOff = std::function<void(CompletedCapture completed)>;

  /// Live mode's processing half: assembles `block`, from `acquire`, into captures without the
  /// mutex that commands take. Each Running capture goes to `hand_off` as it completes or, when
  /// there is none, through `publish_measured` here. A Single's capture is measured here, also
  /// without the mutex, and published with the Single's end in one hold of it, so a long one holds
  /// up processing while it is measured (the blocks acquired meanwhile follow the Single's capture
  /// and are of no use unless `RUN` came first). A capture is counted, and becomes the latest
  /// capture in the same hold of the mutex, only when no command has discarded the capture in
  /// progress since the block was acquired, so neither ever holds settings older than a command
  /// that has already returned. A Single's capture stops the
  /// instrument and hands each line held on it to its Session's Resumer; this thread runs none of
  /// them, so the ring buffer keeps draining. When `RUN` has ended the Single since the block was
  /// acquired, the rest of the block, and every later block acquired while the Single was armed,
  /// is captured as Running, with the trigger restarted at the Single's capture as after any
  /// Single. Blocks must come in the order they were acquired; a block that never arrives is a
  /// dropped block, and the capture that would span it is discarded and counted.
  void process(const AcquiredBlock& block, const CaptureHandOff& hand_off = {});

  /// Measures `completed`, from `process`'s hand-off, for the measurement statistics without the
  /// mutex, then counts and publishes it as `process` would have (#71: measuring a capture of
  /// four 1M-sample channels takes longer than live mode's ring buffer holds blocks). A capture a
  /// command has discarded since is neither measured nor published. Captures must come from one
  /// thread at a time, in the order they were handed off.
  void publish_measured(const CompletedCapture& completed);

  /// Samples per second on the sample clock (`ACQuire:SRATe`), which live mode paces against.
  [[nodiscard]] double sample_rate() const;

  /// The latest capture, or null before the first capture completes. It never waits on the mutex
  /// that commands and capture processing take, so any thread may read it as often as it likes.
  [[nodiscard]] std::shared_ptr<const Capture> latest_capture() const { return latest_.load(); }

  /// How many captures have been discarded because they would have spanned a dropped block.
  [[nodiscard]] std::uint64_t discarded_captures() const {
    return discarded_captures_.load(std::memory_order_relaxed);
  }

 private:
  /// The `*RST` timebase scale in seconds per division.
  static constexpr double kDefaultTimebaseScale = 1e-3;

  /// What the instrument does with captures. Run state and trigger mode are separate (spec #1).
  enum class RunState : std::uint8_t {
    /// Captures are taken continuously (`RUN`, and the `*RST` state).
    kRunning,
    /// No capture is taken; the latest capture stays as it is (`STOP`).
    kStopped,
    /// One capture is taken, then the instrument is Stopped (`SINGle`). The Single is the pending
    /// operation `*OPC?` and `*WAI` wait for.
    kArmedSingle,
  };

  /// One open Session and how its held lines are carried on.
  struct SessionState {
    /// Touched only by the Session's own lines, which run one at a time, so not by mutex_.
    Session session;
    Resumer resumer;
    /// While the Session has a line held, or handed to `resumer` and not yet ended. Needs mutex_.
    bool holding = false;
    /// Until close_session. Needs mutex_.
    bool open = true;
    /// From an operation's end, which released its held line, until that line has ended or waits
    /// again (settle). Needs mutex_.
    bool released = false;
  };

  /// A line from `execute` until it ends, including while `*OPC?` or `*WAI` holds it until no
  /// operation is pending.
  struct RunningLine {
    std::shared_ptr<SessionState> state;
    scpi::Execution execution;
    ReplyHandler on_reply;
  };

  /// Samples `count` indexes from the sample clock's next one, without taking them, under the
  /// current settings; nullopt while Stopped. Needs mutex_.
  std::optional<AcquiredBlock> sample(std::size_t count);
  /// A block of no samples with the current settings and the next sequence number, to sample
  /// into; nullopt while Stopped. Needs mutex_.
  std::optional<AcquiredBlock> snapshot();
  /// Feeds `block` to the capture assembler, which hands each capture to `on_completed` as it
  /// completes. Returns, when the block's Single capture completed inside it, the offset just past
  /// that capture. Touches the processing state only, so it needs no lock.
  std::optional<std::size_t> assemble(const AcquiredBlock& block, const CaptureSink& on_completed);
  /// How many captures assembling `block` makes: its own limit, unless `RUN` has already ended
  /// the Single it was acquired for. Processing state only, so it needs no lock.
  [[nodiscard]] CaptureLimit capture_limit(const AcquiredBlock& block) const;
  /// True until a command discards the capture in progress of `generation` (discard_capture), so
  /// a capture completed in a block of it may still be published; once false, it stays false. A
  /// capture that can never be published is not measured: measuring one long capture holds up the
  /// thread that does it (#71). Takes mutex_.
  [[nodiscard]] bool current(std::uint64_t generation) const;
  /// Counts `completed` in the measurement statistics with its `results` and makes it the latest
  /// capture, unless a command has discarded the capture in progress since the block it completed
  /// in was acquired. Needs mutex_.
  void publish(const CompletedCapture& completed, const CaptureResults& results);
  /// Ends the Single whose capture completed inside `block`, unless a command has discarded the
  /// capture in progress since `block` was acquired; the caller then hands the lines held on it
  /// on (take_woken). Returns true when `RUN` had already ended the Single, so the samples after
  /// that capture are still to be captured as Running. Needs mutex_.
  bool conclude(const AcquiredBlock& block);
  /// Discards the capture in progress: the blocks acquired from now on start a new generation,
  /// and a capture never mixes two generations.
  void discard_capture() { ++generation_; }

  /// Registers `command` with its handler run under mutex_: every command that reads or changes
  /// instrument-wide state. While released lines settle it waits instead (Hold::again); when it
  /// ends the operation, it releases the held lines in the same hold of the mutex.
  void add_command(scpi::Command command);
  /// Registers `command` to run without mutex_: only a command that touches nothing but its own
  /// Session, the latest capture (an atomic load) and the caches, so the costly part of a query
  /// (a block, ASCII text, a spectrum, a measurement) never stalls other Sessions.
  void add_lock_free_command(scpi::Command command) { commands_.add(std::move(command)); }

  /// `RUN`, `STOP`, `SINGle`, and the `*OPC?` and `*WAI` that wait on a Single.
  void add_run_control_commands();
  void add_generator_commands();
  void add_channel_commands();
  void add_acquisition_commands();
  void add_timebase_commands();
  void add_trigger_commands();
  void add_waveform_commands();
  void add_fft_commands();
  void add_measurement_commands();
  /// `MEASure:STATistics?` and `MEASure:STATistics:RESet`.
  void add_statistics_commands();

  /// Replies what `reply` writes from the latest capture and channel `source` (1 to 4) in it.
  /// Before the first capture completes it reports -230; when the source channel is not in the
  /// latest capture, -221 (a query never reads a channel that was not captured). `reply` gets the
  /// shared capture itself, which the measurement and spectrum caches key on.
  scpi::HandlerResult reply_with_source(
      std::size_t source, const std::function<std::string(const std::shared_ptr<const Capture>&,
                                                          const CapturedChannel&)>& reply) const;

  /// Registers `<header> <parameter>`, which runs `set`, and `<header>?`, which replies what `get`
  /// returns. Both receive the calling Session, for the settings a Session owns (ADR 0002).
  void add_setting(std::string_view header, scpi::ParameterSpec parameter,
                   std::function<void(Session&, const scpi::Arguments&)> set,
                   std::function<std::string(Session&, const scpi::Arguments&)> get);
  /// add_setting for a choice of `keywords`, which must outlive the Instrument: `set` receives the
  /// index of the keyword sent, and the query replies the short form of the keyword at the index
  /// `get` returns.
  void add_keyword_setting(std::string_view header, std::span<const std::string_view> keywords,
                           std::function<void(Session&, const scpi::Arguments&, std::size_t)> set,
                           std::function<std::size_t(Session&, const scpi::Arguments&)> get);
  /// add_setting for the `double` `field` of a settings struct: the setter runs `change(arguments,
  /// fn)`, which applies `fn` to the settings the command names, and the query replies the field of
  /// `lookup(arguments)`.
  template <typename Settings, typename Change, typename Lookup>
  void add_number_setting(std::string_view header, const scpi::NumericParameter& range,
                          double Settings::*field, Change change, Lookup lookup);
  /// add_setting for one numeric field of the Generator a `SOURce<n>` header names.
  void add_generator_number(std::string_view header, const scpi::NumericParameter& range,
                            double GeneratorSettings::*field);
  /// add_setting for one numeric field of the channel a `CHANnel<n>` header names.
  void add_channel_number(std::string_view header, const scpi::NumericParameter& range,
                          double ChannelSettings::*field);
  /// add_setting for a choice of channel, `CHANnel<1-4>`: `set` receives the channel number sent
  /// (1 when the suffix is left out), and the query replies `CHAN<n>` for the number `get` returns.
  void add_channel_choice(std::string_view header, std::function<void(Session&, std::size_t)> set,
                          std::function<std::size_t(Session&)> get);
  /// add_setting for one numeric field of the trigger settings.
  void add_trigger_number(std::string_view header, const scpi::NumericParameter& range,
                          double TriggerSettings::*field);
  /// add_keyword_setting for one enum field of the trigger settings, whose enumerators are in the
  /// order of `keywords`.
  template <typename Keyword>
  void add_trigger_keyword(std::string_view header, std::span<const std::string_view> keywords,
                           Keyword TriggerSettings::*field);

  /// Applies `change` to the settings of the Generator a `SOURce<n>` command names, from the next
  /// sample index on, and discards the capture in progress, so no capture mixes two signals.
  void change_generator(const scpi::Arguments& arguments,
                        const std::function<void(GeneratorSettings&)>& change);
  /// Runs `SOURce<n>:FAULt`: adds a Fault to the Generator the command names, starting at the next
  /// sample index. Reports -222 for a magnitude beyond what its kind allows and -221 when the
  /// Generator already holds Generator::kMaxFaults.
  scpi::HandlerResult inject_fault(const scpi::Arguments& arguments);

  /// Applies `change` to the settings of the channel a `CHANnel<n>` command names and discards the
  /// capture in progress, since a capture keeps the channel settings of the block that started it.
  void change_channel(const scpi::Arguments& arguments,
                      const std::function<void(ChannelSettings&)>& change);
  /// Applies `change` to the acquisition settings and discards the capture in progress.
  /// Generators keep their phase across a sample-rate change.
  void change_acquisition(const std::function<void(AcquisitionSettings&)>& change);
  /// Applies `change` to the trigger settings and discards the capture in progress, which restarts
  /// the trigger.
  void change_trigger(const std::function<void(TriggerSettings&)>& change);
  /// What every change of a setting that shapes captures does: discards the capture in progress,
  /// which no longer matches the settings, and resets the statistics, so they never mix data from
  /// two settings (spec #1, story 64). The timebase shapes no capture (ADR 0004), so it does
  /// neither.
  void capture_setting_changed();
  /// `*RST`: restores every instrument-wide setting and the Running run state, restarts every
  /// Generator as if it had run at its defaults since sample index 0 with no Faults, and drops the
  /// latest capture and the statistics. The sample clock keeps counting and Sessions keep their
  /// own state (ADR 0002).
  void reset();
  /// Enters `state`. Stopped and Armed-single drop the capture in progress, so the next capture
  /// starts with the samples after the change; Running keeps it.
  void enter(RunState state);
  /// Deals with `line` once it has run as far as it can: keeps it while an operation is pending
  /// (held by `*OPC?`) or while released lines settle (a command waiting for them), carries it on
  /// at once when that ended since the command looked, or, once it has ended, passes its reply to
  /// its handler and, when it was the last released line to settle, hands on the lines that
  /// waited for that. Takes mutex_ itself.
  void finish(RunningLine line);
  /// For finish: keeps the held `line` (moving it into held_ or waiting_) while what it waits for
  /// lasts, and returns the released lines that keeping it settled; nullopt when it is already
  /// over, so the line carries on now. Takes mutex_ itself.
  std::optional<std::vector<RunningLine>> keep(RunningLine& line);
  /// Carries on `line`, handed to its Session's Resumer, unless the Session has closed since.
  void resume(RunningLine line);
  /// When no operation is pending, takes every held line, in the order they were held, for
  /// `hand_on`, and marks them released: until each has settled, other Sessions' commands that
  /// need mutex_ wait, so the released lines carry on as the operation left the instrument (the
  /// Single's capture and statistics, Stopped). Needs mutex_.
  std::vector<RunningLine> take_woken();
  /// Counts the released line of `state`, if it has one, as settled; once every released line
  /// has, returns the lines whose commands waited for them, for `hand_on`. Needs mutex_.
  std::vector<RunningLine> settle(SessionState& state);
  /// Whether `caller`'s line is a released one. Needs mutex_.
  [[nodiscard]] bool released(const Session& caller) const;
  /// Hands each of `lines` to its Session's Resumer, one line per piece of work. Called without
  /// mutex_, since a Resumer may run the work at once.
  void hand_on(std::vector<RunningLine> lines);
  /// Whether an operation is pending: a Single is armed (GLOSSARY: Pending operation).
  [[nodiscard]] bool pending() const { return run_state_ == RunState::kArmedSingle; }

  /// Guards the Sessions and every setting, the sample clock and the run state: everything down
  /// to the processing state below, except each Session's own state and the caches, which guard
  /// themselves. Held only while one command, or capture processing, reads or changes them.
  mutable std::mutex mutex_;
  scpi::CommandTree commands_;
  std::unordered_map<SessionId, std::shared_ptr<SessionState>> sessions_;
  std::uint64_t next_session_id_ = 0;

  /// The seed every Generator's seed derives from, kept for `*RST`.
  std::uint64_t seed_;
  SampleClock clock_;
  AcquisitionSettings acquisition_;
  std::array<ChannelSettings, kChannelCount> channels_;
  std::array<Generator, kChannelCount> generators_;
  /// Seconds per division of the display viewport. It never changes what is captured (ADR 0004).
  double timebase_scale_ = kDefaultTimebaseScale;
  TriggerSettings trigger_;
  RunState run_state_ = RunState::kRunning;
  MeasurementEngine measurements_;
  SpectrumCache spectra_;
  /// Every capture completed since the last capture_setting_changed(), `*RST` or `MEAS:STAT:RES`.
  MeasurementStatistics statistics_;
  /// The lines held while an operation is pending, at most one per Session: a Session with a
  /// held line sends no other.
  std::vector<RunningLine> held_;
  /// Released lines that have not settled yet (take_woken).
  std::size_t settling_ = 0;
  /// Lines of other Sessions whose command waits for the released lines to settle.
  std::vector<RunningLine> waiting_;
  /// The generation of the blocks acquired from now on (AcquiredBlock::generation).
  std::uint64_t generation_ = 0;
  /// The sequence number of the next block acquired.
  std::uint64_t next_sequence_ = 0;

  // The processing state: only the thread that processes blocks (`advance`'s caller in test mode,
  // `process`'s in live mode) touches it.
  CaptureAssembler captures_;
  /// The generation of the capture in progress, and the sequence number of the block expected
  /// next.
  std::uint64_t assembled_generation_ = 0;
  std::uint64_t expected_sequence_ = 0;
  /// The generation whose Single `RUN` ended before all its Armed-single blocks were processed;
  /// the rest of them are captured as Running. Generations are never reused.
  std::optional<std::uint64_t> run_generation_;

  // Read without the Instrument's mutex, so readers never wait on commands or capture processing.
  std::atomic<std::shared_ptr<const Capture>> latest_;
  std::atomic<std::uint64_t> discarded_captures_ = 0;
};

}  // namespace openscope
