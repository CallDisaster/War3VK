"""Point-only admission and structured diagnostics contracts."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHADOW_H = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
DIAG_H = ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h"
DIAG_CPP = ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"
PERF_H = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h"
PERF_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"


class PointShadowPersistentPointOnlyContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shadow_h = SHADOW_H.read_text(encoding="utf-8")
        cls.shadow_cpp = SHADOW_CPP.read_text(encoding="utf-8")
        cls.diag_h = DIAG_H.read_text(encoding="utf-8")
        cls.diag_cpp = DIAG_CPP.read_text(encoding="utf-8")
        cls.perf_h = PERF_H.read_text(encoding="utf-8")
        cls.perf_cpp = PERF_CPP.read_text(encoding="utf-8")

    def test_persistent_admission_precedes_csm_only_branch(self) -> None:
        run = self.shadow_cpp.split(
            "void War3ShadowReceiverPass::Run", 1
        )[1].split("War3ShadowDataPool Implementation", 1)[0]
        admission = run.index("const bool allowPointShadowPrepare")
        persistent_call = run.index(
            "beginPointShadowCpuPrepare(input, pointLightSnapshot", admission
        )
        csm_only = run.index("if (receiverNeedsShadowMap)", admission)
        self.assertLess(persistent_call, csm_only)
        pre_csm = run[admission:csm_only]
        self.assertIn(
            "pointShadowPersistentConfiguredMode !=", pre_csm
        )
        self.assertIn("War3PointShadowPersistentMode::Off", pre_csm)
        self.assertIn("allowPointShadowPersistentPrepare", pre_csm)
        self.assertIn("pointLightSnapshot.shadowCount > 0u", pre_csm)

    def test_default_off_keeps_legacy_async_admission_inside_csm(self) -> None:
        run = self.shadow_cpp.split(
            "void War3ShadowReceiverPass::Run", 1
        )[1].split("War3ShadowDataPool Implementation", 1)[0]
        admission = run.index("const bool allowPointShadowPrepare")
        csm_only = run.index("if (receiverNeedsShadowMap)", admission)
        legacy = run.index("A2 legacy route", csm_only)
        legacy_block = run[legacy : run.index("if (reuseLastShadowMap)", legacy)]
        self.assertIn("allowPointShadowPrepare", legacy_block)
        self.assertNotIn("allowPointShadowPersistentPrepare", legacy_block)
        self.assertIn("pointShadowPersistentConfiguredMode ==", legacy_block)
        self.assertIn("War3PointShadowPersistentMode::Off", legacy_block)
        self.assertIn("beginPointShadowCpuPrepare", legacy_block)

        begin = self.shadow_cpp.split(
            "void War3ShadowReceiverPass::beginPointShadowCpuPrepare", 1
        )[1].split("void War3ShadowReceiverPass::waitPointShadowCpuPrepare", 1)[0]
        self.assertIn("PointShadowPersistentMode() !=", begin)
        self.assertIn("std::async", begin)
        ctor = self.shadow_cpp.split(
            "War3ShadowReceiverPass::War3ShadowReceiverPass", 1
        )[1].split("War3ShadowReceiverPass::~War3ShadowReceiverPass", 1)[0]
        self.assertNotIn("make_unique<PointShadowPersistentPrepareWorker>", ctor)

        publish = self.shadow_cpp.split(
            "auto pointShadowPersistentDiagnosticsPublish", 1
        )[1].split("// External consumers execute later", 1)[0]
        off_gate = publish.index(
            "pointShadowPersistentConfiguredMode =="
        )
        worker_query = publish.index("m_pointShadowPersistentWorker->diagnostics()")
        status_publish = publish.index("PublishPointShadowPersistentDiagnostics")
        perf_publish = publish.index("notePointShadowPersistentFrame")
        self.assertLess(off_gate, worker_query)
        self.assertLess(off_gate, status_publish)
        self.assertLess(off_gate, perf_publish)
        self.assertIn("!m_pointShadowPersistentWorker", publish[:worker_query])

    def test_begin_contract_is_fail_closed_and_reasoned(self) -> None:
        begin = self.shadow_cpp.split(
            "void War3ShadowReceiverPass::beginPointShadowPersistentPrepare", 1
        )[1].split("tryCollectPointShadowPersistentProposal", 1)[0]
        for token in (
            "m_pointShadowPersistentBeginAttempts",
            "m_pointShadowPersistentBeginEligible",
            "WorkerPrepareDisabled",
            "PointShadowDisabled",
            "NoPointLights",
            "NoShadowCastingLights",
            "lightSnapshot.shadowCount == 0u",
            "NoReplayDraws",
            "InvalidLightSnapshot",
            "InvalidFrameSerial",
            "InvalidRendererEpoch",
            "WorkerCreateFailed",
            "WorkerUnavailable",
            "PreviousJobNotReady",
            "SubmitBusy",
            "SubmitStaleGeneration",
            "AllocationFailure",
            "UnexpectedException",
            "m_pointShadowPersistentRejectedFallback",
        ):
            self.assertIn(token, begin)

    def test_diagnostics_are_pod_and_cover_required_counters(self) -> None:
        diag = self.shadow_h.split(
            "struct PointShadowPersistentDiagnostics", 1
        )[1].split("};", 1)[0]
        self.assertNotIn("std::string", diag)
        self.assertNotIn("const char", diag)
        for field in (
            "configuredMode",
            "effectiveMode",
            "lastBeginRejectReason",
            "workerCreated",
            "workerAvailable",
            "beginAttempts",
            "beginEligible",
            "workerThreadStarts",
            "accepted",
            "ready",
            "deadlineFallback",
            "rejectedFallback",
            "mismatch",
            "failed",
            "busy",
        ):
            self.assertIn(field, diag)
        self.assertIn("enum class PointShadowPersistentBeginRejectReason", self.shadow_h)
        self.assertIn("QueryPointShadowPersistentDiagnostics", self.shadow_cpp)
        self.assertIn("PublishPointShadowPersistentDiagnostics", self.shadow_cpp)

    def test_runtime_status_and_perf_json_expose_numeric_snapshot(self) -> None:
        fields = (
            "pointShadowPersistentConfiguredMode",
            "pointShadowPersistentEffectiveMode",
            "pointShadowPersistentLastBeginRejectReason",
            "pointShadowPersistentBeginAttempts",
            "pointShadowPersistentBeginEligible",
            "pointShadowPersistentWorkerThreadStarts",
            "pointShadowPersistentAccepted",
            "pointShadowPersistentReady",
            "pointShadowPersistentDeadlineFallback",
            "pointShadowPersistentRejectedFallback",
            "pointShadowPersistentMismatch",
            "pointShadowPersistentFailed",
            "pointShadowPersistentBusy",
        )
        for field in fields:
            self.assertIn(field, self.diag_h)
            self.assertIn(field, self.diag_cpp)

        self.assertIn("PointShadowPersistentFrameTelemetry", self.perf_h)
        self.assertIn("notePointShadowPersistentFrame", self.perf_h)
        self.assertIn("notePointShadowPersistentFrame", self.perf_cpp)
        for field in (
            "pointShadowPersistentConfiguredModeLast",
            "pointShadowPersistentEffectiveModeLast",
            "pointShadowPersistentLastBeginRejectReason",
            "pointShadowPersistentAcceptedLast",
            "pointShadowPersistentReadyLast",
            "pointShadowPersistentBusyLast",
        ):
            self.assertIn(field, self.perf_cpp)


if __name__ == "__main__":
    unittest.main()
