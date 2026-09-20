#!/usr/bin/env python3
"""Static contracts for P2 batch 0 native static-shadow producer observability.

This batch only lands counters, Query APIs, and dormant DecideRegisterImage
branches. Hooks stay uninstalled and default-block flags stay false.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONFIG = (
    ROOT / "src" / "d3d9" / "war3" / "core" / "war3_internal_test_config.h"
)
HEADER = ROOT / "src" / "d3d9" / "war3" / "hooks" / "war3_hook_shadow.h"
SOURCE = ROOT / "src" / "d3d9" / "war3" / "hooks" / "war3_hook_shadow.cpp"
POLICY = (
    ROOT / "src" / "d3d9" / "war3" / "hooks" / "war3_shadow_filter_policy.cpp"
)
BRIDGE_H = (
    ROOT / "src" / "d3d9" / "war3" / "render" / "war3_shadow_runtime_bridge.h"
)
BRIDGE_CPP = (
    ROOT / "src" / "d3d9" / "war3" / "render" / "war3_shadow_runtime_bridge.cpp"
)
CONTROL = ROOT / "src" / "d3d9" / "war3" / "tools" / "war3_control_plane.cpp"
PERF = ROOT / "src" / "d3d9" / "war3" / "tools" / "war3_perf_monitor.cpp"

QUERY_APIS = (
    "QueryShadowRegisterImageEnterCount",
    "QueryShadowRegisterImageBlockedCount",
    "QueryShadowRegisterImageStaticStampCount",
    "QueryShadowRegisterImageEmitterStampCount",
    "QueryShadowRegisterImageSelectionCount",
    "QueryShadowRegisterImageOcclusionCount",
    "QueryShadowRegisterImageWithParamsCount",
    "QueryShadowRegisterImageObjectBridgeCount",
    "QueryShadowRegisterImageFromPointCount",
    "QueryShadowRegisterImageFromTwoPointsCount",
    "QueryShadowRegisterImageUnknownSourceCount",
    "QueryShadowPathStaticStampEnterCount",
    "QueryShadowPathStaticStampBlockedCount",
    "QueryShadowPathStaticStampPassthroughCleanupCount",
)


class NativeStaticShadowProducerObservabilityStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = CONFIG.read_text(encoding="utf-8")
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.policy = POLICY.read_text(encoding="utf-8")
        cls.bridge_h = BRIDGE_H.read_text(encoding="utf-8")
        cls.bridge_cpp = BRIDGE_CPP.read_text(encoding="utf-8")
        cls.control = CONTROL.read_text(encoding="utf-8")
        cls.perf = PERF.read_text(encoding="utf-8")
        begin = cls.source.index(
            "void __fastcall Hook_ShadowPath_StaticStamp_Toggle"
        )
        end = cls.source.index(
            "int __fastcall Hook_ShadowProjector_Add_FromObject",
            begin,
        )
        cls.stamp_hook = cls.source[begin:end]
        decide_begin = cls.policy.index(
            "ShadowRegisterDecision DecideRegisterImage"
        )
        decide_end = cls.policy.index(
            "bool ReadAsciiCStringSafe",
            decide_begin,
        )
        cls.decide = cls.policy[decide_begin:decide_end]

    def test_fourteen_query_api_symbols_exist(self) -> None:
        self.assertEqual(14, len(QUERY_APIS))
        for name in QUERY_APIS:
            self.assertIn(f"uint64_t {name}();", self.header)
            self.assertIn(f"uint64_t {name}()", self.source)

    def test_producer_hooks_remain_compile_time_disabled(self) -> None:
        self.assertRegex(
            self.config,
            r"kNativeShadowRegisterImageHookEnabled\s*=\s*false\s*;",
        )
        self.assertRegex(
            self.config,
            r"kNativeShadowStaticStampPathHookEnabled\s*=\s*false\s*;",
        )
        self.assertIn(
            "if constexpr (dxvk::war3::internal::kNativeShadowRegisterImageHookEnabled)",
            self.source,
        )
        self.assertIn(
            "kNativeShadowStaticStampPathHookEnabled) {",
            self.source,
        )

    def test_default_block_flags_stay_dormant_false(self) -> None:
        self.assertRegex(
            self.config,
            r"kNativeShadowBlockStaticStampPathByDefault\s*=\s*false\s*;",
        )
        self.assertRegex(
            self.config,
            r"kNativeShadowRegisterBlockStaticStampByDefault\s*=\s*false\s*;",
        )
        self.assertRegex(
            self.config,
            r"kNativeShadowRegisterBlockShadowTextureKeyByDefault\s*=\s*false\s*;",
        )
        self.assertRegex(
            self.config,
            r"kNativeDoodadStaticStampRuntimeGateDefault\s*=\s*false\s*;",
        )

    def test_static_stamp_hook_counts_enter_cleanup_and_block(self) -> None:
        self.assertIn(
            "g_shadowPathStaticStampEnterCount.fetch_add",
            self.stamp_hook,
        )
        self.assertIn("if (enable == 0)", self.stamp_hook)
        self.assertIn(
            "g_shadowPathStaticStampPassthroughCleanupCount.fetch_add",
            self.stamp_hook,
        )
        self.assertIn(
            "CallShadowPathStaticStampToggleOriginal",
            self.stamp_hook,
        )
        self.assertIn(
            "g_shadowPathStaticStampBlockedCount.fetch_add",
            self.stamp_hook,
        )
        cleanup = self.stamp_hook.index("if (enable == 0)")
        cleanup_count = self.stamp_hook.index(
            "g_shadowPathStaticStampPassthroughCleanupCount.fetch_add",
            cleanup,
        )
        cleanup_call = self.stamp_hook.index(
            "CallShadowPathStaticStampToggleOriginal",
            cleanup_count,
        )
        block_count = self.stamp_hook.index(
            "g_shadowPathStaticStampBlockedCount.fetch_add",
            cleanup_call,
        )
        self.assertLess(cleanup, cleanup_count)
        self.assertLess(cleanup_count, cleanup_call)
        self.assertLess(cleanup_call, block_count)

    def test_filter_policy_has_slash_and_backslash_key_branches(self) -> None:
        self.assertIn(
            r'replaceabletextures\\shadows\\',
            self.policy,
        )
        self.assertIn("replaceabletextures/shadows/", self.policy)
        self.assertIn(
            r'replaceabletextures\\selection\\',
            self.policy,
        )
        self.assertIn("replaceabletextures/selection/", self.policy)
        whitelist = self.decide.index("Default_WhitelistSource")
        selection = self.decide.index("Default_AllowSelectionTextureKey")
        static_stamp = self.decide.index("Default_BlockStaticStamp")
        texture_key = self.decide.index("Default_BlockShadowTextureKey")
        mode_gate = self.decide.index("if (ctx.mode < 1u)")
        self.assertLess(whitelist, selection)
        self.assertLess(selection, static_stamp)
        self.assertLess(static_stamp, texture_key)
        self.assertLess(texture_key, mode_gate)
        early_mode = self.decide[:whitelist]
        self.assertNotIn("if (ctx.mode < 1u)", early_mode)

    def test_summary_export_wires_the_query_counters(self) -> None:
        fields = (
            "registerImageEnterCount",
            "registerImageBlockedCount",
            "registerImageStaticStampCount",
            "registerImageEmitterStampCount",
            "registerImageSelectionCount",
            "registerImageOcclusionCount",
            "registerImageWithParamsCount",
            "registerImageObjectBridgeCount",
            "registerImageFromPointCount",
            "registerImageFromTwoPointsCount",
            "registerImageUnknownSourceCount",
            "staticStampPathEnterCount",
            "staticStampPathBlockedCount",
            "staticStampPathCleanupCount",
        )
        for field in fields:
            self.assertIn(f"uint64_t {field} = 0;", self.bridge_h)
            self.assertIn(f"summary.{field} =", self.bridge_cpp)
            self.assertIn(field, self.control)
            self.assertIn(field, self.perf)
            self.assertIn(f"runtimeSummary.{field}", self.perf)


if __name__ == "__main__":
    unittest.main()
