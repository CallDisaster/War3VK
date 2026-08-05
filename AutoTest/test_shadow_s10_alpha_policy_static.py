import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
SHADOW_CPP = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8"
)
POLICY_H = (
    ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.h"
).read_text(encoding="utf-8")
POLICY_CPP = (
    ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.cpp"
).read_text(encoding="utf-8")
PERF_CPP = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
).read_text(encoding="utf-8")


class ShadowS10AlphaPolicyStaticTest(unittest.TestCase):
    def test_overlay_predicate_is_centralized(self):
        self.assertIn("bool IsShadowVisualOverlay(", POLICY_H)
        self.assertIn("bool IsShadowVisualOverlay(", POLICY_CPP)
        self.assertIn("if (IsShadowVisualOverlay(context))", POLICY_CPP)
        self.assertIn("context.stage == 12", POLICY_CPP)
        self.assertIn("War3BatchTag::RangeIndicatorTarget", POLICY_CPP)

    def test_raw_capture_never_promotes_alpha_blend(self):
        self.assertIn(
            "const bool shouldCaptureUV = alphaTestEnabled;", DEVICE_CPP
        )
        self.assertNotIn(
            "bool shouldCaptureUV = alphaTestEnabled || alphaBlend", DEVICE_CPP
        )
        self.assertNotIn(
            "entry.alphaTestEnabled ||\n        (entry.alphaBlendEnabled",
            DEVICE_CPP,
        )

    def test_incomplete_cutout_payload_fails_closed(self):
        self.assertIn("bool HasCompleteAlphaPayload() const", DEVICE_H)
        self.assertIn(
            "entry.alphaTestEnabled && !entry.HasCompleteAlphaPayload()",
            DEVICE_CPP,
        )
        self.assertIn(
            "Budget pressure may reject a cutout caster", DEVICE_CPP
        )
        self.assertNotIn(
            "budgetDecision.disableAlphaCapture && captureAlphaTest) {\n"
            "    captureAlphaTest = false;",
            DEVICE_CPP,
        )

    def test_renderer_does_not_invent_half_alpha_threshold(self):
        self.assertIn(
            "draw.alphaTestEnabled && alphaPayloadComplete", SHADOW_CPP
        )
        self.assertNotIn(
            "(draw.alphaTestEnabled || draw.alphaBlendEnabled)",
            SHADOW_CPP,
        )
        self.assertNotIn(
            "draw.alphaTestEnabled ? draw.alphaRef : 0.5f", SHADOW_CPP
        )

    def test_stage10_is_current_frame_only(self):
        self.assertIn(
            "if (stage == 10) {\n"
            "    // Stage10 is intentionally current-frame-only",
            DEVICE_CPP,
        )
        self.assertIn(
            "persistentRejectReason = "
            "War3PersistentRejectReason::DynamicSource;",
            DEVICE_CPP,
        )

    def test_stage10_has_one_runtime_gated_immediate_owner(self):
        self.assertIn(
            "bool IsStage10ImmediateLegacyShadowOwnerEnabled()",
            POLICY_H,
        )
        self.assertIn(
            '"DXVK_WAR3_KEEP_STAGE10_TERRAIN_DOODAD_LEGACY_CAPTURE"',
            POLICY_CPP,
        )
        self.assertIn(
            "RejectStage10OwnedByImmediateLegacy",
            POLICY_H,
        )
        self.assertIn(
            "!keepStage10DoodadLegacyCapture &&\n"
            "      !keepStage13WorldObjectLegacyCapture",
            DEVICE_CPP,
        )
        self.assertIn(
            "isStage10DoodadDraw && !keepStage10DoodadLegacyCapture",
            DEVICE_CPP,
        )
        self.assertIn(
            '"DXVK_WAR3_KEEP_STAGE10_TERRAIN_DOODAD_LEGACY_CAPTURE"',
            PERF_CPP,
        )


if __name__ == "__main__":
    unittest.main()
