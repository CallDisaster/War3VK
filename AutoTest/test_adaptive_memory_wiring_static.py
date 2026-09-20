"""Supplementary production wiring guards; CPU/GPU proofs remain separate."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
def source(p): return (ROOT/p).read_text(encoding='utf-8')
class Wiring(unittest.TestCase):
    def test_cache_and_draw_handoffs(self):
        s=source('src/d3d9/d3d9_device.cpp')
        for stream in ('position','index','uv'):
            self.assertEqual(s.count(f'draw.{stream}SnapshotLease = entry.{stream}SnapshotLease;'),3)
            self.assertEqual(s.count(f'cLease = entry.{stream}SnapshotLease,'),1)
            self.assertEqual(s.count(f'entry.{stream}SnapshotLease = std::move(snapshotLease);'),1)
        self.assertEqual(s.count('ctx->retainUntilCompletion(cLease);'),3)
        self.assertIn('page->sliceRecyclingSealed = true;',s)
        self.assertIn('page->sliceRecyclingSealed || !page->slices.hasSlot()',s)
    def test_all_shadow_consumers_retain(self):
        s=source('src/d3d9/d3d9_war3_shadow.cpp')
        self.assertEqual(s.count('war3::memory::TrackSnapshotSlices(*ctx, draw);'), 3)
        outline=source('src/d3d9/d3d9_war3_shadow_outline.cpp')
        self.assertEqual(outline.count('war3::memory::TrackSnapshotSlices(*ctx, draw);'), 2)
        capture=source('src/d3d9/war3/tools/war3_frame_inputs.cpp')
        self.assertLess(capture.index('TrackSnapshotSlices(*ctx, d)'), capture.index('r.spans[0]=gather('))
        pipeline=source('src/d3d9/d3d9_war3_pipeline.cpp')
        self.assertLess(pipeline.index('TrackSnapshotSlices(*ctx, draw)'), pipeline.index('war3shader::internal::UpdateRenderContext(input)'))
        s=source('src/d3d9/war3/memory/war3_snapshot_slice.h')
        for stream in ('position','index','uv'):
            self.assertEqual(s.count(f'tracker.track(draw.{stream}SnapshotLease);'),1)
        device=source('src/d3d9/d3d9_device.cpp')
        for prefix in ('caster','retainedDraw'):
            for stream in ('position','index','uv'):
                self.assertIn(f'{prefix}.{stream}SnapshotLease = nullptr;', device)
    def test_physical_budget_not_adjusted_logical(self):
        s=source('src/d3d9/war3/memory/war3_memory_budget_sample.h')
        self.assertIn('in.committed = heap.memoryCommitted;',s)
        self.assertNotIn('heap.memoryAllocated',s)
        self.assertIn('GlobalMemoryStatusEx',s)
        self.assertIn('heapIndex < info.heapCount',s)
    def test_no_relocation_or_readback(self):
        s=source('src/d3d9/war3/memory/war3_snapshot_slice.h')
        self.assertNotIn('new ',s)
        self.assertNotIn('memcpy(',s)
        self.assertIn('std::memory_order_release',s)
        self.assertIn('std::memory_order_acquire',s)
    def test_arena_retires_before_trim(self):
        s=source('src/d3d9/war3/memory/war3_shadow_arena.cpp')
        self.assertEqual(s.count('frameState.trimHistory.target('), 1)
        reset=s[s.index('void ShadowArena_Reset()'):s.index('uint32_t ShadowArena_UsedBytes()')]
        self.assertNotIn('pages.pop_back', reset)
        s=s[s.index('bool ShadowArena_BeginFrame('):s.index('void ShadowArena_EndFrame(')]
        self.assertLess(s.index('ShadowArenaGenerationCanBeReused('),s.index('frameState.trimHistory.target('))
        self.assertLess(s.index('frameState.trimHistory.target('),s.index('frameState.currentOffset = 0u'))
        self.assertIn('frameState.retireSerial != 0u &&', s)
        self.assertIn('const uint64_t keep = trimRetired', s)
        self.assertIn(': frameState.totalCapacity;', s)
    def test_arena_bootstrap_and_postallocation_pressure(self):
        s=source('src/d3d9/war3/memory/war3_shadow_arena.cpp')
        s=s[s.index('bool AllocateArenaPage('):s.index('bool IsValidAlignment(')]
        self.assertLess(s.index('if (!CanGrowArenaBy(pageCapacity))'), s.index('device->createBuffer('))
        self.assertLess(s.index('after.vaPressure'), s.index('frameState.pages.push_back('))
        self.assertNotIn('g_allocationHeapIndex = kInvalidGenerationIndex', s)
        self.assertIn('catch (const DxvkBufferAllocationError&)', s)
        self.assertIn('if (device->getDeviceStatus() != VK_SUCCESS) throw;', s)
        self.assertNotIn('catch (...)', s)
        self.assertLess(s.index('g_budgetRetry.refused('), s.index('RefreshArenaMemoryBudget('))
        source_device=source('src/d3d9/d3d9_device.cpp')
        allocation=source_device.split('D3D9DeviceEx::War3AllocateStage11Snapshot(',1)[1].split('void D3D9DeviceEx::War3CollectUnusedStage11SnapshotPages()',1)[0]
        self.assertLess(allocation.index('m_war3Stage11BudgetRetry.refused('), allocation.index('SampleShadowMemoryBudget('))
        self.assertIn('m_war3Stage11BudgetRetry.reset();', source_device)
if __name__=='__main__': unittest.main()
