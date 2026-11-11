#include "../tools/render_host/render_command_queue.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>

using namespace warvk::host::commands;

static std::atomic<unsigned> checks{0}, deniedAllocations{0};
static thread_local bool forbidAllocation = false;

static void CheckAllocation() {
  if (forbidAllocation) {
    deniedAllocations.fetch_add(1, std::memory_order_relaxed);
    throw std::bad_alloc();
  }
}
void* operator new(std::size_t size) {
  CheckAllocation();
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  CheckAllocation();
  const auto align = static_cast<std::size_t>(alignment);
#ifdef _WIN32
  if (auto* p = _aligned_malloc(size ? size : 1, align)) return p;
#else
  if (auto* p = std::aligned_alloc(align, ((size ? size : 1) + align - 1) / align * align)) return p;
#endif
  throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept {
#ifdef _WIN32
  _aligned_free(p);
#else
  std::free(p);
#endif
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

struct NoAlloc final {
  NoAlloc() { forbidAllocation = true; }
  ~NoAlloc() { forbidAllocation = false; }
};
static void Check(bool condition, unsigned line) {
  checks.fetch_add(1, std::memory_order_relaxed);
  if (!condition) {
    std::fprintf(stderr, "render command queue check failed line=%u\n", line);
    std::exit(1);
  }
}
#define CHECK(x) Check(bool(x), __LINE__)

static constexpr Scope ScopeA{0x1122334455667788ull, 0x8877665544332211ull, 3, 7};
static constexpr Scope ScopeB{0x9988776655443322ull, 0xaabbccddeeff0011ull, 4, 8};
static_assert(!std::is_copy_constructible_v<SpscRenderCommandQueue>
  && !std::is_move_constructible_v<SpscRenderCommandQueue>);
static_assert(!std::is_convertible_v<PublishResult, bool>);

static uint8_t Pattern(uint64_t sequence, uint32_t offset) {
  return static_cast<uint8_t>(sequence * 17 + offset * 29 + (offset >> 8));
}
static void Fill(std::array<uint8_t, MaxBatchBytes>& data, uint64_t sequence, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes; ++i) data[i] = Pattern(sequence, i);
}
static void Verify(const Batch& batch, uint64_t sequence, uint32_t bytes,
    Scope scope = ScopeA) {
  CHECK(batch.scope == scope);
  CHECK(batch.sequence == sequence && batch.bytes == bytes);
  bool same = true;
  for (uint32_t i = 0; i < bytes; ++i) same &= batch.payload[i] == Pattern(sequence, i);
  CHECK(same);
}
static void Rejected(PublishResult result, Publish expected) {
  CHECK(result.outcome == expected);
  CHECK(result.sequence == 0 && !result.mayCommitLocalState());
}
static void Published(PublishResult result, uint64_t sequence) {
  CHECK(result.outcome == Publish::Published && result.sequence == sequence);
  CHECK(result.mayCommitLocalState());
}
static Counters Balanced(const SpscRenderCommandQueue& queue) {
  const auto c = queue.countersAfterJoin();
  CHECK(c.published == c.dequeued + c.queued && c.queued <= Capacity);
  CHECK(c.dequeued == c.cpuAcknowledged + (c.pendingCpuAck ? 1u : 0u));
  return c;
}

// No thread is running for these direct same-owner calls. Failed publications
// cannot alter caller state, consume a sequence or modify committed payloads.
static void FullRecoveryAndCopies() {
  auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA);
  std::array<uint8_t, MaxBatchBytes> source{};
  Batch output, saved;
  NoAlloc guard;
  output.sequence = 987; output.payload[0] = 213;
  CHECK(queue->tryDequeue(output) == Dequeue::Empty);
  CHECK(output.sequence == 987 && output.payload[0] == 213);
  CHECK(queue->acknowledgeCpu(ScopeA, 1) == CpuAck::NoPending);
  unsigned localTransactions = 0;
  for (uint64_t sequence = 1; sequence <= Capacity; ++sequence) {
    Fill(source, sequence, MaxBatchBytes);
    const auto result = queue->tryPublish(ScopeA, source.data(), MaxBatchBytes);
    Published(result, sequence);
    if (result.mayCommitLocalState()) ++localTransactions;
    source.fill(229); // Published bytes must no longer depend on this source.
  }
  for (unsigned i = 0; i < 2000; ++i) {
    const auto result = queue->tryPublish(ScopeA, source.data(), 97);
    Rejected(result, Publish::Full);
    if (result.mayCommitLocalState()) ++localTransactions;
  }
  CHECK(localTransactions == 4);
  Rejected(queue->tryPublish(ScopeB, source.data(), 1), Publish::Invalid);
  Rejected(queue->tryPublish(ScopeA, nullptr, 1), Publish::Invalid);
  Rejected(queue->tryPublish(ScopeA, source.data(), 0), Publish::Invalid);
  Rejected(queue->tryPublish(ScopeA, source.data(), MaxBatchBytes + 1), Publish::Invalid);
  CHECK(Balanced(*queue).published == 4 && Balanced(*queue).lastPublished == 4);

  CHECK(queue->tryDequeue(output) == Dequeue::Item);
  Verify(output, 1, MaxBatchBytes); saved = output;
  CHECK(queue->tryDequeue(output) == Dequeue::AwaitingCpuAck);
  Verify(output, 1, MaxBatchBytes);
  CHECK(queue->acknowledgeCpu(ScopeB, 1) == CpuAck::Invalid);
  CHECK(queue->acknowledgeCpu(ScopeA, 0) == CpuAck::Invalid);
  CHECK(queue->acknowledgeCpu(ScopeA, 2) == CpuAck::Invalid);
  CHECK(Balanced(*queue).pendingCpuAck);
  CHECK(queue->acknowledgeCpu(ScopeA, 1) == CpuAck::Acknowledged);
  CHECK(queue->acknowledgeCpu(ScopeA, 1) == CpuAck::NoPending);
  Fill(source, 5, 97);
  Published(queue->tryPublish(ScopeA, source.data(), 97), 5);
  source.fill(143);
  Verify(saved, 1, MaxBatchBytes); // A private copy survives reuse of its slot.
  queue->closeProducer();
  Rejected(queue->tryPublish(ScopeA, source.data(), 1), Publish::Closed);
  for (uint64_t sequence = 2; sequence <= 5; ++sequence) {
    CHECK(queue->tryDequeue(output) == Dequeue::Item);
    Verify(output, sequence, sequence == 5 ? 97 : MaxBatchBytes);
    CHECK(queue->tryDequeue(output) == Dequeue::AwaitingCpuAck);
    CHECK(queue->acknowledgeCpu(ScopeA, sequence - 1) == CpuAck::Invalid);
    CHECK(queue->acknowledgeCpu(ScopeA, sequence) == CpuAck::Acknowledged);
  }
  CHECK(queue->tryDequeue(output) == Dequeue::Finished);
  Verify(output, 5, 97);
  const auto c = Balanced(*queue);
  CHECK(c.published == 5 && c.cpuAcknowledged == 5 && c.closed && !c.faulted);
  CHECK(c.lastPublished == 5 && c.lastDequeued == 5 && c.lastCpuAck == 5);
}

