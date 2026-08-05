"""Pure static/mock checks for the partial RenderPerf hook catalog pilot.

This test intentionally does not load d3d9.dll, initialize MinHook, build the
project, or launch Warcraft III. It protects the catalog contract while the
central build/crash gate remains owned by the main agent.
"""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
UTIL_H = ROOT / "src/d3d9/war3/hooks/war3_hook_install_util.h"
UTIL_CPP = ROOT / "src/d3d9/war3/hooks/war3_hook_install_util.cpp"
RENDER_CPP = ROOT / "src/d3d9/war3/hooks/war3_hook_render.cpp"
MONITOR_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
TEMPLATE_H = ROOT / "src/d3d9/war3/tools/war3_perf_report_template.h"
HOT_H = ROOT / "src/d3d9/war3/hooks/war3_hook_perf.h"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
SHADOW_HOOK_CPP = ROOT / "src/d3d9/war3/hooks/war3_hook_shadow.cpp"
NATIVE_HINT_CPP = ROOT / "src/d3d9/war3/native/war3_native_shadow_hint.cpp"


CATALOG_STATES = (
    "Installed",
    "DisabledByCompileConfig",
    "DisabledByEnvironment",
    "SkippedUnsafeABI",
    "AddressUnavailable",
    "InstallFailed",
)


def merge_mock(current: str, installed: bool, incoming: str) -> tuple[str, str, bool]:
    """Mirror the tiny state merge contract, independent of MinHook."""
    last_attempt = incoming
    if incoming == "Installed":
        return "Installed", last_attempt, True
    if not installed:
        current = incoming
    return current, last_attempt, installed


class HookCatalogStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.util_h = UTIL_H.read_text(encoding="utf-8")
        cls.util_cpp = UTIL_CPP.read_text(encoding="utf-8")
        cls.render_cpp = RENDER_CPP.read_text(encoding="utf-8")
        cls.monitor_cpp = MONITOR_CPP.read_text(encoding="utf-8")
        cls.template_h = TEMPLATE_H.read_text(encoding="utf-8")
        cls.hot_h = HOT_H.read_text(encoding="utf-8")
        cls.device_cpp = DEVICE_CPP.read_text(encoding="utf-8")
        cls.shadow_hook_cpp = SHADOW_HOOK_CPP.read_text(encoding="utf-8")
        cls.native_hint_cpp = NATIVE_HINT_CPP.read_text(encoding="utf-8")

    def test_six_exported_states_are_declared_and_named(self) -> None:
        for state in CATALOG_STATES:
            self.assertIn(state, self.util_h)
            self.assertIn(f'return "{state}"', self.util_cpp)

    def test_each_noninstalled_state_is_observable_before_install(self) -> None:
        for state in CATALOG_STATES[1:]:
            current, last_attempt, installed = merge_mock(
                "NotEvaluated", False, state
            )
            self.assertEqual(state, current)
            self.assertEqual(state, last_attempt)
            self.assertFalse(installed)

    def test_installed_is_sticky_but_last_attempt_remains_visible(self) -> None:
        current, last_attempt, installed = merge_mock(
            "NotEvaluated", False, "Installed"
        )
        self.assertEqual("Installed", current)
        self.assertTrue(installed)
        for state in CATALOG_STATES[1:]:
            current, last_attempt, installed = merge_mock(
                current, installed, state
            )
            self.assertEqual("Installed", current)
            self.assertEqual(state, last_attempt)
            self.assertTrue(installed)
        self.assertIn("else if (!runtime.installed)", self.util_cpp)
        self.assertIn(
            "if (!runtime.installed || state ==", self.util_cpp
        )

    def test_renderperf_ids_are_explicit_unique_and_partial(self) -> None:
        values = re.findall(
            r"^\s*RenderPerf\w+\s*=\s*(0x[0-9a-fA-F]+)u?,?\s*$",
            self.util_h,
            flags=re.MULTILINE,
        )
        self.assertEqual(32, len(values))
        self.assertEqual(len(values), len(set(values)))
        self.assertNotIn("RenderPerfCount", self.util_h)
        self.assertIn('\\"mode\\": \\"partial-pilot\\"', self.monitor_cpp)
        self.assertIn('\\"complete\\": false', self.monitor_cpp)

    def test_unsafe_abi_entry_is_catalog_only(self) -> None:
        self.assertIn("WorldPrepare_UnsafeImplicitEdi_368E90", self.render_cpp)
        self.assertIn("0x368E90u", self.render_cpp)
        self.assertIn("SkippedUnsafeABI", self.render_cpp)
        self.assertNotRegex(
            self.render_cpp,
            r"InstallMinHook\s*\(\s*[^;]*UnsafeImplicitEdi",
        )

    def test_catalog_is_separate_from_hotspot_inventory(self) -> None:
        self.assertIn('\\"hookCatalog\\": [', self.monitor_cpp)
        self.assertIn('\\"hookInventory\\": [', self.monitor_cpp)
        self.assertIn('data-view="catalog"', self.template_h)
        start = self.template_h.index("function collectHookBreakdownRows()")
        end = self.template_h.index(
            "// ---------- Static Hook Catalog", start
        )
        hook_breakdown = self.template_h[start:end]
        self.assertNotIn("hookCatalog.forEach", hook_breakdown)
        self.assertIn("function renderHookCatalog()", self.template_h)

    def test_current_draw_synthetic_phases_keep_meaningful_self_names(self) -> None:
        phases = (
            "CurrentDrawContextGate",
            "CurrentDrawRecordSeed",
            "CurrentDrawVisibleBackfill",
            "CurrentDrawFrameIdentity",
            "CurrentDrawBindFieldRefresh",
            "CurrentDrawPublishContract",
            "CurrentDrawPublishLocalGateCache",
            "CurrentDrawPublishTrustedPaletteQueryPack",
            "CurrentDrawPublishSnapshotCommit",
            "CurrentDrawPublishGlobalMaps",
        )
        for phase in phases:
            self.assertIn(f"War3HotHookId::{phase}", self.hot_h)
        # Synthetic nodes own their measured self time. Their descendants omit
        # a non-existent generic WarVKHookLogic segment from parentPath.
        self.assertIn(
            "if (!War3HotHookIsSyntheticPhase(parentId))", self.hot_h
        )
        self.assertIn(
            "if (!War3HotHookIsSyntheticPhase(id))", self.hot_h
        )
        self.assertIn("kWar3HotHookPathCapacity = 192u", self.hot_h)
        self.assertIn("Profiler_HotHookPathOverflow", self.hot_h)
        self.assertIn("Profiler_HotHookPathNearCapacity", self.hot_h)

    def test_shadow_draw_time_breakdown_is_sampled_and_exactly_normalized(self) -> None:
        phases = (
            "IdentityResolve",
            "GpuSkinInput",
            "PositionSource",
            "MarkerGatesAndBounds",
            "FingerprintAndDedup",
            "CacheRecordSetup",
            "PositionBacking",
            "UvBacking",
            "IndexBacking",
            "FinalizeAccounting",
            "GpuSkinSettlement",
        )
        self.assertIn(
            '"DXVK_WAR3_SHADOW_DRAWTIME_BREAKDOWN"', self.monitor_cpp
        )
        enum_match = re.search(
            r"enum class War3ShadowDrawTimeCapturePhase.*?\{(.*?)Count,",
            self.device_cpp,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(enum_match)
        enum_body = enum_match.group(1)
        for phase in phases:
            self.assertIn(phase, enum_body)

        capture_start = self.device_cpp.index(
            "void D3D9DeviceEx::War3TryCaptureShadowCaster("
        )
        capture_end = self.device_cpp.index(
            "shadowCaptureGateTiming.enter(\n"
            "        War3ShadowCaptureGatePhase::SemanticExit);",
            capture_start,
        )
        capture = self.device_cpp[capture_start:capture_end]
        positions = [
            capture.index(f"War3ShadowDrawTimeCapturePhase::{phase}")
            for phase in phases
        ]
        self.assertEqual(positions, sorted(positions))
        self.assertIn(
            "traceShadowCaptureGates || traceShadowDrawTimeCapture",
            capture,
        )
        self.assertIn(
            "traceShadowDrawTimeCapture && traceThisShadowCaptureChild",
            capture,
        )

        flush_start = self.device_cpp.index(
            "static void War3FlushCaptureCpuTlsToPerf()"
        )
        flush_end = self.device_cpp.index(
            "VkDeviceSize War3AlignPersistentBytes", flush_start
        )
        flush = self.device_cpp[flush_start:flush_end]
        self.assertIn(
            "double(drawTimeInGatesTicks) / "
            "double(sampledDrawTimePhaseTicks)",
            flush,
        )
        self.assertIn(
            '"ShadowCapture/Gates/DrawTimeCapture"', flush
        )

    def test_native_hint_producerless_skip_precedes_lookup_and_profiler(self) -> None:
        self.assertIn(
            '"DXVK_WAR3_NATIVE_HINT_PRODUCERLESS_SKIP"', self.monitor_cpp
        )
        self.assertIn(
            "kWar3ShadowProjectorNativeHintEnabled", self.device_cpp
        )
        start = self.device_cpp.index("bool War3TryResolveNativeShadowHint(")
        end = self.device_cpp.index(
            "War3FindRenderObjectForSemantic(", start
        )
        resolver = self.device_cpp[start:end]
        skip = resolver.index("War3NativeHintProducerlessSkipRuntime()")
        scope = resolver.index("War3PerDrawSemanticScope")
        registry = resolver.index("War3NativeShadowHintRegistry::instance()")
        self.assertLess(skip, scope)
        self.assertLess(skip, registry)
        # The producerless miss must retain the legacy API contract: neither
        # path writes outHint before a successful registry hit.
        self.assertNotIn("outHint =", resolver[:scope])
        self.assertIn(
            "if constexpr (dxvk::war3::internal::\n"
            "                    kWar3ShadowProjectorNativeHintEnabled)",
            self.device_cpp,
        )
        self.assertIn(
            "} else if (!War3NativeHintProducerlessSkipRuntime()) {",
            self.device_cpp,
        )

    def test_all_native_hint_producers_share_the_compile_time_gate(self) -> None:
        source_root = ROOT / "src/d3d9"
        producer_sites: list[tuple[pathlib.Path, str]] = []
        pattern = re.compile(r"\.(recordFromObject|recordSimple)\s*\(")
        for path in source_root.rglob("*.cpp"):
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in pattern.finditer(text):
                producer_sites.append((path, text[max(0, match.start() - 240) : match.end()]))
        self.assertEqual(2, len(producer_sites), producer_sites)
        for path, context in producer_sites:
            self.assertEqual(SHADOW_HOOK_CPP, path)
            self.assertIn(
                "kWar3ShadowProjectorNativeHintEnabled", context
            )
        # The registry has no alternate public/private insertion path.
        self.assertEqual(2, self.native_hint_cpp.count("storeHintLocked(hint);"))
        self.assertEqual(
            1,
            self.native_hint_cpp.count(
                "void War3NativeShadowHintRegistry::storeHintLocked("
            ),
        )


if __name__ == "__main__":
    unittest.main()
