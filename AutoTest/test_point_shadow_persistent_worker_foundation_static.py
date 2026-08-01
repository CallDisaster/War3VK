"""Static contracts for the isolated point-shadow persistent-worker foundation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (
    ROOT
    / "src/d3d9/war3/render/war3_point_shadow_prepare_worker.h"
)
UNIT = (
    ROOT
    / "src/d3d9/war3/render/tests/war3_point_shadow_prepare_worker_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"
SHADOW_HEADER = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SHADOW_SOURCE = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"


class PointShadowPersistentWorkerFoundationContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.unit = UNIT.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")
        cls.shadow_header = SHADOW_HEADER.read_text(encoding="utf-8")
        cls.shadow_source = SHADOW_SOURCE.read_text(encoding="utf-8")

    def test_exact_generation_tuple_covers_every_owner_boundary(self) -> None:
        for token in (
            "jobSerial",
            "rendererEpoch",
            "frameSerial",
            "lightGeneration",
            "lhs.jobSerial == rhs.jobSerial",
            "lhs.rendererEpoch == rhs.rendererEpoch",
            "lhs.frameSerial == rhs.frameSerial",
            "lhs.lightGeneration == rhs.lightGeneration",
        ):
            self.assertIn(token, self.header)

    def test_mailbox_is_single_flight_with_distinct_request_result_slots(self) -> None:
        for token in (
            "std::optional<Request> requestSlot",
            "std::optional<Result> resultSlot",
            "state->requestSlot.has_value() || state->workerRunning ||",
            "state->resultSlot.has_value()",
            "War3PointShadowPrepareSubmitStatus::Busy",
            "maximumInFlight",
        ):
            self.assertIn(token, self.header)
        self.assertNotIn("std::deque", self.header)

    def test_worker_has_no_renderer_or_gpu_ownership_surface(self) -> None:
        for forbidden in (
            "War3ShadowReceiverPass",
            "D3D9DeviceEx",
            "Rc<",
            "DxvkImage",
            "DxvkCommandList",
            "invalidatePointShadowPublishedState",
            "d3d9_war3_shadow.h",
        ):
            self.assertNotIn(forbidden, self.header)
        self.assertIn("std::is_empty_v<Processor>", self.header)
        self.assertIn("m_thread = std::thread([state]", self.header)
        worker_start = self.header.index("static void workerLoop(")
        worker_end = self.header.index("std::shared_ptr<SharedState> m_state", worker_start)
        self.assertNotIn("this", self.header[worker_start:worker_end])

    def test_processor_consumes_owned_rvalue_and_can_return_storage(self) -> None:
        self.assertIn(
            "std::is_invocable_r_v<ResultPayload, Processor,",
            self.header,
        )
        self.assertIn("RequestEnvelope&&>", self.header)
        self.assertIn(
            "Processor{}(std::move(*request))",
            self.header,
        )
        for token in (
            "TestOwnedVectorCapacityReturnsForNextJob",
            "std::vector<uint32_t> storage",
            "observedAllocation == originalAllocation",
            "observedCapacity == originalCapacity",
            "storage.capacity() == originalCapacity",
        ):
            self.assertIn(token, self.unit)

    def test_exception_stale_and_shutdown_outcomes_are_fail_closed(self) -> None:
        for token in (
            "result.payload.reset()",
            "result.failure = std::current_exception()",
            "War3PointShadowPrepareResultState::Failed",
            "War3PointShadowPrepareResultState::Cancelled",
            "War3PointShadowPrepareResultState::Stale",
            "m_thread.join()",
            "std::terminate()",
        ):
            self.assertIn(token, self.header)
        self.assertIn(
            "state != War3PointShadowPrepareResultState::Ready ||",
            self.header,
        )

    def test_runnable_test_exercises_thread_reuse_backpressure_and_recovery(self) -> None:
        for token in (
            "TestOneThreadServesSequentialExactJobs",
            "TestOwnedVectorCapacityReturnsForNextJob",
            "TestSingleFlightBackpressureAndMonotonicJobs",
            "TestStaleCollectionDropsPayload",
            "TestExceptionFailsClosedAndWorkerSurvives",
            "TestShutdownJoinsAndCancelsRunningResult",
            "TestShutdownRevokesUncollectedReadyResult",
            "TestConcurrentShutdownJoinsExactlyOnce",
            "threadStarts == 1u",
            "War3PointShadowPrepareSubmitStatus::Busy",
        ):
            self.assertIn(token, self.unit)

    def test_shutdown_serializes_the_join_boundary(self) -> None:
        self.assertIn(
            "std::lock_guard<std::mutex> shutdownLock(m_shutdownMutex)",
            self.header,
        )
        self.assertIn("std::mutex m_shutdownMutex", self.header)
        self.assertIn("diagnostics.threadJoins == 1u", self.unit)

    def test_meson_has_an_isolated_runnable_target(self) -> None:
        self.assertIn("war3_point_shadow_prepare_worker_test", self.meson)
        self.assertIn(
            "war3/render/tests/war3_point_shadow_prepare_worker_test.cpp",
            self.meson,
        )
        self.assertIn("war3_point_shadow_prepare_worker_foundation", self.meson)

    def test_existing_async_runtime_path_is_deliberately_untouched(self) -> None:
        include = '#include "war3/render/war3_point_shadow_prepare_worker.h"'
        self.assertNotIn(include, self.shadow_header)
        self.assertNotIn(include, self.shadow_source)
        self.assertIn("std::future<void> m_pointShadowPrepareFuture", self.shadow_header)
        self.assertIn("std::async(", self.shadow_source)


if __name__ == "__main__":
    unittest.main()
