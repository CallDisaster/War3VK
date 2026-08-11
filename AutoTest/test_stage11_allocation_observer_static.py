from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
PERF = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)
RUNTIME = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
PIN_POLICY = (
    ROOT / "src/d3d9/war3/render/war3_shadow_pinned_upload_policy.h"
).read_text(encoding="utf-8")


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

    def test_direct_source_candidate_is_development_only_and_exact(self) -> None:
        runtime = DEVICE[
            DEVICE.index("inline bool War3Stage11DirectStaticSourceRuntime") :
            DEVICE.index("enum class War3ProducerClaimObserveMode")
        ]
        self.assertIn("kDevelopmentShadowObserversEnabled", runtime)
        self.assertIn("DXVK_WAR3_STAGE11_DIRECT_STATIC_SOURCE_MODE", runtime)
        begin = DEVICE.index("directStaticPositionAllocation")
        end = DEVICE.index("GenerationBackedStreamProof currentIndexSourceProof", begin)
        gate = DEVICE[begin:end]
        self.assertIn("generationBackedStaticCandidate", gate)
        self.assertIn("!DynamicSysmemVBOs", gate)
        self.assertIn("currentPositionSourceProof.valid()", gate)
        self.assertIn("posSlice.buffer() != nullptr", gate)

    def test_direct_source_binding_skips_copy_and_allocation(self) -> None:
        begin = DEVICE.index("const bool needsNewPositionBuffer")
        end = DEVICE.index("War3ShadowDrawTimeCapturePhase::UvBacking", begin)
        body = DEVICE[begin:end]
        self.assertIn("!directStaticPositionSource", body)
        self.assertIn("drawTimeDirectStaticPositionBindCount", DEVICE)
        self.assertIn("directStaticIndexSource", DEVICE)
        self.assertIn("drawTimeDirectStaticIndexBindCount", DEVICE)

    def test_pinned_direct_upload_is_exact_and_retained(self) -> None:
        runtime = DEVICE[
            DEVICE.index("inline bool War3Stage11DirectUploadSourceRuntime") :
            DEVICE.index("enum class War3ProducerClaimObserveMode")
        ]
        self.assertIn("kDevelopmentShadowObserversEnabled", runtime)
        self.assertIn("DXVK_WAR3_STAGE11_DIRECT_UPLOAD_SOURCE_MODE", runtime)
        self.assertIn("DynamicSysmemVBOs", DEVICE)
        self.assertIn("currentPositionSourceProof.valid()", DEVICE)
        self.assertIn("m_war3PerDrawUpload.storage", DEVICE)
        self.assertIn("positionPinnedAllocation", DEVICE_H)
        self.assertIn("positionPinnedAllocation", SCENE)
        self.assertGreaterEqual(SHADOW.count("ctx->track(draw.positionPinnedAllocation)"), 3)
        self.assertIn("requestedBytes > allocationBytes - localOffset", PIN_POLICY)

    def test_direct_upload_skips_secondary_alloc_and_copy(self) -> None:
        begin = DEVICE.index("const bool needsNewPositionBuffer")
        end = DEVICE.index("War3ShadowDrawTimeCapturePhase::UvBacking", begin)
        body = DEVICE[begin:end]
        self.assertIn("!directUploadPositionSource", body)
        self.assertIn("drawTimeDirectUploadPositionBindCount", DEVICE)
        self.assertIn("directUploadIndexSource", DEVICE)
        self.assertIn("drawTimeDirectUploadIndexBindCount", DEVICE)
        self.assertIn("directUploadUvSource", DEVICE)
        self.assertIn("drawTimeDirectUploadUvBindCount", DEVICE)

    def test_diagnostics_reach_runtime_and_perf_json(self) -> None:
        for name in (
            "drawTimePositionAllocRequestCount",
            "drawTimePositionProofUniqueCount",
            "drawTimePositionProofDuplicateCount",
            "drawTimePositionProofInvalidCount",
            "drawTimePositionProofSetOverflowCount",
            "drawTimeDirectUploadPositionBindCount",
            "drawTimeDirectUploadUvBindCount",
            "drawTimeDirectUploadIndexBindCount",
            "drawTimeDirectUploadCandidateCount",
            "drawTimeDirectUploadRejectNoProofCount",
            "drawTimeDirectUploadRejectNoStorageCount",
            "drawTimeDirectUploadRejectRangeCount",
            "drawTimePositionAllocDirectMutableRequestCount",
        ):
            self.assertIn(name, SCENE)
            self.assertGreaterEqual(PERF.count(name), 3)
            self.assertIn(name, RUNTIME)


if __name__ == "__main__":
    unittest.main()
