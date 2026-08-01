#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace dxvk::war3::render {

/**
 * \brief Exact identity of one point-shadow CPU preparation request
 *
 * Every field is required. A result may be consumed only when the complete
 * tuple matches the renderer's current expectation; matching only a frame or
 * light generation is intentionally insufficient.
 */
struct War3PointShadowPrepareGenerationTuple {
  uint64_t jobSerial = 0u;
  uint64_t rendererEpoch = 0u;
  uint64_t frameSerial = 0u;
  uint64_t lightGeneration = 0u;

  constexpr bool valid() const noexcept {
    return jobSerial != 0u && rendererEpoch != 0u && frameSerial != 0u &&
           lightGeneration != 0u;
  }
};

static_assert(
    std::is_trivially_copyable_v<War3PointShadowPrepareGenerationTuple>,
    "generation identity must remain a plain by-value tuple");

constexpr bool operator==(
    const War3PointShadowPrepareGenerationTuple& lhs,
    const War3PointShadowPrepareGenerationTuple& rhs) noexcept {
  return lhs.jobSerial == rhs.jobSerial &&
         lhs.rendererEpoch == rhs.rendererEpoch &&
         lhs.frameSerial == rhs.frameSerial &&
         lhs.lightGeneration == rhs.lightGeneration;
}

constexpr bool operator!=(
    const War3PointShadowPrepareGenerationTuple& lhs,
    const War3PointShadowPrepareGenerationTuple& rhs) noexcept {
  return !(lhs == rhs);
}

enum class War3PointShadowPrepareSubmitStatus : uint8_t {
  Accepted = 0u,
  InvalidGeneration,
  StaleGeneration,
  Busy,
  Stopping,
  Unavailable,
};

enum class War3PointShadowPrepareResultState : uint8_t {
  Empty = 0u,
  NotReady,
  Ready,
  Failed,
  Cancelled,
  Stale,
  InvalidGeneration,
  Stopping,
  Unavailable,
};

/**
 * \brief Owned request envelope transferred into the worker request slot
 *
 * Payload must own or pin every CPU byte it exposes. It must not contain a
 * renderer pointer, a Vulkan/DXVK resource owner, or mutable publication state.
 */
template <typename Payload>
struct War3PointShadowPrepareRequest {
  War3PointShadowPrepareGenerationTuple generation = {};
  Payload payload;
};

/**
 * \brief Completed result envelope transferred into the owner result slot
 *
 * Failed, cancelled, and stale results deliberately contain no payload. The
 * owner can therefore treat every non-ready result as fail-closed without
 * accidentally consuming a partially constructed CPU plan.
 */
template <typename Payload>
struct War3PointShadowPrepareResult {
  War3PointShadowPrepareGenerationTuple generation = {};
  War3PointShadowPrepareResultState state =
      War3PointShadowPrepareResultState::Empty;
  std::optional<Payload> payload;
  std::exception_ptr failure;

  bool failClosed() const noexcept {
    return state != War3PointShadowPrepareResultState::Ready ||
           !payload.has_value();
  }
};

struct War3PointShadowPrepareWorkerDiagnostics {
  uint64_t threadStarts = 0u;
  uint64_t threadJoins = 0u;
  uint64_t submittedJobs = 0u;
  uint64_t startedJobs = 0u;
  uint64_t readyJobs = 0u;
  uint64_t failedJobs = 0u;
  uint64_t cancelledJobs = 0u;
  uint64_t exactCollections = 0u;
  uint64_t staleCollections = 0u;
  uint64_t busyRejections = 0u;
  uint32_t maximumInFlight = 0u;
  bool available = false;
  bool stopping = false;
  bool requestOccupied = false;
  bool workerRunning = false;
  bool resultOccupied = false;
};

/**
 * \brief Single-worker, single-flight request/result mailbox
 *
 * Processor must be an empty, default-constructible function object. This is a
 * deliberate ownership boundary: the persistent thread stores no captured
 * renderer object. A future runtime integration must freeze all CPU inputs into
 * RequestPayload and return all proposed state changes through ResultPayload.
 *
 * There is exactly one request slot and one result slot. A new request is
 * rejected until the prior result has been collected, so neither a pending nor
 * a completed generation can be overwritten.
 */
