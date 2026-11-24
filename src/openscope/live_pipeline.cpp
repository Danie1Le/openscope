#include "openscope/live_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include "openscope/acquired_block.hpp"
#include "openscope/completed_capture.hpp"
#include "openscope/instrument.hpp"
#include "openscope/pacer.hpp"

namespace openscope {

LivePipeline::LivePipeline(Instrument& instrument)
    : instrument_(&instrument),
      measuring_([this](const std::stop_token& stop) { measure_captures(stop); }),
      processing_([this](const std::stop_token& stop) { process_blocks(stop); }),
      acquisition_([this](const std::stop_token& stop) { acquire_blocks(stop); }) {}

void LivePipeline::acquire_blocks(const std::stop_token& stop) {
  Pacer pacer(Pacer::Clock::now());
  while (!stop.stop_requested()) {
    // A round acquires at most what the ring buffer can hold, so a machine too slow for the sample
    // rate runs behind the wall clock instead of piling up a backlog it would rush through later;
    // the samples it skips are counted.
    std::uint64_t due = pacer.release(Pacer::Clock::now(), instrument_->sample_rate(),
                                      kQueueCapacity * Instrument::kMaxBlockLength);
    skipped_samples_.store(pacer.skipped(), std::memory_order_relaxed);
    while (due > 0 && !stop.stop_requested()) {
      const auto count =
          static_cast<std::size_t>(std::min<std::uint64_t>(due, Instrument::kMaxBlockLength));
      if (std::optional<AcquiredBlock> block = instrument_->acquire(count)) {
        // A full buffer drops the block and counts it; processing discards the capture it splits.
        (void)blocks_.try_push(std::move(*block));
      }
      due -= count;
    }
    std::this_thread::sleep_for(kTick);
  }
}

void LivePipeline::process_blocks(const std::stop_token& stop) {
  // Waiting for the measuring thread to make room stalls this thread, so the ring buffer fills
  // and drops blocks, counted, rather than a capture going uncounted. When the pipeline stops
  // first, the capture is counted here instead, so no MEAS:STAT? waits for it for ever.
  const CaptureHandOff to_measuring = [&](const CompletedCapture& completed) {
    if (!captures_.push(completed, stop)) {
      instrument_->count_measured(completed);
    }
  };
  while (!stop.stop_requested()) {
    if (const std::optional<AcquiredBlock> block = blocks_.try_pop()) {
      instrument_->process(*block, to_measuring);
    } else {
      std::this_thread::sleep_for(kTick);
    }
  }
}

void LivePipeline::measure_captures(const std::stop_token& stop) {
  while (const std::optional<CompletedCapture> next = captures_.front(stop)) {
    instrument_->count_measured(*next);
    captures_.pop();
  }
}

}  // namespace openscope
