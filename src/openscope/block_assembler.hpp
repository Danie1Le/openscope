#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "openscope/acquired_block.hpp"
#include "openscope/capture_assembler.hpp"

namespace openscope {

/// Turns AcquiredBlocks, in the order they were acquired, into captures: the processing half of
/// the Instrument (#8). It discards the capture in progress at a new generation (a command changed
/// the settings or run state) and after a dropped block (a sequence number that never arrived),
/// counting the captures a dropped block split. Only the thread that processes blocks uses it, so
/// it needs no lock; `discarded_captures` is the exception, which any thread may read.
class BlockAssembler {
 public:
  /// Assembles `block`, handing each capture to `on_completed` as it completes, up to the
  /// block's capture_limit(). Returns, when the block's Single capture completed inside it, the
  /// offset just past that capture.
  std::optional<std::size_t> assemble(const AcquiredBlock& block, const CaptureSink& on_completed);

  /// How many captures assembling `block` makes: its own limit, unless run_rest() has ended the
  /// Single it was acquired for.
  [[nodiscard]] CaptureLimit capture_limit(const AcquiredBlock& block) const;

  /// `RUN` ended the Single whose capture `block` completed at offset `single_end`: assembles the
  /// rest of `block` as Running, handing captures to `on_completed`, and captures every later
  /// Armed-single block of its generation as Running too, as if acquired after `RUN`.
  void run_rest(const AcquiredBlock& block, std::size_t single_end,
                const CaptureSink& on_completed);

  /// How many captures have been discarded because they would have spanned a dropped block.
  [[nodiscard]] std::uint64_t discarded_captures() const {
    return discarded_captures_.load(std::memory_order_relaxed);
  }

 private:
  CaptureAssembler captures_;
  /// The generation of the capture in progress, and the sequence number of the block expected
  /// next.
  std::uint64_t assembled_generation_ = 0;
  std::uint64_t expected_sequence_ = 0;
  /// The generation whose Single `RUN` ended before all its Armed-single blocks were processed;
  /// the rest of them are captured as Running. Generations are never reused.
  std::optional<std::uint64_t> run_generation_;
  std::atomic<std::uint64_t> discarded_captures_ = 0;
};

}  // namespace openscope
