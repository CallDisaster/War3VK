#!/usr/bin/env python3
"""Contracts for CPU-readable shadow sources and atomic Arena bundles."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


SPAN_H = read("src/d3d9/war3/memory/war3_cpu_readable_buffer_span.h")
SPAN_CPP = read("src/d3d9/war3/memory/war3_cpu_readable_buffer_span.cpp")
SPAN_TEST = read(
    "src/d3d9/war3/memory/tests/war3_cpu_readable_buffer_span_test.cpp"
)
ARENA_H = read("src/d3d9/war3/memory/war3_shadow_arena.h")
ARENA_CPP = read("src/d3d9/war3/memory/war3_shadow_arena.cpp")
DEVICE_H = read("src/d3d9/d3d9_device.h")
DEVICE_CPP = read("src/d3d9/d3d9_device.cpp")
DIAG_H = read("src/d3d9/war3/tools/war3_diagnostics_hub.h")
DIAG_CPP = read("src/d3d9/war3/tools/war3_diagnostics_hub.cpp")
MESON = read("src/d3d9/meson.build")


@dataclass
class Cursor:
    page: int = 0
    offset: int = 0
    committed: int = 0
    tail_waste: int = 0


def simulate_bundle(cursor: Cursor, page_bytes: int, max_bytes: int,
                    sizes: tuple[int, ...]) -> bool:
    start = Cursor(**cursor.__dict__)
    for size in sizes:
        if size <= 0 or size > page_bytes:
            cursor.__dict__.update(start.__dict__)
            return False
        if cursor.offset + size > page_bytes:
            cursor.tail_waste += page_bytes - cursor.offset
            cursor.committed += page_bytes
            cursor.page += 1
            cursor.offset = 0
        if cursor.committed + cursor.offset + size > max_bytes:
            cursor.__dict__.update(start.__dict__)
            return False
        cursor.offset += size
    return True


class ShadowArenaCrashContractStaticTests(unittest.TestCase):
    def test_span_uses_real_allocation_range_and_generation(self) -> None:
        for token in (
            "allocationBytes",
            "requestedOffset",
            "requestedBytes",
            "ownerIdentity",
            "identityGeneration",
            "allocationGeneration",
            "contentGeneration",
            "cpuReadable",
            "AddressOverflow",
        ):
            self.assertIn(token, SPAN_H + SPAN_CPP)
        self.assertIn(
            "input.requestedBytes > input.allocationBytes - input.requestedOffset",
            SPAN_CPP,
        )
        self.assertIn("getBufferInfo()", DEVICE_CPP)
        self.assertGreaterEqual(
            DEVICE_CPP.count("BuildWar3CpuReadableBufferSpan({"), 10
        )

    def test_guard_page_up_and_stale_generation_are_runnable(self) -> None:
        for token in (
            "PAGE_NOACCESS",
            "OffsetOutsideAllocation",
            "LengthOutsideAllocation",
            "MissingGeneration",
            "NotCpuReadable",
            "AddressOverflow",
            "ownedUpBytes",
        ):
            self.assertIn(token, SPAN_TEST)
        self.assertIn("war3_cpu_readable_buffer_span_test", MESON)

    def test_exact_index_domain_compacts_only_proven_current_generation(self) -> None:
        for token in (
            "War3ExactIndexVertexDomain",
            "ComputeWar3ExactIndexVertexDomain",
            "indexElementBytes != 2u && indexElementBytes != 4u",
            "vertex < 0 || vertex >= int64_t(vertexCapacity)",
        ):
            self.assertIn(token, SPAN_H + SPAN_CPP)
        for token in (
            "exactIndexedFreezeTrimCandidate",
            "(DynamicSysmemVBOs || posDynamic)",
            "exactIndexCommon->GetMappingBufferSequenceNumber()",
            "ComputeWar3ExactIndexVertexDomain(",
            "posFreezeByteOffset",
            "capturedVertexOffset = exactIndexedFreezeTrimmed",
            "ShadowArena_NoteExactIndexTrim(",
        ):
            self.assertIn(token, DEVICE_CPP)
        self.assertIn("TestExactIndexVertexDomain", SPAN_TEST)

    def test_arena_bundle_is_all_or_nothing(self) -> None:
        for token in (
            "ShadowArenaBundleTransaction",
            "ShadowArena_BeginBundle",
            "ShadowArena_CommitBundle",
            "ShadowArena_RollbackBundle",
            "startPage",
            "startOffset",
            "startCommittedBytes",
            "startPageTailWasteBytes",
        ):
            self.assertIn(token, ARENA_H + ARENA_CPP)
        rollback = ARENA_CPP.split("bool ShadowArena_RollbackBundle(", 1)[1]
        rollback = rollback.split("void ShadowArena_NoteFreezeCatalogBytes", 1)[0]
        for restore in (
            "frameState.currentPage = transaction.startPage",
            "frameState.currentOffset = transaction.startOffset",
            "frameState.committedBytes = transaction.startCommittedBytes",
            "frameState.pageTailWasteBytes = transaction.startPageTailWasteBytes",
        ):
            self.assertIn(restore, rollback)

    def test_multi_page_failure_model_restores_every_cursor(self) -> None:
        cursor = Cursor(page=1, offset=48, committed=64, tail_waste=3)
        before = Cursor(**cursor.__dict__)
        self.assertFalse(simulate_bundle(cursor, 64, 192, (16, 48, 80)))
        self.assertEqual(before, cursor)

        self.assertTrue(simulate_bundle(cursor, 64, 192, (16, 32, 16)))
        self.assertNotEqual(before, cursor)

    def test_device_reserves_before_recording_any_copy(self) -> None:
        freeze = DEVICE_CPP.split("struct FrameFreezePlan", 1)[1]
        freeze = freeze.split("shadowCapturePostTiming.pause();", 1)[0]
        begin = freeze.index("ShadowArena_BeginBundle(")
        commit = freeze.index("ShadowArena_CommitBundle(")
        emit = freeze.index("EmitCs(")
        self.assertLess(begin, commit)
        self.assertLess(commit, emit)
        self.assertIn("ShadowArena_RollbackBundle(arenaTransaction)", freeze)
        self.assertEqual(1, freeze.count("EmitCs("))
        self.assertIn("FrameFreezeCopyCommand", freeze)
        self.assertIn("for (uint32_t i = 0u; i < cCopyCount; ++i)", freeze)

    def test_admission_is_clamped_to_real_generation_remaining(self) -> None:
        self.assertIn("ShadowArena_RemainingBytes()", DEVICE_CPP)
        self.assertIn("std::min(configuredRemainingBytes, arenaRemainingBytes)", DEVICE_CPP)
        self.assertIn("g_arenaMaxFrameSize - used", ARENA_CPP)
        self.assertIn("384 * 1024 * 1024", ARENA_CPP)
        self.assertIn("size > g_arenaPageSize", ARENA_CPP)

    def test_freeze_catalog_is_current_frame_and_exact_generation(self) -> None:
        for token in (
            "War3FrameFreezeKey",
            "sourceOffset",
            "sourceLength",
            "sourceElementStride",
            "sourceElementSize",
            "allocationIdentity",
            "sourceOwner",
            "identityGeneration",
            "allocationGeneration",
            "contentGeneration",
            "frameSerial",
            "streamType",
        ):
            self.assertIn(token, DEVICE_H)
        self.assertIn(
            "m_war3FrameFreezeCatalogSerial != freezeFrameSerial", DEVICE_CPP
        )
        self.assertIn("m_war3FrameFreezeCatalog.clear();", DEVICE_CPP)
        self.assertIn("common->GetMappedSlice()", DEVICE_CPP)
        self.assertIn("reinterpret_cast<uintptr_t>(sourceMapping.ptr())", DEVICE_CPP)
        self.assertNotIn("fingerprint", DEVICE_H.split("War3FrameFreezeKey", 1)[1].split("War3FrameFreezeEntry", 1)[0].lower())

    def test_runtime_and_incident_diagnostics_cover_integrity_failures(self) -> None:
        for token in (
            "shadowArenaReservedBytes",
            "shadowArenaCommittedBytes",
            "shadowArenaRolledBackBytes",
            "shadowArenaAdmissionRejectedCount",
            "shadowArenaPartialTransactionCount",
            "shadowArenaExactIndexTrimAcceptedCount",
            "shadowArenaExactIndexTrimRejectedCount",
            "shadowArenaExactIndexTrimBytesSaved",
            "shadowCpuSpanRejectedCount",
            "shadowCpuSpanLastRejectReason",
        ):
            self.assertIn(token, DIAG_H)
            self.assertIn(token, DIAG_CPP)
        for reason in (
            "shadow-arena-partial-transaction",
            "shadow-arena-overflow",
            "shadow-arena-admission-rejected",
        ):
            self.assertIn(reason, DIAG_CPP)


if __name__ == "__main__":
    unittest.main()
