// Throughput of the SPSC ring buffer between two threads, as the live pipeline uses it: a
// producer pushes as fast as it can and tries again when the queue is full, while a consumer
// thread pops. Results are recorded in docs/benchmarks.md.

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

#include "openscope/spsc_ring_buffer.hpp"

namespace openscope {
namespace {

// Google Benchmark's `for (auto _ : state)` loop never reads its variable by design.
// NOLINTBEGIN(clang-analyzer-deadcode.DeadStores)

/// Items per second through a queue of `state.range(0)` slots, timed on the producer: each
/// iteration hands one item over. `full_per_item` is how many pushes per item found the queue
/// full and were tried again: how often the producer had to wait for the consumer.
void BM_SpscRingBufferThroughput(benchmark::State& state) {
  SpscRingBuffer<std::uint64_t> ring(static_cast<std::size_t>(state.range(0)));
  std::atomic<bool> done = false;
  std::thread consumer([&] {
    std::uint64_t sum = 0;
    while (!done.load(std::memory_order_acquire)) {
      while (const std::optional<std::uint64_t> item = ring.try_pop()) {
        sum += *item;
      }
    }
    benchmark::DoNotOptimize(sum);
  });
  std::uint64_t next = 0;
  for (auto _ : state) {
    while (!ring.try_push(std::uint64_t{next})) {
      // Full: try again once the consumer has made room.
    }
    ++next;
  }
  done.store(true, std::memory_order_release);
  consumer.join();
  state.SetItemsProcessed(state.iterations());
  state.counters["full_per_item"] =
      benchmark::Counter(static_cast<double>(ring.dropped()), benchmark::Counter::kAvgIterations);
}

// NOLINTEND(clang-analyzer-deadcode.DeadStores)

BENCHMARK(BM_SpscRingBufferThroughput)->RangeMultiplier(16)->Range(16, 4096)->UseRealTime();

}  // namespace
}  // namespace openscope
