#include "../war3_point_shadow_prepare_worker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

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
      const War3PointShadowPrepareRequest<TestRequestPayload>& request) const {
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
    const auto submit = worker.submit(std::move(request));
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

void TestSingleFlightBackpressureAndMonotonicJobs() {
  ResetBlock();
  TestWorker worker;
  const auto firstGeneration = Generation(1u, 2u, 10u, 3u);
  auto submit = worker.submit(
      {firstGeneration, {3u, TestMode::Block}});
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  CHECK(WaitForBlockEntry());

  submit = worker.submit(
      {Generation(2u, 2u, 11u, 3u), {4u, TestMode::Normal}});
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Busy);
  ReleaseBlock();
  auto result = worker.waitAndCollectExact(firstGeneration);
  CHECK(result.state == War3PointShadowPrepareResultState::Ready);

  submit = worker.submit(
      {Generation(1u, 2u, 12u, 3u), {5u, TestMode::Normal}});
  CHECK(submit == War3PointShadowPrepareSubmitStatus::StaleGeneration);
  submit = worker.submit(
      {Generation(1u, 3u, 1u, 1u), {5u, TestMode::Normal}});
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  result = worker.waitAndCollectExact(Generation(1u, 3u, 1u, 1u));
  CHECK(result.state == War3PointShadowPrepareResultState::Ready);
  CHECK(worker.diagnostics().busyRejections == 1u);
}

void TestStaleCollectionDropsPayload() {
  TestWorker worker;
  const auto produced = Generation(1u, 4u, 20u, 8u);
  const auto expected = Generation(1u, 4u, 20u, 9u);
  const auto submit = worker.submit(
      {produced, {6u, TestMode::Normal}});
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
  auto submit = worker.submit(
      {failedGeneration, {7u, TestMode::Throw}});
  CHECK(submit == War3PointShadowPrepareSubmitStatus::Accepted);
  if (submit != War3PointShadowPrepareSubmitStatus::Accepted)
    return;
  auto result = worker.waitAndCollectExact(failedGeneration);
  CHECK(result.state == War3PointShadowPrepareResultState::Failed);
  CHECK(result.failClosed());
  CHECK(!result.payload.has_value());
  CHECK(static_cast<bool>(result.failure));

  const auto recoveryGeneration = Generation(2u, 5u, 31u, 12u);
  submit = worker.submit(
      {recoveryGeneration, {9u, TestMode::Normal}});
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
  const auto submit = worker.submit(
      {generation, {10u, TestMode::Block}});
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
  CHECK(worker.submit({Generation(2u, 6u, 41u, 15u),
                       {1u, TestMode::Normal}}) ==
        War3PointShadowPrepareSubmitStatus::Stopping);
}

void TestShutdownRevokesUncollectedReadyResult() {
  TestWorker worker;
  const auto generation = Generation(1u, 7u, 50u, 17u);
  const auto submit = worker.submit(
      {generation, {12u, TestMode::Normal}});
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
  const auto submit = worker.submit(
      {generation, {13u, TestMode::Block}});
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
  TestSingleFlightBackpressureAndMonotonicJobs();
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
