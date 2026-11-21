#pragma once

#include <cstddef>
#include <cstdint>

namespace openscope {

/// The position of a sample on the sample clock, counting from zero since acquisition started. It
/// is the instrument's notion of time (ADR 0003).
using SampleIndex = std::uint64_t;

/// The single clock that samples every channel at the same instants. It counts sample indexes
/// only; the sample rate that turns an index into seconds is an acquisition setting (ADR 0003).
class SampleClock {
 public:
  /// The index of the next sample to be taken.
  [[nodiscard]] SampleIndex now() const { return next_; }

  /// Takes the next `count` sample indexes and returns the first of them.
  SampleIndex take(std::size_t count) {
    const SampleIndex first = next_;
    next_ += count;
    return first;
  }

 private:
  SampleIndex next_ = 0;
};

}  // namespace openscope
