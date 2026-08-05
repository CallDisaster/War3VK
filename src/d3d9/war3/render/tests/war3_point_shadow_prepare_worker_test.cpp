#include "../war3_point_shadow_prepare_worker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace dxvk::war3::render;

int g_failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
                   #condition);                                                \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

enum class TestMode : uint32_t {
  Normal = 0u,
  Throw,
  Block,
};

struct TestRequestPayload {
  uint64_t value = 0u;
  TestMode mode = TestMode::Normal;
};

struct TestResultPayload {
  uint64_t value = 0u;
  size_t workerThreadHash = 0u;
};

std::mutex g_blockMutex;
std::condition_variable g_blockCv;
bool g_blockEntered = false;
bool g_blockRelease = false;

struct TestProcessor {
  TestResultPayload operator()(
      War3PointShadowPrepareRequest<TestRequestPayload>&& request) const {
    if (request.payload.mode == TestMode::Throw)
      throw std::runtime_error("injected point-shadow prepare failure");
    if (request.payload.mode == TestMode::Block) {
      std::unique_lock<std::mutex> lock(g_blockMutex);
      g_blockEntered = true;
      g_blockCv.notify_all();
      g_blockCv.wait(lock, [] { return g_blockRelease; });
    }
    TestResultPayload result;
    result.value = request.payload.value * 2u;
    result.workerThreadHash =
        std::hash<std::thread::id>{}(std::this_thread::get_id());
    return result;
  }
};

using TestWorker = War3PointShadowPersistentPrepareWorker<
    TestRequestPayload, TestResultPayload, TestProcessor>;

struct RecycleRequestPayload {
  std::vector<uint32_t> storage;
  uint32_t marker = 0u;
  TestMode mode = TestMode::Normal;
};

struct RecycleResultPayload {
  std::vector<uint32_t> storage;
  uintptr_t observedAllocation = 0u;
  size_t observedCapacity = 0u;
  uint32_t marker = 0u;
};

struct RecycleProcessor {
  RecycleResultPayload operator()(
      War3PointShadowPrepareRequest<RecycleRequestPayload>&& request) const {
    if (request.payload.mode == TestMode::Block) {
      std::unique_lock<std::mutex> lock(g_blockMutex);
      g_blockEntered = true;
      g_blockCv.notify_all();
      g_blockCv.wait(lock, [] { return g_blockRelease; });
    }
    const uintptr_t observedAllocation =
        reinterpret_cast<uintptr_t>(request.payload.storage.data());
    const size_t observedCapacity = request.payload.storage.capacity();
    const uint32_t marker = request.payload.marker;
    std::vector<uint32_t> storage = std::move(request.payload.storage);
    storage.clear();
    storage.push_back(marker);
    return {std::move(storage), observedAllocation, observedCapacity, marker};
  }
};

using RecycleWorker = War3PointShadowPersistentPrepareWorker<
    RecycleRequestPayload, RecycleResultPayload, RecycleProcessor>;

static_assert(std::is_invocable_r_v<
              TestResultPayload, TestProcessor, TestWorker::Request&&>);
static_assert(!std::is_invocable_v<
              TestProcessor, const TestWorker::Request&>);

War3PointShadowPrepareGenerationTuple Generation(
    uint64_t job, uint64_t renderer = 1u, uint64_t frame = 1u,
    uint64_t light = 1u) {
  return {job, renderer, frame, light};
}

void ResetBlock() {
  std::lock_guard<std::mutex> lock(g_blockMutex);
  g_blockEntered = false;
  g_blockRelease = false;
}

bool WaitForBlockEntry() {
  std::unique_lock<std::mutex> lock(g_blockMutex);
  return g_blockCv.wait_for(lock, std::chrono::seconds(5), [] {
    return g_blockEntered;
  });
}

void ReleaseBlock() {
  std::lock_guard<std::mutex> lock(g_blockMutex);
  g_blockRelease = true;
  g_blockCv.notify_all();
}