static void ScopeFaultAndConfiguration() {
  std::array<uint8_t, MaxBatchBytes> source{};
  Batch output;
  {
    auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA);
    NoAlloc guard;
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
      Fill(source, sequence, 97);
      Published(queue->tryPublish(ScopeA, source.data(), 97), sequence);
    }
    CHECK(queue->tryDequeue(output) == Dequeue::Item); Verify(output, 1, 97);
    queue->requestFault(); queue->requestStop(); queue->closeProducer();
    Rejected(queue->tryPublish(ScopeA, source.data(), 1), Publish::Faulted);
    CHECK(queue->tryDequeue(output) == Dequeue::Faulted); Verify(output, 1, 97);
    CHECK(queue->acknowledgeCpu(ScopeA, 1) == CpuAck::Faulted);
    queue->requestFault();
    CHECK(queue->tryDequeue(output) == Dequeue::Faulted);
    const auto c = Balanced(*queue);
    CHECK(c.faulted && c.stopped && c.closed && c.pendingCpuAck);
    CHECK(c.queued == 2 && c.published == 3 && c.cpuAcknowledged == 0);
  }
  {
    auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeB);
    NoAlloc guard;
    // Every part of immutable identity is checked independently, not merely a
    // changed nonce hiding a missing map/device comparison.
    auto other = ScopeB;
    ++other.nonceLow; Rejected(queue->tryPublish(other, source.data(), 1), Publish::Invalid);
    other = ScopeB; ++other.nonceHigh;
    Rejected(queue->tryPublish(other, source.data(), 1), Publish::Invalid);
    other = ScopeB; ++other.mapEpoch;
    Rejected(queue->tryPublish(other, source.data(), 1), Publish::Invalid);
    other = ScopeB; ++other.deviceEpoch;
    Rejected(queue->tryPublish(other, source.data(), 1), Publish::Invalid);
    Rejected(queue->tryPublish(ScopeA, source.data(), 1), Publish::Invalid);
    Fill(source, 1, 1); Published(queue->tryPublish(ScopeB, source.data(), 1), 1);
    CHECK(queue->tryDequeue(output) == Dequeue::Item); Verify(output, 1, 1, ScopeB);
    CHECK(queue->acknowledgeCpu(ScopeA, 1) == CpuAck::Invalid);
    CHECK(queue->acknowledgeCpu(ScopeB, 1) == CpuAck::Acknowledged);
    queue->requestStop();
    Rejected(queue->tryPublish(ScopeB, source.data(), 1), Publish::Stopped);
    CHECK(queue->tryDequeue(output) == Dequeue::Empty); Verify(output, 1, 1, ScopeB);
    queue->closeProducer(); CHECK(queue->tryDequeue(output) == Dequeue::Finished);
    CHECK(Balanced(*queue).published == 1);
  }
  for (const auto scope : {Scope{}, Scope{1, 0, 0, 1}, Scope{1, 0, 1, 0}}) {
    auto queue = std::make_unique<SpscRenderCommandQueue>(scope);
    NoAlloc guard;
    Rejected(queue->tryPublish(scope, source.data(), 1), Publish::Unconfigured);
    CHECK(queue->tryDequeue(output) == Dequeue::Unconfigured);
    CHECK(queue->acknowledgeCpu(scope, 1) == CpuAck::Unconfigured);
    CHECK(Balanced(*queue).published == 0);
  }
  for (const auto limits : {Limits{0, 2, 1}, Limits{3, 2, 1},
      Limits{1, 2, 0}, Limits{1, 2, MaxBatchBytes + 1}}) {
    auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA, limits);
    NoAlloc guard;
    Rejected(queue->tryPublish(ScopeA, source.data(), 1), Publish::Unconfigured);
    CHECK(queue->tryDequeue(output) == Dequeue::Unconfigured);
    CHECK(Balanced(*queue).published == 0);
  }
  for (const uint32_t bytes : {1u, 97u, MaxBatchBytes - 1, MaxBatchBytes}) {
    auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA, Limits{1, 2, bytes});
    NoAlloc guard;
    Rejected(queue->tryPublish(ScopeA, source.data(), bytes + 1), Publish::Invalid);
    Fill(source, 1, bytes); Published(queue->tryPublish(ScopeA, source.data(), bytes), 1);
    CHECK(queue->tryDequeue(output) == Dequeue::Item); Verify(output, 1, bytes);
    CHECK(queue->acknowledgeCpu(ScopeA, 1) == CpuAck::Acknowledged);
    queue->closeProducer(); CHECK(queue->tryDequeue(output) == Dequeue::Finished);
  }
}

