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
#include <vector>

#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/measurement.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/session.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

/// Identifies one Session opened on an Instrument. Ids count up and are never reused: 64 bits do
/// not wrap, so a closed Session's id never reaches a later client.
enum class SessionId : std::uint64_t {};

/// Receives the reply to one line when the line ends: the replies of its queries joined with `;`,
/// or nullopt when no query replied. The line may end after `execute` returns, inside `advance` or
/// `process` (which may run on another thread) or inside another Session's `execute`. It runs
/// while the Instrument holds its mutex, so a ReplyHandler must not call back into the
/// Instrument; it only hands the reply to the transport.
using ReplyHandler = std::function<void(std::optional<std::string> reply)>;

/// The single entry point for instrument behaviour. It owns every Session and the instrument-wide
/// state (generators, channels, acquisition and trigger settings, the run state, the sample clock,
/// the latest capture and its measurements), and executes SCPI text on a Session's behalf, so
/// transports and tests all reach the instrument through the same path.
///
/// The sample clock runs in one of two ways, never both on one Instrument. In test mode one thread
/// steps it with `advance`. In live mode an acquisition thread calls `acquire` and a processing
/// thread calls `process` on the blocks it acquired (#8). Either way any thread may call
/// `open_session`, `close_session`, `execute`, `sample_rate`, `latest_capture` and
/// `discarded_captures` at the same time.
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

  /// Opens a new Session with an empty error queue.
  SessionId open_session();

  /// Closes `session` when its client disconnects; its SessionId is never valid again. A line it
  /// has held is dropped without a reply. Throws std::out_of_range for a SessionId this Instrument
  /// did not open or already closed.
  void close_session(SessionId session);

  /// Executes one line of SCPI text for `session` and passes its reply to `on_reply`, once, when
  /// the line ends. `line` excludes its terminator: the transport strips the trailing `\n` and an
  /// optional `\r` before calling. A line longer than kMaxLineLength only queues -363. Errors are
  /// pushed onto that Session's error queue, not replied.
  ///
  /// While an operation is pending (a Single is armed), `*OPC?` and `*WAI` hold the line: the rest
  /// of it runs, and `on_reply` is called, only once nothing is pending, from inside the call that
  /// ended the operation (`advance`, or `execute` for any Session), after that call's own reply.
  /// There is no timeout. A transport waits for `on_reply` without blocking, then sends the
  /// Session's next line (the Session's commands run in order). Throws std::logic_error when
  /// `session` still has a line held, and std::out_of_range for a SessionId this Instrument did
  /// not open or already closed.
  void execute(SessionId session, std::string_view line, ReplyHandler on_reply);

  /// Advances the sample clock by `samples` sample indexes with no wall-clock pacing, as tests do
  /// (ADR 0003), in sample blocks of at most kMaxBlockLength. Unless the instrument is Stopped,
  /// every enabled channel is sampled and each capture that fills becomes the latest capture. A
  /// Single's capture stops the instrument and ends the lines held on it at its last sample, and
  /// the samples after it follow whatever those lines change. So one call gives the same captures
  /// and replies as any split of it into smaller calls.
  void advance(std::size_t samples);

  /// Live mode's acquisition half: takes the next `count` sample indexes on the sample clock and
  /// returns them sampled, with a snapshot of the settings `process` needs. While Stopped it
  /// still takes them, but samples nothing and returns nullopt. It holds the mutex that commands
  /// take only to take the indexes and the snapshot, not while it samples, so a command never
  /// waits for a block to be sampled. Throws std::invalid_argument when `count` exceeds
  /// kMaxBlockLength.
  [[nodiscard]] std::optional<AcquiredBlock> acquire(std::size_t count);

  /// Live mode's processing half: assembles `block`, from `acquire`, into captures without the
  /// mutex that commands take; it takes it only to publish a capture or end a Single, which
  /// happens about once a capture. A capture completes into the latest capture only when no
  /// command has discarded the capture in progress since the block was acquired, so it never
  /// holds settings older than a command that has already returned. A Single's capture stops the
  /// instrument and ends the lines held on it, on this thread; when `RUN` has ended the Single
  /// since the block was acquired, the rest of the block, and every later block acquired while
  /// the Single was armed, is captured as Running, with the trigger restarted at the Single's
  /// capture as after any Single. Blocks must come in the order they were acquired; a block that
  /// never arrives is a dropped block, and the capture that would span it is discarded and
  /// counted.
  void process(const AcquiredBlock& block);

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

  /// A line `*OPC?` or `*WAI` holds until no operation is pending.
  struct HeldLine {
    SessionId session;
    scpi::Execution execution;
    ReplyHandler on_reply;
  };

  /// Samples `count` indexes from the sample clock's next one, without taking them, under the
  /// current settings; nullopt while Stopped. Needs mutex_.
  std::optional<AcquiredBlock> sample(std::size_t count);
  /// A block of no samples with the current settings and the next sequence number, to sample
  /// into; nullopt while Stopped. Needs mutex_.
  std::optional<AcquiredBlock> snapshot();
  /// Feeds `block` to the capture assembler. Touches the processing state only, so it needs no
  /// lock.
  Pushed assemble(const AcquiredBlock& block);
  /// Publishes what assembling `block` produced and ends a Single its capture completes, unless a
  /// command has discarded the capture in progress since `block` was acquired. Returns true when
  /// the block's Single capture ended inside it but `RUN` had already ended the Single, so the
  /// samples after `pushed.end` are still to be captured as Running. Needs mutex_.
  bool conclude(const AcquiredBlock& block, const Pushed& pushed);
  /// Discards the capture in progress: the blocks acquired from now on start a new generation,
  /// and a capture never mixes two generations.
  void discard_capture() { ++generation_; }

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

  /// Replies what `reply` writes from the latest capture and channel `source` (1 to 4) in it.
  /// Before the first capture completes it reports -230; when the source channel is not in the
  /// latest capture, -221 (a query never reads a channel that was not captured).
  scpi::HandlerResult reply_with_source(
      std::size_t source,
      const std::function<std::string(const Capture&, const CapturedChannel&)>& reply) const;

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
  /// `*RST`: restores every instrument-wide setting and the Running run state, restarts every
  /// Generator as if it had run at its defaults since sample index 0 with no Faults, and drops the
  /// latest capture. The sample clock keeps counting and Sessions keep their own state (ADR 0002).
  void reset();
  /// Enters `state`. Stopped and Armed-single drop the capture in progress, so the next capture
  /// starts with the samples after the change; Running keeps it.
  void enter(RunState state);
  /// When no operation is pending, carries on every held line, in the order they were held, and
  /// passes the reply of each line that ends to its handler.
  void resume_held();
  /// Whether an operation is pending: a Single is armed (GLOSSARY: Pending operation).
  [[nodiscard]] bool pending() const { return run_state_ == RunState::kArmedSingle; }

  /// Guards the Sessions and every setting, the sample clock and the run state: everything down
  /// to the processing state below.
  mutable std::mutex mutex_;
  scpi::CommandTree commands_;
  std::unordered_map<SessionId, Session> sessions_;
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
  /// At most one per Session: a Session with a held line sends no other.
  std::vector<HeldLine> held_;
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
