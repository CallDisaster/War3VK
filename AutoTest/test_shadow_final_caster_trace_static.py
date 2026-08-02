"""Static contracts for exact final shadow-caster frame tracing."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
BRIDGE_H = ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.h"
BRIDGE_CPP = ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp"
MONITOR_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
ANALYZER = ROOT / "AutoTest/analyze_shadow_final_caster_trace.py"
PROBE_RUNNER = ROOT / "AutoTest/run_bridge_ramp_visual_probe.py"


class ShadowFinalCasterTraceStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shadow = SHADOW_CPP.read_text(encoding="utf-8")
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")
        cls.bridge_h = BRIDGE_H.read_text(encoding="utf-8")
        cls.bridge = BRIDGE_CPP.read_text(encoding="utf-8")
        cls.monitor = MONITOR_CPP.read_text(encoding="utf-8")
        cls.analyzer = ANALYZER.read_text(encoding="utf-8")
        cls.probe_runner = PROBE_RUNNER.read_text(encoding="utf-8")

    def test_trace_runs_at_final_replay_choke_point(self) -> None:
        start = self.shadow.index(
            "const std::vector<const War3ShadowCasterDraw*>& "
            "BuildShadowReplayDraws"
        )
        end = self.shadow.index(
            "uint32_t ComputeAdaptiveShadowMapPeriod", start
        )
        block = self.shadow[start:end]
        self.assertEqual(block.count("NoteFinalShadowCasterFrame"), 3)
        self.assertIn(
            "void NoteFinalShadowCasterFrame(",
            self.bridge_h,
        )

    def test_trace_records_identity_geometry_backing_and_world(self) -> None:
        start = self.bridge.index(
            "void WriteFinalShadowCasterRecordEvent("
        )
        end = self.bridge.index(
            "template <typename T>\nuint32_t ApplyTraceRecordLimit", start
        )
        block = self.bridge[start:end]
        required = (
            "identityHash",
            "backingHash",
            "contentHash",
            "positionSampleHash",
            "indexSampleHash",
            "worldMatrix",
            "worldTranslation",
            "positionStorageGeneration",
            "indexStorageGeneration",
            "validationFlags",
            "gpuSkin",
        )
        for token in required:
            self.assertIn(token, block)

    def test_trace_is_opt_in_and_visible_in_perf_env_snapshot(self) -> None:
        names = (
            "DXVK_WAR3_SHADOW_POSE_FULL_TRACE",
            "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTERS",
            "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CASTERS",
            "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTER_SAMPLE_BYTES",
        )
        for name in names:
            self.assertIn(f'"{name}"', self.bridge)
            self.assertIn(f'"{name}"', self.monitor)
        self.assertIn(
            'env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE"), false',
            self.bridge,
        )

    def test_analyzer_joins_exact_capture_serial(self) -> None:
        self.assertIn('"shadowFrameSerial"', self.analyzer)
        self.assertIn('"shadowFinalCasterFrame"', self.analyzer)
        self.assertIn('"shadowFinalCasterRecord"', self.analyzer)
        self.assertIn("backingChangedForStableIdentity", self.analyzer)
        self.assertIn("largeGeometryAnchoredNearOrigin", self.analyzer)
        self.assertIn("captureTraceCoveragePct", self.analyzer)
        self.assertIn("shadowPoseFullTraceFrame", self.analyzer)
        self.assertIn("frame_index_by_trace_serial", self.analyzer)
        self.assertIn(
            '"mappedFinalCasterFrameSerial"', self.analyzer
        )
        self.assertIn("traceSerialDomainContract", self.analyzer)
        self.assertIn("analysisWarmupTransition", self.analyzer)
        self.assertIn("captureZeroExcludedAsWarmup", self.analyzer)
        self.assertIn("explainedPartDisappearanceCount", self.analyzer)
        self.assertIn("MissingRequiredPart", self.analyzer)
        self.assertIn("RetiredAfterAuthoritativeAbsence", self.analyzer)
        self.assertIn("TombstoneRetired", self.analyzer)
        self.assertIn(
            "full_domain_masked_anonymous_small_marker", self.analyzer
        )
        self.assertIn("fullDomainMaskedMarkerLeakCount", self.analyzer)
        self.assertIn(
            "grace_resurrected_anonymous_small_marker", self.analyzer
        )
        self.assertIn("graceResurrectedMarkerLeakCount", self.analyzer)
        for token in (
            'int(record.get("partLifecycleState", -1)) == 2',
            'int(record.get("positionStride", 0)) == 12',
            'bool(int(record.get("vertexBlendEnabled", 0)))',
            'bool(int(record.get("vertexBlendIndexed", 0)))',
        ):
            self.assertIn(token, self.analyzer)

    def test_populate_snapshot_is_traced_before_downstream_filters(self) -> None:
        snapshot = self.device.index(
            "SnapshotPublishedCurrentDrawContracts(\n            "
            "snapshotOptions)"
        )
        trace = self.device.index("NoteCurrentDrawSnapshotFrame(", snapshot)
        diagnostics = self.device.index(
            'enterDirectDetailPhase("SnapshotDiagnostics")', snapshot
        )
        self.assertLess(snapshot, trace)
        self.assertLess(trace, diagnostics)
        self.assertIn(
            "void NoteCurrentDrawSnapshotFrame(",
            self.bridge_h,
        )
        self.assertIn(
            "shadowCurrentDrawSnapshotFrame",
            self.bridge,
        )

    def test_expired_lease_releases_backing_without_pruning_core(self) -> None:
        helper = self.device.index("const auto noteLeaseExpiredBackingOnly")
        first_expiry = self.device.index(
            "noteLeaseExpiredBackingOnly();", helper + 1
        )
        second_expiry = self.device.index(
            "noteLeaseExpiredBackingOnly();", first_expiry + 1
        )
        core_gate = self.device.index(
            "semanticSceneShadowManifestMissingRequiredPartCount +=",
            second_expiry,
        )
        self.assertLess(helper, first_expiry)
        self.assertLess(first_expiry, second_expiry)
        self.assertLess(second_expiry, core_gate)
        self.assertGreaterEqual(
            self.device.count("noteLeaseExpiredBackingOnly();"), 2
        )
        helper_block = self.device[helper:first_expiry]
        self.assertIn(
            "semanticSceneShadowManifestLeaseExpiredBackingOnlyCount++",
            helper_block,
        )
        self.assertNotIn("committedPartKeys", helper_block)
        self.assertNotIn("observationPartKeys", helper_block)
        self.assertNotIn("objectCoreSets", helper_block)

    def test_part_lifecycle_is_exported_to_trace_and_report(self) -> None:
        counters = (
            "semanticSceneShadowManifestLeaseExpiredBackingOnlyCount",
            "semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount",
            "semanticSceneShadowManifestMissingRequiredPartCount",
            "semanticSceneShadowManifestGraceUsedCount",
            "semanticSceneShadowManifestTombstoneRetiredCount",
        )
        for counter in counters:
            self.assertIn(counter, self.bridge_h)
            self.assertIn(f'\\"{counter}\\":', self.bridge)
            self.assertIn(f'"    \\"{counter}\\": "', self.monitor)

    def test_exact_capture_can_wait_for_final_caster_trace_witness(self) -> None:
        self.assertIn("--wait-for-shadow-trace-sec", self.probe_runner)
        self.assertIn(
            'marker = b\'"type":"shadowFinalCasterFrame"\'',
            self.probe_runner,
        )

    def test_online_dark_trigger_excludes_capture_zero_warmup(self) -> None:
        self.assertIn(
            "and previous_capture_index > 0",
            self.probe_runner,
        )
        self.assertIn(
            "Capture zero is the exact-frame warmup witness",
            self.probe_runner,
        )

    def test_currentdraw_join_does_not_overclaim_missing_identity(self) -> None:
        current_join = (
            ROOT / "AutoTest/analyze_shadow_currentdraw_final_join.py"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "CurrentDrawHandleJoinUnavailable",
            current_join,
        )
        self.assertIn("synchronizedFullAbsenceFrameCount", current_join)
        self.assertIn("zeroFramesAfterFirstPositive", current_join)
        witness = self.probe_runner.index(
            "shadow_trace_witness = _wait_for_final_caster_trace("
        )
        capture = self.probe_runner.index(
            "for index in range(capture_count):",
            witness,
        )
        self.assertLess(witness, capture)
        self.assertIn(
            '"shadowTraceWitness": shadow_trace_witness',
            self.probe_runner,
        )


if __name__ == "__main__":
    unittest.main()
