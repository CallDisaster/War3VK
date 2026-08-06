#!/usr/bin/env python3
"""Static contracts for profiler GPU union and exact shadow route telemetry."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PERF_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(
    encoding="utf-8"
)
PERF_CPP = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)
SHADOW_CPP = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8"
)
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8"
)
DIAG_H = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h").read_text(
    encoding="utf-8"
)
DIAG_CPP = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp").read_text(
    encoding="utf-8"
)
CONTROL = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(
    encoding="utf-8"
)
AUTOTEST = (ROOT / "AutoTest/war3_autotest_mcp.py").read_text(encoding="utf-8")


class PerfGpuUnionAndShadowRouteContracts(unittest.TestCase):
    def test_gpu_total_is_interval_union_not_nested_scope_sum(self) -> None:
        self.assertIn("gpuTimestampIntervals", PERF_H)
        self.assertIn("GpuTimestampIntervalUnionMs", PERF_CPP)
        self.assertIn("sorted[i][0] <= end", PERF_CPP)
        archive = PERF_CPP[PERF_CPP.index("void War3PerfMonitor::archiveFrame()") :]
        archive = archive[: archive.index("void War3PerfMonitor::resetHistory()")]
        self.assertIn("GpuTimestampIntervalUnionMs", archive)
        self.assertNotIn("totalGpu += s.gpuSumMs", archive)

    def test_late_gpu_results_recompute_original_frame_union(self) -> None:
        tick = PERF_CPP[PERF_CPP.index("void War3PerfMonitor::tick()") :]
        tick = tick[: tick.index("void War3PerfMonitor::archiveFrame()")]
        self.assertIn("frameIt->gpuTimestampIntervals.push_back", tick)
        self.assertIn("frameIt->totalGpuMs = GpuTimestampIntervalUnionMs", tick)

    def test_gpu_direct_input_is_classified_as_skinned(self) -> None:
        self.assertGreaterEqual(
            SHADOW_CPP.count(
                "draw.vertexBlendEnabled || draw.gpuSkinInput.valid"
            ),
            2,
        )

    def test_final_exact_owner_rejects_are_accounted(self) -> None:
        gate = DEVICE_CPP[
            DEVICE_CPP.index("// Defensive final ownership gate") :
            DEVICE_CPP.index("War3FallbackAppendRawTiming", DEVICE_CPP.index("// Defensive final ownership gate"))
        ]
        self.assertGreaterEqual(
            gate.count("drawTimeSemanticProducerOwnedDirectGroupedSkipCount++"),
            3,
        )

    def test_runtime_status_exports_route_counters(self) -> None:
        names = (
            "drawTimeSemanticProducerOwnedDirectGroupedSkipCount",
            "gpuSkinVsShadowDirectAttempts",
            "gpuSkinVsShadowDirectInputRejects",
            "gpuSkinVsShadowDirectStateRejects",
            "gpuSkinVsShadowDirectDrawsSubmitted",
            "gpuSkinVsShadowReplayDirectional",
            "gpuSkinVsShadowReplayPoint",
        )
        for name in names:
            self.assertIn(name, DIAG_H)
            self.assertIn(f'{{"{name}"', DIAG_CPP)
            self.assertIn(f'result["{name}"]', CONTROL)

    def test_life_and_death_gate_observes_before_consume(self) -> None:
        start = AUTOTEST.index("def run_life_and_death_tdr_scenario(")
        block = AUTOTEST[start : AUTOTEST.index("def ", start + 10)]
        self.assertIn(
            'user_env.setdefault("DXVK_WAR3_SEMANTIC_COMPACT_WORK_TABLE", "1")',
            block,
        )
        self.assertIn(
            'user_env.setdefault("DXVK_WAR3_SEMANTIC_DIRECT_PHASE_BREAKDOWN", "1")',
            block,
        )
        self.assertIn(
            'user_env.setdefault("DXVK_WAR3_SEMANTIC_BUILD_ELIGIBLE_TRACE_PERIOD", "64")',
            block,
        )


if __name__ == "__main__":
    unittest.main()
