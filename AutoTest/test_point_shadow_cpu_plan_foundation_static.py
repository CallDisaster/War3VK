"""Static contracts for the isolated owned-value point-shadow CPU planner."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/render/war3_point_shadow_cpu_plan.h"
SOURCE = ROOT / "src/d3d9/war3/render/war3_point_shadow_cpu_plan.cpp"
UNIT = ROOT / "src/d3d9/war3/render/tests/war3_point_shadow_cpu_plan_test.cpp"
MESON = ROOT / "src/d3d9/meson.build"
SHADOW_HEADER = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SHADOW_SOURCE = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"


class PointShadowCpuPlanFoundationContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.unit = UNIT.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")
        cls.shadow_header = SHADOW_HEADER.read_text(encoding="utf-8")
        cls.shadow_source = SHADOW_SOURCE.read_text(encoding="utf-8")

    def test_runtime_integration_is_opt_in_and_nonblocking(self) -> None:
        for token in (
            "kWar3PointShadowCpuPlanRuntimeIntegrated = true",
            "kWar3PointShadowCpuPlanOwnerBuilderIntegrated = true",
            "kWar3PointShadowCpuPlanRuntimeDefaultEnabled = false",
            "kWar3PointShadowCpuPlanConsumeDefaultEnabled = false",
            "kWar3PointShadowCpuPlanRenderThreadMayWait = false",
            "kWar3PointShadowCpuPlanSameFrameFallbackIntegrated = true",
            "kWar3PointShadowCpuPlanRejectedSubmitStorageRecoveryIntegrated = true",
            "kWar3PointShadowCpuPlanFailedJobStorageRecoveryIntegrated = false",
            "kWar3PointShadowCpuPlanMayPublishRendererState = false",
            "kWar3PointShadowCpuPlanMayOwnGpuResources = false",
            "ownerMustInvalidatePublication",
            "ownerMustClearFaceValidityBeforeRecord",
            "War3PointShadowCpuPlanSealTuple",
        ):
            self.assertIn(token, self.header)
        include = '#include "war3/render/war3_point_shadow_cpu_plan.h"'
        self.assertIn(include, self.shadow_header)
        self.assertIn("PointShadowPersistentMode", self.shadow_source)
        self.assertIn("tryCollectPointShadowPersistentProposal", self.shadow_source)
        self.assertIn("std::async(", self.shadow_source)

    def test_request_and_result_expose_no_renderer_or_gpu_owner(self) -> None:
        combined = self.header + self.source
        for forbidden in (
            "War3ShadowReceiverPass",
            "War3ShadowCasterDraw",
            "War3PipelineInput",
            "D3D9DeviceEx",
            "Rc<",
            "DxvkCommandList",
            "DxvkImage",
            "VkBuffer",
            ".ptr()",
            "reinterpret_cast",
        ):
            self.assertNotIn(forbidden, combined)
        for token in (
            "std::is_trivially_copyable_v<War3PointShadowCpuCaster>",
            "std::is_empty_v<War3PointShadowCpuPlanner>",
            "War3PointShadowCpuPlanRequest&& request",
            "War3PointShadowPrepareRequest<War3PointShadowCpuPlanRequestPayload>",
        ):
            self.assertIn(token, self.header)

    def test_complete_freeze_is_required_without_changing_legacy_hash(self) -> None:
        self.assertIn("bool frozenComplete = false", self.header)
        self.assertIn("public POD bool this is not", self.header)
        self.assertIn("single", self.header)
        self.assertIn("renderer owner", self.header)
        self.assertIn("IncompleteCasterFreeze", self.header)
        self.assertIn("InvalidSealTuple", self.header)
        self.assertIn("if (!caster.frozenComplete)", self.source)
        mix_start = self.source.index("void MixCasterSignature")
        mix_end = self.source.index("bool CasterInLightRange", mix_start)
        self.assertNotIn("frozenComplete", self.source[mix_start:mix_end])
        self.assertIn("TestNoWorkAndMalformedInputsFailClosed", self.unit)

    def test_legacy_signature_oracles_and_conditional_hash_are_locked(self) -> None:
        for token in (
            "0xb5d6942cfc00ffaeull",
            "0xff6737aa6748ed15ull",
            "0xd3199c952ea5844cull",
            "static_cast<uint32_t>(caster.vertexOffset)",
            "static_cast<uint64_t>(light.id)",
            "signature.mixMatrix(caster.worldMatrix)",
            "incrementalSignature",
        ):
            self.assertIn(token, self.source + self.unit)

    def test_temporal_and_per_light_face_budget_contracts_are_exercised(self) -> None:
        for token in (
            "history.temporalAge) + 1ull < uint64_t(period)",
            "War3PointShadowCpuPlanDisposition::ReusePublished",
            "result.forceFullFaceUpdate = !signatureUnchanged || hasDynamicCaster",
            "size_t(lightIndex) * kWar3PointShadowCpuPlanFaceCount + face",
            "result.updateMask[lightIndex] = updateMask",
            "result.updateMask[0] == 0x26u",
            "result.updateMask[index] == 0x01u",
        ):
            self.assertIn(token, self.source + self.unit)

    def test_culling_cap_and_nonfinite_hardening_are_explicit(self) -> None:
        for token in (
            "Any non-finite component is now unknown",
            "HasNonFiniteBounds",
            "nonFiniteBoundsPinnedCount",
            "unknownBoundsPinnedCount",
            "surfaceDistanceSquared",
            "std::partition(",
            "std::nth_element(",
            "candidateCount - std::min(candidateCount, keptCount)",
            "TestNonFiniteBoundsArePinnedHardeningDelta",
            "TestRangeFaceCullingBufferGateAndNearestSurfaceCap",
            "TestLargeCappedCohortKeepsOnlyNearestKnownSubset",
            "faceDroppedCount[face] == 4080u",
        ):
            self.assertIn(token, self.source + self.header + self.unit)
        self.assertNotIn("std::stable_partition", self.source)
        self.assertNotIn("std::sort(knownBegin", self.source)

    def test_all_owned_vectors_are_returned_for_recycling(self) -> None:
        for token in (
            "War3PointShadowCpuPlanOwnedStorage",
            "rangeCandidateIndices",
            "rankedCandidates",
            "faceCasters",
            "std::move(request.payload.storage)",
            "result.storage = std::move(storage)",
            "TestOwnedStorageRecyclesAcrossTwoHundredWorkerJobs",
            "iteration <= 200u",
            "recycled.casters.data() == casterAllocation",
            "recycled.faceCasters[face].data() == faceAllocations[face]",
        ):
            self.assertIn(token, self.header + self.source + self.unit)

    def test_matrix_and_resource_budget_algorithms_have_runnable_oracles(self) -> None:
        for token in (
            "kWar3PointShadowCpuPlanResourceBudgetBytes",
            "War3ResolvePointShadowCpuPlanCapacity(1025u, 4u) == 3u",
            "War3ResolvePointShadowCpuPlanCapacity(2048u, 4u) == 1u",
            "MultiplyLikeDxvkMatrix4(projection, view)",
            "TestCubeMatricesUseTranslatedViewAndClipXFlip",
        ):
            self.assertIn(token, self.header + self.source + self.unit)

    def test_meson_builds_production_and_isolated_runnable(self) -> None:
        self.assertEqual(
            self.meson.count("war3/render/war3_point_shadow_cpu_plan.cpp"), 2
        )
        self.assertIn("war3_point_shadow_cpu_plan_test", self.meson)
        self.assertIn("war3_point_shadow_cpu_plan_foundation", self.meson)
        production_prefix = self.meson.split("war3_point_shadow_cpu_plan_test =", 1)[0]
        self.assertIn("war3_point_shadow_cpu_plan.cpp", production_prefix)


if __name__ == "__main__":
    unittest.main()
