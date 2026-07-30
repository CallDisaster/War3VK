"""Static contracts for point-shadow radial receiver bias."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SETTINGS_H = ROOT / "src/d3d9/d3d9_war3_settings.h"
SHADOW_H = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
VOLUME_CPP = ROOT / "src/d3d9/d3d9_war3_volumetric_light.cpp"
RECEIVER = ROOT / "subprojects/war3fx/shaders/war3_shadow_receiver.frag"
VOLUME = ROOT / "subprojects/war3fx/shaders/war3_volumetric_light.frag"
CASTER = ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_frag.frag"
POINT_CASTER = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_point_frag.frag"
)


class PointShadowReceiverBiasTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.settings = SETTINGS_H.read_text(encoding="utf-8")
        cls.shadow_h = SHADOW_H.read_text(encoding="utf-8")
        cls.shadow = SHADOW_CPP.read_text(encoding="utf-8")
        cls.volume_cpp = VOLUME_CPP.read_text(encoding="utf-8")
        cls.receiver = RECEIVER.read_text(encoding="utf-8")
        cls.volume = VOLUME.read_text(encoding="utf-8")
        cls.caster = CASTER.read_text(encoding="utf-8")
        cls.point_caster = POINT_CASTER.read_text(encoding="utf-8")

    def test_point_pipeline_selects_the_dedicated_radial_depth_shader(self) -> None:
        self.assertIn("#include <war3_shadow_caster_point_frag.h>", self.shadow)
        self.assertIn("bool pointShadowRadialDepth = false", self.shadow_h)
        self.assertIn(
            "pointShadowRadialDepth == other.pointShadowRadialDepth",
            self.shadow_h,
        )
        self.assertIn("h.add(uint32_t(pointShadowRadialDepth))", self.shadow_h)
        point_start = self.shadow.index(
            "void War3ShadowReceiverPass::renderPointShadow("
        )
        point_block = self.shadow[point_start:]
        self.assertIn("key.pointShadowRadialDepth = true", point_block)
        self.assertIn(
            "util::DxvkBuiltInShaderStage(war3_shadow_caster_point_frag",
            self.shadow,
        )
        self.assertIn(
            "key.casterMaskEnabled && key.pointShadowRadialDepth",
            self.shadow,
        )

    def test_only_point_caster_writes_radial_depth(self) -> None:
        self.assertNotIn("gl_FragDepth =", self.caster)
        self.assertIn("float faceDepth = 1.0 /", self.point_caster)
        self.assertIn("float radialDepth =", self.point_caster)
        self.assertIn("radialDepth / range", self.point_caster)
        self.assertIn("gl_FragDepth =", self.point_caster)

    def test_point_receivers_compare_the_same_radial_depth_domain(self) -> None:
        self.assertIn(
            "currentDist / max(shadowRange, 1e-4)", self.receiver
        )
        self.assertIn("currentDistance / shadowRange", self.volume)

    def test_default_bias_covers_a_meaningful_texel_fraction(self) -> None:
        self.assertIn("pointShadowTexelBiasScale = 0.35f", self.settings)
        self.assertGreaterEqual(
            self.shadow_h.count("Vector4(0.65f, 1.15f, 0.35f, 0.78f)"),
            2,
        )
        self.assertIn("0.35f),\n                 0.0f, 1.0f)", self.shadow)
        self.assertIn("0.0f, 1.0f, 0.35f", self.volume_cpp)

    def test_surface_bias_is_additive_and_receiver_slope_aware(self) -> None:
        start = self.receiver.index("float samplePointShadowPcf(")
        end = self.receiver.index("return visible * (1.0 / 16.0);", start)
        block = self.receiver[start:end]
        self.assertIn("float receiverCosine", block)
        self.assertIn("float receiverSlope =", block)
        self.assertIn("float slopeScale =", block)
        self.assertIn("max(biasBase, 0.0) +", block)
        self.assertIn("texelWorld * texelBiasScale * slopeScale", block)
        self.assertNotIn(
            "max(max(biasBase, 0.0), texelWorld * texelBiasScale)",
            block,
        )
        self.assertIn("slopeBias, nFactor", self.receiver)

    def test_volume_uses_same_additive_radial_depth_contract(self) -> None:
        start = self.volume.index("float samplePointVolumeShadow(")
        end = self.volume.index("bool decodeDepth(", start)
        block = self.volume[start:end]
        self.assertIn("worldBias + texelWorld * texelBiasScale", block)
        self.assertNotIn(
            "max(worldBias, texelWorld * texelBiasScale)", block
        )


if __name__ == "__main__":
    unittest.main()
