#include "openscope/block_assembler.hpp"

#include <atomic>
#include <cstddef>
#include <optional>
#include <span>

#include "openscope/acquired_block.hpp"
#include "openscope/capture.hpp"
#include "openscope/capture_assembler.hpp"
#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"

namespace openscope {
namespace {

/// The samples of `block` from offset `from` (at most its count) on, as a block of their own.
SampleBlock samples_from(const SampleBlock& block, std::size_t from) {
  SampleBlock rest{
      .first_index = block.first_index + from, .count = block.count - from, .channels = {}};
  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    const std::span<const float> samples(block.channels.at(channel));
    if (!samples.empty()) {  // An empty one is a channel the block does not carry.
      const std::span<const float> kept = samples.subspan(from);
      rest.channels.at(channel).assign(kept.begin(), kept.end());
    }
  }
  return rest;
}

}  // namespace

std::optional<std::size_t> BlockAssembler::assemble(const AcquiredBlock& block,
                                                    const CaptureSink& on_completed) {
  const bool follows_drop = block.sequence != expected_sequence_;
  expected_sequence_ = block.sequence + 1;
  const bool new_generation = block.generation != assembled_generation_;
  // A new generation was acquired under changed settings or run state. After a dropped block the
  // trigger restarts too: it never sees an edge across the samples that are missing.
  if (new_generation || follows_drop) {
    const bool split = captures_.discard();
    // A capture of an older generation would have been discarded with or without the gap.
    if (split && !new_generation) {
      discarded_captures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  assembled_generation_ = block.generation;
  return captures_.push(block.samples, block.acquisition, block.channels, block.trigger,
                        capture_limit(block), on_completed);
}

CaptureLimit BlockAssembler::capture_limit(const AcquiredBlock& block) const {
  // Once RUN has ended this generation's Single, its other Armed-single blocks are captured as
  // Running, as if they had been acquired after RUN.
  return block.generation == run_generation_ ? CaptureLimit::kEvery : block.limit;
}

void BlockAssembler::run_rest(const AcquiredBlock& block, std::size_t single_end,
                              const CaptureSink& on_completed) {
  run_generation_ = block.generation;
  if (single_end < block.samples.count) {
    captures_.push(samples_from(block.samples, single_end), block.acquisition, block.channels,
                   block.trigger, CaptureLimit::kEvery, on_completed);
  }
}

}  // namespace openscope
