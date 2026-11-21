// The SPSC ring buffer (spec #1, Testing Decisions seam 3): first in, first out, fixed capacity,
// and a full queue drops and counts the new item. The two-thread tests run a real producer and
// consumer at once, so the TSan build checks them for data races.

#include "openscope/spsc_ring_buffer.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace openscope {
namespace {

using testing::ElementsAre;
using testing::Optional;

/// An item that owns heap memory, as a sample block does, so a torn or raced hand-over shows up
/// as a wrong payload (and under TSan as a race).
struct Stamped {
  std::uint64_t sequence = 0;
  std::vector<std::uint64_t> payload;
};

/// Items the two-thread tests send, and the payload length of each.
constexpr std::uint64_t kItems = 100'000;
constexpr std::size_t kPayloadLength = 16;

/// The payload item `sequence` carries.
std::vector<std::uint64_t> payload_of(std::uint64_t sequence) {
  return std::vector<std::uint64_t>(kPayloadLength, sequence);
}

Stamped stamped(std::uint64_t sequence) {
  return {.sequence = sequence, .payload = payload_of(sequence)};
}

/// Pops every item `ring` delivers until the producer has finished (`done`) and the queue is
/// empty, and returns their sequence numbers in the order they came out. Fails the test for an
/// item whose payload is not its own.
std::vector<std::uint64_t> consume(SpscRingBuffer<Stamped>& ring, const std::atomic<bool>& done) {
  std::vector<std::uint64_t> sequences;
  while (true) {
    // Read `done` before popping: an empty queue after the producer finished stays empty.
    const bool finished = done.load(std::memory_order_acquire);
    std::optional<Stamped> item = ring.try_pop();
    if (!item) {
      if (finished) {
        return sequences;
      }
      std::this_thread::yield();
      continue;
    }
    EXPECT_EQ(item->payload, payload_of(item->sequence));
    sequences.push_back(item->sequence);
  }
}

/// What a producer does with an item the full queue refuses.
enum class Refused : std::uint8_t { kTryAgain, kGiveUp };

/// What arrived at the consumer, and how many pushes the queue refused.
struct Delivery {
  std::vector<std::uint64_t> sequences;
  std::uint64_t refused = 0;
};

/// Sends items 0 to kItems - 1 through `ring` from a producer thread while this thread consumes
/// them, both at full speed.
Delivery deliver(SpscRingBuffer<Stamped>& ring, Refused refused_item) {
  std::atomic<bool> done = false;
  std::uint64_t refused = 0;
  std::thread producer([&] {
    for (std::uint64_t sequence = 0; sequence < kItems; ++sequence) {
      while (!ring.try_push(stamped(sequence))) {
        ++refused;
        if (refused_item == Refused::kGiveUp) {
          break;
        }
        std::this_thread::yield();
      }
    }
    done.store(true, std::memory_order_release);
  });
  Delivery delivery{.sequences = consume(ring, done), .refused = 0};
  producer.join();
  delivery.refused = refused;
  return delivery;
}

TEST(SpscRingBufferTest, CapacityOfZeroIsRejected) {
  EXPECT_THROW(SpscRingBuffer<int>(0), std::invalid_argument);
}

// The spare slot must fit: SIZE_MAX + 1 would wrap to an empty vector that the first push
// overruns.
TEST(SpscRingBufferTest, CapacityWithNoRoomForTheSpareSlotIsRejected) {
  EXPECT_THROW(SpscRingBuffer<int>(std::numeric_limits<std::size_t>::max()),
               std::invalid_argument);
  EXPECT_THROW(SpscRingBuffer<int>(std::vector<int>().max_size()), std::invalid_argument);
}

TEST(SpscRingBufferTest, EmptyQueuePopsNothing) {
  SpscRingBuffer<int> ring(4);

  EXPECT_EQ(ring.try_pop(), std::nullopt);
  EXPECT_EQ(ring.dropped(), 0U);
}

TEST(SpscRingBufferTest, ItemsComeOutInTheOrderTheyWentIn) {
  SpscRingBuffer<int> ring(4);
  ASSERT_TRUE(ring.try_push(1));
  ASSERT_TRUE(ring.try_push(2));
  ASSERT_TRUE(ring.try_push(3));

  EXPECT_THAT(ring.try_pop(), Optional(1));
  EXPECT_THAT(ring.try_pop(), Optional(2));
  EXPECT_THAT(ring.try_pop(), Optional(3));
  EXPECT_EQ(ring.try_pop(), std::nullopt);
}

TEST(SpscRingBufferTest, FullQueueDropsAndCountsEachNewItemAndKeepsTheOldOnes) {
  SpscRingBuffer<int> ring(3);
  for (const int item : {1, 2, 3}) {
    ASSERT_TRUE(ring.try_push(int{item}));
  }

  EXPECT_FALSE(ring.try_push(4));
  EXPECT_FALSE(ring.try_push(5));

  EXPECT_EQ(ring.dropped(), 2U);
  std::vector<int> popped;
  while (const std::optional<int> item = ring.try_pop()) {
    popped.push_back(*item);
  }
  EXPECT_THAT(popped, ElementsAre(1, 2, 3));
}

TEST(SpscRingBufferTest, DroppedItemIsLeftWithTheProducer) {
  SpscRingBuffer<Stamped> ring(1);
  ASSERT_TRUE(ring.try_push(stamped(1)));
  Stamped refused = stamped(2);

  EXPECT_FALSE(ring.try_push(std::move(refused)));

  // NOLINTNEXTLINE(bugprone-use-after-move): try_push leaves a refused item as it was.
  EXPECT_EQ(refused.payload, payload_of(2));
}

TEST(SpscRingBufferTest, RoomFreedByAPopTakesTheNextItem) {
  SpscRingBuffer<int> ring(1);
  ASSERT_TRUE(ring.try_push(1));
  ASSERT_FALSE(ring.try_push(2));

  EXPECT_THAT(ring.try_pop(), Optional(1));
  EXPECT_TRUE(ring.try_push(3));

  EXPECT_THAT(ring.try_pop(), Optional(3));
  EXPECT_EQ(ring.dropped(), 1U);
}

// Each pass fills the queue and drains it, so the indexes wrap round many times.
TEST(SpscRingBufferTest, KeepsOrderAcrossManyWrapsRound) {
  constexpr int kCapacity = 3;
  SpscRingBuffer<int> ring(kCapacity);
  std::vector<int> pushed;
  std::vector<int> popped;
  for (int pass = 0; pass < 100; ++pass) {
    for (int n = 0; n <= pass % kCapacity; ++n) {
      const int item = static_cast<int>(pushed.size());
      EXPECT_TRUE(ring.try_push(int{item}));
      pushed.push_back(item);
    }
    while (const std::optional<int> item = ring.try_pop()) {
      popped.push_back(*item);
    }
  }
  EXPECT_EQ(popped, pushed);
  EXPECT_EQ(ring.dropped(), 0U);
}

// A producer that tries again whenever the queue is full loses nothing: every item arrives, in
// order, and each refusal it saw is counted once.
TEST(SpscRingBufferThreadsTest, EveryItemArrivesInOrderWhenTheProducerRetries) {
  SpscRingBuffer<Stamped> ring(8);

  const Delivery delivery = deliver(ring, Refused::kTryAgain);

  ASSERT_EQ(delivery.sequences.size(), kItems);
  for (std::uint64_t n = 0; n < kItems; ++n) {
    ASSERT_EQ(delivery.sequences[n], n);
  }
  EXPECT_EQ(ring.dropped(), delivery.refused);
}

// A producer that never waits loses the items it finds no room for. The items that arrive keep
// their order, and the gaps between them add up to exactly the drop count.
TEST(SpscRingBufferThreadsTest, GapsInWhatArrivesAddUpToTheDropCount) {
  SpscRingBuffer<Stamped> ring(4);

  const Delivery delivery = deliver(ring, Refused::kGiveUp);

  ASSERT_GT(delivery.refused, 0U) << "no drops: the test did not exercise the full queue";
  std::uint64_t gaps = 0;
  std::uint64_t expected = 0;
  for (const std::uint64_t sequence : delivery.sequences) {
    ASSERT_GE(sequence, expected) << "out of order";
    gaps += sequence - expected;
    expected = sequence + 1;
  }
  gaps += kItems - expected;  // Items dropped after the last one that arrived.
  EXPECT_EQ(ring.dropped(), delivery.refused);
  EXPECT_EQ(gaps, delivery.refused);
  EXPECT_EQ(delivery.sequences.size() + delivery.refused, kItems);
}

}  // namespace
}  // namespace openscope
