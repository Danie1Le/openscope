#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace openscope {

/// The count, mean, minimum, maximum and standard deviation of a stream of values, updated one
/// value at a time in constant memory. Measurement statistics keep one per measurement and channel
/// across captures, whose number has no bound.
///
/// The spread uses Welford's update, which works on each value's deviation from the running mean:
/// a running sum of squares cancels catastrophically when the values are large and close together.
class RunningStatistics {
 public:
  /// Adds `value`.
  void add(double value) {
    minimum_ = count_ == 0 ? value : std::min(minimum_, value);
    maximum_ = count_ == 0 ? value : std::max(maximum_, value);
    ++count_;
    const double delta = value - mean_;
    mean_ += delta / static_cast<double>(count_);
    sum_of_squared_deviations_ += delta * (value - mean_);
  }

  /// How many values have been added.
  [[nodiscard]] std::size_t count() const { return count_; }

  // The statistics below are meaningful only once a value has been added (count() > 0).

  /// The mean of the values.
  [[nodiscard]] double mean() const { return mean_; }
  /// The lowest value.
  [[nodiscard]] double minimum() const { return minimum_; }
  /// The highest value.
  [[nodiscard]] double maximum() const { return maximum_; }
  /// The population standard deviation: the root of the mean squared deviation from the mean, so
  /// one value has a standard deviation of 0.
  [[nodiscard]] double standard_deviation() const {
    return count_ == 0 ? 0.0 : std::sqrt(sum_of_squared_deviations_ / static_cast<double>(count_));
  }

 private:
  std::size_t count_ = 0;
  double mean_ = 0.0;
  double minimum_ = 0.0;
  double maximum_ = 0.0;
  double sum_of_squared_deviations_ = 0.0;
};

}  // namespace openscope
