from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
PERF = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)


class Stage11AllocationObserverStaticTest(unittest.TestCase):
    def test_observer_is_development_build_only(self) -> None:
        runtime = DEVICE[
            DEVICE.index("inline bool War3Stage11AllocationObserverRuntime") :
            DEVICE.index("enum class War3ProducerClaimObserveMode")
        ]
        self.assertIn("kDevelopmentShadowObserversEnabled", runtime)
        self.assertIn("DXVK_WAR3_STAGE11_ALLOC_OBSERVER", runtime)
        self.assertIn("== 1u", runtime)
        self.assertIn("WARVK_ENABLE_SHADOW_OBSERVERS_DEV", DEVICE_H)

    def test_allocation_reasons_are_counted_before_budget_defer(self) -> None:
        begin = DEVICE.index("const bool needsNewPositionBuffer")
        end = DEVICE.index("if (needsNewPositionBuffer) {", begin)
        block = DEVICE[begin:end]
        self.assertIn("ClassifyWar3Stage11PositionAllocation", block)
        for name in (
            "drawTimePositionAllocNewEntryCount",
            "drawTimePositionAllocMissingBackingCount",
            "drawTimePositionAllocCapacityGrowthCount",
            "drawTimePositionAllocLeaseDetachCount",
            "drawTimePositionAllocStaticRequestCount",
            "drawTimePositionAllocDynamicRequestCount",
        ):
            self.assertIn(name, block)
        defer = DEVICE[
            DEVICE.index("m_war3DrawTimeVBCacheBudgetDeferredCount++", begin) :
            DEVICE.index("entry.positionInfo = {};", begin)
        ]
        self.assertIn("drawTimePositionDeferredNewEntryCount", defer)
        self.assertIn("drawTimePositionDeferredCapacityGrowthCount", defer)

    def test_generation_proof_observe_does_not_mutate_backing(self) -> None:
        begin = DEVICE.index("drawTimeAllocObserverEnabled = 1u")
        end = DEVICE.index("const VkPrimitiveTopology captureTopology", begin)
        block = DEVICE[begin:end]
        self.assertIn("currentPositionSourceProof.valid()", block)
        self.assertIn("drawTimePositionProofDuplicateBytes", block)
        self.assertNotIn("createBuffer", block)
        self.assertNotIn("positionBuffer =", block)
        self.assertNotIn("EmitCs", block)

    def test_diagnostics_reach_runtime_and_perf_json(self) -> None:
        for name in (
            "drawTimePositionAllocRequestCount",
            "drawTimePositionProofUniqueCount",
            "drawTimePositionProofDuplicateCount",
            "drawTimePositionProofInvalidCount",
            "drawTimePositionProofSetOverflowCount",
        ):
            self.assertIn(name, SCENE)
            self.assertGreaterEqual(PERF.count(name), 3)


if __name__ == "__main__":
    unittest.main()
