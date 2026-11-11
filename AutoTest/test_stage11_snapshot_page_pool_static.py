#!/usr/bin/env python3
"""Structural contracts for the bounded Stage11 snapshot page pool."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
RETIREMENT = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_session_retirement.h"
).read_text(encoding="utf-8")
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


def code_only(source: str) -> str:
    """Blank comments/literals before checking actual lifetime transfers."""
    tokens = re.compile(
        r'//[^\n]*|/\*.*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})'
        r'\(.*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    return tokens.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), source)


class Stage11SnapshotPagePoolStaticTest(unittest.TestCase):
    @staticmethod
    def body(signature: str, source: str = DEVICE) -> str:
        start = source.index(signature)
        opening = source.index("{", start)
        depth = 0
        for index in range(opening, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
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
        reset = self.body("void D3D9DeviceEx::War3ResetShadowSessionState(", code_only(DEVICE))
        commit = self.body("prepared.commit(", reset)
        self.assertRegex(
            commit,
            r"prepared\.commit\s*\(\s*m_war3ShadowPersistentCharges\s*,\s*"
            r"m_war3GpuSkinMapEpoch\s*,\s*retireSerial\s*,\s*"
            r"\[this\]\s*\(\s*War3RetiredShadowSession\s*&\s*target\s*\)\s*noexcept\s*\{",
        )
        self.assertRegex(
            commit,
            r"TransferShadowSessionOwner\s*\(\s*target\.drawTimeVbCache\s*,\s*"
            r"m_war3DrawTimeVBCache\s*\)\s*;",
        )
        commit_end = reset.index(commit) + len(commit)
        self.assertRegex(reset[commit_end:], r"^\s*\)\s*;")
        page_reset = reset.index("War3ResetStage11SnapshotPages()")
        self.assertLess(commit_end, page_reset)
        transfer = self.body("void TransferShadowSessionOwner(", code_only(RETIREMENT))
        self.assertIsNotNone(re.fullmatch(
            r"\s*static_assert\s*\(\s*std::is_nothrow_move_assignable_v<T>\s*,\s*\)\s*;"
            r"\s*destination\s*=\s*std::move\s*\(\s*source\s*\)\s*;\s*",
            transfer[transfer.index("{") + 1:-1],
        ))
        helper_commit = self.body("Session& commit(", code_only(RETIREMENT))
        self.assertRegex(helper_commit, r"auto\s*&\s*session\s*=\s*\*m_slot\s*;")
        self.assertEqual(helper_commit.count("transfer(session);"), 1)
        self.assertLess(helper_commit.index("transfer(session);"),
                        helper_commit.index("m_committed = true;"))
        page_body = self.body("void D3D9DeviceEx::War3ResetStage11SnapshotPages(", code_only(DEVICE))
        self.assertIn("m_war3Stage11SnapshotPages.clear()", page_body)
        self.assertNotIn("page->used = 0", page_body)

    def test_reclamation_requires_no_cache_lease(self):
        collect = self.body("void D3D9DeviceEx::War3CollectUnusedStage11SnapshotPages(")
        self.assertIn("it->use_count() != 1u", collect)
        self.assertIn("m_war3Stage11SnapshotPages.erase(it)", collect)
        self.assertNotIn("->used = 0", collect)

    def test_publication_owns_scalar_commit_and_does_not_swallow_gpu_errors(self):
        allocator = self.body("D3D9DeviceEx::War3AllocateStage11Snapshot(")
        self.assertIn("War3PublishStage11SnapshotPage(", allocator)
        self.assertNotIn("m_war3Stage11SnapshotResidentBytes +=", allocator)
        self.assertNotIn("m_war3Stage11SnapshotNextPageId++", allocator)
        self.assertLess(allocator.index("createdPage = publication == War3Stage11PagePublication::Success"),
                        allocator.index("++m_war3DrawTimeVBCacheAllocBudgetThisFrame"))
        self.assertIn("if (createdPage) {\n        ++m_war3DrawTimeVBCacheAllocBudgetThisFrame", allocator)
        self.assertIn("publicationFailure = uint64_t(publication)", allocator)
        self.assertIn("War3Stage11SnapshotAllocationResult::AllocationFailure,\n                    publicationFailure", allocator)
        self.assertLess(POLICY.index("pages.push_back(std::move(page))"),
                        POLICY.index("residentBytes += pageBytes"))
        self.assertIn("catch (const std::bad_alloc&)", POLICY)
        self.assertNotIn("catch (...)" , POLICY)
        # Only explicitly typed capacity failures may borrow an existing tail;
        # arbitrary GPU errors, device loss and allocator bad_alloc propagate.
        self.assertEqual(allocator.count("catch ("), 1)
        self.assertIn("catch (const DxvkBufferAllocationError&)", allocator)
        self.assertIn("if (m_dxvkDevice->getDeviceStatus() != VK_SUCCESS)\n        throw;", allocator)

    def test_failure_evidence_preserves_the_actual_reason_and_budget(self):
        allocator = self.body("D3D9DeviceEx::War3AllocateStage11Snapshot(")
        self.assertEqual(allocator.count('"snapshot-alloc/v1"'), 1)
        for reason in ("InvalidRange", "ResidentCapacity", "PageCreateBudget", "AllocationFailure"):
            self.assertIn("fail(War3Stage11SnapshotAllocationResult::"+reason, allocator)
        self.assertIn("m_war3Stage11SnapshotResidentBytes, used", allocator)
        self.assertIn("publication, m_war3Stage11SnapshotNextPageId", allocator)
        self.assertIn("const auto session = war3::tools::evidence::ActiveSession()", allocator)

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