template <typename RequestPayload, typename ResultPayload, typename Processor>
class War3PointShadowPersistentPrepareWorker final {
  using RequestEnvelope =
      War3PointShadowPrepareRequest<RequestPayload>;

  static_assert(!std::is_pointer_v<RequestPayload> &&
                    !std::is_reference_v<RequestPayload>,
                "request payload must be an owned CPU value");
  static_assert(!std::is_pointer_v<ResultPayload> &&
                    !std::is_reference_v<ResultPayload>,
                "result payload must be an owned CPU value");
  static_assert(std::is_nothrow_move_constructible_v<RequestPayload>,
                "request payload must transfer without throwing");
  static_assert(std::is_nothrow_move_constructible_v<ResultPayload>,
                "result payload must transfer without throwing");
  static_assert(std::is_nothrow_destructible_v<RequestPayload>,
                "request payload teardown must not escape the worker");
  static_assert(std::is_nothrow_destructible_v<ResultPayload>,
                "result payload teardown must not escape the worker");
  static_assert(std::is_empty_v<Processor>,
                "processor must be stateless and may not capture renderer state");
  static_assert(std::is_default_constructible_v<Processor>,
                "processor must be constructed inside the worker");
  static_assert(
      std::is_invocable_r_v<ResultPayload, Processor,
                            const RequestEnvelope&>,
      "processor must map an owned request to an owned result");

public:
  using Request = RequestEnvelope;
  using Result = War3PointShadowPrepareResult<ResultPayload>;

  War3PointShadowPersistentPrepareWorker()
      : m_state(std::make_shared<SharedState>()) {
    try {
      const std::shared_ptr<SharedState> state = m_state;
      {
        // The object is not externally visible during construction. Publish
        // availability before launching so an immediately failing worker can
        // revoke it without the constructor later overwriting that failure.
        std::lock_guard<std::mutex> lock(state->mutex);
        state->available = true;
      }
      m_thread = std::thread([state] { workerLoop(state); });
      std::lock_guard<std::mutex> lock(state->mutex);
      state->diagnostics.threadStarts = 1u;
    } catch (...) {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      m_state->available = false;
      m_state->stopping = true;
      m_state->diagnostics.threadStarts = 0u;
      m_state->startupFailure = std::current_exception();
    }
  }

  ~War3PointShadowPersistentPrepareWorker() {
    shutdown();
  }

  War3PointShadowPersistentPrepareWorker(
      const War3PointShadowPersistentPrepareWorker&) = delete;
  War3PointShadowPersistentPrepareWorker& operator=(
      const War3PointShadowPersistentPrepareWorker&) = delete;
  War3PointShadowPersistentPrepareWorker(
      War3PointShadowPersistentPrepareWorker&&) = delete;
  War3PointShadowPersistentPrepareWorker& operator=(
      War3PointShadowPersistentPrepareWorker&&) = delete;

  War3PointShadowPrepareSubmitStatus submit(Request request) noexcept {
    if (!request.generation.valid())
      return War3PointShadowPrepareSubmitStatus::InvalidGeneration;

    const std::shared_ptr<SharedState> state = m_state;
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->available && state->startupFailure &&
        state->diagnostics.threadStarts == 0u)
      return War3PointShadowPrepareSubmitStatus::Unavailable;
    if (state->stopping)
      return War3PointShadowPrepareSubmitStatus::Stopping;
    if (!state->available)
      return War3PointShadowPrepareSubmitStatus::Unavailable;
    if (isStaleSubmission(state->lastSubmitted, request.generation))
      return War3PointShadowPrepareSubmitStatus::StaleGeneration;
    if (state->requestSlot.has_value() || state->workerRunning ||
        state->resultSlot.has_value()) {
      ++state->diagnostics.busyRejections;
      return War3PointShadowPrepareSubmitStatus::Busy;
    }

