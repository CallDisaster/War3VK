import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERT = (ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_vert.vert").read_text(
    encoding="utf-8"
)
FRAG = (ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_frag.frag").read_text(
    encoding="utf-8"
)
POINT = (
    ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_point_frag.frag"
).read_text(encoding="utf-8")
SETTINGS = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(encoding="utf-8")
CONTRACT = (
    ROOT / "src/d3d9/war3/render/war3_shadow_hashed_alpha_contract.h"
).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", source)
    if not match:
        raise AssertionError(f"missing function {name}")
    depth = 1
    index = match.end()
    while index < len(source) and depth:
        depth += source[index] == "{"
        depth -= source[index] == "}"
        index += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return source[match.end() : index - 1]


class ShadowHashedAlphaStabilityStaticTests(unittest.TestCase):
    def test_directional_surface_seed_is_object_anchored(self):
        self.assertIn("layout(location = 1) out vec3 v_surfaceCoord", VERT)
        self.assertIn("v_surfaceCoord = in_pos.xyz;", VERT)
        fragment_main = function_body(FRAG, "main")
        hashed = function_body(FRAG, "stableHashedAlphaThreshold")
        self.assertIn("v_surfaceCoord", fragment_main)
        for unstable_seed in (
            "p_paletteOffset",
            "p_samplerIndex",
            "gl_FragCoord",
            "frame",
            "time",
        ):
            self.assertNotIn(unstable_seed, hashed)

    def test_paper_scale_interpolation_and_cdf_are_present(self):
        hashed = function_body(FRAG, "stableHashedAlphaThreshold")
        cdf = function_body(FRAG, "uniformizeInterpolatedHashes")
        for token in (
            "max(length(surfaceDx), length(surfaceDy))",
            "log2(pixelScale)",
            "floor(logScale)",
            "ceil(logScale)",
            "exp2",
            "uniformizeInterpolatedHashes",
        ):
            self.assertIn(token, hashed)
        for token in ("x * x / denominator", "1.0 - a", "1.0e-6"):
            self.assertIn(token, cdf)

    def test_derivatives_precede_every_discard_and_sampling_is_explicit(self):
        main = function_body(FRAG, "main")
        discard = re.search(r"\bdiscard\s*;", main)
        self.assertIsNotNone(discard)
        first_discard = discard.start()
        for token in (
            "dFdx(v_uv)",
            "dFdy(v_uv)",
            "dFdx(v_surfaceCoord)",
            "dFdy(v_surfaceCoord)",
            "textureGrad(",
        ):
            self.assertGreaterEqual(main.find(token), 0)
            self.assertLess(main.index(token), first_discard)
        self.assertNotIn("float alpha = texture(", main)

    def test_magnified_path_preserves_authored_threshold(self):
        blend = function_body(FRAG, "stableCoverageBlend")
        main = function_body(FRAG, "main")
        self.assertIn("maxFootprint <= 1.0", blend)
        self.assertIn("return 0.0", blend)
        self.assertIn("normalized * normalized", blend)
        self.assertIn("/ 6.0", blend)
        self.assertIn("mix(", main)
        self.assertIn("finiteFloat(p_alphaRef)", main)
        self.assertIn("authoredAlphaRef, hashedThreshold, coverageBlend", main)

    def test_invalid_derivatives_fail_soft_to_hard_cutoff(self):
        hashed = function_body(FRAG, "stableHashedAlphaThreshold")
        blend = function_body(FRAG, "stableCoverageBlend")
        self.assertIn("!finiteVec3(surfaceCoord)", hashed)
        self.assertIn("maxDerivative <= 1.0e-8", hashed)
        self.assertIn("return false", hashed)
        self.assertIn("!finiteVec2(uvDx)", blend)
        self.assertIn("return 0.0", blend)

    def test_point_shadow_algorithm_is_not_connected_to_new_path(self):
        self.assertNotIn("v_surfaceCoord", POINT)
        self.assertNotIn("stableHashedAlphaThreshold", POINT)
        self.assertNotIn("stableCoverageBlend", POINT)
        self.assertIn("gl_FragDepth = clamp(radialDepth / range", POINT)

    def test_candidate_remains_release_off_until_physical_gate(self):
        self.assertIn("bool alphaShadowHashed = false;", SETTINGS)

    def test_cpu_oracle_is_bounded_and_fail_soft(self):
        for token in (
            "maxDerivative <= 1.0e-8",
            "std::clamp(std::log2(pixelScale), -24.0, 24.0)",
            "uniformizeInterpolatedHashes",
            "fullHashLod = 6.0",
            "normalized * normalized",
        ):
            self.assertIn(token, CONTRACT)


if __name__ == "__main__":
    unittest.main()
