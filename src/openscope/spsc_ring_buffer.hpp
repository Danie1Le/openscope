#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace openscope {

/// A lock-free single-producer single-consumer queue of fixed capacity, which carries sample
/// blocks from the acquisition thread to the processing thread (spec #1). One thread may push and
/// one other thread may pop at the same time; neither ever blocks or waits for the other. When the
/// queue is full the producer's item is dropped and counted rather than waiting for room, so
/// acquisition keeps pace with the sample clock however far processing falls behind.
template <typename T>
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): the padding keeps the indexes apart.
class SpscRingBuffer {
 public:
  /// A queue that holds up to `capacity` items. Throws std::invalid_argument for a capacity of 0.
  explicit SpscRingBuffer(std::size_t capacity) : slots_(slot_count(capacity)) {}

  /// Producer only: appends `item`, or, when the queue is full, leaves `item` untouched, counts it
  /// as dropped and returns false.
  bool try_push(T&& item) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = advance(tail);
    // Acquire: the consumer has finished moving out of the slot it released.
    if (next == head_.load(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    slots_[tail] = std::move(item);
    tail_.store(next, std::memory_order_release);  // Publishes the slot to the consumer.
    return true;
  }

  /// Consumer only: removes and returns the oldest item, or nullopt when the queue is empty.
  std::optional<T> try_pop() {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    // Acquire: the producer has finished writing the slot it published.
    if (head == tail_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    std::optional<T> item(std::move(slots_[head]));
    head_.store(advance(head), std::memory_order_release);  // Hands the slot back.
    return item;
  }

  /// How many items try_push has dropped because the queue was full. Any thread may read it.
  [[nodiscard]] std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  /// One slot more than the capacity, so a full queue (tail just behind head) differs from an
  /// empty one (tail at head) without a shared count.
  static std::size_t slot_count(std::size_t capacity) {
    if (capacity == 0) {
      throw std::invalid_argument("ring buffer capacity of zero");
    }
    return capacity + 1;
  }

  /// The slot after `index`, wrapping round.
  [[nodiscard]] std::size_t advance(std::size_t index) const {
    return index + 1 == slots_.size() ? 0 : index + 1;
  }

  // The producer's and the consumer's index sit on separate cache lines, so each thread's writes
  // do not keep invalidating the line the other one writes.
  static constexpr std::size_t kCacheLine = 64;

  std::vector<T> slots_;
  /// The next slot to pop; written by the consumer only.
  alignas(kCacheLine) std::atomic<std::size_t> head_ = 0;
  /// The next slot to fill; written by the producer only.
  alignas(kCacheLine) std::atomic<std::size_t> tail_ = 0;
  std::atomic<std::uint64_t> dropped_ = 0;
};

}  // namespace openscope
