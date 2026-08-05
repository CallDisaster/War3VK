#!/usr/bin/env python3
"""资源驻留 chunk 数值身份的纯合成闭合测试。

本脚本不会加载 DXVK、AutoTest 或《魔兽争霸 III》。它只向 analyzer 提供合成 JSON，
并静态检查 C++ 路径没有把原始指针或释放权限带入普查。
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

import analyze_resource_residency_census as analyzer


ROOT = Path(__file__).resolve().parents[1]
CENSUS_HEADER = (
    ROOT / "src" / "d3d9" / "war3" / "tools"
    / "war3_resource_residency_census.h"
)
CENSUS_CPP = CENSUS_HEADER.with_suffix(".cpp")
TEXTURE_CPP = ROOT / "src" / "d3d9" / "d3d9_common_texture.cpp"
PERF_CPP = (
    ROOT / "src" / "d3d9" / "war3" / "tools" / "war3_perf_monitor.cpp"
)
DEVICE_CPP = ROOT / "src" / "d3d9" / "d3d9_device.cpp"
RUNNER = ROOT / "AutoTest" / "run_resource_residency_census_isolated.py"


def _bucket(name: str, host_bytes: int = 0) -> dict[str, object]:
    return {
        "class": name,
        "pool": 1,
        "mapMode": 1,
        "dynamic": False,
        "writeOnly": False,
        "resources": 1,
        "logicalBytes": host_bytes,
        "deviceAllocationBytes": host_bytes,
        "hostBackingLogicalBytes": host_bytes,
        "hostMappedLogicalBytes": 0,
        "duplicateHostBackingLogicalBytes": host_bytes,
        "readLocks": 0,
        "partialSubresourceWriteLocks": 0,
        "externalDirtyCalls": 0,
        "observedCandidateHostBytes": host_bytes,
        "lazyReadbackCandidateHostBytes": 0,
    }


def _valid_report() -> dict[str, object]:
    coverage = {
        "hostBackingResources": 2,
        "hostBackingLogicalBytes": 500,
        "exactD3D9MemoryBindingResources": 1,
        "exactD3D9MemoryBindingLogicalBytes": 480,
        "exactBindings": 1,
        "boundAlignedSliceBytes": 512,
        "mappedBindingAlignedSliceBytes": 512,
        "externalHostBackingResources": 1,
        "externalHostBackingLogicalBytes": 20,
        "unresolvedD3D9MemoryResources": 0,
        "unresolvedD3D9MemoryLogicalBytes": 0,
        "unregisteredHostBackingResources": 0,
        "unregisteredHostBackingLogicalBytes": 0,
        "missingChunkBindingCount": 0,
        "invalidBindingCount": 0,
        "outOfBoundsBindingCount": 0,
        "duplicateBindingCount": 0,
        "overlapBindingCount": 0,
        "unclassifiedAlignedSliceBytes": 0,
        "unregisteredAllocatorPayloadBytes": 0,
        "boundBytesExceedAllocatorUsed": False,
        "bindingBytesClosure": True,
    }
    chunk = {
        "chunkId": 7,
        "reserveBytes": 1024,
        "chunkOccupiedBytes": 576,
        "freePayloadBytes": 448,
        "freeRangeCount": 1,
        "sharedMappedRefs": 1,
        "sharedMappedBytes": 256,
        "standaloneMappedRefs": 0,
        "standaloneMappedBytes": 0,
        "mapFailureCount": 0,
        "unmapFailureCount": 0,
        "mappingStateFaultCount": 0,
        "boundResources": 1,
        "boundLiveAlignedSliceBytes": 512,
        "candidateAlignedSliceBytes": 512,
        "directCandidateAlignedSliceBytes": 512,
        "lazyReadbackCandidateAlignedSliceBytes": 0,
        "nonCandidateAlignedSliceBytes": 0,
        "unclassifiedAlignedSliceBytes": 0,
        "mappedBindingAlignedSliceBytes": 512,
        "observedCandidateOnly": True,
        "reserveReclaimUpperBoundBytes": 1024,
        "mappedReclaimUpperBoundBytes": 256,
    }
    diagnostics = {
        "contract": "d3d9-host-chunk-diagnostics-v1",
        "available": True,
        "authority": False,
        "chunkBacked": True,
        "accountingClosure": True,
        "mutationGeneration": 19,
        "mutationGenerationBegin": 19,
        "mutationGenerationEnd": 19,
        "generationStable": True,
        "mutationGenerationSaturated": False,
        "reserveBytes": 1024,
        "allocatorUsedPayloadBytes": 512,
        "chunkOccupiedBytes": 576,
        "internalFragmentationBytes": 64,
        "freePayloadBytes": 448,
        "sharedMappedRefs": 1,
        "sharedMappedBytes": 256,
        "standaloneMappedRefs": 0,
        "standaloneMappedBytes": 0,
        "mappedRefs": 1,
        "mappedBytes": 256,
        "mapFailureCount": 0,
        "unmapFailureCount": 0,
        "mappingStateFaultCount": 0,
        "duplicateAllocatorChunkIdCount": 0,
        "candidateOnlyChunkCount": 1,
        "candidateOnlyReserveBytesUpperBound": 1024,
        "candidateOnlyMappedBytesUpperBound": 256,
        "bindingCoverage": coverage,
        "chunks": [chunk],
    }
    gpu_pools = {
        "staticAtlasReservedBytes": 4096,
        "staticAtlasUsedBytes": 1024,
        "staticReadyRecords": 1,
        "staticPendingRecords": 0,
        "staticInvalidRecords": 0,
        "queuedStaticMissHostBytes": 0,
        "peakQueuedStaticMissHostBytes": 20,
        "uploadResidentBytes": 20,
        "uploadActiveUsedBytes": 20,
        "uploadPendingUsedBytes": 0,
        "uploadRetiredUsedBytes": 0,
        "uploadIdleCapacityBytes": 0,
        "outputResidentBytes": 256,
        "outputCursorBytes": 128,
        "outputOutstandingSlices": 0,
    }
    buckets = [
        _bucket("texture2D", 480),
        _bucket("gpuSkinStaticMirror"),
        _bucket("gpuSkinStaticAtlas"),
        _bucket("gpuSkinUploadPage", 20),
        _bucket("gpuSkinOutputPage"),
    ]
    census = {
        "enabled": True,
        "contract": "diagnostics-only-observation-v1",
        "evictionAuthority": False,
        "performanceComparable": False,
        "processPrivateCommitComparable": False,
        "snapshotCoverage": "known-live-steady-plus-gpu-skin-pool-peaks-v1",
        "gameDllOriginalAllocationsIncluded": False,
        "driverAllocatorPoolReserveIncluded": False,
        "transientShadowBuildCopiesIncluded": False,
        "retainedOldPublishedStoresIncluded": False,
        "resourceFieldsAreQuiescentSnapshot": False,
        "gpuSkinQueuedMissPeakIncluded": True,
        "frameSerial": 300,
        "lifetimeRegistrations": 5,
        "liveResources": 5,
        "totals": {
            "hostBackingLogicalBytes": 500,
            "observedCandidateHostBytes": 480,
            "lazyReadbackCandidateHostBytes": 0,
        },
        "d3d9HostAllocator": {
            "allocatedBackingBytes": 1024,
            "usedPayloadBytes": 512,
            "mappedAddressBytes": 256,
            "chunkDiagnostics": diagnostics,
        },
        "gpuSkinPools": gpu_pools,
        "modelCache": {
            "uniqueGeosetCapacityBytes": 1,
            "aliasDuplicateCapacityBytes": 0,
            "shadowStorePayloadCapacityBytes": 1,
            "hashContainerOverheadIncluded": 0,
            "shadowStoreHashContainerOverheadIncluded": 0,
        },
        "buckets": buckets,
        "largestHostBackings": [],
    }
    start = 116_444_736_000_010_000
    return {
        "gpuSkinSnapshot": {
            "processId": 123,
            "processStartFileTime100ns": start,
            "processStartFileTime100nsExact": str(start),
        },
        "resourceResidencyCensus": census,
    }


def _analyze(report: dict[str, object]) -> dict[str, object]:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "report.json"
        path.write_text(json.dumps(report), encoding="utf-8")
        return analyzer.analyze(path)


class ResourceResidencyChunkCensusOfflineTests(unittest.TestCase):
    @staticmethod
    def _make_generation_unstable(
        report: dict[str, object], end_generation: int
    ) -> dict[str, object]:
        diagnostics = report["resourceResidencyCensus"]["d3d9HostAllocator"][
            "chunkDiagnostics"
        ]
        diagnostics["mutationGenerationEnd"] = end_generation
        diagnostics["generationStable"] = False
        diagnostics["bindingCoverage"]["bindingBytesClosure"] = False
        diagnostics["candidateOnlyChunkCount"] = 0
        diagnostics["candidateOnlyReserveBytesUpperBound"] = 0
        diagnostics["candidateOnlyMappedBytesUpperBound"] = 0
        chunk = diagnostics["chunks"][0]
        chunk["observedCandidateOnly"] = False
        chunk["reserveReclaimUpperBoundBytes"] = 0
        chunk["mappedReclaimUpperBoundBytes"] = 0
        return report

    def test_valid_candidate_only_chunk_closes_without_authority(self) -> None:
        result = _analyze(_valid_report())
        contract = result["allocatorChunkContract"]
        self.assertTrue(contract["closed"])
        self.assertFalse(contract["authority"])
        self.assertEqual(contract["candidateOnlyChunkCount"], 1)
        self.assertEqual(contract["candidateOnlyReserveBytesUpperBound"], 1024)
        self.assertEqual(contract["candidateOnlyMappedBytesUpperBound"], 256)

    def test_mixed_chunk_is_closed_but_has_no_reclaim_upper_bound(self) -> None:
        report = _valid_report()
        diagnostics = report["resourceResidencyCensus"]["d3d9HostAllocator"][
            "chunkDiagnostics"
        ]
        chunk = diagnostics["chunks"][0]
        chunk["candidateAlignedSliceBytes"] = 256
        chunk["directCandidateAlignedSliceBytes"] = 256
        chunk["nonCandidateAlignedSliceBytes"] = 256
        chunk["observedCandidateOnly"] = False
        chunk["reserveReclaimUpperBoundBytes"] = 0
        chunk["mappedReclaimUpperBoundBytes"] = 0
        diagnostics["candidateOnlyChunkCount"] = 0
        diagnostics["candidateOnlyReserveBytesUpperBound"] = 0
        diagnostics["candidateOnlyMappedBytesUpperBound"] = 0
        contract = _analyze(report)["allocatorChunkContract"]
        self.assertTrue(contract["closed"])
        self.assertEqual(contract["candidateOnlyChunkCount"], 0)

    def test_unregistered_allocator_payload_fails_closed(self) -> None:
        report = _valid_report()
        diagnostics = report["resourceResidencyCensus"]["d3d9HostAllocator"][
            "chunkDiagnostics"
        ]
        coverage = diagnostics["bindingCoverage"]
        coverage["boundAlignedSliceBytes"] = 448
        coverage["unregisteredAllocatorPayloadBytes"] = 64
        coverage["bindingBytesClosure"] = False
        chunk = diagnostics["chunks"][0]
        chunk["boundLiveAlignedSliceBytes"] = 448
        chunk["candidateAlignedSliceBytes"] = 448
        chunk["directCandidateAlignedSliceBytes"] = 448
        chunk["mappedBindingAlignedSliceBytes"] = 448
        chunk["observedCandidateOnly"] = False
        chunk["reserveReclaimUpperBoundBytes"] = 0
        chunk["mappedReclaimUpperBoundBytes"] = 0
        coverage["mappedBindingAlignedSliceBytes"] = 448
        diagnostics["candidateOnlyChunkCount"] = 0
        diagnostics["candidateOnlyReserveBytesUpperBound"] = 0
        diagnostics["candidateOnlyMappedBytesUpperBound"] = 0
        contract = _analyze(report)["allocatorChunkContract"]
        self.assertFalse(contract["closed"])
        self.assertEqual(contract["unregisteredAllocatorPayloadBytes"], 64)
        self.assertEqual(contract["candidateOnlyChunkCount"], 0)

    def test_legacy_report_remains_analyzable_but_not_closed(self) -> None:
        report = _valid_report()
        del report["resourceResidencyCensus"]["d3d9HostAllocator"][
            "chunkDiagnostics"
        ]
        contract = _analyze(report)["allocatorChunkContract"]
        self.assertTrue(contract["legacyInput"])
        self.assertFalse(contract["available"])
        self.assertFalse(contract["closed"])

    def test_allocator_generation_drift_clears_all_upper_bounds(self) -> None:
        report = self._make_generation_unstable(_valid_report(), 20)
        contract = _analyze(report)["allocatorChunkContract"]
        self.assertFalse(contract["generationStable"])
        self.assertTrue(contract["generationFieldsClosed"])
        self.assertFalse(contract["bindingBytesClosure"])
        self.assertEqual(contract["candidateOnlyChunkCount"], 0)
        self.assertEqual(contract["candidateOnlyReserveBytesUpperBound"], 0)
        self.assertEqual(contract["candidateOnlyMappedBytesUpperBound"], 0)
        self.assertFalse(contract["closed"])

    def test_same_generation_structural_drift_still_fails_closed(self) -> None:
        # C++ 会在结构、映射或故障累计字段漂移时把 generationStable 清零，
        # 即使两端 generation 数字相同也不能恢复回收上限。
        report = self._make_generation_unstable(_valid_report(), 19)
        contract = _analyze(report)["allocatorChunkContract"]
        self.assertFalse(contract["generationStable"])
        self.assertTrue(contract["generationFieldsClosed"])
        self.assertEqual(contract["candidateOnlyReserveBytesUpperBound"], 0)
        self.assertFalse(contract["closed"])

    def test_generation_alias_mismatch_cannot_preserve_upper_bound(self) -> None:
        report = _valid_report()
        diagnostics = report["resourceResidencyCensus"]["d3d9HostAllocator"][
            "chunkDiagnostics"
        ]
        diagnostics["mutationGeneration"] = 18
        contract = _analyze(report)["allocatorChunkContract"]
        self.assertFalse(contract["generationFieldsClosed"])
        self.assertFalse(contract["bindingBytesClosure"])
        self.assertFalse(contract["candidateUpperBoundClosed"])
        self.assertEqual(contract["candidateOnlyReserveBytesUpperBound"], 0)
        self.assertFalse(contract["closed"])

    def test_static_path_has_numeric_binding_and_runner_hard_gate(self) -> None:
        header = CENSUS_HEADER.read_text(encoding="utf-8")
        begin = header.index("struct HostBackingBinding")
        end = header.index("struct ResourceRegistration", begin)
        binding = header[begin:end]
        self.assertIn("uint64_t chunkId", binding)
        self.assertIn("uint64_t offset", binding)
        self.assertIn("uint64_t alignedSliceBytes", binding)
        self.assertNotIn("void*", binding)

        census = CENSUS_CPP.read_text(encoding="utf-8")
        texture = TEXTURE_CPP.read_text(encoding="utf-8")
        serializer = PERF_CPP.read_text(encoding="utf-8")
        runner = RUNNER.read_text(encoding="utf-8")
        self.assertIn("GetDiagnosticBinding()", texture)
        self.assertIn("bindingCoverage.bindingBytesClosure", census)
        self.assertNotIn("UpdateD3D9HostAllocator", texture)
        self.assertIn("mutationGenerationBegin", census)
        self.assertIn("mutationGenerationEnd", census)
        self.assertIn("SameAllocatorSnapshot", census)
        self.assertIn("CaptureSnapshot(m_resourceCensusAllocator)", serializer)
        self.assertIn('\\"authority\\":false', serializer)
        self.assertIn("chunk_contract.get(\"closed\") is True", runner)
        self.assertIn("DEFAULT_MAP_PATH", runner)
        self.assertIn("(4)生与死v1.28读档bug修复.w3x", runner)
        self.assertIn('"--map-path"', runner)
        self.assertIn("diagnostics-only-resource-census-runner-v2", runner)

    def test_host_backing_identity_is_published_under_one_mutex(self) -> None:
        census = CENSUS_CPP.read_text(encoding="utf-8")
        update_begin = census.index("void UpdateHostBacking(")
        update_end = census.index("void NoteLock(", update_begin)
        update = census[update_begin:update_end]
        lock_at = update.index("std::lock_guard lock(handle->hostBindingMutex)")
        self.assertLess(lock_at, update.index("hostBackingLogicalBytes.store"))
        self.assertLess(lock_at, update.index("hostMappedLogicalBytes.store"))
        self.assertLess(
            lock_at, update.index("duplicateHostBackingLogicalBytes.store")
        )
        self.assertLess(lock_at, update.index("handle->hostBinding = hostBinding"))

        copy_begin = census.index("CoherentHostBacking CopyHostBacking(")
        copy_end = census.index("struct PendingHostBinding", copy_begin)
        copy_body = census[copy_begin:copy_end]
        copy_lock_at = copy_body.index(
            "std::lock_guard lock(record.hostBindingMutex)"
        )
        for field in (
            "record.hostBackingLogicalBytes.load",
            "record.hostMappedLogicalBytes.load",
            "record.duplicateHostBackingLogicalBytes.load",
            "result.binding = record.hostBinding",
        ):
            self.assertLess(copy_lock_at, copy_body.index(field))

    def test_allocator_lifetime_and_two_snapshot_window_are_explicit(self) -> None:
        census = CENSUS_CPP.read_text(encoding="utf-8")
        capture_begin = census.index("ResourceResidencySnapshot CaptureSnapshot(")
        capture_end = census.index("const char* ResourceClassName", capture_begin)
        capture = census[capture_begin:capture_end]
        first = capture.index("allocator->CaptureDiagnosticSnapshot()")
        resource_scan = capture.index("for (const auto& record : live)")
        second = capture.index(
            "allocator->CaptureDiagnosticSnapshot()", first + 1
        )
        self.assertLess(first, resource_scan)
        self.assertLess(resource_scan, second)
        self.assertIn("SameAllocatorSnapshot(allocatorBegin, allocatorEnd)", capture)
        self.assertIn("chunkDiagnostics.generationStable", capture)

        monitor = PERF_CPP.read_text(encoding="utf-8")
        device = DEVICE_CPP.read_text(encoding="utf-8")
        setter_begin = monitor.index(
            "void War3PerfMonitor::setResourceCensusAllocator("
        )
        setter_end = monitor.index(
            "void War3PerfMonitor::clearResourceCensusAllocator(",
            setter_begin,
        )
        setter = monitor[setter_begin:setter_end]
        clear_end = monitor.index("void War3PerfMonitor::shutdown()", setter_end)
        clear = monitor[setter_end:clear_end]
        self.assertIn("std::lock_guard lock(m_mutex)", setter)
        self.assertIn("std::lock_guard lock(m_mutex)", clear)
        self.assertIn("m_resourceCensusAllocator == allocator", clear)

        active = device.index("war3::SetActiveDevice(this);")
        register = device.index("setResourceCensusAllocator(", active)
        constructor_success_tail = device.index(
            "m_unlockAdditionalFormats = m_parent->HasFormatsUnlocked();",
            active,
        )
        self.assertLess(active, register)
        self.assertLess(constructor_success_tail, register)
        destructor = device.index("D3D9DeviceEx::~D3D9DeviceEx()")
        export = device.index("perfMonitor.exportHtmlReport", destructor)
        clear_owner = device.index(
            "perfMonitor.clearResourceCensusAllocator(&m_memoryAllocator)",
            destructor,
        )
        shutdown = device.index("perfMonitor.shutdown()", destructor)
        self.assertLess(export, clear_owner)
        self.assertLess(clear_owner, shutdown)


if __name__ == "__main__":
    unittest.main(verbosity=2)
