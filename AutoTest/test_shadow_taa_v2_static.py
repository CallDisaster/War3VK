import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SETTINGS = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(
    encoding="utf-8"
)
PIPELINE = (ROOT / "src/d3d9/d3d9_war3_pipeline.cpp").read_text(
    encoding="utf-8"
)
SHADOW_H = (ROOT / "src/d3d9/d3d9_war3_shadow.h").read_text(
    encoding="utf-8"
)
SHADOW_CPP = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8"
)
RESOURCES = (
    ROOT / "src/d3d9/d3d9_war3_shadow_resources.cpp"
).read_text(encoding="utf-8")
SHADER = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_receiver.frag"
).read_text(encoding="utf-8")
UI = (ROOT / "src/d3d9/war3/ui/war3_imgui.cpp").read_text(
    encoding="utf-8"
)
PERF_H = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h"
).read_text(encoding="utf-8")
PERF_CPP = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
).read_text(encoding="utf-8")


class ShadowTaaV2StaticTests(unittest.TestCase):
    def test_history_stores_visibility_and_linear_depth_in_rg16f(self):
        self.assertIn("VK_FORMAT_R16G16_SFLOAT", RESOURCES)
        self.assertIn(
            "layout(set = 1, binding = 10, rg16f) uniform image2D",
            SHADER,
        )
        self.assertIn("histSample.g", SHADER)
        self.assertIn("currLinearDepth", SHADER)
        self.assertIn("depthTolerance", SHADER)

    def test_preblur_and_low_edge_weight_are_removed(self):
        self.assertNotIn("resolvedCurr", SHADER)
        self.assertNotIn("sumVis", SHADER)
        self.assertNotIn("0.018", SHADER)
        self.assertIn("reactive", SHADER)
        self.assertIn("max(blend, 0.30)", SHADER)

    def test_variance_clipping_is_center_anchored_and_reactive(self):
        for token in (
            "vec2 visibilityMoments",
            "meanVisibility",
            "float variance = max(",
            "float sigma = sqrt(variance)",
            "float gamma = mix(0.85, 1.10, edgeFactor)",
            "min(currVis, meanVisibility - gamma * sigma)",
            "max(currVis, meanVisibility + gamma * sigma)",
            "float rawHistVis = histSample.r",
            "float historyDisagreement = abs(rawHistVis - currVis)",
            "historyRectification",
        ):
            self.assertIn(token, SHADER)
        self.assertNotIn("float minN =", SHADER)
        self.assertNotIn("float maxN =", SHADER)

    def test_ordinary_shadow_evolution_is_not_a_global_history_cut(self):
        start = SHADOW_CPP.index(
            "if (shadowTaaTemporalActive && m_shadowHistoryValid)"
        )
        end = SHADOW_CPP.index(
            "const bool shadowHistoryValidBefore", start
        )
        contract = SHADOW_CPP[start:end]
        self.assertNotIn("currentCsmHash", contract)
        self.assertNotIn("m_shadowTaaHistoryReplayContentHash", contract)
        self.assertIn("ShadowTaaDisableOnSunMotionEnabled()", contract)
        self.assertIn(
            "ShadowTaaDisableForSemanticDynamicEnabled()", contract
        )

    def test_camera_cut_uses_pose_not_raw_world_scale_matrix_delta(self):
        self.assertIn("bool ShadowTaaIsCameraCut(", SHADOW_CPP)
        self.assertIn("const Vector4 currentEye = currentInvView[3]", SHADOW_CPP)
        self.assertIn("shadowFarDistance * 0.10f", SHADOW_CPP)
        self.assertIn("forwardCosine < 0.64278764f", SHADOW_CPP)
        self.assertNotIn("if (viewProjDelta > 0.25f)", SHADOW_CPP)
        self.assertIn("m_shadowTaaHistoryView", SHADOW_H)
        self.assertIn(
            "? m_shadowTaaHistoryViewProj\n"
            "        : (m_hasPrevFrameData ? m_prevViewProj : currentViewProj)",
            SHADOW_CPP,
        )

    def test_shader_and_cpu_use_four_execution_states(self):
        self.assertIn("0 DirectInline", SHADER)
        self.assertIn("1 PrepassCurrentOnly", SHADER)
        self.assertIn("2 TemporalCurrentOnly", SHADER)
        self.assertIn("3 TemporalHistory", SHADER)
        self.assertIn("taaMode = 1.0f", SHADOW_CPP)
        self.assertIn(
            "taaMode = shadowHistoryReadable ? 3.0f : 2.0f",
            SHADOW_CPP,
        )
        self.assertIn(
            "shadowHistoryWriteExecuted =\n"
            "        shadowTaaTemporalActive",
            SHADOW_CPP,
        )

    def test_history_contract_covers_required_invalidators(self):
        for token in (
            "kShadowTaaInvalidateLifecycle",
            "kShadowTaaInvalidateCameraCut",
            "kShadowTaaInvalidateProjection",
            "kShadowTaaInvalidateViewport",
            "kShadowTaaInvalidateSun",
            "kShadowTaaInvalidateCsm",
            "kShadowTaaInvalidateCasterContent",
            "kShadowTaaInvalidateDynamicPose",
            "kShadowTaaInvalidateShadowMapResource",
            "kShadowTaaInvalidateTaaResource",
        ):
            self.assertIn(token, SHADOW_CPP)
        self.assertIn("m_shadowTaaHistoryContractValid", SHADOW_H)
        self.assertIn("historyInvalidationMask", PERF_H)
        self.assertGreaterEqual(
            PERF_CPP.count('\\"shadowTaaHistoryInvalidationReasonFrames\\"'),
            2,
        )

    def test_clarity_first_default_ui_and_environment_are_unified(self):
        self.assertIn("shadowTaaBlendFactor = 0.20f", SETTINGS)
        self.assertIn('"阴影时域模式"', UI)
        self.assertIn("0.12f,\n                             0.30f", UI)
        self.assertIn(
            '"DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT"', PIPELINE
        )
        self.assertIn(
            '"DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT"', PERF_CPP
        )

    def test_incomplete_history_never_advances(self):
        for token in (
            "shadowVisibilityExecutedThisFrame",
            "shadowMotionVectorExecutedThisFrame",
            "receiverDrawExecutedThisFrame",
            "shadowHistoryWriteExecuted",
        ):
            self.assertIn(token, SHADOW_CPP)
        self.assertIn(
            "m_shadowTaaWasActiveLastFrame = shadowHistoryWriteComplete",
            SHADOW_CPP,
        )


if __name__ == "__main__":
    unittest.main()
