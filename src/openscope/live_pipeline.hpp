#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>

#include "openscope/acquired_block.hpp"
#include "openscope/instrument.hpp"
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
/// When processing falls behind and the buffer is full, the block is dropped and counted; when
/// acquisition falls behind the wall clock, the samples it skips are counted too. The
/// threads start when the pipeline is built and stop when it is destroyed; blocks still in the
/// buffer are dropped then, and lines held on a Single stay held until a command ends it.
class LivePipeline {
 public:
  /// Blocks the ring buffer holds before it drops one: at least kQueueCapacity ticks of samples.
  static constexpr std::size_t kQueueCapacity = 64;
  /// How long the acquisition thread sleeps between rounds. Each round acquires what fell due
  /// since the last, so at 1 MS/s a block is about 1000 samples.
  static constexpr std::chrono::milliseconds kTick{1};

  /// Starts running `instrument` live. It must outlive the pipeline, and nothing may `advance` it
  /// meanwhile. Reply handlers of held lines run on the processing thread.
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

  SpscRingBuffer<AcquiredBlock> blocks_{kQueueCapacity};
  Instrument* instrument_;
  std::atomic<std::uint64_t> skipped_samples_ = 0;
  // Declared last so they stop and join first, while what they use still exists; acquisition
  // stops before processing.
  std::jthread processing_;
  std::jthread acquisition_;
};

}  // namespace openscope
