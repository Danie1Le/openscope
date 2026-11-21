#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/session.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

/// Identifies one Session opened on an Instrument. Ids count up and are never reused: 64 bits do
/// not wrap, so a closed Session's id never reaches a later client.
enum class SessionId : std::uint64_t {};

/// Receives the reply to one line when the line ends: the replies of its queries joined with `;`,
/// or nullopt when no query replied. The line may end after `execute` returns, inside `advance`
/// (which may run on another thread) or inside another Session's `execute`. So a ReplyHandler
/// must not call back into the Instrument; it only hands the reply to the transport.
using ReplyHandler = std::function<void(std::optional<std::string> reply)>;

/// The single entry point for instrument behaviour. It owns every Session and the instrument-wide
/// state (generators, channels, acquisition and trigger settings, the sample clock and the latest
/// capture), and executes SCPI text on a Session's behalf, so transports and tests all reach the
/// instrument through the same path.
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

  /// Closes `session` when its client disconnects; its SessionId is never valid again. Throws
  /// std::out_of_range for a SessionId this Instrument did not open or already closed.
  void close_session(SessionId session);

  /// Executes one line of SCPI text for `session` and passes its reply to `on_reply`, once, when
  /// the line ends. `line` excludes its terminator: the transport strips the trailing `\n` and an
  /// optional `\r` before calling. A line longer than kMaxLineLength only queues -363. Errors are
  /// pushed onto that Session's error queue, not replied. Throws std::out_of_range for a SessionId
  /// this Instrument did not open or already closed.
  void execute(SessionId session, std::string_view line, ReplyHandler on_reply);

  /// Advances the sample clock by `samples` sample indexes with no wall-clock pacing, as tests do
  /// (ADR 0003): every enabled channel is sampled and each capture that fills becomes the latest
  /// capture.
  void advance(std::size_t samples);

  /// The latest capture, or null before the first capture completes.
  [[nodiscard]] std::shared_ptr<const Capture> latest_capture() const { return captures_.latest(); }

 private:
  /// The `*RST` timebase scale in seconds per division.
  static constexpr double kDefaultTimebaseScale = 1e-3;

  void add_generator_commands();
  void add_channel_commands();
  void add_acquisition_commands();
  void add_timebase_commands();
  void add_trigger_commands();
  void add_waveform_commands();

  /// Replies what `reply` writes from the latest capture and the caller's `WAV:SOUR` channel in
  /// it. Before the first capture completes it reports -230; when the source channel is not in
  /// the latest capture, -221 (a query never reads a channel that was not captured).
  scpi::HandlerResult reply_with_source(
      Session& caller,
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
  /// sample index on.
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
  /// `*RST`: restores every instrument-wide setting, restarts every Generator as if it had run at
  /// its defaults since sample index 0 with no Faults, and drops the latest capture. The sample
  /// clock keeps counting and Sessions keep their own state (ADR 0002).
  void reset();

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
  CaptureAssembler captures_;
};

}  // namespace openscope
