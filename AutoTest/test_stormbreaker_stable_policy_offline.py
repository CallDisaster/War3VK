#!/usr/bin/env python3
"""StormBreaker 稳定产品策略的纯 Python 离线合同测试。

本脚本不加载 DXVK、Storm.dll 或《魔兽争霸 III》，也不模拟分配器性能。它只把产品默认
策略中最容易回归的安全边界压缩成确定性模型：精确大块阈值、禁止实验性全量接管、近期
释放指针去重、低 2 GiB 页目录，以及原生小块空闲链的保守 search 修复。
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import unittest


STABLE_LARGE_THRESHOLD = 0xFE7C
PAGE_SHIFT = 16
PAGE_SIZE = 1 << PAGE_SHIFT
PAGE_DIRECTORY_ENTRIES = 1 << (32 - PAGE_SHIFT)
LOW_2G_LIMIT = 0x80000000
RECENT_FREED_SLOTS = 4096
REPO_ROOT = Path(__file__).resolve().parents[1]


def allocation_route(size: int) -> str:
    """按稳定策略选择原生小块路径或 TLSF 大块路径。"""
    if size < 0:
        raise ValueError("size must not be negative")
    return "managed" if size >= STABLE_LARGE_THRESHOLD else "native"


def accepts_takeover_mode(value: str | None) -> bool:
    """未设置时采用默认 stable；显式值只接受 stable/large。"""
    if value is None:
        return True
    return value.lower() in ("stable", "large")


class RecentFreedTable:
    """固定容量的近期释放指针表；哈希命中后仍必须比较完整指针。"""

    def __init__(self, slot_count: int = RECENT_FREED_SLOTS) -> None:
        if slot_count <= 0 or slot_count & (slot_count - 1):
            raise ValueError("slot_count must be a positive power of two")
        self._slots: list[int | None] = [None] * slot_count
        self._mask = slot_count - 1

    def slot(self, pointer: int) -> int:
        if pointer < 0 or pointer > 0xFFFFFFFF:
            raise ValueError("pointer must fit in 32 bits")
        mixed = (pointer >> 4) * 2654435761
        return mixed & self._mask

    def remember(self, pointer: int) -> None:
        self._slots[self.slot(pointer)] = pointer

    def contains(self, pointer: int) -> bool:
        return self._slots[self.slot(pointer)] == pointer

    def clear_reused(self, pointer: int) -> None:
        slot = self.slot(pointer)
        if self._slots[slot] == pointer:
            self._slots[slot] = None


class LowAddressPageDirectory:
    """用 64 KiB 页粒度记录已托管范围，并拒绝任何跨越低 2 GiB 的范围。"""

    def __init__(self) -> None:
        self._pages = bytearray(PAGE_DIRECTORY_ENTRIES)

    @staticmethod
    def _page_span(base: int, size: int) -> range:
        if base < 0 or size <= 0:
            raise ValueError("base and size must describe a non-empty range")
        end = base + size
        if end <= base or base >= LOW_2G_LIMIT or end > LOW_2G_LIMIT:
            raise ValueError("range must remain wholly below 0x80000000")
        return range(base >> PAGE_SHIFT, ((end - 1) >> PAGE_SHIFT) + 1)

    def register(self, base: int, size: int) -> None:
        pages = self._page_span(base, size)
        if any(self._pages[page] for page in pages):
            raise ValueError("range overlaps an existing managed page")
        for page in pages:
            self._pages[page] = 1

    def unregister(self, base: int, size: int) -> None:
        pages = self._page_span(base, size)
        if not all(self._pages[page] for page in pages):
            raise ValueError("range includes an unregistered page")
        for page in pages:
            self._pages[page] = 0

    def contains(self, pointer: int) -> bool:
        return (
            0 <= pointer < LOW_2G_LIMIT
            and bool(self._pages[pointer >> PAGE_SHIFT])
        )


@dataclass(frozen=True)
class SearchResult:
    promoted: bool
    promoted_size: int | None = None
    source_bin: int | None = None


class NativeSmallSearchModel:
    """原生小块 search 的最小模型；列表元素表示空闲块的可用字节数。"""

    def __init__(self, bins: list[list[int]]) -> None:
        self.bins = [list(blocks) for blocks in bins]

    def promote_fit(self, target_bin: int, requested_size: int) -> SearchResult:
        if not 0 <= target_bin < len(self.bins):
            raise ValueError("target_bin is out of range")
        if requested_size <= 0:
            raise ValueError("requested_size must be positive")

        target_blocks = self.bins[target_bin]
        if not target_blocks:
            return SearchResult(False)
        if any(block_size >= requested_size for block_size in target_blocks):
            return SearchResult(False)

        for source_bin in range(target_bin + 1, len(self.bins)):
            candidates = [
                (block_size - requested_size, block_index, block_size)
                for block_index, block_size in enumerate(self.bins[source_bin])
                if block_size >= requested_size
            ]
            if not candidates:
                continue
            _, block_index, block_size = min(candidates)
            del self.bins[source_bin][block_index]
            target_blocks.insert(0, block_size)
            return SearchResult(True, block_size, source_bin)

        return SearchResult(False)


class StableRouteTests(unittest.TestCase):
    def test_exact_large_threshold_partition(self) -> None:
        self.assertEqual(allocation_route(0xFE7B), "native")
        self.assertEqual(allocation_route(0xFE7C), "managed")
        self.assertEqual(allocation_route(0xFE7D), "managed")

    def test_default_and_explicit_stable_large_modes_are_accepted(self) -> None:
        self.assertTrue(accepts_takeover_mode(None))
        self.assertTrue(accepts_takeover_mode("stable"))
        self.assertTrue(accepts_takeover_mode("large"))
        self.assertTrue(accepts_takeover_mode("STABLE"))
        for rejected in ("", "full", "hybrid", "FULL", " stable "):
            with self.subTest(rejected=rejected):
                self.assertFalse(accepts_takeover_mode(rejected))


class SourceWiringTests(unittest.TestCase):
    """确认离线模型描述的边界确实接入当前 C++ 产品路径。"""

    @classmethod
    def setUpClass(cls) -> None:
        memory = REPO_ROOT / "src/d3d9/war3/memory"
        cls.hook = (memory / "war3_storm_hook.cpp").read_text(encoding="utf-8")
        cls.pool = (memory / "war3_tlsf_pool.cpp").read_text(encoding="utf-8")
        cls.repair = (memory / "war3_storm_native_small_repair.cpp").read_text(
            encoding="utf-8"
        )

    def test_product_policy_is_wired_to_stable_large_search(self) -> None:
        self.assertIn("kStormNativeLargeBlockThreshold = 0xFE7Cu", self.hook)
        self.assertIn('fullTakeover=0', self.hook)
        self.assertIn('StormNativeSmallRepairMode::Search', self.repair)

    def test_freed_pointer_and_low_address_guards_are_wired(self) -> None:
        self.assertIn("kFreedPointerTableSize = 4096u", self.hook)
        self.assertIn("RememberFreedPointer(userPtr)", self.hook)
        self.assertIn("TlsfPool_InspectExactBlock", self.hook)
        self.assertIn("tlsf_block_is_valid_in_range", self.pool)
        self.assertIn("kStormSignedAddressLimit = 0x80000000u", self.pool)
        self.assertIn("g_addressDirectory", self.pool)

    def test_storm_binary_identity_guard_is_wired(self) -> None:
        self.assertIn("kExpectedStormSha256[32]", self.hook)
        self.assertIn("HashFileSha256(path, digest, fileSize)", self.hook)

    def test_native_large_counter_correction_is_wired(self) -> None:
        self.assertIn("QueryNativeLargeBlockForCounterFix", self.hook)
        self.assertIn("ApplyNativeLargeFreeCounterCorrection", self.hook)
        self.assertIn("kStormTotalAllocOffset = 0x5738Cu", self.hook)

    def test_exact_claim_and_failed_free_restore_are_wired(self) -> None:
        self.assertIn("context->claim && !TryClaimManagedHeader(hdr)", self.hook)
        self.assertIn("QueryManagedBlock(ptr, &hdr, &origSize, true)", self.hook)
        self.assertIn("if (!TlsfPool_Free(hdr))", self.hook)
        self.assertIn("SetupManagedHeader(userPtr, requestedSize)", self.hook)

    def test_managed_realloc_preserves_public_16_byte_alignment(self) -> None:
        self.assertIn(
            "void *rawNew = TlsfPool_ReallocInPlace(rawOld, newAllocSize);",
            self.hook,
        )
        self.assertNotIn(
            "void *rawNew = TlsfPool_Realloc(rawOld, newAllocSize);",
            self.hook,
        )
        self.assertIn(
            "(reinterpret_cast<uintptr_t>(rawPtr) & 0x0Fu) != 0u",
            self.hook,
        )

    def test_release_build_avoids_startup_self_test_and_size_wrap(self) -> None:
        self.assertIn("tlsf_allocation_pool_size(size, effectiveAlignment)", self.pool)
        tlsf_source = (
            REPO_ROOT / "src/d3d9/war3/memory/tlsf.c"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "#if defined(_DEBUG) || defined(STORMBREAKER_TESTING)",
            tlsf_source,
        )
        self.assertIn("initialize_mapping_table();", tlsf_source)


class RecentFreedTableTests(unittest.TestCase):
    def test_remember_and_reuse_clear(self) -> None:
        table = RecentFreedTable()
        pointer = 0x12345000
        self.assertFalse(table.contains(pointer))
        table.remember(pointer)
        self.assertTrue(table.contains(pointer))
        table.clear_reused(pointer)
        self.assertFalse(table.contains(pointer))

    def test_hash_collision_never_becomes_pointer_false_positive(self) -> None:
        table = RecentFreedTable(slot_count=8)
        first = 0x00100010
        collision = next(
            candidate
            for candidate in range(first + 0x10, first + 0x10000, 0x10)
            if table.slot(candidate) == table.slot(first)
        )
        self.assertNotEqual(first, collision)

        table.remember(first)
        self.assertTrue(table.contains(first))
        self.assertFalse(table.contains(collision))

        table.clear_reused(collision)
        self.assertTrue(table.contains(first))
        table.remember(collision)
        self.assertTrue(table.contains(collision))
        self.assertFalse(table.contains(first))


class PageDirectoryTests(unittest.TestCase):
    def test_exact_low_2g_end_is_accepted_and_unregistered(self) -> None:
        directory = LowAddressPageDirectory()
        base = LOW_2G_LIMIT - PAGE_SIZE
        directory.register(base, PAGE_SIZE)
        self.assertTrue(directory.contains(base))
        self.assertTrue(directory.contains(LOW_2G_LIMIT - 1))
        self.assertFalse(directory.contains(LOW_2G_LIMIT))
        directory.unregister(base, PAGE_SIZE)
        self.assertFalse(directory.contains(base))

    def test_ranges_crossing_or_starting_at_high_bit_are_rejected(self) -> None:
        directory = LowAddressPageDirectory()
        for base, size in (
            (LOW_2G_LIMIT - PAGE_SIZE, PAGE_SIZE + 1),
            (LOW_2G_LIMIT, PAGE_SIZE),
            (LOW_2G_LIMIT + PAGE_SIZE, PAGE_SIZE),
        ):
            with self.subTest(base=hex(base), size=size):
                with self.assertRaises(ValueError):
                    directory.register(base, size)

    def test_64k_page_coverage_includes_both_partial_edge_pages(self) -> None:
        directory = LowAddressPageDirectory()
        base = 0x0010FFF0
        size = 0x30
        directory.register(base, size)
        self.assertTrue(directory.contains(0x00100000))
        self.assertTrue(directory.contains(0x00110000))
        self.assertFalse(directory.contains(0x00120000))


class NativeSmallSearchTests(unittest.TestCase):
    def test_promotes_only_for_nonempty_target_without_fit(self) -> None:
        model = NativeSmallSearchModel([[16, 24], [32], [96, 128]])
        result = model.promote_fit(target_bin=0, requested_size=80)
        self.assertEqual(result, SearchResult(True, 96, 2))
        self.assertEqual(model.bins[0], [96, 16, 24])
        self.assertEqual(model.bins[2], [128])

    def test_target_empty_does_not_promote_even_when_higher_fit_exists(self) -> None:
        model = NativeSmallSearchModel([[], [128]])
        before = [list(blocks) for blocks in model.bins]
        self.assertEqual(model.promote_fit(0, 64), SearchResult(False))
        self.assertEqual(model.bins, before)

    def test_existing_target_fit_does_not_promote(self) -> None:
        model = NativeSmallSearchModel([[32, 96], [128]])
        before = [list(blocks) for blocks in model.bins]
        self.assertEqual(model.promote_fit(0, 64), SearchResult(False))
        self.assertEqual(model.bins, before)

    def test_no_higher_fit_does_not_mutate_bins(self) -> None:
        model = NativeSmallSearchModel([[16, 24], [32, 48], [63]])
        before = [list(blocks) for blocks in model.bins]
        self.assertEqual(model.promote_fit(0, 64), SearchResult(False))
        self.assertEqual(model.bins, before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
