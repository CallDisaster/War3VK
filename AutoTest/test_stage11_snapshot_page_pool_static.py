#!/usr/bin/env python3
"""Structural contracts for the bounded Stage11 snapshot page pool."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
POLICY = (ROOT / "src/d3d9/war3/render/war3_stage11_snapshot_page_policy.h").read_text(
    encoding="utf-8"
)
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
BRIDGE_H = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.h").read_text(
    encoding="utf-8"
)
BRIDGE = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
PERF_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(
    encoding="utf-8"
)
PERF = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)


class Stage11SnapshotPagePoolStaticTest(unittest.TestCase):
    @staticmethod
    def body(signature: str) -> str:
        start = DEVICE.index(signature)
        opening = DEVICE.index("{", start)
        depth = 0
        for index in range(opening, len(DEVICE)):
            if DEVICE[index] == "{":
                depth += 1
            elif DEVICE[index] == "}":
                depth -= 1
                if depth == 0:
                    return DEVICE[start : index + 1]
        raise AssertionError(signature)

    def test_policy_is_bounded_and_never_reuses_holes(self):
        self.assertIn("kWar3Stage11SnapshotAlignment = 256u", POLICY)
        self.assertIn("kWar3Stage11SnapshotPageBytes = 16u << 20u", POLICY)
        self.assertIn("kWar3Stage11SnapshotResidentCapBytes = 384u << 20u", POLICY)
        self.assertIn("384 MiB / 16 MiB = 24 pages", POLICY)
        self.assertIn("result.offset = alignedUsed", POLICY)
        self.assertIn("result.nextUsed = alignedUsed + alignedBytes", POLICY)
        self.assertNotIn("freeList", POLICY)

    def test_cache_entries_own_page_lifetimes_per_stream(self):
        for token in (
            "positionSnapshotPage",
            "uvSnapshotPage",
            "indexSnapshotPage",
            "positionSnapshotOffset",
            "uvSnapshotOffset",
            "indexSnapshotOffset",
        ):
            self.assertIn(token, HEADER)
        self.assertIn("std::shared_ptr<War3Stage11SnapshotPage>", HEADER)
        self.assertNotIn("DxvkDevice*", POLICY)

    def test_only_page_creation_consumes_the_legacy_allocation_gate(self):
        allocator = self.body("D3D9DeviceEx::War3AllocateStage11Snapshot(")
        increment = allocator.index("++m_war3DrawTimeVBCacheAllocBudgetThisFrame")
        create = allocator.index("m_dxvkDevice->createBuffer(")
        self.assertLess(create, increment)
        self.assertEqual(
            DEVICE.count("++m_war3DrawTimeVBCacheAllocBudgetThisFrame"), 1
        )
        self.assertIn("War3Stage11SnapshotAllocationResult::PageCreateBudget", allocator)

    def test_copy_and_slice_use_the_same_logical_offsets(self):
        for stream in ("position", "uv", "index"):
            offset = f"entry.{stream}SnapshotOffset"
            self.assertGreaterEqual(DEVICE.count(offset), 3)
        self.assertIn("ctx->copyBuffer(cDst, cDstOff, cSrc, cSrcOff, cBytes)", DEVICE)
        self.assertNotIn('debugName = "War3DrawTimeVBPos"', DEVICE)
        self.assertNotIn('debugName = "War3DrawTimeVBUv"', DEVICE)
        self.assertNotIn('debugName = "War3DrawTimeVBIdx"', DEVICE)

    def test_map_reset_never_reuses_old_epoch_pages(self):
        reset = self.body("void D3D9DeviceEx::War3ResetShadowSessionState(")
        move = reset.index("retired.drawTimeVbCache = std::move(m_war3DrawTimeVBCache)")
        page_reset = reset.index("War3ResetStage11SnapshotPages()", move)
        self.assertLess(move, page_reset)
        page_body = self.body("void D3D9DeviceEx::War3ResetStage11SnapshotPages(")
        self.assertIn("m_war3Stage11SnapshotPages.clear()", page_body)
        self.assertNotIn("page->used = 0", page_body)

    def test_reclamation_requires_no_cache_lease(self):
        collect = self.body("void D3D9DeviceEx::War3CollectUnusedStage11SnapshotPages(")
        self.assertIn("it->use_count() != 1u", collect)
        self.assertIn("m_war3Stage11SnapshotPages.erase(it)", collect)
        self.assertNotIn("->used = 0", collect)

    def test_runtime_and_performance_diagnostics_are_wired(self):
        fields = (
            "drawTimeSnapshotPageResidentBytes",
            "drawTimeSnapshotPageUsedBytes",
            "drawTimeSnapshotPageCreateCount",
            "drawTimeSnapshotSuballocationCount",
            "drawTimeSnapshotSuballocationBytes",
            "drawTimeSnapshotPageReclaimedCount",
            "drawTimeSnapshotPageCapacityRejectCount",
            "drawTimeSnapshotPageAllocationFailureCount",
        )
        for field in fields:
            self.assertIn(field, SCENE)
            self.assertIn(field, BRIDGE_H)
            self.assertIn(field, BRIDGE)
            self.assertIn(field, PERF_H)
            self.assertIn(field, PERF)


if __name__ == "__main__":
    unittest.main()
