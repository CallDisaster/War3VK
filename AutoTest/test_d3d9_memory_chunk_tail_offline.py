#!/usr/bin/env python3
"""针对 D3D9MemoryChunk 账本的确定性契约测试。

本脚本有意不加载 DXVK 或《魔兽争霸 III》。它精确模拟 D3D9MemoryChunk 使用的首次适配、
拆分与合并规则，并检查 C++ 实现是否保留小于 4 KiB 的尾部空间，而不是静默遗失这部分
空间。源码检查还要求 MapViewOfFile 调用失败时不得改动引用计数与映射内存账本。
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import random
import unittest


ROOT = Path(__file__).resolve().parents[1]
CPP_PATH = ROOT / "src" / "d3d9" / "d3d9_mem.cpp"


@dataclass
class Range:
    offset: int
    length: int


@dataclass(frozen=True)
class Allocation:
    chunk: "Chunk"
    offset: int
    size: int


class Chunk:
    def __init__(self, size: int) -> None:
        if size <= 0:
            raise ValueError("chunk size must be positive")
        self.size = size
        self.free_ranges = [Range(0, size)]
        self.live: dict[int, int] = {}

    def alloc(self, size: int) -> Allocation | None:
        if size <= 0:
            raise ValueError("allocation size must be positive")

        for index, free_range in enumerate(self.free_ranges):
            if free_range.length < size:
                continue

            offset = free_range.offset
            free_range.offset += size
            free_range.length -= size
            if free_range.length == 0:
                del self.free_ranges[index]

            if offset in self.live:
                raise AssertionError("duplicate live allocation offset")
            self.live[offset] = size
            self.validate()
            return Allocation(self, offset, size)

        self.validate()
        return None

    def free(self, allocation: Allocation) -> None:
        if allocation.chunk is not self:
            raise AssertionError("allocation belongs to another chunk")
        if self.live.pop(allocation.offset, None) != allocation.size:
            raise AssertionError("allocation was not live")

        offset = allocation.offset
        size = allocation.size
        index = 0
        while index < len(self.free_ranges):
            current = self.free_ranges[index]
            if current.offset == offset + size:
                size += current.length
                del self.free_ranges[index]
            elif current.offset + current.length == offset:
                offset -= current.length
                size += current.length
                del self.free_ranges[index]
            else:
                index += 1

        self.free_ranges.append(Range(offset, size))
        self.validate()

    def is_empty(self) -> bool:
        return (
            len(self.free_ranges) == 1
            and self.free_ranges[0].offset == 0
            and self.free_ranges[0].length == self.size
        )

    def validate(self) -> None:
        ordered = sorted(self.free_ranges, key=lambda entry: entry.offset)
        assert all(entry.length > 0 for entry in ordered), "zero-length free range"
        assert all(
            0 <= entry.offset < self.size
            and entry.offset + entry.length <= self.size
            for entry in ordered
        ), "free range outside chunk"
        assert all(
            left.offset + left.length <= right.offset
            for left, right in zip(ordered, ordered[1:])
        ), "overlapping free ranges"
        assert sum(entry.length for entry in ordered) + sum(self.live.values()) == self.size


class Allocator:
    def __init__(self, chunk_size: int) -> None:
        self.chunk_size = chunk_size
        self.chunks: list[Chunk] = []
        self.used_memory = 0
        self.allocated_memory = 0

    def alloc(self, size: int) -> Allocation:
        for chunk in self.chunks:
            allocation = chunk.alloc(size)
            if allocation is not None:
                self.used_memory += allocation.size
                return allocation

        chunk = Chunk(max(self.chunk_size, size))
        self.chunks.append(chunk)
        self.allocated_memory += chunk.size
        allocation = chunk.alloc(size)
        assert allocation is not None
        self.used_memory += allocation.size
        return allocation

    def free(self, allocation: Allocation) -> None:
        allocation.chunk.free(allocation)
        self.used_memory -= allocation.size
        if allocation.chunk.is_empty():
            self.allocated_memory -= allocation.chunk.size
            self.chunks.remove(allocation.chunk)


class D3D9MemoryChunkTailTests(unittest.TestCase):
    def test_cpp_uses_exact_exhaustion_not_sub_4k_tail_swallow(self) -> None:
        source = CPP_PATH.read_text(encoding="utf-8")
        begin = source.index("D3D9Memory D3D9MemoryChunk::AllocLocked")
        end = source.index("void D3D9MemoryChunk::FreeLocked", begin)
        body = source[begin:end]

        self.assertIn("if (range->length == 0)", body)
        self.assertNotIn("range->length < (4 << 10)", body)
        self.assertNotIn("size += range->length", body)
        self.assertIn("D3D9Memory(this, offset, Size)", body)

    def test_cpp_map_failure_does_not_publish_or_account_mapping(self) -> None:
        source = CPP_PATH.read_text(encoding="utf-8")
        begin = source.index("void* D3D9MemoryChunk::MapLocked")
        end = source.index("uint32_t D3D9MemoryChunk::UnmapLocked", begin)
        body = source[begin:end]

        large_begin = body.index("if (alignedSize > mappingGranularity)")
        small_begin = body.index("auto& mappingRange", large_begin)
        large_body = body[large_begin:small_begin]
        small_body = body[small_begin:]

        large_failure = large_body.index("if (unlikely(basePtr == nullptr))")
        large_return = large_body.index("return nullptr;", large_failure)
        large_account = large_body.index("mappedSize = alignedSize;")
        self.assertLess(large_return, large_account)

        small_failure = small_body.index(
            "if (unlikely(mappingRange.ptr == nullptr))"
        )
        small_return = small_body.index("return nullptr;", small_failure)
        small_account = small_body.index("mappedSize = mappingGranularity;")
        small_ref = small_body.index("mappingRange.refCount++;")
        self.assertLess(small_return, small_account)
        self.assertLess(small_return, small_ref)

    def test_cpp_zero_request_cannot_create_an_empty_chunk(self) -> None:
        source = CPP_PATH.read_text(encoding="utf-8")
        begin = source.index("D3D9Memory D3D9MemoryAllocator::Alloc")
        end = source.index("void D3D9MemoryAllocator::Free", begin)
        body = source[begin:end]

        zero_guard = body.index("if (unlikely(Size == 0))")
        zero_return = body.index("return {};", zero_guard)
        chunk_create = body.index("new D3D9MemoryChunk", zero_return)
        self.assertLess(zero_guard, zero_return)
        self.assertLess(zero_return, chunk_create)

    def test_sub_4k_tails_remain_free_and_finally_close(self) -> None:
        chunk_size = 16 * 1024
        for tail in (1, 63, 64, 4095):
            with self.subTest(tail=tail):
                chunk = Chunk(chunk_size)
                requested = chunk_size - tail
                allocation = chunk.alloc(requested)
                self.assertIsNotNone(allocation)
                assert allocation is not None

                self.assertEqual(allocation.size, requested)
                self.assertEqual([(r.offset, r.length) for r in chunk.free_ranges], [
                    (requested, tail),
                ])
                chunk.free(allocation)
                self.assertTrue(chunk.is_empty())

    def test_exact_fit_erases_instead_of_retaining_zero_length_range(self) -> None:
        chunk = Chunk(4096)
        allocation = chunk.alloc(4096)
        self.assertIsNotNone(allocation)
        self.assertEqual(chunk.free_ranges, [])
        assert allocation is not None
        chunk.free(allocation)
        self.assertTrue(chunk.is_empty())

    def test_deterministic_random_free_order_closes_one_chunk(self) -> None:
        rng = random.Random(0xD3D9)
        chunk = Chunk(256 * 1024)
        allocations: list[Allocation] = []

        while True:
            allocation = chunk.alloc(rng.randint(1, 4095))
            if allocation is None:
                break
            allocations.append(allocation)

        self.assertGreater(len(allocations), 50)
        rng.shuffle(allocations)
        for allocation in allocations:
            chunk.free(allocation)
        self.assertTrue(chunk.is_empty())

    def test_multi_chunk_random_free_restores_all_accounting(self) -> None:
        rng = random.Random(0xC10_5E)
        allocator = Allocator(8192)
        allocations = [allocator.alloc(rng.randint(1, 3000)) for _ in range(200)]

        self.assertGreater(len(allocator.chunks), 1)
        self.assertEqual(allocator.used_memory, sum(item.size for item in allocations))
        self.assertTrue(all(
            free_range.length > 0
            for chunk in allocator.chunks
            for free_range in chunk.free_ranges
        ))

        rng.shuffle(allocations)
        for allocation in allocations:
            allocator.free(allocation)

        self.assertEqual(allocator.used_memory, 0)
        self.assertEqual(allocator.allocated_memory, 0)
        self.assertEqual(allocator.chunks, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