static void SequenceBoundaries() {
  const auto max64 = std::numeric_limits<uint64_t>::max();
  const uint64_t max32 = std::numeric_limits<uint32_t>::max();
  std::array<uint8_t, MaxBatchBytes> source{};
  Batch output;
  for (const auto limits : {Limits{1, 2, 97}, Limits{max32 - 1, max32 + 6, 97},
      Limits{max64 - 1, max64, 97}}) {
    auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA, limits);
    NoAlloc guard;
    uint64_t sequence = limits.firstSequence;
    for (;;) {
      Rejected(queue->tryPublish(ScopeB, source.data(), 97), Publish::Invalid);
      Fill(source, sequence, 97);
      Published(queue->tryPublish(ScopeA, source.data(), 97), sequence);
      CHECK(queue->tryDequeue(output) == Dequeue::Item); Verify(output, sequence, 97);
      CHECK(queue->acknowledgeCpu(ScopeA, sequence) == CpuAck::Acknowledged);
      if (sequence == limits.lastSequence) break;
      ++sequence;
    }
    Rejected(queue->tryPublish(ScopeA, source.data(), 97), Publish::Exhausted);
    Rejected(queue->tryPublish(ScopeA, source.data(), 97), Publish::Exhausted);
    CHECK(queue->tryDequeue(output) == Dequeue::Empty);
    queue->closeProducer(); CHECK(queue->tryDequeue(output) == Dequeue::Finished);
    const auto c = Balanced(*queue);
    CHECK(c.exhausted && c.lastPublished == limits.lastSequence && c.lastCpuAck == limits.lastSequence);
    CHECK(c.published == limits.lastSequence - limits.firstSequence + 1);
  }
}

