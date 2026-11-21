#!/usr/bin/env python3
"""x86 address-space relief (docs/research/2026-11-17-x64-x86-memory-relief.md): text checks.

The behaviour is proved by AutoTest/test_tlsf_pool_direct_blocks.cpp (the
production TLSF pool in a 32-bit test process) and by the same-map A/B gate
(run_x64_phase52_ab_gate.py); this keeps the agreements from drifting:

  - TLSF: an oversize request the pools cannot serve becomes a direct block,
    registered in the low-2-GiB page directory, released whole on free.
  - The producer palette caches are not allocated in proxy mode, and every
    reader treats a missing table as a miss.
  - The shader API draw-call ring and the frame timeline store exist only once
    used.
  - The render queue tracker logs its per-frame occupancy.
  - The probe's va=regions scan covers 4 GiB without changing freeBytes.
"""
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src/d3d9"


def read(relative):
    return (SRC / relative).read_text(encoding="utf-8")


class TlsfDirectBlocks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pool = read("war3/memory/war3_tlsf_pool.cpp")

    def test_oversize_requests_become_direct_blocks(self):
        self.assertIn("constexpr size_t kDirectBlockThreshold = kExtendGranularity / 2;", self.pool)
        grow = self.pool[self.pool.index("void *AllocWithExtendLocked("):]
        grow = grow[:grow.index("\n}\n")]
        direct = grow.index("return AllocDirectBlockLocked(size);")
        # Only after the pools failed, and before any growth pool is sized.
        self.assertLess(grow.index("tlsf_memalign(g_tlsf, align, size)"), direct)
        self.assertLess(direct, grow.index("kGrowthSlack"))

    def test_direct_blocks_keep_the_low_range_and_ownership_rules(self):
        alloc = self.pool[self.pool.index("void *AllocDirectBlockLocked("):]
        alloc = alloc[:alloc.index("\n}\n")]
        self.assertIn('AllocateLowRange(bytes, "direct")', alloc)
        self.assertIn("RegisterAddressRangeLocked(mem, bytes)", alloc)
        release = self.pool[self.pool.index("void ReleaseDirectBlockLocked("):]
        release = release[:release.index("\n}\n")]
        self.assertLess(release.index("UnregisterAddressRangeLocked"), release.index("VirtualFree(block->base, 0, MEM_RELEASE)"))
        exact = self.pool[self.pool.index("bool IsExactAllocatedBlockLocked("):]
        self.assertIn("IsDirectBlockLocked(ptr)", exact[:exact.index("\n}\n")])

    def test_every_tlsf_entry_point_handles_direct_blocks(self):
        for name in ("void *TlsfPool_Realloc(", "void *TlsfPool_ReallocInPlace(", "bool TlsfPool_Free(",
                     "size_t TlsfPool_BlockSize("):
            body = self.pool[self.pool.index(name):]
            body = body[:body.index("\n}\n")]
            self.assertRegex(body, r"FindDirectBlockLocked|IsDirectBlockLocked|BlockSizeLocked", name)
        self.assertIn("g_directBlocks.clear();", self.pool)


class ProducerPaletteTables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hook = read("war3/model/war3_model_hook.cpp")

    def test_tables_are_not_allocated_in_proxy_mode(self):
        self.assertIn("dxvk::war3::proxy::Requested()", self.hook)
        self.assertIn("static ProducerPaletteTable<BlendedPaletteEntry, kSlotBlendedPaletteCacheSize>", self.hook)
        self.assertIn("static ProducerPaletteTable<RenderablePartPaletteBindingEntry,", self.hook)
        self.assertNotRegex(self.hook, r"static std::array<(BlendedPaletteEntry|RenderablePartPaletteBindingEntry)")
        # The DLL is loaded and freed more than once in a game process: the
        # tables must go with each unload (an in-process run kept three copies).
        table = self.hook[self.hook.index("class ProducerPaletteTable {"):]
        table = table[:table.index("\n};\n")]
        self.assertIn("~ProducerPaletteTable() {", table)
        self.assertIn("delete[] entries;", table)

    def test_every_indexed_access_is_behind_a_table_check(self):
        for table in ("s_slotBlendedPaletteCache", "s_renderablePartPaletteBindings"):
            for match in re.finditer(re.escape(table) + r"\[", self.hook):
                start = self.hook.rfind("\n}\n", 0, match.start())
                body = self.hook[start:match.start()]
                guarded = ("!" + table) in body or "ValidateBlendedPaletteBySlotIndexExact(" in body
                self.assertTrue(guarded, f"{table} indexed at offset {match.start()} without a null check")


