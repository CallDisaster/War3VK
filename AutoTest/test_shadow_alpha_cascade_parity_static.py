#!/usr/bin/env python3
"""Issue #4 contracts for identical default alpha cutout silhouettes per CSM cascade."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SETTINGS = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
API = (ROOT / "src/d3d9/war3_shader_api.cpp").read_text(encoding="utf-8")
HELPER = (
    ROOT / "src/d3d9/war3/render/war3_shadow_alpha_cascade_contract.h"
).read_text(encoding="utf-8")
RECEIVER = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_receiver.frag"
).read_text(encoding="utf-8")
VISIBILITY = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_visibility.frag"
).read_text(encoding="utf-8")
CASTER = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_frag.frag"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def normalized(source: str) -> str:
    return " ".join(source.split())


class ShadowAlphaCascadeParityStaticTests(unittest.TestCase):
    def test_release_defaults_keep_cutout_parity_and_deterministic_alpha(self):
        self.assertIn("bool alphaShadowHashed = false;", SETTINGS)
        self.assertIn("bool alphaShadowUseMip = false;", SETTINGS)
        self.assertIn("float alphaShadowMipLodBias = 0.0f;", SETTINGS)
        self.assertIn("float alphaShadowFarAlphaRefBias = 0.0f;", SETTINGS)

    def test_pure_bias_helper_guards_finite_range_count_and_index(self):
        sanitize = function_body(HELPER, "SanitizeShadowAlphaFarRefBias")
        self.assertIn("std::isfinite(configuredBias)", sanitize)
        self.assertIn("configuredBias < 0.0f", sanitize)
        self.assertIn("return 0.0f", sanitize)
        self.assertIn("std::clamp(configuredBias, 0.0f, 1.0f)", sanitize)

        cascade = function_body(HELPER, "ShadowAlphaRefBiasForCascade")
        self.assertIn("SanitizeShadowAlphaFarRefBias(configuredBias)", cascade)
        self.assertIn("cascadeCount <= 1u", cascade)
        self.assertIn("cascadeCount - 1u", cascade)
        self.assertIn("std::min(cascadeIndex, lastCascade)", cascade)
        self.assertIn("float(safeCascade) / float(lastCascade)", cascade)
        for forbidden in ("new ", "Logger", "settings", "GetMutable"):
            self.assertNotIn(forbidden, cascade)

    def test_depth_and_terrain_mask_use_the_same_helper_and_push_clamp(self):
        self.assertIn("SanitizeShadowAlphaFarRefBias(", SHADOW)
        self.assertEqual(SHADOW.count("ShadowAlphaRefBiasForCascade("), 2)
        self.assertNotIn(
            "alphaShadowFarAlphaRefBias * (float(c) / float(cascadeCount - 1))",
            normalized(SHADOW),
        )
        self.assertEqual(
            SHADOW.count(
                "std::clamp(draw.alphaRef + alphaRefBiasCascade, 0.0f, 1.0f)"
            ),
            2,
        )

    def test_mip_sampler_and_shader_api_reject_nonfinite_float_inputs(self):
        sampler = function_body(SHADOW, "War3ShadowReceiverPass::getFallbackSampler")
        self.assertLess(
            sampler.index("std::isfinite(mipLodBias)"),
            sampler.index("std::clamp(finiteMipLodBias, -4.0f, 4.0f)"),
        )
        self.assertLess(
            sampler.index("std::clamp(finiteMipLodBias, -4.0f, 4.0f)"),
            sampler.index("std::lround(clampedBias / kStep)"),
        )

        mip = function_body(API, "SetShadowAlphaMipLodBias")
        self.assertLess(mip.index("!std::isfinite(bias)"), mip.index("GetMutableSettings"))
        self.assertIn("std::clamp(bias, -4.0f, 4.0f)", mip)

        far = function_body(API, "SetShadowAlphaFarAlphaRefBias")
        self.assertLess(far.index("!std::isfinite(bias)"), far.index("GetMutableSettings"))
        self.assertIn("std::clamp(bias, 0.0f, 1.0f)", far)
        self.assertNotIn("std::max(0.0f, bias)", far)

    def test_alpha_parity_does_not_touch_shader_push_or_point_shadow_paths(self):
        for shader in (RECEIVER, VISIBILITY, CASTER):
            self.assertNotIn("ShadowAlphaRefBiasForCascade", shader)
            self.assertNotIn("alphaShadowFarAlphaRefBias", shader)

        point = function_body(SHADOW, "War3ShadowReceiverPass::renderPointShadow")
        self.assertIn("pc.alphaRef = draw.alphaRef;", point)
        self.assertNotIn("alphaRefBiasCascade", point)

        push_start = SHADOW.index("struct ShadowCasterPushConstants")
        push_end = SHADOW.index("};", push_start)
        push = SHADOW[push_start:push_end]
        for field in (
            "Matrix4 mvp",
            "uint32_t paletteOffset",
            "uint32_t blendCount",
            "uint32_t flags",
            "float alphaRef",
            "uint32_t samplerIndex",
            "float terrainDepthBias",
            "uint32_t padding[2]",
            "Vector4 pointLightPosRange",
        ):
            self.assertIn(field, push)
        self.assertEqual(SHADOW.count("sizeof(ShadowCasterPushConstants)"), 2)


if __name__ == "__main__":
    unittest.main()
