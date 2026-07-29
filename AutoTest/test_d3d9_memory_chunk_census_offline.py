#!/usr/bin/env python3
"""D3D9 内存块普查接口的纯离线合同测试。

本脚本不会加载 DXVK、《魔兽争霸 III》或 AutoTest。它只检查 C++ 诊断接口的静态边界，
并用一个小型 Python 账本验证 chunk ID、共享/独立映射、退休后故障保留和闭合判定。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src" / "d3d9" / "d3d9_mem.h"
CPP_PATH = ROOT / "src" / "d3d9" / "d3d9_mem.cpp"
UINT64_MAX = (1 << 64) - 1


def source_block(source: str, begin_marker: str, end_marker: str) -> str:
    begin = source.index(begin_marker)
    end = source.index(end_marker, begin)
    return source[begin:end]


@dataclass
class ChunkModel:
    chunk_id: int
    reserve_bytes: int
    used_payload_bytes: int
    occupied_bytes: int
    free_bytes: int
    shared_page_refs: list[int] = field(default_factory=list)
    standalone_bytes: list[int] = field(default_factory=list)
    map_failures: int = 0
    unmap_failures: int = 0
    state_faults: int = 0


class AllocatorModel:
    """只模拟本次新增的诊断账本，不模拟实际虚拟内存操作。"""

    def __init__(self, next_chunk_id: int = 1, page_bytes: int = 1 << 20) -> None:
        self.next_chunk_id = next_chunk_id
        self.page_bytes = page_bytes
        self.generation = 1
        self.generation_saturated = False
        self.map_failures = 0
        self.unmap_failures = 0
        self.state_faults = 0
        self.chunks: list[ChunkModel] = []

    def advance(self) -> None:
        if self.generation == UINT64_MAX:
            self.generation_saturated = True
            return
        self.generation += 1
        if self.generation == UINT64_MAX:
            self.generation_saturated = True

    def allocate_chunk(
        self,
        reserve_bytes: int,
        used_payload_bytes: int,
        internal_fragmentation_bytes: int = 0,
    ) -> ChunkModel | None:
        chunk_id = self.next_chunk_id
        if chunk_id != 0:
            self.next_chunk_id = (
                0 if chunk_id == UINT64_MAX else chunk_id + 1
            )
        occupied_bytes = used_payload_bytes + internal_fragmentation_bytes
        chunk = ChunkModel(
            chunk_id=chunk_id,
            reserve_bytes=reserve_bytes,
            used_payload_bytes=used_payload_bytes,
            occupied_bytes=occupied_bytes,
            free_bytes=reserve_bytes - occupied_bytes,
        )
        self.chunks.append(chunk)
        self.advance()
        return chunk

    def record_map_failure(self, chunk: ChunkModel) -> None:
        chunk.map_failures += 1
        self.map_failures += 1
        self.advance()

    def record_unmap_failure(self, chunk: ChunkModel) -> None:
        chunk.unmap_failures += 1
        self.unmap_failures += 1
        self.advance()

    def retire(self, chunk: ChunkModel) -> None:
        self.chunks.remove(chunk)
        self.advance()

    def snapshot(self) -> dict[str, int | bool]:
        reserve = sum(chunk.reserve_bytes for chunk in self.chunks)
        used = sum(chunk.used_payload_bytes for chunk in self.chunks)
        occupied = sum(chunk.occupied_bytes for chunk in self.chunks)
        free = sum(chunk.free_bytes for chunk in self.chunks)
        fragmentation = occupied - used
        shared_refs = sum(
            refs for chunk in self.chunks for refs in chunk.shared_page_refs
        )
        shared_bytes = sum(
            self.page_bytes
            for chunk in self.chunks
            for refs in chunk.shared_page_refs
            if refs > 0
        )
        standalone_refs = sum(
            len(chunk.standalone_bytes) for chunk in self.chunks
        )
        standalone_bytes = sum(
            size for chunk in self.chunks for size in chunk.standalone_bytes
        )
        state_faults = self.state_faults + sum(
            chunk.state_faults for chunk in self.chunks
        )
        return {
            "reserve": reserve,
            "used": used,
            "occupied": occupied,
            "fragmentation": fragmentation,
            "free": free,
            "shared_refs": shared_refs,
            "shared_bytes": shared_bytes,
            "standalone_refs": standalone_refs,
            "standalone_bytes": standalone_bytes,
            "mapped_refs": shared_refs + standalone_refs,
            "mapped_bytes": shared_bytes + standalone_bytes,
            "map_failures": self.map_failures,
            "unmap_failures": self.unmap_failures,
            "state_faults": state_faults,
            "closure": (
                reserve == occupied + free
                and occupied == used + fragmentation
                and self.unmap_failures == 0
                and state_faults == 0
            ),
        }


class D3D9MemoryChunkCensusOfflineTests(unittest.TestCase):
    def test_binding_only_exposes_numeric_identity(self) -> None:
        header = HEADER_PATH.read_text(encoding="utf-8")
        block = source_block(
            header,
            "struct D3D9MemoryDiagnosticBinding",
            "struct D3D9MemoryChunkDiagnosticSnapshot",
        )

        for declaration in (
            "uint64_t chunkId",
            "uint64_t offset",
            "uint64_t alignedSliceBytes",
            "bool mapped",
        ):
            self.assertIn(declaration, block)
        self.assertNotIn("void*", block)
        self.assertNotIn("D3D9MemoryChunk*", block)
        self.assertNotIn("D3D9MemoryAllocator*", block)

        snapshots = source_block(
            header,
            "struct D3D9MemoryChunkDiagnosticSnapshot",
            "class D3D9MemoryAllocator;",
        )
        self.assertNotIn("void*", snapshots)
        self.assertNotIn("D3D9MemoryChunk*", snapshots)

    def test_snapshot_copies_under_allocator_lock(self) -> None:
        source = CPP_PATH.read_text(encoding="utf-8")
        body = source_block(
            source,
            "D3D9MemoryAllocator::CaptureDiagnosticSnapshot()",
            "D3D9MemoryChunk::D3D9MemoryChunk(",
        )
        lock = body.index("std::lock_guard<dxvk::mutex> lock(m_mutex);")
        traversal = body.index("for (const auto& chunk : m_chunks)")
        self.assertLess(lock, traversal)
        for field in (
            "reserveBytes",
            "allocatorUsedPayloadBytes",
            "chunkOccupiedBytes",
            "internalFragmentationBytes",
            "freePayloadBytes",
            "sharedMappedRefs",
            "sharedMappedBytes",
            "standaloneMappedRefs",
            "standaloneMappedBytes",
            "mutationGeneration",
            "mapFailureCount",
            "unmapFailureCount",
        ):
            self.assertIn(field, body)

    def test_cpp_allocator_retains_mapping_failure_history(self) -> None:
        source = CPP_PATH.read_text(encoding="utf-8")
        map_body = source_block(
            source,
            "void* D3D9MemoryChunk::MapLocked",
            "uint32_t D3D9MemoryChunk::UnmapLocked",
        )
        unmap_body = source_block(
            source,
            "uint32_t D3D9MemoryChunk::UnmapLocked",
            "D3D9Memory D3D9MemoryChunk::AllocLocked",
        )
        snapshot_body = source_block(
            source,
            "D3D9MemoryAllocator::CaptureDiagnosticSnapshot()",
            "D3D9MemoryChunk::D3D9MemoryChunk(",
        )
        self.assertGreaterEqual(map_body.count("m_allocator->m_mapFailureCount++"), 2)
        self.assertGreaterEqual(
            unmap_body.count("m_allocator->m_unmapFailureCount++"), 2
        )
        self.assertIn("snapshot.mapFailureCount = m_mapFailureCount;", snapshot_body)
        self.assertIn("snapshot.unmapFailureCount = m_unmapFailureCount;", snapshot_body)

    def test_chunk_id_exhaustion_fails_diagnostics_not_allocation(self) -> None:
        source = CPP_PATH.read_text(encoding="utf-8")
        self.assertIn(
            "std::atomic<uint64_t> g_nextD3D9MemoryChunkId { 1 };",
            source,
        )
        allocator = AllocatorModel(next_chunk_id=UINT64_MAX - 1)
        first = allocator.allocate_chunk(4096, 64)
        second = allocator.allocate_chunk(4096, 64)
        exhausted = allocator.allocate_chunk(4096, 64)
        self.assertEqual([first.chunk_id, second.chunk_id], [UINT64_MAX - 1, UINT64_MAX])
        self.assertIsNotNone(exhausted)
        self.assertEqual(exhausted.chunk_id, 0)
        self.assertNotEqual(first.chunk_id, 0)
        self.assertNotEqual(second.chunk_id, 0)
        alloc_body = source_block(
            source,
            "D3D9Memory D3D9MemoryAllocator::Alloc(uint32_t Size)",
            "void D3D9MemoryAllocator::Free(D3D9Memory *Memory)",
        )
        exhausted_begin = alloc_body.index("if (unlikely(chunkId == 0))")
        exhausted_end = alloc_body.index("uint32_t chunkSize", exhausted_begin)
        self.assertNotIn("return {};", alloc_body[exhausted_begin:exhausted_end])

    def test_shared_and_standalone_mappings_have_separate_units(self) -> None:
        allocator = AllocatorModel()
        chunk = allocator.allocate_chunk(64 << 20, 24 << 20)
        assert chunk is not None
        chunk.shared_page_refs = [0, 3, 1]
        chunk.standalone_bytes = [1_310_720, 2_097_152]

        snapshot = allocator.snapshot()
        self.assertEqual(snapshot["shared_refs"], 4)
        self.assertEqual(snapshot["shared_bytes"], 2 << 20)
        self.assertEqual(snapshot["standalone_refs"], 2)
        self.assertEqual(snapshot["standalone_bytes"], 3_407_872)
        self.assertEqual(snapshot["mapped_refs"], 6)
        self.assertEqual(snapshot["mapped_bytes"], 5_505_024)
        self.assertTrue(snapshot["closure"])

    def test_allocator_failure_history_survives_chunk_retirement(self) -> None:
        allocator = AllocatorModel()
        chunk = allocator.allocate_chunk(64 << 20, 8 << 20)
        assert chunk is not None
        allocator.record_map_failure(chunk)
        allocator.record_unmap_failure(chunk)
        allocator.retire(chunk)

        snapshot = allocator.snapshot()
        self.assertEqual(snapshot["reserve"], 0)
        self.assertEqual(snapshot["map_failures"], 1)
        self.assertEqual(snapshot["unmap_failures"], 1)
        self.assertFalse(snapshot["closure"])

    def test_mutation_generation_saturates_without_aba(self) -> None:
        allocator = AllocatorModel()
        allocator.generation = UINT64_MAX - 1
        allocator.advance()
        self.assertEqual(allocator.generation, UINT64_MAX)
        self.assertTrue(allocator.generation_saturated)
        allocator.advance()
        self.assertEqual(allocator.generation, UINT64_MAX)
        self.assertTrue(allocator.generation_saturated)

    def test_legacy_tail_absorption_is_reported_as_fragmentation(self) -> None:
        allocator = AllocatorModel()
        chunk = allocator.allocate_chunk(16 << 10, 13 << 10, 3 << 10)
        assert chunk is not None
        snapshot = allocator.snapshot()
        self.assertEqual(snapshot["used"], 13 << 10)
        self.assertEqual(snapshot["occupied"], 16 << 10)
        self.assertEqual(snapshot["fragmentation"], 3 << 10)
        self.assertEqual(snapshot["free"], 0)
        self.assertTrue(snapshot["closure"])

        source = CPP_PATH.read_text(encoding="utf-8")
        alloc_body = source_block(
            source,
            "D3D9Memory D3D9MemoryAllocator::Alloc",
            "void D3D9MemoryAllocator::Free",
        )
        chunk_alloc_body = source_block(
            source,
            "D3D9Memory D3D9MemoryChunk::AllocLocked",
            "void D3D9MemoryChunk::FreeLocked",
        )
        self.assertNotIn("if (unlikely(Size == 0))", alloc_body)
        self.assertIn("range->length < (4 << 10)", chunk_alloc_body)
        self.assertIn("size += range->length", chunk_alloc_body)

    def test_chunk_release_requires_every_live_slice_to_be_candidate(self) -> None:
        allocator = AllocatorModel()
        chunk = allocator.allocate_chunk(64 << 20, 44 << 20)
        assert chunk is not None
        exact_bindings = [
            (chunk.chunk_id, 0, 24 << 20, True),
            (chunk.chunk_id, 24 << 20, 20 << 20, False),
        ]
        live_after_candidate_eviction = sum(
            size for _, _, size, candidate in exact_bindings if not candidate
        )
        self.assertEqual(live_after_candidate_eviction, 20 << 20)
        self.assertNotEqual(live_after_candidate_eviction, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