class LazyStores(unittest.TestCase):
    def test_draw_call_ring_is_allocated_by_its_writer(self):
        api = read("war3_shader_api.cpp")
        self.assertIn("std::unique_ptr<DrawCallBuffer[]> g_drawCallBuffers;", api)
        self.assertNotIn("DrawCallBuffer g_drawCallBuffers[kRingBufferSize];", api)
        update = api[api.index("void UpdateRenderContext("):]
        self.assertIn("g_drawCallBuffers.reset(new (std::nothrow) DrawCallBuffer[kRingBufferSize]())", update)
        draws = api[api.index("WAR3_SHADER_API const DrawCall* GetDrawCalls("):]
        self.assertIn("if (!g_drawCallBuffers)", draws[:draws.index("\n}\n")])

    def test_timeline_store_is_built_on_first_enabled_use(self):
        timeline = read("war3/tools/war3_frame_timeline.cpp")
        self.assertIn("Store& store() { static const std::unique_ptr<Store> s(new Store()); return *s; }", timeline)
        install = timeline[timeline.index("void NativeFrameSyncInstalled("):]
        self.assertNotIn("store()", install[:install.index("\n}\n")])


class ProbeAddressSpaceScan(unittest.TestCase):
    """va=regions covers 4 GiB; freeBytes keeps native_game_va_sample's raw meaning."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT / "AutoTest"))
        import run_x64_game_proxy_probe as probe
        cls.probe = probe

    def test_scan_covers_the_whole_wow64_space(self):
        self.assertEqual(self.probe.VA_REGIONS_END, 1 << 32)

    def test_free_bytes_are_raw_and_usable_bytes_aligned(self):
        holes = [(0x11000, 0x30000), (0x7FFF8000, 0x80020000)]
        low = self.probe.va_holes(holes, 0x10000, 0x80000000)
        self.assertEqual(low["freeBytes"], 0x1F000 + 0x8000)
        self.assertEqual(low["usableFreeBytes"], 0x10000)  # 0x20000..0x30000; the tail below 2 GiB is too short
        self.assertEqual(low["largestAlignedFreeHoleBytes"], 0x10000)
        high = self.probe.va_holes(holes, 0x80000000, 1 << 32)
        self.assertEqual((high["freeBytes"], high["usableFreeBytes"]), (0x20000, 0x20000))

    def test_gate_keeps_the_low_half_totals(self):
        gate = (ROOT / "AutoTest/run_x64_phase52_ab_gate.py").read_text(encoding="utf-8")
        self.assertIn("LOW_END = 0x80000000", gate)
        self.assertIn('low = [a for a in allocations if a["base"] < LOW_END]', gate)
        self.assertIn('freeBytes=va.get("freeBytes")', gate)


class RenderQueueTrackerOccupancy(unittest.TestCase):
    def test_occupancy_is_logged(self):
        tracker = read("war3/render/war3_render_queue_tracker.cpp")
        self.assertIn("WarVK RenderQueueTracker: capacity=", tracker)
        self.assertIn("constexpr uint32_t kRenderQueueTrackerCapacity = 1u << 16;", tracker)
        self.assertIn("m_epochStamped.fetch_add(stamped", tracker)
        self.assertIn("m_epochProbeFailures.fetch_add(probeFailures", tracker)


if __name__ == "__main__":
    unittest.main(verbosity=2)