static void StopWithCommittedBatches() {
  auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA);
  std::array<uint8_t, MaxBatchBytes> source{};
  Batch output;
  NoAlloc guard;
  for (uint64_t sequence = 1; sequence <= 4; ++sequence) {
    Fill(source, sequence, 97);
    Published(queue->tryPublish(ScopeA, source.data(), 97), sequence);
  }
  queue->requestStop();
  Rejected(queue->tryPublish(ScopeA, source.data(), 97), Publish::Stopped);
  CHECK(Balanced(*queue).queued == 4);
  for (uint64_t sequence = 1; sequence <= 4; ++sequence) {
    CHECK(queue->tryDequeue(output) == Dequeue::Item); Verify(output, sequence, 97);
    CHECK(queue->acknowledgeCpu(ScopeA, sequence) == CpuAck::Acknowledged);
  }
  CHECK(queue->tryDequeue(output) == Dequeue::Empty); Verify(output, 4, 97);
  queue->closeProducer(); CHECK(queue->tryDequeue(output) == Dequeue::Finished);
  const auto c = Balanced(*queue);
  CHECK(c.stopped && c.closed && c.published == 4 && c.cpuAcknowledged == 4 && c.queued == 0);
}

struct StartGate {
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  void arriveAndWait() {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
  }
  void releaseBoth() {
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    start.store(true, std::memory_order_release);
  }
};

// The producer must join while the consumer is deliberately paused. No queue
// method may hide retries or wait for space. Test barriers are outside the API.
static void PausedConsumer() {
  auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA);
  StartGate gate;
  std::atomic<bool> resume{false};
  std::thread consumer([&] {
    Batch output; NoAlloc guard; gate.arriveAndWait();
    while (!resume.load(std::memory_order_acquire)) std::this_thread::yield();
    for (uint64_t sequence = 1; sequence <= 4; ++sequence) {
      CHECK(queue->tryDequeue(output) == Dequeue::Item); Verify(output, sequence, 97);
      CHECK(queue->acknowledgeCpu(ScopeA, sequence) == CpuAck::Acknowledged);
    }
    CHECK(queue->tryDequeue(output) == Dequeue::Finished);
  });
  std::thread producer([&] {
    std::array<uint8_t, MaxBatchBytes> source{}; NoAlloc guard; gate.arriveAndWait();
    for (uint64_t sequence = 1; sequence <= 4; ++sequence) {
      Fill(source, sequence, 97); Published(queue->tryPublish(ScopeA, source.data(), 97), sequence);
    }
    for (unsigned i = 0; i < 2000; ++i)
      Rejected(queue->tryPublish(ScopeA, source.data(), 97), Publish::Full);
    queue->closeProducer();
  });
  gate.releaseBoth(); producer.join();
  CHECK(!resume.load(std::memory_order_relaxed));
  resume.store(true, std::memory_order_release); consumer.join();
  const auto c = Balanced(*queue);
  CHECK(c.closed && c.published == 4 && c.cpuAcknowledged == 4 && c.queued == 0);
}

static uint32_t ConcurrentBytes(uint64_t sequence) {
  return sequence % 32 == 0 ? MaxBatchBytes : (sequence % 3 == 0 ? 1u : 97u);
}
static uint64_t ConcurrentRecovery() {
  auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA);
  StartGate gate;
  uint64_t sentBytes = 0, receivedBytes = 0;
  std::thread consumer([&] {
    Batch output; NoAlloc guard; gate.arriveAndWait(); uint64_t next = 1;
    for (;;) {
      const auto result = queue->tryDequeue(output);
      if (result == Dequeue::Finished) break;
      if (result == Dequeue::Empty) { std::this_thread::yield(); continue; }
      CHECK(result == Dequeue::Item); Verify(output, next, ConcurrentBytes(next));
      receivedBytes += output.bytes;
      CHECK(queue->acknowledgeCpu(ScopeA, next) == CpuAck::Acknowledged); ++next;
    }
    CHECK(next == 10001);
  });
  std::thread producer([&] {
    std::array<uint8_t, MaxBatchBytes> source{}; NoAlloc guard; gate.arriveAndWait();
    for (uint64_t sequence = 1; sequence <= 10000; ++sequence) {
      const auto bytes = ConcurrentBytes(sequence); Fill(source, sequence, bytes);
      for (;;) {
        const auto result = queue->tryPublish(ScopeA, source.data(), bytes);
        if (result.outcome == Publish::Published) { Published(result, sequence); break; }
        Rejected(result, Publish::Full);
        std::this_thread::yield(); // Explicit caller policy; no retry inside queue.
      }
      sentBytes += bytes; source.fill(171);
    }
    queue->closeProducer();
  });
  gate.releaseBoth(); producer.join(); consumer.join();
  CHECK(sentBytes == receivedBytes);
  const auto c = Balanced(*queue);
  CHECK(c.published == 10000 && c.cpuAcknowledged == 10000 && c.queued == 0 && c.closed);
  return receivedBytes;
}

