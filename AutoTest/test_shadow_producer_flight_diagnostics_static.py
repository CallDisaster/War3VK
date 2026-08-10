#!/usr/bin/env python3
"""Contracts for producer/cache evidence in runtime and GPU incidents."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
BRIDGE_H = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.h").read_text(
    encoding="utf-8"
)
BRIDGE_CPP = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
DIAG_H = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h").read_text(
    encoding="utf-8"
)
DIAG_CPP = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp").read_text(
    encoding="utf-8"
)
CONTROL = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(
    encoding="utf-8"
)
PERF_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(
    encoding="utf-8"
)
PERF_CPP = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)


COUNTERS = (
    "producerRequiredCasterOmissionCount",
    "producerExactBudgetDeferredUniqueCasterCount",
    "producerPositionAllocBudgetCount",
    "producerUvAllocBudgetCount",
    "producerIndexAllocBudgetCount",
    "producerAllocationFailureCount",
    "producerFallbackByteBudgetCount",
    "producerArenaAdmissionCount",
    "producerFreezeFailureCount",
    "producerCompletenessReasonMask",
    "producerCompletenessSealed",
    "producerCompletenessCounterOverflow",
    "drawTimeVBCacheStaticLiveBytes",
    "drawTimeVBCacheStaticProtectedBytes",
    "drawTimeVBCacheStaticOverCapBytes",
    "drawTimeVBCacheStaticOverCapFrameCount",
    "drawTimeVBCacheStaticEvictedBytes",
    "drawTimeVBCacheStaticEvictedEntryCount",
    "drawTimeVBCacheIndexedUnknownRangeFallbackCount",
)


class ProducerFlightDiagnosticsContractTest(unittest.TestCase):
    def test_seal_identity_is_mirrored_after_seal(self):
        for name in (
            "producerSealFrameSerial",
            "producerSealMapEpoch",
            "producerSealDeviceEpoch",
        ):
            self.assertIn(name, SCENE)
        seal = DEVICE.index("void D3D9DeviceEx::War3SealShadowProducerCompleteness(")
        seal_body = DEVICE[seal : DEVICE.index("\n}\n", seal) + 3]
        self.assertIn("scene.producerCompleteness.seal(", seal_body)
        self.assertIn("completeness.sealFrameSerial", seal_body)
        self.assertIn("completeness.mapEpoch", seal_body)
        self.assertIn("completeness.deviceEpoch", seal_body)

    def test_scene_stats_flow_to_bridge_summary(self):
        for name in COUNTERS:
            self.assertIn(name, SCENE)
            self.assertIn(name, BRIDGE_H)
            self.assertIn(f"summary.{name}", BRIDGE_CPP)
            self.assertIn(f"g_shadowSceneStats.{name}", BRIDGE_CPP)

    def test_runtime_status_exposes_independent_reasons(self):
        for name in COUNTERS:
            self.assertIn(name, DIAG_H)
            self.assertIn(f"bridgeSummary.{name}", DIAG_CPP)
            self.assertIn(f'{{"{name}"', CONTROL)

    def test_flight_and_incident_own_the_same_bounded_values(self):
        record = DIAG_CPP.index("void RecordGpuFlightFrame(uint64_t frameSerial)")
        record_body = DIAG_CPP[record : DIAG_CPP.index("\n}\n\nWar3RuntimeStatusFrameSnapshot", record)]
        self.assertIn("QueryShadowProducerRuntimeDiagnostics()", record_body)
        self.assertNotIn("QueryShadowRuntimeBridgeSummary()", record_body)
        for name in COUNTERS:
            self.assertIn(f"frame.{name} =", record_body)
            self.assertIn(name, DIAG_H)
            self.assertIn(f'{{"{name}"', DIAG_CPP)

    def test_perf_report_keeps_reason_and_working_set_breakdown(self):
        for name in (
            "producerPositionAllocBudgetCount",
            "producerUvAllocBudgetCount",
            "producerIndexAllocBudgetCount",
            "producerAllocationFailureCount",
            "producerFallbackByteBudgetCount",
            "producerArenaAdmissionCount",
            "producerFreezeFailureCount",
            "drawTimeVBCacheStaticLiveBytesLast",
            "drawTimeVBCacheStaticProtectedBytesLast",
            "drawTimeVBCacheStaticOverCapBytesLast",
            "drawTimeVBCacheStaticEvictedEntryCount",
            "drawTimeVBCacheIndexedUnknownRangeFallbackCount",
        ):
            self.assertIn(name, PERF_H)
            self.assertIn(name, PERF_CPP)

    def test_diagnostics_do_not_change_shadow_policy(self):
        for forbidden in (
            "alphaShadowFarAlphaRefBias",
            "alphaShadowHashed",
            "kShadowDrawTimeVBCacheAllocBudgetPerFrame =",
        ):
            self.assertNotIn(forbidden, DIAG_CPP)
            self.assertNotIn(forbidden, BRIDGE_CPP)


if __name__ == "__main__":
    unittest.main()