void TestGenerationTupleIsExact() {
  CHECK(!Generation(0u).valid());
  CHECK(!Generation(1u, 0u).valid());
  CHECK(!Generation(1u, 1u, 0u).valid());
  CHECK(!Generation(1u, 1u, 1u, 0u).valid());
  CHECK(Generation(1u).valid());
  CHECK(Generation(7u, 2u, 9u, 4u) == Generation(7u, 2u, 9u, 4u));
  CHECK(Generation(7u, 2u, 9u, 4u) != Generation(7u, 2u, 9u, 5u));
}

void TestOneThreadServesSequentialExactJobs() {
  TestWorker worker;
  CHECK(worker.available());
  size_t persistentThreadHash = 0u;
  for (uint64_t job = 1u; job <= 8u; ++job) {
    TestWorker::Request request{Generation(job, 1u, job, 11u),
                                {job, TestMode::Normal}};
    const auto submit = worker.submit(request);
    CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
    if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
      return;
    auto result = worker.waitAndCollectExact(Generation(job, 1u, job, 11u));
    CHECK(result.state == War3PointShadowPrepareResultState::Ready);
    CHECK(!result.failClosed());
    CHECK(result.payload.has_value());
    if (!result.payload.has_value())
      return;
    CHECK(result.payload->value == job * 2u);
    if (persistentThreadHash == 0u)
      persistentThreadHash = result.payload->workerThreadHash;
    CHECK(result.payload->workerThreadHash == persistentThreadHash);
  }
  const auto diagnostics = worker.diagnostics();
  CHECK(diagnostics.threadStarts == 1u);
  CHECK(diagnostics.submittedJobs == 8u);
  CHECK(diagnostics.startedJobs == 8u);
  CHECK(diagnostics.readyJobs == 8u);
  CHECK(diagnostics.exactCollections == 8u);
  CHECK(diagnostics.maximumInFlight == 1u);
}

