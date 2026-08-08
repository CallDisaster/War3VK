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

    def test_perf_report_closes_draw_time_producer_ledger(self) -> None:
        names = (
            "drawTimeSemanticProducerVisibleCandidateCount",
            "drawTimeSemanticProducerFreshEntryCount",
            "drawTimeSemanticProducerClaimedCount",
            "drawTimeSemanticProducerSubmittedCount",
            "drawTimeSemanticProducerMissNoFreshEntryCount",
            "drawTimeSemanticProducerFallbackCurrentDrawCount",
            "drawTimeSemanticProducerOwnedDirectGroupedSkipCount",
            "drawTimeSemanticProducerLifecycleMergedCount",
        )
        for name in names:
            self.assertIn(f"uint64_t {name} = 0", PERF_H)
            self.assertIn(f"agg.{name} +=", PERF_CPP)
            # Both shadowBudgetSummary and shadowRuntimeV2Summary export the
            # same closed ledger for report-side ratio checks.
            self.assertEqual(PERF_CPP.count(f'\\"{name}\\"'), 2)

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

    def test_visible_index_slice_reuse_requires_authoritative_generation(self) -> None:
        start = DEVICE_CPP.index(
            "class War3CurrentDrawVisibleIndexSliceCache final"
        )
        end = DEVICE_CPP.index(
            "bool War3TryAttachCurrentDrawVisibleIndexSlice", start
        )
        cache = DEVICE_CPP[start:end]
        self.assertIn("static thread_local std::array<Entry", cache)
        self.assertIn(
            "DXVK_WAR3_CURRENT_DRAW_GENERATION_INDEX_SLICE_CACHE",
            DEVICE_CPP,
        )
        self.assertIn(
            '"DXVK_WAR3_CURRENT_DRAW_GENERATION_INDEX_SLICE_CACHE", 0u',
            DEVICE_CPP,
        )
        self.assertIn("entry.mapEpoch == geoset->mapEpoch", cache)
        self.assertIn(
            "entry.immutableModelGeneration == geoset->immutableModelGeneration",
            cache,
        )
        self.assertIn("geoset->readyForShadowConsumer()", cache)
        self.assertNotIn("contentHash ==", cache)
        self.assertNotIn("memcmp", cache)

        find_start = DEVICE_CPP.index("War3FindDirectPacketGeosetResource(")
        find_end = DEVICE_CPP.index(
            "War3GetDirectPacketGeosetResource(", find_start
        )
        find = DEVICE_CPP[find_start:find_end]
        self.assertIn("it->second->mapEpoch != currentMapEpoch", find)

        get_end = DEVICE_CPP.index(
            "bool War3SemanticMaterialSignatureCacheRuntime()", find_end
        )
        get = DEVICE_CPP[find_end:get_end]
        self.assertGreaterEqual(
            get.count("it->second->immutableModelGeneration"), 2
        )


if __name__ == "__main__":
    unittest.main()
