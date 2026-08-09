import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/d3d9/war3/render/war3_gpu_workload_governor.h"
SOURCE = ROOT / "src/d3d9/war3/render/war3_gpu_workload_governor.cpp"
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
SHADOW_RESOURCES = ROOT / "src/d3d9/d3d9_war3_shadow_resources.cpp"
MESON = ROOT / "src/d3d9/meson.build"


class GpuWorkloadGovernorStaticTest(unittest.TestCase):
    def test_pure_policy_has_three_dimensional_checked_admission(self):
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        for token in (
            "DirectionalCsm",
            "VolumeSun",
            "PointShadow",
            "maxDraws",
            "maxVertices",
            "maxIndices",
            "checkedAdd",
            "checkedMultiply",
            "addRepeatedDraw",
        ):
            self.assertIn(token, header + source)
        self.assertIn("Commit only after every dimension", source)

    def test_volume_reserves_before_commands_and_directional_before_draw(self):
        text = SHADOW.read_text(encoding="utf-8")
        start = text.index("bool War3ShadowReceiverPass::renderShadowMap")
        end = text.index("namespace {\n// A2 Worker_Prepare", start)
        body = text[start:end]
        volume_reserve = body.index("m_gpuWorkloadGovernor.tryReserve")
        directional_reserve = body.index(
            "War3GpuWorkloadConsumer::DirectionalCsm", volume_reserve
        )
        self.assertLess(volume_reserve, body.index("ensureShadowMatrixBuffer"))
        self.assertLess(volume_reserve, body.index("ctx->cmdPipelineBarrier"))
        self.assertLess(directional_reserve, body.index("ctx->cmdBeginRendering"))
        self.assertIn("War3GpuWorkloadConsumer::VolumeSun", body)
        self.assertIn("War3GpuWorkloadConsumer::DirectionalCsm", body)
        self.assertIn("visibleCascadeCount", body)
        self.assertIn("restoreShadowTargetsToRead", body[directional_reserve:])

    def test_point_faces_are_one_atomic_pre_record_reservation(self):
        text = SHADOW.read_text(encoding="utf-8")
        start = text.index("void War3ShadowReceiverPass::renderPointShadow")
        end = text.index("void War3ShadowReceiverPass::drawReceiver", start)
        body = text[start:end]
        reserve = body.index("War3GpuWorkloadConsumer::PointShadow")
        self.assertLess(reserve, body.index("ensureShadowMatrixBuffer"))
        self.assertLess(reserve, body.index("ctx->cmdPipelineBarrier"))
        self.assertLess(reserve, body.index("ctx->cmdBeginRendering"))
        self.assertIn("pointWorkloadFaceCount", body)
        reject_body = body[reserve:body.index("// 不整表清 ready", reserve)]
        self.assertIn("holdPointShadowLastCompleteAfterBudgetReject", reject_body)
        self.assertIn("if (!heldLastComplete)", reject_body)
        self.assertIn("invalidatePointShadowPublishedState", reject_body)

    def test_point_budget_hold_requires_exact_complete_publication(self):
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        shadow = SHADOW.read_text(encoding="utf-8")
        self.assertIn("War3GpuPointShadowPublicationIdentity", header)
        self.assertIn("War3GpuCanHoldPointShadowLastComplete", source)
        for token in (
            "current.mapEpoch != published.mapEpoch",
            "current.deviceEpoch != published.deviceEpoch",
            "current.resourceGeneration != published.resourceGeneration",
            "current.lightGeneration != published.lightGeneration",
            "current.settingsRevision != published.settingsRevision",
            "ExactPointLightIdentity",
        ):
            self.assertIn(token, source)
        self.assertIn("m_pointShadowPublishedResourceGeneration", shadow)
        self.assertIn("m_pointShadowFaceValidMask", shadow)
        resources = SHADOW_RESOURCES.read_text(encoding="utf-8")
        self.assertIn("m_pointShadowResourceGeneration + 1u", resources)

    def test_csm_budget_reject_uses_bounded_last_good_or_no_shadow(self):
        text = SHADOW.read_text(encoding="utf-8")
        self.assertIn("m_workloadGovernorRejectedThisFrame = true", text)
        self.assertGreaterEqual(
            text.count("m_workloadGovernorRejectedThisFrame"), 4
        )
        self.assertIn("m_replayValidationHoldFramesRemaining", text)
        self.assertIn("m_hasCompleteShadowMap = false", text)
        self.assertIn("csmDataBeforeShadowCandidate", text)
        self.assertIn("m_csmData = csmDataBeforeShadowCandidate", text)

    def test_policy_is_built_in_production_and_as_runnable(self):
        meson = MESON.read_text(encoding="utf-8")
        self.assertGreaterEqual(
            meson.count("war3/render/war3_gpu_workload_governor.cpp"), 2
        )
        self.assertIn("war3_gpu_workload_governor_test", meson)
        self.assertIn("test(\n  'war3_gpu_workload_governor'", meson)


if __name__ == "__main__":
    unittest.main()
