#pragma once

#include <memory>
#include <mutex>

#include "openscope/capture.hpp"

namespace openscope {

/// Values computed from one capture, kept for as long as it is the capture asked about: what
/// MeasurementEngine and SpectrumCache cache. It holds the capture by a weak pointer, so it never
/// keeps a capture alive; once that capture is gone it matches no capture, even one that reuses
/// its address. Any thread may ask about any capture (queries of several Sessions run at the same
/// time, #67); those that ask about the same capture share its values, so `Values` must be safe
/// to use from several threads at once, as ComputedOnce is.
template <typename Values>
class CaptureCache {
 public:
  /// The values kept for `capture`. Asking about another capture starts afresh from `Values{}`;
  /// callers still holding the old values keep them.
  std::shared_ptr<Values> of(const std::shared_ptr<const Capture>& capture) {
    const std::scoped_lock lock(mutex_);
    if (capture_.lock() != capture) {
      capture_ = capture;
      values_ = std::make_shared<Values>();
    }
    return values_;
  }

 private:
  std::mutex mutex_;
  std::weak_ptr<const Capture> capture_;
  std::shared_ptr<Values> values_ = std::make_shared<Values>();
};

/// A value computed at most once: the first caller computes it, and callers at the same time wait
/// for that result instead of computing it again. Any thread may call `get`. `Value` is default
/// constructible; the default is never returned.
template <typename Value>
class ComputedOnce {
 public:
  /// The value, from `compute()` on the first call.
  template <typename Compute>
  const Value& get(Compute compute) {
    std::call_once(once_, [&] { value_ = compute(); });
    return value_;
  }

 private:
  std::once_flag once_;
  Value value_{};
};

}  // namespace openscope
