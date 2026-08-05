"""Static and analytic contracts for nearest cubemap point-shadow taps."""

from __future__ import annotations

import math
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RECEIVER = ROOT / "subprojects/war3fx/shaders/war3_shadow_receiver.frag"


def _quantize_cube_ray(direction: tuple[float, float, float], resolution: int):
    """Reference copy of the Vulkan face table used by the GLSL helper."""
    x, y, z = direction
    ax, ay, az = abs(x), abs(y), abs(z)
    if az >= ay and az >= ax:
        major = az
        if z >= 0.0:
            face, sc, tc = 4, x / major, -y / major
        else:
            face, sc, tc = 5, -x / major, -y / major
    elif ay >= ax:
        major = ay
        if y >= 0.0:
            face, sc, tc = 2, x / major, z / major
        else:
            face, sc, tc = 3, x / major, -z / major
    else:
        major = ax
        if x >= 0.0:
            face, sc, tc = 0, -z / major, -y / major
        else:
            face, sc, tc = 1, z / major, -y / major

    def centre(v: float) -> float:
        texel = min(max(math.floor((v * 0.5 + 0.5) * resolution), 0),
                    resolution - 1)
        return ((texel + 0.5) / resolution) * 2.0 - 1.0

    sc, tc = centre(sc), centre(tc)
    rays = {
        0: (1.0, -tc, -sc),
        1: (-1.0, -tc, sc),
        2: (sc, 1.0, tc),
        3: (sc, -1.0, -tc),
        4: (sc, -tc, 1.0),
        5: (-sc, -tc, -1.0),
    }
    return face, rays[face]


class PointShadowTexelCenterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.receiver = RECEIVER.read_text(encoding="utf-8")

    def test_shader_quantizes_to_the_nearest_cube_texel_center(self) -> None:
        helper_start = self.receiver.index("vec3 pointCubeNearestTexelRay(")
        sample_start = self.receiver.index("float samplePointShadowPcf(")
        helper = self.receiver[helper_start:sample_start]
        self.assertIn("floor(uv * cubeResolution)", helper)
        self.assertIn("(texel + vec2(0.5)) / cubeResolution", helper)
        self.assertIn("return vec3(1.0, -texelSt.y, -texelSt.x);", helper)
        self.assertIn("return vec3(-texelSt.x, -texelSt.y, -1.0);", helper)

    def test_receiver_reference_and_depth_fetch_share_the_texel_ray(self) -> None:
        start = self.receiver.index("float samplePointShadowPcf(")
        end = self.receiver.index("return visible * (1.0 / 16.0);", start)
        block = self.receiver[start:end]
        self.assertIn(
            "vec3 texelRay = pointCubeNearestTexelRay(\n"
            "        sampleDir, cubeResolution);",
            block,
        )
        self.assertIn("dot(receiverNormal, texelRay)", block)
        self.assertIn("float texelRayLength = sqrt(texelRayLenSq);", block)
        self.assertIn("vec4(texelRay, float(lightIndex))", block)
        self.assertNotIn("dot(receiverNormal, sampleDir)", block)
        self.assertNotIn("vec4(sampleDir, float(lightIndex))", block)

    def test_debug_mode_uses_the_production_normal_contract(self) -> None:
        start = self.receiver.index("if (debugMode == 6)")
        end = self.receiver.index("vec4 viewH =", start)
        block = self.receiver[start:end]
        self.assertIn("computePointLightViewNormal", block)
        self.assertIn("transpose(mat3(ubo.u_view))", block)
        self.assertIn("smoothstep(", block)
        self.assertNotIn("-normalize(lightToFrag), 0.0", block)

    def test_quantized_ray_is_idempotent_and_inside_its_selected_face(self) -> None:
        directions = (
            (1.0, 0.2, -0.3), (-1.0, 0.7, 0.1),
            (0.3, 1.0, -0.4), (0.3, -1.0, 0.4),
            (0.6, -0.2, 1.0), (-0.6, 0.2, -1.0),
            (1.0, 1.0, 1.0), (-1.0, -1.0, -1.0),
        )
        for direction in directions:
            face, ray = _quantize_cube_ray(direction, 1024)
            face2, ray2 = _quantize_cube_ray(ray, 1024)
            self.assertEqual(face2, face)
            self.assertEqual(ray2, ray)
            major = max(abs(v) for v in ray)
            others = sorted((abs(v) for v in ray), reverse=True)
            self.assertEqual(major, 1.0)
            self.assertLess(others[1], 1.0)


if __name__ == "__main__":
    unittest.main()
