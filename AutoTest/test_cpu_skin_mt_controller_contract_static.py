#!/usr/bin/env python3
"""Static contracts for the isolated CPU-MT Phase 2A ownership model."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/gpu_skin/war3_cpu_skin_mt_controller_contract.h"
SOURCE = ROOT / "src/d3d9/war3/gpu_skin/war3_cpu_skin_mt_controller_contract.cpp"
RUNNABLE = (
    ROOT
    / "src/d3d9/war3/gpu_skin/tests/war3_cpu_skin_mt_controller_contract_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"


class CpuSkinMtControllerContractStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.runnable = RUNNABLE.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_value_contract_is_not_a_production_route(self) -> None:
        for gate in (
            "kCpuSkinMtControllerRuntimeIntegrated = false",
            "kCpuSkinMtControllerNativeParityProven = false",
            "kCpuSkinMtControllerConsumeEnabled = false",
            "kCpuSkinMtControllerProductionDefault = false",
        ):
            with self.subTest(gate=gate):
                self.assertIn(gate, self.header)
        self.assertNotIn("war3_cpu_skin_mt_controller_contract.cpp", self.meson)
        self.assertNotIn(
            "war3_cpu_skin_mt_controller_contract_test.cpp", self.meson
        )

    def test_producer_result_is_destination_free(self) -> None:
        self.assertNotIn("CpuSkinMtControllerExactKey", self.header)
        self.assertNotIn("CpuSkinMtControllerOwnedOutput", self.header)
        self.assertIn("CpuSkinMtControllerProducerResultProof", self.header)
        self.assertIn("CpuSkinMtControllerFrozenKey frozenKey;", self.header)
        create = self.source.split(
            "CpuSkinMtControllerOwnedProducerResult::Create(", 1
        )[1]
        create = create.split(
            "CpuSkinMtControllerOwnedProducerResult::\n"
            "CpuSkinMtControllerOwnedProducerResult(",
            1,
        )[0]
        for token in (
            "CpuSkinMtEligibilityMatchesFrozenKey(frozenKey, eligibility)",
            "ExpectedOutputBytes(frozenKey)",
            "CpuSkinMtCaptureMxcsrStatusDelta(",
        ):
            with self.subTest(token=token):
                self.assertIn(token, create)
        self.assertNotIn("destination", create)

    def test_render_owner_mints_lock_time_commit_envelope(self) -> None:
        envelope_class = self.header.split(
            "class CpuSkinMtControllerRenderCommitEnvelope final", 1
        )[1].split("class CpuSkinMtControllerNativeBodyLease", 1)[0]
        self.assertIn("private:", envelope_class)
        self.assertIn("MintAfterLock(", envelope_class)
        self.assertIn("friend class CpuSkinMtControllerJob", envelope_class)
        route = self.source.split("::trySelectKernelRoute(", 1)[1]
        route = route.split("::copyProducerResultUnderLease(", 1)[0]
        self.assertIn("m_state != CpuSkinMtControllerState::AwaitingKernel", route)
        self.assertIn(
            "CpuSkinMtControllerRenderCommitEnvelope::MintAfterLock(", route
        )
        self.assertIn("m_destination.lockSerial", route)
        self.assertIn("m_commitEnvelope = std::move(envelope)", route)
        complete = self.source.split(
            "CpuSkinMtControllerRenderCommitEnvelopeComplete(", 1
        )[1].split(
            "CpuSkinMtControllerRenderCommitEnvelope\n"
            "CpuSkinMtControllerRenderCommitEnvelope::MintAfterLock(",
            1,
        )[0]
        self.assertIn("CpuSkinMtControllerDestinationValid(", complete)
        self.assertIn(
            "proof.producerResult.outputByteSize == proof.destination.size",
            complete,
        )

    def test_publication_and_consumption_are_distinct_events(self) -> None:
        publish = self.source.split("::publishProducerResult(", 1)[1]
        publish = publish.split("::bindOuter(", 1)[0]
        self.assertIn("CpuSkinMtControllerResult::Published", publish)
        self.assertIn("producerResultPublications", publish)
        self.assertIn("liveProducerResults", publish)
        self.assertNotIn("producerResultConsumptions", publish)
        copy = self.source.split("::copyProducerResultUnderLease(", 1)[1]
        copy = copy.split("::completeNativeBody(", 1)[0]
        self.assertIn("settleProducerConsumedLocked()", copy)
        self.assertIn("CpuSkinMtControllerResult::Consumed", copy)
        self.assertIn("m_producerConsumed = true", self.source)

    def test_native_route_issues_exact_body_lease(self) -> None:
        lease = self.header.split(
            "class CpuSkinMtControllerNativeBodyLease final", 1
        )[1].split("struct CpuSkinMtControllerKernelDecision", 1)[0]
        self.assertIn("private:", lease)
        self.assertIn("friend class CpuSkinMtControllerJob", lease)
        self.assertIn("m_lockSerial", lease)
        selector = self.source.split("::selectNativeWithoutWaiting(", 1)[1]
        selector = selector.split("::rejectStaleLocked", 1)[0]
        self.assertIn(
            "compare_exchange_strong(\n"
            "          expected, CpuSkinMtControllerKernelRoute::Native",
            selector,
        )
        self.assertIn("makeNativeBodyLease()", selector)
        self.assertIn("nativeBodyLeases", selector)
        complete = self.source.split("::completeNativeBody(", 1)[1]
        complete = complete.split("::noteUnlock(", 1)[0]
        self.assertIn("nativeBodyLeaseMatchesLocked(lease)", complete)
        self.assertIn("InvalidNativeLease", complete)
        self.assertIn("nativeBodiesCompleted", complete)

    def test_reset_and_cancel_defer_while_outer_body_is_unsettled(self) -> None:
        defer = self.source.split("::shouldDeferCancellationLocked()", 1)[1]
        defer = defer.split("::queueDeferredCancellationLocked(", 1)[0]
        self.assertIn(
            "m_route == CpuSkinMtControllerKernelRoute::Native", defer
        )
        self.assertIn("!m_bodyCompleted", defer)
        self.assertIn("!m_unlockObserved", defer)
        queue = self.source.split("::queueDeferredCancellationLocked(", 1)[1]
        queue = queue.split("::finishLocked(", 1)[0]
        self.assertIn("m_deferredTerminal = terminal", queue)
        self.assertIn("CpuSkinMtControllerResult::Deferred", queue)
        finalize = self.source.split("::finalizeRouteAfterUnlockLocked()", 1)[1]
        finalize = finalize.split("::submit(", 1)[0]
        self.assertIn("if (!m_unlockObserved || !m_bodyCompleted)", finalize)
        self.assertIn(
            "m_deferredTerminal != CpuSkinMtControllerTerminal::None",
            finalize,
        )

    def test_outer_settlement_and_terminal_ledger_close(self) -> None:
        closure = self.source.split("::closureHolds()", 1)[1]
        closure = closure.split(
            "CpuSkinMtControllerTerminalLedger::snapshot", 1
        )[0]
        for token in (
            "jobsCreated == liveJobs + terminalJobs()",
            "producerResultPublications == liveProducerResults",
            "producerResultClaims == liveProducerClaims",
            "nativeBodyLeases == liveNativeBodyLeases",
            "outerSettlementsOpened == liveOuterSettlements",
        ):
            with self.subTest(token=token):
                self.assertIn(token, closure)
        finish = self.source.split("::finishLocked(", 1)[1]
        finish = finish.split("::finalizeRouteAfterUnlockLocked()", 1)[0]
        self.assertIn("outerSettlementsCompleted", finish)
        self.assertIn("nativeBodyLeasesAbandoned", finish)

    def test_mxcsr_control_and_status_delta_are_separate_proofs(self) -> None:
        self.assertIn("kCpuSkinMtMxcsrStatusMask = 0x003fu", self.header)
        self.assertIn("CpuSkinMtControllerMxcsrStatusDelta", self.header)
        capture = self.source.split("CpuSkinMtCaptureMxcsrStatusDelta(", 1)[1]
        capture = capture.split("CpuSkinMtControllerFrozenKeyValid", 1)[0]
        for token in (
            "result.statusBefore",
            "result.statusAfter",
            "result.raisedStatus",
            "result.clearedStatus",
            "beforeControl == afterControl",
        ):
            with self.subTest(token=token):
                self.assertIn(token, capture)
        copy = self.source.split("::copyProducerResultUnderLease(", 1)[1]
        copy = copy.split("::completeNativeBody(", 1)[0]
        self.assertIn(
            "m_producerResult->proof().mxcsrStatusDelta", copy
        )
        self.assertIn("mxcsrStatusDeltaMismatches", copy)

    def test_runnable_covers_phase2a_races_and_failure_edges(self) -> None:
        for token in (
            "TestDestinationFreeProducerResultProof",
            "TestPublicationIsNotConsumption",
            "TestCopyCommitEnvelopeAndOuterSettlement",
            "TestCopyMxcsrStatusDeltaMismatchFailsClosed",
            "TestNativeLeaseDefersResetThroughOuterSettlement",
            "TestNativeLeaseRejectsForgeryAndDuplicateOwner",
            "TestNativeBodyUnlockRace",
            "TestNativeResetBodyRaceDefers",
            "TestCopyBodyResetRaceDefers",
            "TestStateLockContentionIssuesNativeLease",
            "TestProducerPublicationVersusNativeRace",
            "TestSingleRouteClaimRace",
            "TestDestructorClosesOutstandingNativeLease",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.runnable)


if __name__ == "__main__":
    unittest.main()