// Stop can race either side of the bounded copy. An already-linearized publish
// is allowed to succeed; after the synchronized stop every later call rejects.
// The schedule does not claim to force a particular instruction-level race.
static unsigned ConcurrentStop() {
  unsigned acceptedRounds = 0;
  for (unsigned round = 0; round < 64; ++round) {
    auto queue = std::make_unique<SpscRenderCommandQueue>(ScopeA);
    StartGate gate; std::atomic<bool> stopComplete{false};
    unsigned published = 0, received = 0;
    std::thread consumer([&] {
      Batch output; NoAlloc guard; gate.arriveAndWait();
      for (;;) {
        const auto result = queue->tryDequeue(output);
        if (result == Dequeue::Finished) break;
        if (result == Dequeue::Empty) { std::this_thread::yield(); continue; }
        CHECK(result == Dequeue::Item); Verify(output, 1, MaxBatchBytes); ++received;
        CHECK(queue->acknowledgeCpu(ScopeA, 1) == CpuAck::Acknowledged);
      }
    });
    std::thread producer([&] {
      std::array<uint8_t, MaxBatchBytes> source{}; NoAlloc guard;
      Fill(source, 1, MaxBatchBytes); gate.arriveAndWait();
      const auto result = queue->tryPublish(ScopeA, source.data(), MaxBatchBytes);
      if (result.outcome == Publish::Published) { Published(result, 1); ++published; }
      else Rejected(result, Publish::Stopped);
      while (!stopComplete.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned i = 0; i < 32; ++i)
        Rejected(queue->tryPublish(ScopeA, source.data(), 1), Publish::Stopped);
      queue->closeProducer();
    });
    gate.releaseBoth();
    if (round % 2) std::this_thread::yield();
    queue->requestStop(); stopComplete.store(true, std::memory_order_release);
    producer.join(); consumer.join();
    const auto c = Balanced(*queue);
    CHECK(published == received && c.published == published && c.cpuAcknowledged == published);
    CHECK(c.closed && c.stopped && !c.faulted && c.queued == 0);
    acceptedRounds += published;
  }
  return acceptedRounds;
}

int main() {
  // Make a broken handshake/blocking implementation fail instead of hanging CI.
  std::atomic<bool> done{false};
  std::thread watchdog([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::fprintf(stderr, "render command queue watchdog timeout\n"); std::_Exit(2);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  bool injectionObserved = false;
  try {
    NoAlloc guard;
    void* probe = ::operator new(1); // Direct call prevents new-expression elision.
    ::operator delete(probe);
  } catch (const std::bad_alloc&) { injectionObserved = true; }
  CHECK(injectionObserved && deniedAllocations.load() == 1);
  FullRecoveryAndCopies(); ScopeFaultAndConfiguration(); SequenceBoundaries(); StopWithCommittedBatches();
  PausedConsumer(); const auto bytes = ConcurrentRecovery(); const auto stopped = ConcurrentStop();
  CHECK(deniedAllocations.load() == 1); // The sole denial was the deliberate probe.
  done.store(true, std::memory_order_release); watchdog.join();
  std::printf("render command queue checks=%u failures=0 bits=%zu queueBytes=%zu batchBytes=%zu "
    "capacity=%u maxBatchBytes=%u concurrentPublished=10000 concurrentBytes=%llu "
    "stopRacePublished=%u allocationDenials=1 contiguousSequences=1 immutableCopy=1 "
    "stopDrained=1 oldScopeRejected=1 ipcTested=0 gpuTested=0\n",
    checks.load(), sizeof(void*) * 8, sizeof(SpscRenderCommandQueue), sizeof(Batch),
    Capacity, MaxBatchBytes, static_cast<unsigned long long>(bytes), stopped);
}
