#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>

#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/instrument.hpp"
#include "openscope/measuring_queue.hpp"
#include "openscope/spsc_ring_buffer.hpp"

namespace openscope {

/// What live mode has lost so far, as the engine reports it (spec #1 story 41, NFR-1).
struct LiveCounters {
  /// Dropped blocks (GLOSSARY): blocks lost because processing fell behind (dropped_blocks()).
  std::uint64_t dropped_blocks = 0;
  /// Skipped samples (GLOSSARY): released by the wall clock, never acquired (skipped_samples()).
  std::uint64_t skipped_samples = 0;
  /// Instrument::discarded_captures: captures that would have spanned a dropped block.
  std::uint64_t discarded_captures = 0;

  bool operator==(const LiveCounters&) const = default;
};

/// Runs an Instrument in real time (live mode, spec #1). An acquisition thread acquires sample
/// blocks as the Pacer releases them and pushes them through an SPSC ring buffer; a processing
/// thread pops them and assembles captures, so a slow capture never holds up the sample clock.
/// A measuring thread measures and publishes the Running captures processing completes, so
/// measuring a long capture, which takes longer than the ring buffer holds blocks, never holds up
/// processing (#71); a Single's capture is still measured by processing (Instrument::process).
/// When processing falls behind and the buffer is full, the block is dropped and counted; when
/// acquisition falls behind the wall clock, the samples it skips are counted too.
/// When measuring falls behind, processing waits for it, so its captures are never lost unseen:
/// the blocks that then overflow the ring buffer are counted as dropped. The threads start when
/// the pipeline is built and stop when it is destroyed; blocks still in the buffer are dropped
/// then, and so is a capture processing is still waiting to queue, but captures already queued
/// are measured and published first; lines held on a Single stay held until a command ends it.
class LivePipeline {
 public:
  /// Blocks the ring buffer holds before it drops one: at least kQueueCapacity ticks of samples.
  static constexpr std::size_t kQueueCapacity = 64;
  /// The most captures waiting for the measuring thread, counting the one it measures: as many
  /// short captures as the ring buffer holds blocks.
  static constexpr std::size_t kMeasuringCaptures = kQueueCapacity;
  /// The most samples, summed over channels, in captures waiting for the measuring thread,
  /// counting the one it measures: one capture of the longest record on every channel.
  static constexpr std::size_t kMeasuringSamples =
      AcquisitionSettings::kMaxRecordLength * kChannelCount;
  /// How long the acquisition thread sleeps between rounds. Each round acquires what fell due
  /// since the last, so at 1 MS/s a block is about 1000 samples.
  static constexpr std::chrono::milliseconds kTick{1};

  /// Starts running `instrument` live. It must outlive the pipeline, and nothing may `advance` it
  /// meanwhile. The processing thread hands lines held on a Single to their Sessions' Resumers.
  explicit LivePipeline(Instrument& instrument);

  /// Blocks dropped because the ring buffer was full. Any thread may read it.
  [[nodiscard]] std::uint64_t dropped_blocks() const { return blocks_.dropped(); }

  /// Samples the wall clock released that were never acquired, because the acquisition thread
  /// fell behind it (Pacer::skipped). Any thread may read it.
  [[nodiscard]] std::uint64_t skipped_samples() const {
    return skipped_samples_.load(std::memory_order_relaxed);
  }

  /// The three counts of what live mode has lost, read one after another. Any thread may read
  /// them.
  [[nodiscard]] LiveCounters counters() const {
    return {.dropped_blocks = dropped_blocks(),
            .skipped_samples = skipped_samples(),
            .discarded_captures = instrument_->discarded_captures()};
  }

 private:
  void acquire_blocks(const std::stop_token& stop);
  void process_blocks(const std::stop_token& stop);
  void measure_captures(const std::stop_token& stop);

  SpscRingBuffer<AcquiredBlock> blocks_{kQueueCapacity};
  MeasuringQueue captures_{kMeasuringCaptures, kMeasuringSamples};
  Instrument* instrument_;
  std::atomic<std::uint64_t> skipped_samples_ = 0;
  // Declared last so they stop and join first, while what they use still exists; acquisition
  // stops before processing, and processing before measuring.
  std::jthread measuring_;
  std::jthread processing_;
  std::jthread acquisition_;
};

}  // namespace openscope
