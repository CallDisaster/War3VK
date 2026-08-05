"""Static contracts for exact VS-B1 skinning in shadow caster passes."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CASTER = ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_vert.vert"
MAIN_VS = ROOT / "src/d3d9/shaders/d3d9_fixed_function_vert.vert"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"


class ShadowGpuSkinDirectCasterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.caster = CASTER.read_text(encoding="utf-8")
        cls.main_vs = MAIN_VS.read_text(encoding="utf-8")
        cls.shadow_cpp = SHADOW_CPP.read_text(encoding="utf-8")

    def test_shadow_layout_consumes_the_pinned_source_and_palette(self) -> None:
        self.assertIn("layout(set = 1, binding = 3, std430)", self.caster)
        self.assertIn("readonly restrict buffer GpuSkinStaticSource", self.caster)
        self.assertIn("layout(set = 1, binding = 4, std430)", self.caster)
        self.assertIn("readonly restrict buffer GpuSkinPalette", self.caster)
        self.assertIn("descriptors[3].descriptorType =", self.shadow_cpp)
        self.assertIn("descriptors[4].descriptorType =", self.shadow_cpp)
        self.assertIn("draw.gpuSkinInput.staticSource.getSliceInfo()", self.shadow_cpp)
        self.assertIn("draw.gpuSkinInput.palette.getSliceInfo()", self.shadow_cpp)

    def test_irreversible_direct_route_skins_before_light_projection(self) -> None:
        direct = self.caster.index("if ((p_flags & 0x40u) != 0u)")
        end = self.caster.index("// 非混合模式", direct)
        block = self.caster[direct:end]
        self.assertIn("(p_flags & 0x80u) != 0u", block)
        self.assertIn("tryLoadGpuSkinDirectVertex(position, uv)", block)
        self.assertLess(
            block.index("tryLoadGpuSkinDirectVertex(position, uv)"),
            block.index("position * p_mvp"),
        )
        self.assertIn("gl_Position = vec4(2.0, 2.0, 2.0, 1.0)", block)

    def test_layout_metadata_and_draw_domain_are_shader_gated(self) -> None:
        load = self.caster.index("bool tryLoadGpuSkinDirectVertex(")
        end = self.caster.index("void main()", load)
        block = self.caster[load:end]
        for token in (
            "gpuSkinMetadataMask = 0x000fff00u",
            "gpuSkinFormat2Layout1Uv1 = 0x00011200u",
            "uint(gl_VertexIndex) >= p_pad1",
            "groupSlot >= p_blendCount",
            "uint groupSlotBase = normalBase + vertexCount * 12u",
            "uint texcoord0Base = (groupSlotBase + vertexCount + 3u) & ~3u",
        ):
            self.assertIn(token, block)

    def test_shadow_and_main_use_the_same_native_3x4_sequence(self) -> None:
        sequence = (
            "positionX01 = m0 * px + m3 * py",
            "positionX2 = positionX01 + m6 * pz",
            "positionX = positionX2 + m9",
            "positionY01 = m1 * px + m4 * py",
            "positionY2 = positionY01 + m7 * pz",
            "positionY = positionY2 + m10",
            "positionZ01 = m2 * px + m5 * py",
            "positionZ2 = positionZ01 + m8 * pz",
            "positionZ = positionZ2 + m11",
        )
        for token in sequence:
            self.assertIn(token, self.caster)
            self.assertIn(token, self.main_vs)

    def test_host_marks_only_irreversible_input_as_no_fallback(self) -> None:
        self.assertIn(
            "if (draw.gpuSkinInput.irreversible)\n"
            "          pc.flags |= kShadowCasterFlagGpuSkinNoFallback;",
            self.shadow_cpp,
        )
        self.assertIn("pc.padding[1] = draw.gpuSkinInput.desc.vertexCount", self.shadow_cpp)
        self.assertIn("pc.blendCount = draw.gpuSkinInput.desc.paletteMatrixCount", self.shadow_cpp)


if __name__ == "__main__":
    unittest.main()