    state->lastSubmitted = request.generation;
    state->requestSlot.emplace(std::move(request));
    ++state->diagnostics.submittedJobs;
    state->diagnostics.maximumInFlight =
        std::max(state->diagnostics.maximumInFlight, 1u);
    lock.unlock();
    state->workAvailable.notify_one();
    return War3PointShadowPrepareSubmitStatus::Accepted;
  }

  Result tryCollectExact(
      const War3PointShadowPrepareGenerationTuple& expected) noexcept {
    if (!expected.valid())
      return makeStateResult(
          expected, War3PointShadowPrepareResultState::InvalidGeneration);

    const std::shared_ptr<SharedState> state = m_state;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->resultSlot.has_value())
      return makeStateResult(
          expected, War3PointShadowPrepareResultState::NotReady);
    return consumeResultLocked(*state, expected);
  }

  Result waitAndCollectExact(
      const War3PointShadowPrepareGenerationTuple& expected) noexcept {
    if (!expected.valid())
      return makeStateResult(
          expected, War3PointShadowPrepareResultState::InvalidGeneration);

    const std::shared_ptr<SharedState> state = m_state;
    std::unique_lock<std::mutex> lock(state->mutex);
    state->resultAvailable.wait(lock, [&] {
      return state->resultSlot.has_value() ||
             (!state->available && !state->workerRunning &&
              !state->requestSlot.has_value());
    });
    if (state->resultSlot.has_value())
      return consumeResultLocked(*state, expected);
    return makeStateResult(
        expected,
        state->startupFailure
            ? War3PointShadowPrepareResultState::Unavailable
            : War3PointShadowPrepareResultState::Stopping);
  }

  void shutdown() noexcept {
    // shutdown() is intentionally idempotent and may be requested by more
    // than one owner-side teardown path. std::thread::joinable()/join() are
    // not safe to race, so serialize the complete stop-and-join sequence.
    std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex);
    const std::shared_ptr<SharedState> state = m_state;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->stopping) {
        state->stopping = true;
        cancelPendingRequestLocked(*state);
        cancelCompletedResultLocked(*state);
      }
    }
    state->workAvailable.notify_all();
    state->resultAvailable.notify_all();

    if (m_thread.joinable()) {
      try {
        m_thread.join();
      } catch (...) {
        // A joinable std::thread reaching its destructor terminates anyway.
        // Terminating here makes the failed lifetime proof explicit and avoids
        // silently detaching a worker that may still own request bytes.
        std::terminate();
      }
      std::lock_guard<std::mutex> lock(state->mutex);
      state->diagnostics.threadJoins = 1u;
    }

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->available = false;
    }
    state->resultAvailable.notify_all();
  }

  bool available() const noexcept {
    const std::shared_ptr<SharedState> state = m_state;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->available && !state->stopping;
  }

  std::exception_ptr startupFailure() const noexcept {
    const std::shared_ptr<SharedState> state = m_state;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->startupFailure;
  }

  War3PointShadowPrepareWorkerDiagnostics diagnostics() const noexcept {
    const std::shared_ptr<SharedState> state = m_state;
    std::lock_guard<std::mutex> lock(state->mutex);
    War3PointShadowPrepareWorkerDiagnostics result = state->diagnostics;
    result.available = state->available;
    result.stopping = state->stopping;
    result.requestOccupied = state->requestSlot.has_value();
    result.workerRunning = state->workerRunning;
    result.resultOccupied = state->resultSlot.has_value();
    return result;
  }

