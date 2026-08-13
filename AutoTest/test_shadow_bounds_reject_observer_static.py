#!/usr/bin/env python3
"""Static contracts for non-mutating shadow-bounds rejection telemetry."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "src/d3d9/war3/render/war3_shadow_bounds_policy.h"
SHADOW_H = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
SCENE = ROOT / "src/d3d9/d3d9_war3_scene.h"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
PERF_H = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h"
PERF_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
ANALYZER = ROOT / "AutoTest/war3_autotest_mcp.py"


class ShadowBoundsRejectObserverContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.policy = POLICY.read_text(encoding="utf-8")
        cls.shadow_h = SHADOW_H.read_text(encoding="utf-8")
        cls.shadow_cpp = SHADOW_CPP.read_text(encoding="utf-8")
        cls.scene = SCENE.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.perf_h = PERF_H.read_text(encoding="utf-8")
        cls.perf_cpp = PERF_CPP.read_text(encoding="utf-8")
        cls.analyzer = ANALYZER.read_text(encoding="utf-8")

    def test_policy_has_bounded_reason_count_and_index(self) -> None:
        self.assertIn("InvalidRadius,\n  Count,", self.policy)
        self.assertIn("kWar3ShadowBoundsCullRejectReasonCount", self.policy)
        self.assertIn("War3ShadowBoundsCullRejectReasonIndex", self.policy)

    def test_terrain_and_object_record_the_policy_reason(self) -> None:
        loop = self.shadow_cpp.split(
            "for (const uint32_t drawIndex : sortedDrawIndices)", 1
        )[1].split("const auto cascadeVisible", 1)[0]
        self.assertEqual(loop.count("War3ShadowBoundsCullRejectReasonIndex"), 2)
        self.assertIn("terrainBoundsRejectReasonHistogram", loop)
        self.assertIn("objectBoundsRejectReasonHistogram", loop)

    def test_histograms_cross_scene_and_perf_value_layers(self) -> None:
        self.assertIn("terrainBoundsRejectReasonHistogram", self.shadow_h)
        self.assertIn("objectBoundsRejectReasonHistogram", self.shadow_h)
        for text in (self.scene, self.perf_h):
            self.assertIn("TerrainBoundsRejectReasonHistogram", text)
            self.assertIn("ObjectBoundsRejectReasonHistogram", text)
        self.assertIn("semanticSceneTerrainBoundsRejectReasonHistogram[i] +=", self.perf_cpp)
        self.assertIn("semanticSceneObjectBoundsRejectReasonHistogram[i] +=", self.perf_cpp)

    def test_report_exports_bounded_arrays(self) -> None:
        self.assertIn("semanticSceneTerrainBoundsRejectReasonHistogram", self.perf_cpp)
        self.assertIn("semanticSceneObjectBoundsRejectReasonHistogram", self.perf_cpp)
        self.assertIn('"rejectReasons": _reject_histogram', self.analyzer)

    def test_observer_does_not_change_cull_authority(self) -> None:
        self.assertIn("if (!boundsPolicy.mayCull)\n      return true;", self.shadow_cpp)
        self.assertIn("s_objectBoundsCullConsume && c >= 2u", self.shadow_cpp)
        self.assertNotIn("RejectReasonHistogram[", self.policy)

    def test_union_observer_uses_the_same_provenance_authority(self) -> None:
        union_loop = self.shadow_cpp.split(
            "if (unionCullMode != war3::render::War3UnionVisibilityMode::Off)", 1
        )[1].split("reconciliation.unionCullObserveOverheadNs", 1)[0]
        self.assertIn("const auto boundsPolicy = evaluateBoundsPolicy(draw);", union_loop)
        self.assertIn("!boundsPolicy.mayCull", union_loop)
        self.assertIn(
            "query.generations.boundsFrameGeneration = draw.boundsFrameSerial;",
            union_loop,
        )
        self.assertIn("query.identityKnown = draw.boundsIdentityProven;", union_loop)
        self.assertIn("query.boundsKnown = boundsPolicy.mayCull;", union_loop)
        self.assertIn("query.dynamic = draw.boundsFrameLocalDynamic;", union_loop)
        self.assertIn("query.skinned = draw.boundsSourceWasSkinned;", union_loop)
        self.assertNotIn(
            "query.generations.boundsFrameGeneration = input.frameSerial;",
            union_loop,
        )

    def test_terrain_producer_chain_is_observed_without_authorizing_cull(self) -> None:
        fields = (
            "ProducerS1AttemptCount",
            "ProducerFallbackAttemptCount",
            "ProducerExactRangeCount",
            "ProducerMissingExactRangeCount",
            "ProducerUpSourceAttemptCount",
            "ProducerMappedSourceAttemptCount",
            "ProducerNoSourceCount",
            "ProducerSpanAcceptedCount",
            "ProducerSpanRejectedCount",
            "ProducerComputeSuccessCount",
            "ProducerComputeFailureCount",
            "ProducerValidSphereCount",
            "ProducerInvalidSphereCount",
            "ProducerPublishedExactCount",
        )
        for suffix in fields:
            field = f"semanticSceneTerrainBounds{suffix}"
            self.assertIn(field, self.scene)
            self.assertIn(field, self.perf_h)
            self.assertIn(field, self.perf_cpp)
        self.assertGreaterEqual(
            self.device.count(
                "semanticSceneTerrainBoundsProducerPublishedExactCount"
            ),
            3,
        )
        self.assertIn("War3RecordTerrainBoundsProducerSpan(", self.device)
        self.assertEqual(
            self.perf_cpp.count(
                "semanticSceneTerrainBoundsProducerHistogram"
            ),
            2,
        )
        self.assertIn('"producer": _terrain_producer_histogram()', self.analyzer)

    def test_exact_index_scan_is_decoupled_from_frozen_trim_mutation(self) -> None:
        scan = self.device.split(
            "const bool exactIndexedFreezeTrimCandidate =", 1
        )[1].split("if (exactIndexedDomainScanCandidate)", 1)[0]
        self.assertIn("exactIndexedTerrainBoundsObserveCandidate", scan)
        self.assertIn("exactIndexedDomainScanCandidate", scan)
        self.assertIn("exactIndexedFreezeTrimCandidate ||", scan)
        trim = self.device.split(
            "if (exactIndexedDomainScanCandidate)", 1
        )[1].split("ShadowArena_NoteExactIndexTrim", 1)[0]
        self.assertIn(
            "if ((exactIndexedFreezeTrimCandidate || consumeCoherentRealTrim) &&",
            trim,
        )
        self.assertIn(
            "(exactIndexedFreezeTrimCandidate &&\n"
            "           SUCCEEDED(FlushBuffer(exactIndexCommon)))",
            trim,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