void TestOwnedVectorCapacityReturnsForNextJob() {
  RecycleWorker worker;
  std::vector<uint32_t> storage;
  storage.reserve(256u);
  for (uint32_t value = 0u; value < 32u; ++value)
    storage.push_back(value);

  const uintptr_t originalAllocation =
      reinterpret_cast<uintptr_t>(storage.data());
  const size_t originalCapacity = storage.capacity();
  CHECK(originalAllocation != 0u);
  CHECK(originalCapacity >= 256u);

  const auto firstGeneration = Generation(1u, 20u, 100u, 30u);
  RecycleWorker::Request firstRequest{
      firstGeneration, {std::move(storage), 0x1234u}};
  CHECK(reinterpret_cast<uintptr_t>(firstRequest.payload.storage.data()) ==
        originalAllocation);
  CHECK(firstRequest.payload.storage.capacity() == originalCapacity);
  auto submit = worker.submit(firstRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;

  auto firstResult = worker.waitAndCollectExact(firstGeneration);
  CHECK(firstResult.state == War3PointShadowPrepareResultState::Ready);
  CHECK(firstResult.payload.has_value());
  if (!firstResult.payload.has_value())
    return;
  CHECK(firstResult.payload->observedAllocation == originalAllocation);
  CHECK(firstResult.payload->observedCapacity == originalCapacity);
  CHECK(reinterpret_cast<uintptr_t>(firstResult.payload->storage.data()) ==
        originalAllocation);
  CHECK(firstResult.payload->storage.capacity() == originalCapacity);
  CHECK(firstResult.payload->storage.size() == 1u);
  CHECK(firstResult.payload->storage.front() == 0x1234u);

  const auto secondGeneration = Generation(2u, 20u, 101u, 30u);
  RecycleWorker::Request secondRequest{
      secondGeneration,
      {std::move(firstResult.payload->storage), 0x5678u}};
  CHECK(reinterpret_cast<uintptr_t>(secondRequest.payload.storage.data()) ==
        originalAllocation);
  CHECK(secondRequest.payload.storage.capacity() == originalCapacity);
  submit = worker.submit(secondRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;

  auto secondResult = worker.waitAndCollectExact(secondGeneration);
  CHECK(secondResult.state == War3PointShadowPrepareResultState::Ready);
  CHECK(secondResult.payload.has_value());
  if (!secondResult.payload.has_value())
    return;
  CHECK(secondResult.payload->observedAllocation == originalAllocation);
  CHECK(secondResult.payload->observedCapacity == originalCapacity);
  CHECK(reinterpret_cast<uintptr_t>(secondResult.payload->storage.data()) ==
        originalAllocation);
  CHECK(secondResult.payload->storage.capacity() == originalCapacity);
  CHECK(secondResult.payload->storage.size() == 1u);
  CHECK(secondResult.payload->storage.front() == 0x5678u);
}

void TestSingleFlightBackpressureAndMonotonicJobs() {
  ResetBlock();
  TestWorker worker;
  const auto firstGeneration = Generation(1u, 2u, 10u, 3u);
  TestWorker::Request firstRequest{
      firstGeneration, {3u, TestMode::Block}};
  auto submit = worker.submit(firstRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  CHECK(WaitForBlockEntry());

  TestWorker::Request busyRequest{
      Generation(2u, 2u, 11u, 3u), {4u, TestMode::Normal}};
  submit = worker.submit(busyRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Busy);
  CHECK(busyRequest.payload.value == 4u);
  ReleaseBlock();
  auto result = worker.waitAndCollectExact(firstGeneration);
  CHECK(result.state == War3PointShadowPrepareResultState::Ready);

  TestWorker::Request staleRequest{
      Generation(1u, 2u, 12u, 3u), {5u, TestMode::Normal}};
  submit = worker.submit(staleRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::StaleGeneration);
  CHECK(staleRequest.payload.value == 5u);
  TestWorker::Request nextEpochRequest{
      Generation(1u, 3u, 1u, 1u), {5u, TestMode::Normal}};
  submit = worker.submit(nextEpochRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  result = worker.waitAndCollectExact(Generation(1u, 3u, 1u, 1u));
  CHECK(result.state == War3PointShadowPrepareResultState::Ready);
  CHECK(worker.diagnostics().busyRejections == 1u);
}

void TestRejectedSubmissionsRetainExactOwnedStorageForSyncFallback() {
  ResetBlock();
  RecycleWorker worker;
  std::vector<uint32_t> blockingStorage = {1u};
  RecycleWorker::Request blockingRequest{
      Generation(10u, 9u, 100u, 5u),
      {std::move(blockingStorage), 1u, TestMode::Block}};
  const auto blockingGeneration = blockingRequest.generation;
  const uintptr_t acceptedAllocation =
      reinterpret_cast<uintptr_t>(blockingRequest.payload.storage.data());
  CHECK(worker.submit(blockingRequest) ==
        War3PointShadowPrepareSubmitStatus::Accepted);
  CHECK(blockingRequest.payload.storage.empty());
  CHECK(reinterpret_cast<uintptr_t>(blockingRequest.payload.storage.data()) !=
        acceptedAllocation);
  CHECK(WaitForBlockEntry());

  auto makeOwnedRequest = [](const War3PointShadowPrepareGenerationTuple& gen,
                             uint32_t marker) {
    std::vector<uint32_t> storage;
    storage.reserve(64u);
    storage.push_back(marker);
    return RecycleWorker::Request{
        gen, {std::move(storage), marker, TestMode::Normal}};
  };
  auto verifySyncOwnership = [](RecycleWorker::Request& request,
                                uintptr_t allocation,
                                size_t capacity,
                                uint32_t marker) {
    CHECK(reinterpret_cast<uintptr_t>(request.payload.storage.data()) ==
          allocation);
    CHECK(request.payload.storage.capacity() == capacity);
    CHECK(request.payload.storage.size() == 1u);
    CHECK(request.payload.storage.front() == marker);
    const auto synchronous = RecycleProcessor{}(std::move(request));
    CHECK(synchronous.observedAllocation == allocation);
    CHECK(synchronous.observedCapacity == capacity);
    CHECK(synchronous.storage.data() ==
          reinterpret_cast<const uint32_t*>(allocation));
    CHECK(synchronous.storage.front() == marker);
  };

  auto invalidRequest =
      makeOwnedRequest(Generation(0u, 9u, 99u, 5u), 0xddddu);
  const uintptr_t invalidAllocation =
      reinterpret_cast<uintptr_t>(invalidRequest.payload.storage.data());
  const size_t invalidCapacity = invalidRequest.payload.storage.capacity();
  CHECK(worker.submit(invalidRequest) ==
        War3PointShadowPrepareSubmitStatus::InvalidGeneration);
  verifySyncOwnership(invalidRequest, invalidAllocation, invalidCapacity,
                      0xddddu);

  auto busyRequest =
      makeOwnedRequest(Generation(11u, 9u, 101u, 5u), 0xaaaau);
  const uintptr_t busyAllocation =
      reinterpret_cast<uintptr_t>(busyRequest.payload.storage.data());
  const size_t busyCapacity = busyRequest.payload.storage.capacity();
  CHECK(worker.submit(busyRequest) ==
        War3PointShadowPrepareSubmitStatus::Busy);
  verifySyncOwnership(busyRequest, busyAllocation, busyCapacity, 0xaaaau);

  ReleaseBlock();
  const auto completed = worker.waitAndCollectExact(blockingGeneration);
  CHECK(completed.state == War3PointShadowPrepareResultState::Ready);

  auto staleRequest =
      makeOwnedRequest(Generation(9u, 9u, 102u, 5u), 0xbbbbu);
  const uintptr_t staleAllocation =
      reinterpret_cast<uintptr_t>(staleRequest.payload.storage.data());
  const size_t staleCapacity = staleRequest.payload.storage.capacity();
  CHECK(worker.submit(staleRequest) ==
        War3PointShadowPrepareSubmitStatus::StaleGeneration);
  verifySyncOwnership(staleRequest, staleAllocation, staleCapacity, 0xbbbbu);

  worker.shutdown();
  auto stoppedRequest =
      makeOwnedRequest(Generation(12u, 9u, 103u, 5u), 0xccccu);
  const uintptr_t stoppedAllocation =
      reinterpret_cast<uintptr_t>(stoppedRequest.payload.storage.data());
  const size_t stoppedCapacity = stoppedRequest.payload.storage.capacity();
  CHECK(worker.submit(stoppedRequest) ==
        War3PointShadowPrepareSubmitStatus::Stopping);
  verifySyncOwnership(stoppedRequest, stoppedAllocation, stoppedCapacity,
                      0xccccu);
}

void TestStaleCollectionDropsPayload() {
  TestWorker worker;
  const auto produced = Generation(1u, 4u, 20u, 8u);
  const auto expected = Generation(1u, 4u, 20u, 9u);
  TestWorker::Request request{produced, {6u, TestMode::Normal}};
  const auto submit = worker.submit(request);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  auto result = worker.waitAndCollectExact(expected);
  CHECK(result.generation == produced);
  CHECK(result.state == War3PointShadowPrepareResultState::Stale);
  CHECK(result.failClosed());
  CHECK(!result.payload.has_value());
  CHECK(!result.failure);
  CHECK(worker.diagnostics().staleCollections == 1u);
}

void TestExceptionFailsClosedAndWorkerSurvives() {
  TestWorker worker;
  const auto failedGeneration = Generation(1u, 5u, 30u, 12u);
  TestWorker::Request failedRequest{
      failedGeneration, {7u, TestMode::Throw}};
  auto submit = worker.submit(failedRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  auto result = worker.waitAndCollectExact(failedGeneration);
  CHECK(result.state == War3PointShadowPrepareResultState::Failed);
  CHECK(result.failClosed());
  CHECK(!result.payload.has_value());
  CHECK(static_cast<bool>(result.failure));

  const auto recoveryGeneration = Generation(2u, 5u, 31u, 12u);
  TestWorker::Request recoveryRequest{
      recoveryGeneration, {9u, TestMode::Normal}};
  submit = worker.submit(recoveryRequest);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  result = worker.waitAndCollectExact(recoveryGeneration);
  CHECK(result.state == War3PointShadowPrepareResultState::Ready);
  CHECK(result.payload.has_value());
  if (result.payload.has_value())
    CHECK(result.payload->value == 18u);
  const auto diagnostics = worker.diagnostics();
  CHECK(diagnostics.failedJobs == 1u);
  CHECK(diagnostics.readyJobs == 1u);
}

void TestShutdownJoinsAndCancelsRunningResult() {
  ResetBlock();
  TestWorker worker;
  const auto generation = Generation(1u, 6u, 40u, 15u);
  TestWorker::Request request{generation, {10u, TestMode::Block}};
  const auto submit = worker.submit(request);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  CHECK(WaitForBlockEntry());

  std::atomic<bool> shutdownReturned{false};
  std::thread shutdownThread([&] {
    worker.shutdown();
    shutdownReturned.store(true, std::memory_order_release);
  });
  const auto stopDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!worker.diagnostics().stopping &&
         std::chrono::steady_clock::now() < stopDeadline) {
    std::this_thread::yield();
  }
  CHECK(worker.diagnostics().stopping);
  CHECK(!shutdownReturned.load(std::memory_order_acquire));
  ReleaseBlock();
  shutdownThread.join();
  CHECK(shutdownReturned.load(std::memory_order_acquire));

  auto result = worker.waitAndCollectExact(generation);
  CHECK(result.state == War3PointShadowPrepareResultState::Cancelled);
  CHECK(result.failClosed());
  CHECK(!result.payload.has_value());
  const auto diagnostics = worker.diagnostics();
  CHECK(diagnostics.threadStarts == 1u);
  CHECK(diagnostics.threadJoins == 1u);
  CHECK(diagnostics.cancelledJobs == 1u);
  CHECK(!diagnostics.available);
  TestWorker::Request stoppedRequest{
      Generation(2u, 6u, 41u, 15u), {1u, TestMode::Normal}};
  CHECK(worker.submit(stoppedRequest) ==
        War3PointShadowPrepareSubmitStatus::Stopping);
}

void TestShutdownRevokesUncollectedReadyResult() {
  TestWorker worker;
  const auto generation = Generation(1u, 7u, 50u, 17u);
  TestWorker::Request request{generation, {12u, TestMode::Normal}};
  const auto submit = worker.submit(request);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!worker.diagnostics().resultOccupied &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  CHECK(worker.diagnostics().resultOccupied);
  worker.shutdown();
  auto result = worker.waitAndCollectExact(generation);
  CHECK(result.state == War3PointShadowPrepareResultState::Cancelled);
  CHECK(result.failClosed());
  CHECK(!result.payload.has_value());
  CHECK(worker.diagnostics().cancelledJobs == 1u);
}

void TestConcurrentShutdownJoinsExactlyOnce() {
  ResetBlock();
  TestWorker worker;
  const auto generation = Generation(1u, 8u, 60u, 19u);
  TestWorker::Request request{generation, {13u, TestMode::Block}};
  const auto submit = worker.submit(request);
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  CHECK(WaitForBlockEntry());

  std::atomic<uint32_t> shutdownReturns{0u};
  std::thread first([&] {
    worker.shutdown();
    shutdownReturns.fetch_add(1u, std::memory_order_release);
  });
  std::thread second([&] {
    worker.shutdown();
    shutdownReturns.fetch_add(1u, std::memory_order_release);
  });

  const auto stopDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!worker.diagnostics().stopping &&
         std::chrono::steady_clock::now() < stopDeadline) {
    std::this_thread::yield();
  }
  CHECK(worker.diagnostics().stopping);
  CHECK(shutdownReturns.load(std::memory_order_acquire) == 0u);
  ReleaseBlock();
  first.join();
  second.join();
  CHECK(shutdownReturns.load(std::memory_order_acquire) == 2u);

  const auto diagnostics = worker.diagnostics();
  CHECK(diagnostics.threadStarts == 1u);
  CHECK(diagnostics.threadJoins == 1u);
  CHECK(diagnostics.cancelledJobs == 1u);
  CHECK(!diagnostics.available);
}

} // namespace

int main() {
  TestGenerationTupleIsExact();
  TestOneThreadServesSequentialExactJobs();
  TestOwnedVectorCapacityReturnsForNextJob();
  TestSingleFlightBackpressureAndMonotonicJobs();
  TestRejectedSubmissionsRetainExactOwnedStorageForSyncFallback();
  TestStaleCollectionDropsPayload();
  TestExceptionFailsClosedAndWorkerSurvives();
  TestShutdownJoinsAndCancelsRunningResult();
  TestShutdownRevokesUncollectedReadyResult();
  TestConcurrentShutdownJoinsExactlyOnce();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d point-shadow worker checks failed\n", g_failures);
    return 1;
  }
  std::puts("war3_point_shadow_prepare_worker_test: PASS");
  return 0;
}
