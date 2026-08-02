#!/usr/bin/env python3
"""Static contracts for the isolated CPU-MT skinning ownership model."""

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
        self.assertNotIn("war3_cpu_skin_mt_controller_contract_test.cpp", self.meson)

    def test_owned_output_is_factory_created_and_exactly_bound(self) -> None:
        self.assertIn(
            "std::shared_ptr<const CpuSkinMtControllerOwnedOutput> Create(",
            self.header,
        )
        create = self.source.split("CpuSkinMtControllerOwnedOutput::Create(", 1)[1]
        create = create.split("CpuSkinMtControllerOwnedOutput::CpuSkinMtControllerOwnedOutput(", 1)[0]
        for token in (
            "CpuSkinMtEligibilityMatchesExactKey(exactKey, eligibility)",
            "bytes.size() != exactKey.destination.size",
            "std::shared_ptr<const CpuSkinMtControllerOwnedOutput>",
        ):
            with self.subTest(token=token):
                self.assertIn(token, create)
        proof = self.source.split("::proofComplete() const noexcept", 1)[1]
        proof = proof.split("CpuSkinMtControllerTerminalLedgerSnapshot::terminalJobs", 1)[0]
        for token in (
            "CpuSkinMtControllerExactKeyComplete(m_proof.exactKey)",
            "m_proof.ownedBytesIdentity",
            "m_proof.outputByteSize != m_proof.exactKey.destination.size",
            "HashBytes(m_bytes.data(), m_bytes.size())",
        ):
            with self.subTest(token=token):
                self.assertIn(token, proof)

    def test_kernel_decision_is_nonblocking_and_single_route(self) -> None:
        route = self.source.split("::trySelectKernelRoute(", 1)[1]
        route = route.split("::copyOutputUnderLease(", 1)[0]
        self.assertIn("std::try_to_lock", route)
        self.assertIn("StateLockContended", route)
        self.assertIn("selectNativeWithoutWaiting(", route)
        self.assertIn("m_routeAuthority.compare_exchange_strong(", route)
        self.assertNotIn("std::lock_guard", route)
        selector = self.source.split("::selectNativeWithoutWaiting(", 1)[1]
        selector = selector.split("::rejectStaleLocked(", 1)[0]
        self.assertIn("CpuSkinMtControllerKernelRoute::Native", selector)
        self.assertIn("compare_exchange_strong", selector)

    def test_guarded_copy_and_unlock_form_one_linearization_window(self) -> None:
        copy = self.source.split("::copyOutputUnderLease(", 1)[1]
        copy = copy.split("::completeOriginalNormal(", 1)[0]
        for token in (
            "std::lock_guard<std::mutex> lock(m_mutex)",
            "if (m_unlockObserved)",
            "m_output->proof().exactKey != m_key",
            "m_bodySucceeded = callback(context, view, m_key.destination)",
            "m_state = CpuSkinMtControllerState::CopyAwaitingUnlock",
        ):
            with self.subTest(token=token):
                self.assertIn(token, copy)
        unlock = self.source.split("::noteUnlock(", 1)[1]
        unlock = unlock.split("::cancelTemplateMismatch(", 1)[0]
        self.assertIn("CpuSkinMtControllerUnlockMatchesDestination", unlock)
        self.assertIn("m_unlockObserved = true", unlock)
        self.assertGreaterEqual(unlock.count("finalizeRouteAfterUnlockLocked()"), 2)

    def test_native_and_copy_only_terminalize_after_unlock_result(self) -> None:
        finalize = self.source.split("::finalizeRouteAfterUnlockLocked()", 1)[1]
        finalize = finalize.split("::isTerminalLocked()", 1)[0]
        for token in (
            "if (!m_unlockObserved || !m_bodyCompleted)",
            "CpuSkinMtControllerTerminal::CopiedNormal",
            "CpuSkinMtControllerTerminal::CopyFault",
            "CpuSkinMtControllerTerminal::OriginalNormal",
            "CpuSkinMtControllerTerminal::OriginalFault",
        ):
            with self.subTest(token=token):
                self.assertIn(token, finalize)

    def test_terminal_ledger_has_a_closed_accounting_identity(self) -> None:
        self.assertIn("uint64_t terminalJobs() const noexcept", self.header)
        self.assertIn("bool closureHolds() const noexcept", self.header)
        closure = self.source.split("::closureHolds()", 1)[1]
        closure = closure.split("CpuSkinMtControllerTerminalLedger::snapshot", 1)[0]
        self.assertIn("jobsCreated == liveJobs + terminalJobs()", closure)

    def test_runnable_covers_required_races_and_failure_edges(self) -> None:
        for token in (
            "TestOwnedOutputLifetimeAndUnlockCommit",
            "TestKernelWindowContentionFallsBackImmediately",
            "TestCopyFaultsAreUnlockFinalized",
            "TestNativeBodyVersusUnlockRace",
            "TestUnlockBeforeCopyRejectsAllWrites",
            "TestGuardedCopyLinearizesBeforeResetOrCancel",
            "TestClaimVersusNativeRace",
            "TestProducerVersusNativeRaceAndLateOutput",
            "TestKeyMismatchResetAndDestructorClosure",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.runnable)


if __name__ == "__main__":
    unittest.main()