private:
  struct SharedState {
    std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable resultAvailable;
    bool available = false;
    bool stopping = false;
    bool workerRunning = false;
    std::optional<Request> requestSlot;
    std::optional<Result> resultSlot;
    War3PointShadowPrepareGenerationTuple lastSubmitted = {};
    std::exception_ptr startupFailure;
    War3PointShadowPrepareWorkerDiagnostics diagnostics = {};
  };

  static bool isStaleSubmission(
      const War3PointShadowPrepareGenerationTuple& previous,
      const War3PointShadowPrepareGenerationTuple& candidate) noexcept {
    if (!previous.valid())
      return false;
    if (candidate.rendererEpoch != previous.rendererEpoch)
      return candidate.rendererEpoch < previous.rendererEpoch;
    return candidate.jobSerial <= previous.jobSerial;
  }

  static Result makeStateResult(
      const War3PointShadowPrepareGenerationTuple& generation,
      War3PointShadowPrepareResultState resultState) noexcept {
    Result result;
    result.generation = generation;
    result.state = resultState;
    return result;
  }

  static Result consumeResultLocked(
      SharedState& state,
      const War3PointShadowPrepareGenerationTuple& expected) noexcept {
    Result result = std::move(*state.resultSlot);
    state.resultSlot.reset();
    if (result.generation != expected) {
      result.state = War3PointShadowPrepareResultState::Stale;
      result.payload.reset();
      result.failure = {};
      ++state.diagnostics.staleCollections;
      return result;
    }
    ++state.diagnostics.exactCollections;
    return result;
  }

  static void cancelPendingRequestLocked(SharedState& state) noexcept {
    if (!state.requestSlot.has_value())
      return;
    const auto generation = state.requestSlot->generation;
    state.requestSlot.reset();
    state.resultSlot.emplace(makeStateResult(
        generation, War3PointShadowPrepareResultState::Cancelled));
    ++state.diagnostics.cancelledJobs;
  }

  static void cancelCompletedResultLocked(SharedState& state) noexcept {
    if (!state.resultSlot.has_value() || state.workerRunning)
      return;
    Result& result = *state.resultSlot;
    if (result.state == War3PointShadowPrepareResultState::Cancelled)
      return;
    result.payload.reset();
    result.failure = {};
    result.state = War3PointShadowPrepareResultState::Cancelled;
    ++state.diagnostics.cancelledJobs;
  }

  static void workerLoop(const std::shared_ptr<SharedState>& state) noexcept {
    std::optional<War3PointShadowPrepareGenerationTuple> activeGeneration;
    try {
      for (;;) {
        std::optional<Request> request;
        {
          std::unique_lock<std::mutex> lock(state->mutex);
          state->workAvailable.wait(lock, [&] {
            return state->stopping || state->requestSlot.has_value();
          });
          if (state->stopping && !state->requestSlot.has_value())
            break;
          request.emplace(std::move(*state->requestSlot));
          state->requestSlot.reset();
          state->workerRunning = true;
          activeGeneration = request->generation;
          ++state->diagnostics.startedJobs;
        }

        Result result;
        result.generation = request->generation;
        try {
          result.payload.emplace(Processor{}(*request));
          result.state = War3PointShadowPrepareResultState::Ready;
        } catch (...) {
          result.payload.reset();
          result.failure = std::current_exception();
          result.state = War3PointShadowPrepareResultState::Failed;
        }
        request.reset();

        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->workerRunning = false;
          activeGeneration.reset();
          if (state->stopping) {
            result.payload.reset();
            result.failure = {};
            result.state = War3PointShadowPrepareResultState::Cancelled;
            ++state->diagnostics.cancelledJobs;
          } else if (result.state ==
                     War3PointShadowPrepareResultState::Ready) {
            ++state->diagnostics.readyJobs;
          } else {
            ++state->diagnostics.failedJobs;
          }
          state->resultSlot.emplace(std::move(result));
        }
        state->resultAvailable.notify_all();
      }
    } catch (...) {
      const std::exception_ptr failure = std::current_exception();
      std::lock_guard<std::mutex> lock(state->mutex);
      state->available = false;
      state->stopping = true;
      state->workerRunning = false;
      if (activeGeneration.has_value() && !state->resultSlot.has_value()) {
        Result result = makeStateResult(
            *activeGeneration, War3PointShadowPrepareResultState::Failed);
        result.failure = failure;
        state->resultSlot.emplace(std::move(result));
        ++state->diagnostics.failedJobs;
      } else if (state->requestSlot.has_value() &&
                 !state->resultSlot.has_value()) {
        Result result = makeStateResult(
            state->requestSlot->generation,
            War3PointShadowPrepareResultState::Failed);
        result.failure = failure;
        state->requestSlot.reset();
        state->resultSlot.emplace(std::move(result));
        ++state->diagnostics.failedJobs;
      }
      state->startupFailure = failure;
      state->resultAvailable.notify_all();
    }
  }

  std::shared_ptr<SharedState> m_state;
  std::mutex m_shutdownMutex;
  std::thread m_thread;
};

} // namespace dxvk::war3::render
