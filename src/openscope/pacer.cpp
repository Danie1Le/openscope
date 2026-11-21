#include "openscope/pacer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace openscope {

std::uint64_t Pacer::release(Clock::time_point now, double sample_rate, std::uint64_t most) {
  if (sample_rate != rate_) {
    if (rate_ > 0.0) {
      anchor_ += std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(static_cast<double>(released_) / rate_));
    }
    rate_ = sample_rate;
    released_ = 0;
  }
  const std::chrono::duration<double> elapsed = now - anchor_;
  if (elapsed.count() <= 0.0) {
    return 0;
  }
  const auto due = static_cast<std::uint64_t>(std::floor(elapsed.count() * rate_));
  if (due <= released_) {
    return 0;
  }
  const std::uint64_t fresh = due - released_;
  released_ = due;
  return std::min(fresh, most);
}

}  // namespace openscope
