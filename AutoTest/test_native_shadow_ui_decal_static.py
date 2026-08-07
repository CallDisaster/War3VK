#!/usr/bin/env python3
"""Static contracts that keep Warcraft selection and marker decals visible."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONFIG = (
    ROOT / "src" / "d3d9" / "war3" / "core" / "war3_internal_test_config.h"
)
SOURCE = ROOT / "src" / "d3d9" / "war3" / "hooks" / "war3_hook_shadow.cpp"


def function_body(source: str, signature: str, next_marker: str) -> str:
    begin = source.index(signature)
    end = source.index(next_marker, begin)
    return source[begin:end]


class NativeShadowUiDecalStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = CONFIG.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.layer = function_body(
            cls.source,
            "void __fastcall Hook_Terrain_RenderShadowLayer",
            "int __fastcall Hook_Terrain_RenderListA",
        )
        cls.list_b = function_body(
            cls.source,
            "void __fastcall Hook_Terrain_RenderListB",
            "// 阴影贴图写入链路上游拦截",
        )

    def test_selection_and_marker_types_are_explicitly_preserved(self) -> None:
        self.assertRegex(
            self.config,
            r"kNativeShadowListBPreserveUiDecalsByDefault\s*=\s*true\s*;",
        )
        self.assertIn("argType == 1 || argType == 2", self.list_b)
        self.assertIn("PreserveUiDecalType1Or2", self.list_b)

    def test_ui_decal_allow_precedes_broad_list_b_block(self) -> None:
        preserve = self.list_b.index("if (preserveUiDecalByDefault)")
        broad_block = self.list_b.index("kNativeShadowListBBlockAllByDefault")
        self.assertLess(preserve, broad_block)

    def test_shadow_layer_never_bulk_disables_list_b(self) -> None:
        self.assertNotRegex(self.layer, r"\ba3\s*=\s*0\s*;")
        self.assertIn("if (mode >= 2u)", self.layer)
        self.assertRegex(self.layer, r"\ba2\s*=\s*0\s*;")

    def test_precise_unit_and_building_shadow_producer_gates_remain(self) -> None:
        for name in (
            "kNativeShadowBlockCUnitUiUnitShadowByDefault",
            "kNativeShadowBlockCUnitUiBuildingShadowByDefault",
        ):
            self.assertRegex(self.config, rf"{name}\s*=\s*true\s*;")


if __name__ == "__main__":
    unittest.main()
