#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/scpi/command_tree.hpp"
#include "openscope/session.hpp"

namespace openscope {

/// Identifies one Session opened on an Instrument.
enum class SessionId : std::uint32_t {};

/// The single entry point for instrument behaviour. It owns every Session and the instrument-wide
/// state (generators, channels, acquisition settings, the sample clock and the latest capture),
/// and executes SCPI text on a Session's behalf, so transports and tests all reach the instrument
/// through the same path.
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

  /// Executes one line of SCPI text for `session` and returns its reply, if it has one.
  /// `line` excludes its terminator: the transport strips the trailing `\n` and an optional `\r`
  /// before calling. A line longer than kMaxLineLength only queues -363. Errors are pushed onto
  /// that Session's error queue, not returned. Throws std::out_of_range for a SessionId this
  /// Instrument did not open.
  std::optional<std::string> execute(SessionId session, std::string_view line);

  /// Advances the sample clock by `samples` sample indexes with no wall-clock pacing, as tests do
  /// (ADR 0003): every enabled channel is sampled and each capture that fills becomes the latest
  /// capture.
  void advance(std::size_t samples);

  /// The latest capture, or null before the first capture completes.
  [[nodiscard]] std::shared_ptr<const Capture> latest_capture() const { return captures_.latest(); }

 private:
  scpi::CommandTree commands_;
  std::unordered_map<SessionId, Session> sessions_;
  std::uint32_t next_session_id_ = 0;

  SampleClock clock_;
  AcquisitionSettings acquisition_;
  std::array<ChannelSettings, kChannelCount> channels_;
  std::array<Generator, kChannelCount> generators_;
  CaptureAssembler captures_;
};

}  // namespace openscope
