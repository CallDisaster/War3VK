#!/usr/bin/env python3
"""Static safety contracts for the native doodad type=0 stamp A/B gate."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONFIG = (
    ROOT / "src" / "d3d9" / "war3" / "core" / "war3_internal_test_config.h"
)
HEADER = ROOT / "src" / "d3d9" / "war3" / "hooks" / "war3_hook_shadow.h"
SOURCE = ROOT / "src" / "d3d9" / "war3" / "hooks" / "war3_hook_shadow.cpp"


class NativeDoodadStaticStampGateStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = CONFIG.read_text(encoding="utf-8")
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        begin = cls.source.index(
            "int __fastcall Hook_Doodad_ToggleStaticStampFromObject"
        )
        end = cls.source.index(
            "// =====================================================================",
            begin,
        )
        cls.hook = cls.source[begin:end]

    def test_hook_installed_but_runtime_gate_defaults_off(self) -> None:
        self.assertRegex(
            self.config,
            r"kNativeShadowDoodadStampHookEnabled\s*=\s*true\s*;",
        )
        self.assertRegex(
            self.config,
            r"kNativeDoodadStaticStampRuntimeGateDefault\s*=\s*false\s*;",
        )
        self.assertRegex(
            self.config,
            r"kNativeShadowBlockDoodadStaticStampWhenMode1\s*=\s*false\s*;",
        )
        canonical = "DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW"
        legacy = "DXVK_WAR3_NATIVE_DOODAD_STATIC_STAMP"
        self.assertIn(canonical, self.config)
        self.assertIn(canonical, self.source)
        self.assertIn(legacy, self.source)
        self.assertLess(self.source.index(canonical), self.source.index(legacy))

    def test_only_active_gate_can_block_enable(self) -> None:
        self.assertIn(
            "const bool blocked = gateActive && enable != 0;",
            self.hook,
        )
        self.assertNotIn("GetNativeShadowMode", self.hook)

    def test_cleanup_always_calls_original_before_block_decision(self) -> None:
        cleanup = self.hook.index("if (enable == 0)")
        cleanup_call = self.hook.index(
            "return CallDoodadStaticStampOriginal", cleanup
        )
        block_decision = self.hook.index("const bool blocked =", cleanup_call)
        self.assertLess(cleanup, cleanup_call)
        self.assertLess(cleanup_call, block_decision)

    def test_type4_and_broad_shadow_paths_are_not_hooked_here(self) -> None:
        self.assertNotIn("Hook_Doodad_ToggleEmitter", self.source)
        self.assertNotRegex(
            self.source,
            r"InstallMinHook\s*\(\s*"
            r"addrs\.terrainShadowToggleEmitterStampAddr",
        )
        self.assertNotIn("RegisterImage", self.hook)
        self.assertNotIn("ListA", self.hook)
        self.assertNotIn("fog", self.hook.lower())
        self.assertNotIn("los", self.hook.lower())

    def test_summary_query_api_exposes_all_gate_counters(self) -> None:
        required = (
            "QueryDoodadStaticStampEnterCount",
            "QueryDoodadStaticStampBlockedCount",
            "QueryDoodadStaticStampPassthroughCleanupCount",
            "QueryDoodadStaticStampGateActiveCount",
        )
        for name in required:
            self.assertIn(f"uint64_t {name}();", self.header)
            self.assertIn(f"uint64_t {name}()", self.source)


if __name__ == "__main__":
    unittest.main()
