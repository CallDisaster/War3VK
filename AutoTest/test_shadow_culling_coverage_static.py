#!/usr/bin/env python3
"""Static contracts for the 2026-09-20 shadow-culling coverage lane.

These checks pin the current, source-provable separation between:
- draw-time capture and the 384 MiB Stage11 snapshot allocation,
- final CSM cascade visibility decisions,
- default-deny Observe/Consume authority.

They do not compile or run the engine.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "src/d3d9/war3/render/war3_shadow_bounds_policy.h"
UNION_H = ROOT / "src/d3d9/war3/render/war3_union_consumer_visibility.h"
UNION_CPP = ROOT / "src/d3d9/war3/render/war3_union_consumer_visibility.cpp"
SNAPSHOT_POLICY = (
    ROOT / "src/d3d9/war3/render/war3_stage11_snapshot_page_policy.h"
)
OBSERVER_POLICY = (
    ROOT / "src/d3d9/war3/render/war3_shadow_observer_build_policy.h"
)
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
CSM = ROOT / "src/d3d9/d3d9_war3_csm.cpp"
INTERNAL = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"


class ShadowCullingCoverageStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.policy = POLICY.read_text(encoding="utf-8")
        cls.union_h = UNION_H.read_text(encoding="utf-8")
        cls.union_cpp = UNION_CPP.read_text(encoding="utf-8")
        cls.snapshot_policy = SNAPSHOT_POLICY.read_text(encoding="utf-8")
        cls.observer_policy = OBSERVER_POLICY.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.shadow = SHADOW.read_text(encoding="utf-8")
        cls.csm = CSM.read_text(encoding="utf-8")
        cls.internal = INTERNAL.read_text(encoding="utf-8")

    def test_snapshot_pool_is_384_mib_and_allocated_in_capture(self) -> None:
        self.assertIn("384u << 20u", self.snapshot_policy)
        self.assertIn("16u << 20u", self.snapshot_policy)
        self.assertIn(
            "kWar3Stage11SnapshotResidentCapMinBytes = 128u << 20u",
            self.snapshot_policy,
        )
        self.assertIn(
            "kWar3Stage11SnapshotResidentCapMaxBytes = 512u << 20u",
            self.snapshot_policy,
        )
        self.assertIn("War3AllocateStage11Snapshot(", self.device)
        self.assertIn("ResidentCapacity", self.device)
        self.assertIn("War3CollectUnusedStage11SnapshotPages", self.device)
        self.assertNotIn("War3AllocateStage11Snapshot(", self.shadow)
        self.assertNotIn("War3CollectUnusedStage11SnapshotPages", self.shadow)

    def test_final_cull_happens_after_prepare_and_capture(self) -> None:
        render = self.shadow.index(
            "bool War3ShadowReceiverPass::renderShadowMap"
        )
        prepare = self.shadow.index(
            "for (uint32_t i = 0; i < casterCount; i++)", render
        )
        cull_setup = self.shadow.index(
            "for (const uint32_t drawIndex : sortedDrawIndices)", prepare
        )
        record = self.shadow.index(
            "for (uint32_t c = 0; c < cascadeCount; c++)", cull_setup
        )
        self.assertLess(prepare, cull_setup)
        self.assertLess(cull_setup, record)
        self.assertNotIn("War3Stage11Snapshot", self.shadow)
        self.assertNotIn("drawTimeSnapshotPage", self.shadow)

    def test_actual_consume_is_default_denied(self) -> None:
        self.assertIn("kReleaseFreezeExperimentalShadowRoutes = true",
                      self.internal)
        self.assertIn(
            "!war3::internal::kReleaseFreezeExperimentalShadowRoutes &&",
            self.shadow,
        )
        self.assertIn("s_objectBoundsCullConsume && c >= 2u", self.shadow)
        self.assertIn("consumeTerrainCascade", self.shadow)
        self.assertIn("War3TerrainBoundsCullMode::Consume", self.shadow)
        self.assertIn("query.consumeAdmissionGranted = false;", self.shadow)
        self.assertNotIn("query.consumeAdmissionGranted = true", self.shadow)
        union_runtime = self.shadow.split(
            "War3UnionCullModeRuntime()", 1
        )[1].split("War3TerrainBoundsCullModeRuntime", 1)[0]
        terrain_runtime = self.shadow.split(
            "War3TerrainBoundsCullModeRuntime()", 1
        )[1].split("War3ShadowTaaMode", 1)[0]
        self.assertNotIn("::Consume", union_runtime)
        self.assertNotIn("::Consume", terrain_runtime)
        self.assertIn("kDevelopmentShadowObserversEnabled", union_runtime)
        self.assertIn("kDevelopmentShadowObserversEnabled", terrain_runtime)
        self.assertNotIn("Consume", self.observer_policy)

    def test_bounds_evidence_requires_exact_current_provenance(self) -> None:
        for token in (
            "War3ShadowBoundsProvenanceIsExact",
            "SourceGenerationUnknown",
            "FrameGenerationUnknown",
            "FrameGenerationStale",
            "IdentityUnproven",
            "DynamicOrSkinned",
            "AnimatedAttachment",
            "NonFiniteBounds",
            "InvalidRadius",
        ):
            self.assertIn(token, self.policy)
        body = self.policy.split("War3EvaluateBoundsCullEvidence", 1)[1]
        self.assertIn("evidence.frameLocalDynamic &&", body)
        self.assertIn(
            "evidence.provenance != War3ShadowBoundsProvenance::ExactCurrentWorld",
            body,
        )
        self.assertIn("return {War3ShadowBoundsCullRejectReason::None, true};",
                      body)

    def test_union_policy_has_no_camera_frustum_or_default_consume(self) -> None:
        self.assertIn("bool failVisible = true", self.union_h)
        self.assertIn("bool consumeAdmissionGranted = false", self.union_h)
        self.assertIn("result.effectiveVisibleMask = requestedMask",
                      self.union_cpp)
        self.assertIn("query.consumeAdmissionGranted", self.union_cpp)
        self.assertNotIn("worldCamera", self.union_cpp)
        self.assertNotIn("viewProj", self.union_cpp)
        for token in (
            "NonOrthographicProjection",
            "MapGenerationUnknown",
            "MapGenerationMismatch",
            "DeviceGenerationUnknown",
            "DeviceGenerationMismatch",
            "currentMapGeneration",
            "consumerDeviceGeneration",
        ):
            self.assertIn(token, self.union_h)
        self.assertIn("IsExactOrthographicWRow", self.union_cpp)
        self.assertIn("matrix.columns[3][3] == 1.0f", self.union_cpp)

    def test_capture_coarse_cull_is_disabled_and_not_authoritative(self) -> None:
        self.assertIn("kShadowCaptureCoarseCullEnabled = false",
                      self.internal)
        start = self.device.index(
            "if constexpr (dxvk::war3::internal::"
            "kShadowCaptureCoarseCullEnabled) {"
        )
        end = self.device.index(
            "War3ShadowCaptureBoundsPhase::BudgetCandidate", start
        )
        block = self.device[start:end]
        self.assertIn("estimatedBoundsRadius", block)
        self.assertIn("m_war3Scene.worldCamera.viewProj", block)
        self.assertNotIn("War3EvaluateBoundsCullEvidence", block)
        self.assertNotIn("boundsProvenance", block)
        self.assertNotIn("War3ShadowBoundsProvenance", block)

    def test_csm_light_volume_is_orthographic_and_later_than_capture(self) -> None:
        self.assertIn("makeOrthoOffCenterLH", self.csm)
        self.assertIn(
            "const Matrix4 lightViewProj = lightProj * lightView;", self.csm
        )
        compute = self.shadow.index("m_csmData = newCsm;")
        render = self.shadow.index("renderShadowMap(ctx, input", compute)
        self.assertLess(compute, render)
        self.assertNotIn("m_csmData", self.device)
        self.assertIn("War3AllocateStage11Snapshot(", self.device)



    def test_b05_counterexamples_cover_consumer_surface_and_gaps(self) -> None:
        counter = (
            ROOT / "AutoTest/test_shadow_culling_counterexamples.cpp"
        ).read_text(encoding="utf-8")
        for token in (
            "TestSingleConsumerMaskRepresentation",
            "TestOffCameraOtherConsumerCounterexample",
            "TestProofDomainGapsStayVisible",
            "TestAbsentConsumerClosureIsNotProof",
            "War3UnionConsumerMain",
            "War3UnionConsumerCsm0",
            "War3UnionConsumerCsm1",
            "War3UnionConsumerCsm2",
            "War3UnionConsumerCsm3",
            "War3UnionConsumerPointShadow",
            "War3UnionConsumerOutline",
            "volume-sun",
        ):
            self.assertIn(token, counter)
        for call in (
            "TestSingleConsumerMaskRepresentation();",
            "TestOffCameraOtherConsumerCounterexample();",
            "TestProofDomainGapsStayVisible();",
            "TestAbsentConsumerClosureIsNotProof();",
        ):
            self.assertIn(call, counter)

    def test_b05_production_observer_is_csm2csm3_only(self) -> None:
        query = self.shadow.split(
            "War3UnionCsmSphereQuery query = {};", 1
        )[1].split("const auto decision =", 1)[0]
        self.assertIn("War3UnionConsumerCsm2", query)
        self.assertIn("War3UnionConsumerCsm3", query)
        for token in (
            "kWar3UnionConsumerAllMask",
            "War3UnionConsumerMain",
            "War3UnionConsumerCsm0",
            "War3UnionConsumerCsm1",
            "War3UnionConsumerPointShadow",
            "War3UnionConsumerOutline",
        ):
            self.assertNotIn(token, query)
        self.assertIn("query.consumeAdmissionGranted = false;", query)

    def test_b05_volume_sun_has_no_union_consumer_bit(self) -> None:
        self.assertNotIn("VolumeSun", self.union_h)
        self.assertNotIn("volumeSun", self.union_h)

if __name__ == "__main__":
    unittest.main(verbosity=2)
