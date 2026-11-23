#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {

/// The settings that alone define a capture (ADR 0004). The defaults are the `*RST` values.
struct AcquisitionSettings {
  /// The longest record length `ACQuire:POINts` accepts (2^20 samples).
  static constexpr std::size_t kMaxRecordLength = std::size_t{1} << 20U;

  /// Samples per second on the sample clock (`ACQuire:SRATe`).
  double sample_rate = 1e6;
  /// Sample indexes per capture (`ACQuire:POINts`).
  std::size_t record_length = 10'000;
};

/// One enabled channel's part of a capture, with the vertical settings its samples were taken at.
struct CapturedChannel {
  /// The channel's settings when the capture started, so its input-range limits are the ones
  /// that clipped the samples. `enabled` is always true: only enabled channels are captured.
  ChannelSettings settings;
  /// One sample per sample index of the capture, in volts, already clipped.
  std::vector<float> samples;
};

/// The samples of every enabled channel over the same record-length span of sample indexes.
struct Capture {
  /// The sample index of the first sample.
  SampleIndex first_index = 0;
  /// Samples per second the capture was taken at.
  double sample_rate = 0.0;
  /// Sample indexes in the capture: the length of every channel's samples.
  std::size_t record_length = 0;
  /// The time-zero sample (GLOSSARY): the sample of the capture where time zero sits, the trigger
  /// event or, in an untriggered capture, the one the trigger position names. Either way it is the
  /// trigger's pre-trigger sample count.
  std::size_t time_zero_sample = 0;
  /// True when no trigger event placed time zero (an untriggered capture).
  bool untriggered = true;
  /// Each channel's samples, indexed by channel number - 1; empty for a channel that was not
  /// enabled.
  std::array<std::optional<CapturedChannel>, kChannelCount> channels;
};

/// Channel `channel` (1 to 4) of `capture`. Throws std::out_of_range when it is not in the capture.
const CapturedChannel& captured_channel(const Capture& capture, std::size_t channel);

/// How many of the captures a sample block could complete `CaptureAssembler::push` completes, as
/// the run state the block was acquired in says (AcquiredBlock::limit).
enum class CaptureLimit : std::uint8_t {
  /// Every one (the Running run state).
  kEvery,
  /// The first, after which push stops (the Armed-single run state).
  kFirst,
};

}  // namespace openscope
