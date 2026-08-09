#!/usr/bin/env python3
"""Static and numerical contracts for compare-first directional-shadow PCF."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHADOW_CPP = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8"
)
SHADOW_H = (ROOT / "src/d3d9/d3d9_war3_shadow.h").read_text(
    encoding="utf-8"
)
SETTINGS = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(
    encoding="utf-8"
)
RECEIVER = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_receiver.frag"
).read_text(encoding="utf-8")
VISIBILITY = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_visibility.frag"
).read_text(encoding="utf-8")
MOTION = (
    ROOT / "subprojects/war3fx/shaders/war3_motion_vector.frag"
).read_text(encoding="utf-8")


def bilinear_compare(depths, reference, fx, fy):
    """Reference PCF: compare four texels, then interpolate visibility."""
    visibility = [1.0 if reference <= depth else 0.0 for depth in depths]
    top = visibility[0] * (1.0 - fx) + visibility[1] * fx
    bottom = visibility[2] * (1.0 - fx) + visibility[3] * fx
    return top * (1.0 - fy) + bottom * fy


def receiver_plane_gradient(uv_dx, uv_dy, depth_dx, depth_dy):
    determinant = uv_dx[0] * uv_dy[1] - uv_dx[1] * uv_dy[0]
    return (
        (depth_dx * uv_dy[1] - uv_dx[1] * depth_dy) / determinant,
        (uv_dx[0] * depth_dy - depth_dx * uv_dy[0]) / determinant,
    )


class ShadowComparePcfStaticTests(unittest.TestCase):
    def test_compare_happens_before_bilinear_filtering(self):
        self.assertEqual(
            bilinear_compare([0.2, 1.0, 0.2, 1.0], 0.5, 0.5, 0.5),
            0.5,
        )
        # The removed implementation interpolated raw depth to 0.6 first and
        # therefore returned fully lit for the same footprint.
        self.assertEqual(1.0 if 0.5 <= 0.6 else 0.0, 1.0)

    def test_raw_and_comparison_samplers_are_separate(self):
        for token in (
            "rawShadowSampler",
            "compareShadowSampler",
            "shadowCompareMode",
            "m_shadowCompareSampler",
            "m_shadowCompareSamplerLinear",
        ):
            self.assertIn(token, SHADOW_CPP + SHADOW_H)
        self.assertIn(
            "setDepthCompare(true, VK_COMPARE_OP_LESS_OR_EQUAL)", SHADOW_CPP
        )
        self.assertIn(
            "VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT", SHADOW_CPP
        )

    def test_shader_uses_comparison_sampler_for_pcf_only(self):
        for source in (RECEIVER, VISIBILITY):
            self.assertIn("sampler2DArrayShadow", source)
            self.assertIn("p_compareShadowSampler", source)
            self.assertIn("p_rawShadowSampler", source)
            self.assertRegex(
                source,
                re.compile(
                    r"float\s+shadowMapDepth.*?p_rawShadowSampler",
                    re.DOTALL,
                ),
            )
            self.assertRegex(
                source,
                re.compile(
                    r"float\s+shadowCompare.*?sampler2DArrayShadow.*?"
                    r"p_compareShadowSampler",
                    re.DOTALL,
                ),
            )
            self.assertIn("manualShadowCompareLinear2x2", source)

    def test_push_constant_sampler_contract_is_four_u32(self):
        cpp_push = re.search(
            r"struct ReceiverPushConstants \{(.*?)\};",
            SHADOW_CPP,
            re.DOTALL,
        )
        self.assertIsNotNone(cpp_push)
        self.assertEqual(cpp_push.group(1).count("uint32_t"), 4)
        for source in (RECEIVER, VISIBILITY, MOTION):
            shader_push = re.search(
                r"uniform push_block \{(.*?)\};", source, re.DOTALL
            )
            self.assertIsNotNone(shader_push)
            self.assertEqual(shader_push.group(1).count("uint "), 4)

    def test_release_default_has_no_periodic_poisson_rotation(self):
        self.assertIn("bool pcfRotate = false;", SETTINGS)
        self.assertIn(
            "War3ShadowPcfRotateMode pcfRotateMode = "
            "War3ShadowPcfRotateMode::Off;",
            SETTINGS,
        )
        self.assertNotIn(
            "dot(worldPos.xy, vec2(0.03125, 0.015625))", RECEIVER
        )

    def test_default_poisson16_is_exactly_paired_and_zero_centroid(self):
        arrays = []
        for source in (RECEIVER, VISIBILITY):
            block = source[
                source.index("const vec2 kPoisson16") :
                source.index("const vec2 kPoisson25")
            ]
            points = [
                (float(x), float(y))
                for x, y in re.findall(
                    r"vec2\(\s*([-+]?\d*\.\d+)\s*,\s*"
                    r"([-+]?\d*\.\d+)\s*\)",
                    block,
                )
            ]
            self.assertEqual(len(points), 16)
            for index in range(0, 16, 2):
                self.assertAlmostEqual(points[index][0], -points[index + 1][0])
                self.assertAlmostEqual(points[index][1], -points[index + 1][1])
            self.assertAlmostEqual(sum(x for x, _ in points), 0.0)
            self.assertAlmostEqual(sum(y for _, y in points), 0.0)
            arrays.append(points)
        self.assertEqual(arrays[0], arrays[1])

    def test_stable_wall_filter_is_continuous_and_shared(self):
        for source in (RECEIVER, VISIBILITY):
            self.assertNotIn("snappedUv", source)
            self.assertNotIn("sampleShadowStableWall", source)
            self.assertIn("wallFilterWeight", source)
            self.assertIn(
                "mix(radius0, max(radius0, 1.50), wallFilterWeight)", source
            )

    def test_receiver_plane_reference_is_per_tap_and_kernel_atomic(self):
        gradient = receiver_plane_gradient(
            (0.5, 0.0), (0.0, 0.25), 0.1, -0.05
        )
        self.assertAlmostEqual(gradient[0], 0.2)
        self.assertAlmostEqual(gradient[1], -0.2)
        tap_reference = 0.5 + gradient[0] * 0.01 + gradient[1] * -0.02
        self.assertAlmostEqual(tap_reference, 0.506)

        for source in (RECEIVER, VISIBILITY):
            for token in (
                "computeReceiverPlaneDepthGradient",
                "receiverPlaneKernelValid",
                "receiverPlaneTapReference",
                "dot(gradient, tapOffsetUv)",
                "kMaxReceiverPlaneDepthDelta = 0.0025",
                "worldDx = dFdx(worldPos)",
                "worldDy = dFdy(worldPos)",
            ):
                self.assertIn(token, source)
            self.assertIn(
                "receiverPlaneGradient, kernelPlaneValid", source
            )
            self.assertRegex(
                source,
                re.compile(
                    r"bool\s+kernelPlaneValid\s*=\s*receiverPlaneValid\s*&&\s*"
                    r"receiverPlaneKernelValid",
                    re.DOTALL,
                ),
            )

    def test_direct_and_prepass_share_pcss_and_finite_fallbacks(self):
        for source in (RECEIVER, VISIBILITY):
            for token in (
                "float computePcssRadius",
                "float depth = shadowMapDepth(cascadeIndex, tapUv)",
                "if (tapUv.x < 0.0 || tapUv.x > 1.0",
                "!validVec3(ndc)",
                "!validVec4(l0)",
                "!validVec3(n0)",
            ):
                self.assertIn(token, source)
        self.assertIn("!validVec4(l1)", RECEIVER)
        self.assertIn("!validVec3(n1)", RECEIVER)
        self.assertIn("validVec4(l1) && l1.w > 0.0", VISIBILITY)
        self.assertIn("validVec3(n1) && n1.z >= 0.0", VISIBILITY)
        self.assertIn(
            "return vis0;", VISIBILITY[
                VISIBILITY.index("float computeShadowVisibility") :
                VISIBILITY.index("void main()")
            ],
        )


if __name__ == "__main__":
    unittest.main()
