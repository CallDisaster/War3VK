"""Static contracts: shadow caster interface constants have a single source."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_interface.h"
CASTER_VERT = ROOT / "subprojects/war3fx/shaders/war3_shadow_caster_vert.vert"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"

FLAG_MACROS = {
    "WAR3_SHADOW_CASTER_FLAG_USE_BLEND": 0x1,
    "WAR3_SHADOW_CASTER_FLAG_INDEXED_BLEND": 0x2,
    "WAR3_SHADOW_CASTER_FLAG_ALPHA_TEST": 0x4,
    "WAR3_SHADOW_CASTER_FLAG_HASH_ALPHA": 0x8,
    "WAR3_SHADOW_CASTER_FLAG_STAGE1_TERRAIN": 0x10,
    "WAR3_SHADOW_CASTER_FLAG_POINT_SHADOW_LINEAR_DEPTH": 0x20,
    "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_DIRECT_INPUT": 0x40,
    "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_NO_FALLBACK": 0x80,
}
METADATA_MACROS = {
    "WAR3_SHADOW_CASTER_GPU_SKIN_OUTPUT_FORMAT_SHIFT": 8,
    "WAR3_SHADOW_CASTER_GPU_SKIN_LAYOUT_GENERATION_SHIFT": 12,
    "WAR3_SHADOW_CASTER_GPU_SKIN_UV_LAYER_COUNT_SHIFT": 16,
    "WAR3_SHADOW_CASTER_GPU_SKIN_METADATA_MASK": 0x000FFF00,
    "WAR3_SHADOW_CASTER_GPU_SKIN_FORMAT2_LAYOUT1_UV1": 0x00011200,
}
EXPECTED_MACROS = {**FLAG_MACROS, **METADATA_MACROS}

# The vertex shader consumes only these macros; the shifts and the two flags
# used by other caster stages (hash alpha, point linear depth) stay CPU-side.
SHADER_MACROS = (
    "WAR3_SHADOW_CASTER_FLAG_USE_BLEND",
    "WAR3_SHADOW_CASTER_FLAG_INDEXED_BLEND",
    "WAR3_SHADOW_CASTER_FLAG_ALPHA_TEST",
    "WAR3_SHADOW_CASTER_FLAG_STAGE1_TERRAIN",
    "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_DIRECT_INPUT",
    "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_NO_FALLBACK",
    "WAR3_SHADOW_CASTER_GPU_SKIN_METADATA_MASK",
    "WAR3_SHADOW_CASTER_GPU_SKIN_FORMAT2_LAYOUT1_UV1",
)

CPU_ALIASES = {
    "kShadowCasterFlagUseBlend": "WAR3_SHADOW_CASTER_FLAG_USE_BLEND",
    "kShadowCasterFlagIndexedBlend": "WAR3_SHADOW_CASTER_FLAG_INDEXED_BLEND",
    "kShadowCasterFlagAlphaTest": "WAR3_SHADOW_CASTER_FLAG_ALPHA_TEST",
    "kShadowCasterFlagHashAlpha": "WAR3_SHADOW_CASTER_FLAG_HASH_ALPHA",
    "kShadowCasterFlagStage1Terrain": "WAR3_SHADOW_CASTER_FLAG_STAGE1_TERRAIN",
    "kShadowCasterFlagPointShadowLinearDepth":
        "WAR3_SHADOW_CASTER_FLAG_POINT_SHADOW_LINEAR_DEPTH",
    "kShadowCasterFlagGpuSkinDirectInput":
        "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_DIRECT_INPUT",
    "kShadowCasterFlagGpuSkinNoFallback":
        "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_NO_FALLBACK",
    "kShadowCasterGpuSkinOutputFormatShift":
        "WAR3_SHADOW_CASTER_GPU_SKIN_OUTPUT_FORMAT_SHIFT",
    "kShadowCasterGpuSkinLayoutGenerationShift":
        "WAR3_SHADOW_CASTER_GPU_SKIN_LAYOUT_GENERATION_SHIFT",
    "kShadowCasterGpuSkinUvLayerCountShift":
        "WAR3_SHADOW_CASTER_GPU_SKIN_UV_LAYER_COUNT_SHIFT",
    "kShadowCasterGpuSkinMetadataMask":
        "WAR3_SHADOW_CASTER_GPU_SKIN_METADATA_MASK",
}

DEFINE_RE = re.compile(
    r"^#define\s+(WAR3_SHADOW_CASTER_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+u|\d+u)\s*$",
    re.MULTILINE,
)


def parse_header_macros(text: str) -> dict[str, int]:
    return {name: int(value[:-1], 0) for name, value in DEFINE_RE.findall(text)}


class ShadowCasterInterfaceConstantsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.caster = CASTER_VERT.read_text(encoding="utf-8")
        cls.shadow_cpp = SHADOW_CPP.read_text(encoding="utf-8")
        cls.macros = parse_header_macros(cls.header)

    def test_header_defines_exactly_the_pinned_wire_values(self) -> None:
        self.assertEqual(self.macros, EXPECTED_MACROS)
        self.assertIn("#ifndef WAR3_SHADOW_CASTER_INTERFACE_H", self.header)
        self.assertIn("#define WAR3_SHADOW_CASTER_INTERFACE_H", self.header)

    def test_header_is_pure_preprocessor_for_cxx_and_glsl(self) -> None:
        for banned in ("constexpr", "const uint", "static_assert", ";"):
            self.assertNotIn(banned, self.header)

    def test_cpu_side_aliases_the_shared_header(self) -> None:
        self.assertIn(
            '#include "../../subprojects/war3fx/shaders/'
            'war3_shadow_caster_interface.h"',
            self.shadow_cpp,
        )
        for alias, macro in CPU_ALIASES.items():
            self.assertIn(
                f"constexpr uint32_t {alias} = {macro};",
                self.shadow_cpp,
                alias,
            )
        for token in (
            "static_assert(kShadowCasterFlagGpuSkinDirectInput == 0x40u);",
            "static_assert(kShadowCasterFlagGpuSkinNoFallback == 0x80u);",
            "static_assert(kShadowCasterGpuSkinMetadataMask == 0x000fff00u);",
            "static_assert(PackShadowCasterGpuSkinMetadata(2u, 1u, 1u) == 0x00011200u);",
            "WAR3_SHADOW_CASTER_GPU_SKIN_FORMAT2_LAYOUT1_UV1);",
        ):
            self.assertIn(token, self.shadow_cpp)

    def test_cpu_pack_uses_the_shared_shift_constants(self) -> None:
        pack = self.shadow_cpp.index("constexpr uint32_t PackShadowCasterGpuSkinMetadata(")
        pack_end = self.shadow_cpp.index("}", pack)
        block = self.shadow_cpp[pack:pack_end]
        for shift in (
            "kShadowCasterGpuSkinOutputFormatShift",
            "kShadowCasterGpuSkinLayoutGenerationShift",
            "kShadowCasterGpuSkinUvLayerCountShift",
        ):
            self.assertIn(shift, block)
        packed = (
            (2 << self.macros["WAR3_SHADOW_CASTER_GPU_SKIN_OUTPUT_FORMAT_SHIFT"])
            | (1 << self.macros["WAR3_SHADOW_CASTER_GPU_SKIN_LAYOUT_GENERATION_SHIFT"])
            | (1 << self.macros["WAR3_SHADOW_CASTER_GPU_SKIN_UV_LAYER_COUNT_SHIFT"])
        )
        self.assertEqual(
            packed, self.macros["WAR3_SHADOW_CASTER_GPU_SKIN_FORMAT2_LAYOUT1_UV1"]
        )
        self.assertEqual(packed, 0x00011200)
        self.assertEqual(
            packed & self.macros["WAR3_SHADOW_CASTER_GPU_SKIN_METADATA_MASK"], packed
        )

    def test_shader_side_uses_the_shared_header(self) -> None:
        self.assertIn('#include "war3_shadow_caster_interface.h"', self.caster)
        for macro in SHADER_MACROS:
            self.assertIn(macro, self.caster, macro)
        for removed in (
            "gpuSkinDirectFlag",
            "gpuSkinNoFallbackFlag",
            "gpuSkinMetadataMask",
            "gpuSkinFormat2Layout1Uv1",
            "(p_flags & 0x1u)",
            "(p_flags & 0x2u)",
            "(p_flags & 0x4u)",
            "(p_flags & 0x10u)",
            "(p_flags & 0x40u)",
            "(p_flags & 0x80u)",
            "0x000fff00u",
            "0x00011200u",
        ):
            self.assertNotIn(removed, self.caster, removed)

    def test_shader_flag_checks_match_the_pinned_flags(self) -> None:
        for macro in (
            "WAR3_SHADOW_CASTER_FLAG_USE_BLEND",
            "WAR3_SHADOW_CASTER_FLAG_INDEXED_BLEND",
            "WAR3_SHADOW_CASTER_FLAG_ALPHA_TEST",
            "WAR3_SHADOW_CASTER_FLAG_STAGE1_TERRAIN",
            "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_DIRECT_INPUT",
            "WAR3_SHADOW_CASTER_FLAG_GPU_SKIN_NO_FALLBACK",
        ):
            self.assertIn(f"(p_flags & {macro})", self.caster, macro)


if __name__ == "__main__":
    unittest.main()
