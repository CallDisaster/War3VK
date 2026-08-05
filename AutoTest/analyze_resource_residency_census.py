#!/usr/bin/env python3
"""汇总仅用于诊断的 War3 资源驻留普查。

输入可以是 War3 性能 HTML 报告（``const data = {...}``），也可以是其中嵌入的
JSON 本身。本工具绝不会把观察结果视为驱逐授权；它只会把原始账本整理成紧凑的
优先级摘要。
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def _load_report(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() == ".json":
        raw = json.loads(text)
    else:
        marker = "const data = "
        offset = text.find(marker)
        if offset < 0:
            raise ValueError("embedded report data marker not found")
        raw, _ = json.JSONDecoder().raw_decode(text[offset + len(marker):])
    if not isinstance(raw, dict):
        raise ValueError("report root is not an object")
    return raw


def _u64(obj: dict[str, Any], name: str) -> int:
    value = obj.get(name)
    if type(value) is not int or value < 0:
        raise ValueError(f"{name} is not an unsigned integer")
    return value


def _bool(obj: dict[str, Any], name: str) -> bool:
    value = obj.get(name)
    if type(value) is not bool:
        raise ValueError(f"{name} is not a boolean")
    return value


def _analyze_chunk_diagnostics(
    allocator: dict[str, Any], totals: dict[str, Any]
) -> dict[str, Any]:
    """独立闭合 chunk 数值身份；结果始终只是观察，不产生释放权限。"""
    raw = allocator.get("chunkDiagnostics")
    if raw is None:
        return {
            "contract": "d3d9-host-chunk-diagnostics-v1",
            "available": False,
            "legacyInput": True,
            "authority": False,
            "closed": False,
        }
    if not isinstance(raw, dict):
        raise ValueError("d3d9HostAllocator.chunkDiagnostics is malformed")
    if raw.get("contract") != "d3d9-host-chunk-diagnostics-v1":
        raise ValueError("unexpected D3D9 host chunk diagnostics contract")
    if _bool(raw, "authority") is not False:
        raise ValueError("chunk diagnostics must not claim authority")

    available = _bool(raw, "available")
    chunk_backed = _bool(raw, "chunkBacked")
    accounting_closure = _bool(raw, "accountingClosure")
    generation_saturated = _bool(raw, "mutationGenerationSaturated")
    generation_stable = _bool(raw, "generationStable")
    numeric_names = (
        "mutationGeneration",
        "mutationGenerationBegin",
        "mutationGenerationEnd",
        "reserveBytes",
        "allocatorUsedPayloadBytes",
        "chunkOccupiedBytes",
        "internalFragmentationBytes",
        "freePayloadBytes",
        "sharedMappedRefs",
        "sharedMappedBytes",
        "standaloneMappedRefs",
        "standaloneMappedBytes",
        "mappedRefs",
        "mappedBytes",
        "mapFailureCount",
        "unmapFailureCount",
        "mappingStateFaultCount",
        "duplicateAllocatorChunkIdCount",
        "candidateOnlyChunkCount",
        "candidateOnlyReserveBytesUpperBound",
        "candidateOnlyMappedBytesUpperBound",
    )
    values = {name: _u64(raw, name) for name in numeric_names}
    generation_fields_closed = bool(
        values["mutationGeneration"] == values["mutationGenerationBegin"]
        and (
            not generation_stable
            or (
                not generation_saturated
                and values["mutationGenerationBegin"]
                == values["mutationGenerationEnd"]
            )
        )
    )
    coverage = raw.get("bindingCoverage")
    chunks = raw.get("chunks")
    if not isinstance(coverage, dict) or not isinstance(chunks, list):
        raise ValueError("chunk binding coverage/chunks are missing")

    coverage_names = (
        "hostBackingResources",
        "hostBackingLogicalBytes",
        "exactD3D9MemoryBindingResources",
        "exactD3D9MemoryBindingLogicalBytes",
        "exactBindings",
        "boundAlignedSliceBytes",
        "mappedBindingAlignedSliceBytes",
        "externalHostBackingResources",
        "externalHostBackingLogicalBytes",
        "unresolvedD3D9MemoryResources",
        "unresolvedD3D9MemoryLogicalBytes",
        "unregisteredHostBackingResources",
        "unregisteredHostBackingLogicalBytes",
        "missingChunkBindingCount",
        "invalidBindingCount",
        "outOfBoundsBindingCount",
        "duplicateBindingCount",
        "overlapBindingCount",
        "unclassifiedAlignedSliceBytes",
        "unregisteredAllocatorPayloadBytes",
    )
    coverage_values = {name: _u64(coverage, name) for name in coverage_names}
    reported_bound_excess = _bool(coverage, "boundBytesExceedAllocatorUsed")
    reported_binding_closure = _bool(coverage, "bindingBytesClosure")

    chunk_numeric_names = (
        "chunkId",
        "reserveBytes",
        "chunkOccupiedBytes",
        "freePayloadBytes",
        "freeRangeCount",
        "sharedMappedRefs",
        "sharedMappedBytes",
        "standaloneMappedRefs",
        "standaloneMappedBytes",
        "mapFailureCount",
        "unmapFailureCount",
        "mappingStateFaultCount",
        "boundResources",
        "boundLiveAlignedSliceBytes",
        "candidateAlignedSliceBytes",
        "directCandidateAlignedSliceBytes",
        "lazyReadbackCandidateAlignedSliceBytes",
        "nonCandidateAlignedSliceBytes",
        "unclassifiedAlignedSliceBytes",
        "mappedBindingAlignedSliceBytes",
        "reserveReclaimUpperBoundBytes",
        "mappedReclaimUpperBoundBytes",
    )
    chunk_rows: list[dict[str, Any]] = []
    chunk_ids: set[int] = set()
    chunk_ids_closed = True
    chunk_arithmetic_closed = True
    for item in chunks:
        if not isinstance(item, dict):
            raise ValueError("malformed D3D9 host chunk row")
        row = {name: _u64(item, name) for name in chunk_numeric_names}
        row["observedCandidateOnly"] = _bool(item, "observedCandidateOnly")
        chunk_id = int(row["chunkId"])
        if chunk_id == 0 or chunk_id in chunk_ids:
            chunk_ids_closed = False
        chunk_ids.add(chunk_id)
        chunk_arithmetic_closed = bool(
            chunk_arithmetic_closed
            and row["reserveBytes"]
            == row["chunkOccupiedBytes"] + row["freePayloadBytes"]
            and row["candidateAlignedSliceBytes"]
            == row["directCandidateAlignedSliceBytes"]
            + row["lazyReadbackCandidateAlignedSliceBytes"]
            and row["boundLiveAlignedSliceBytes"]
            == row["candidateAlignedSliceBytes"]
            + row["nonCandidateAlignedSliceBytes"]
            and row["mappedBindingAlignedSliceBytes"]
            <= row["boundLiveAlignedSliceBytes"]
        )
        chunk_rows.append(row)

    def chunk_sum(name: str) -> int:
        return sum(int(row[name]) for row in chunk_rows)

    allocator_aliases_closed = bool(
        _u64(allocator, "allocatedBackingBytes") == values["reserveBytes"]
        and _u64(allocator, "usedPayloadBytes")
        == values["allocatorUsedPayloadBytes"]
        and _u64(allocator, "mappedAddressBytes") == values["mappedBytes"]
    )
    allocator_arithmetic_closed = bool(
        values["reserveBytes"]
        == values["chunkOccupiedBytes"] + values["freePayloadBytes"]
        and values["chunkOccupiedBytes"]
        == values["allocatorUsedPayloadBytes"]
        + values["internalFragmentationBytes"]
        and values["mappedRefs"]
        == values["sharedMappedRefs"] + values["standaloneMappedRefs"]
        and values["mappedBytes"]
        == values["sharedMappedBytes"] + values["standaloneMappedBytes"]
    )
    chunk_sums_closed = bool(
        chunk_sum("reserveBytes") == values["reserveBytes"]
        and chunk_sum("chunkOccupiedBytes") == values["chunkOccupiedBytes"]
        and chunk_sum("freePayloadBytes") == values["freePayloadBytes"]
        and chunk_sum("sharedMappedRefs") == values["sharedMappedRefs"]
        and chunk_sum("sharedMappedBytes") == values["sharedMappedBytes"]
        and chunk_sum("standaloneMappedRefs")
        == values["standaloneMappedRefs"]
        and chunk_sum("standaloneMappedBytes")
        == values["standaloneMappedBytes"]
        and chunk_sum("mapFailureCount") <= values["mapFailureCount"]
        and chunk_sum("unmapFailureCount") <= values["unmapFailureCount"]
        and chunk_sum("mappingStateFaultCount")
        <= values["mappingStateFaultCount"]
    )

    host_resource_partition_closed = bool(
        coverage_values["hostBackingResources"]
        == coverage_values["exactD3D9MemoryBindingResources"]
        + coverage_values["externalHostBackingResources"]
        + coverage_values["unresolvedD3D9MemoryResources"]
        + coverage_values["unregisteredHostBackingResources"]
        and coverage_values["hostBackingLogicalBytes"]
        == coverage_values["exactD3D9MemoryBindingLogicalBytes"]
        + coverage_values["externalHostBackingLogicalBytes"]
        + coverage_values["unresolvedD3D9MemoryLogicalBytes"]
        + coverage_values["unregisteredHostBackingLogicalBytes"]
        and coverage_values["hostBackingLogicalBytes"]
        == _u64(totals, "hostBackingLogicalBytes")
        and coverage_values["exactBindings"]
        == coverage_values["exactD3D9MemoryBindingResources"]
    )
    bound_bytes = coverage_values["boundAlignedSliceBytes"]
    allocator_used = values["allocatorUsedPayloadBytes"]
    expected_bound_excess = bound_bytes > allocator_used
    expected_unregistered = max(allocator_used - bound_bytes, 0)
    binding_error_counts_clean = all(
        coverage_values[name] == 0
        for name in (
            "missingChunkBindingCount",
            "invalidBindingCount",
            "outOfBoundsBindingCount",
            "duplicateBindingCount",
            "overlapBindingCount",
        )
    )
    expected_binding_closure = bool(
        available
        and chunk_backed
        and accounting_closure
        and generation_stable
        and generation_fields_closed
        and coverage_values["unresolvedD3D9MemoryResources"] == 0
        and binding_error_counts_clean
        and not expected_bound_excess
        and expected_unregistered == 0
        and bound_bytes == allocator_used
    )
    binding_coverage_closed = bool(
        host_resource_partition_closed
        and coverage_values["boundAlignedSliceBytes"]
        == chunk_sum("boundLiveAlignedSliceBytes")
        and coverage_values["mappedBindingAlignedSliceBytes"]
        == chunk_sum("mappedBindingAlignedSliceBytes")
        and coverage_values["unclassifiedAlignedSliceBytes"]
        >= chunk_sum("unclassifiedAlignedSliceBytes")
        and reported_bound_excess == expected_bound_excess
        and coverage_values["unregisteredAllocatorPayloadBytes"]
        == expected_unregistered
        and reported_binding_closure == expected_binding_closure
    )

    faults_clean = bool(
        values["mapFailureCount"] == 0
        and values["unmapFailureCount"] == 0
        and values["mappingStateFaultCount"] == 0
        and values["duplicateAllocatorChunkIdCount"] == 0
    )
    clean_upper_bound = bool(
        expected_binding_closure
        and generation_stable
        and not generation_saturated
        and faults_clean
    )
    expected_candidate_only_count = 0
    expected_reserve_upper = 0
    expected_mapped_upper = 0
    candidate_rows_closed = True
    for row in chunk_rows:
        expected_candidate_only = bool(
            clean_upper_bound
            and row["boundLiveAlignedSliceBytes"] > 0
            and row["candidateAlignedSliceBytes"]
            == row["boundLiveAlignedSliceBytes"]
            and row["nonCandidateAlignedSliceBytes"] == 0
            and row["unclassifiedAlignedSliceBytes"] == 0
        )
        expected_reserve = row["reserveBytes"] if expected_candidate_only else 0
        expected_mapped = (
            row["sharedMappedBytes"] + row["standaloneMappedBytes"]
            if expected_candidate_only
            else 0
        )
        candidate_rows_closed = bool(
            candidate_rows_closed
            and row["observedCandidateOnly"] == expected_candidate_only
            and row["reserveReclaimUpperBoundBytes"] == expected_reserve
            and row["mappedReclaimUpperBoundBytes"] == expected_mapped
        )
        if expected_candidate_only:
            expected_candidate_only_count += 1
            expected_reserve_upper += expected_reserve
            expected_mapped_upper += expected_mapped
    candidate_upper_bound_closed = bool(
        candidate_rows_closed
        and values["candidateOnlyChunkCount"]
        == expected_candidate_only_count
        and values["candidateOnlyReserveBytesUpperBound"]
        == expected_reserve_upper
        and values["candidateOnlyMappedBytesUpperBound"]
        == expected_mapped_upper
    )
    closed = bool(
        available
        and chunk_backed
        and accounting_closure
        and generation_stable
        and generation_fields_closed
        and not generation_saturated
        and allocator_aliases_closed
        and allocator_arithmetic_closed
        and chunk_ids_closed
        and chunk_arithmetic_closed
        and chunk_sums_closed
        and binding_coverage_closed
        and expected_binding_closure
        and faults_clean
        and candidate_upper_bound_closed
    )
    return {
        "contract": "d3d9-host-chunk-diagnostics-v1",
        "available": available,
        "legacyInput": False,
        "authority": False,
        "mutationGeneration": values["mutationGeneration"],
        "mutationGenerationBegin": values["mutationGenerationBegin"],
        "mutationGenerationEnd": values["mutationGenerationEnd"],
        "generationStable": generation_stable,
        "generationFieldsClosed": generation_fields_closed,
        "allocatorAliasesClosed": allocator_aliases_closed,
        "allocatorArithmeticClosed": allocator_arithmetic_closed,
        "chunkIdsClosed": chunk_ids_closed,
        "chunkArithmeticClosed": chunk_arithmetic_closed,
        "chunkSumsClosed": chunk_sums_closed,
        "hostResourcePartitionClosed": host_resource_partition_closed,
        "bindingCoverageClosed": binding_coverage_closed,
        "bindingBytesClosure": expected_binding_closure,
        "faultsClean": faults_clean,
        "candidateUpperBoundClosed": candidate_upper_bound_closed,
        "candidateOnlyChunkCount": expected_candidate_only_count,
        "candidateOnlyReserveBytesUpperBound": expected_reserve_upper,
        "candidateOnlyMappedBytesUpperBound": expected_mapped_upper,
        "unregisteredAllocatorPayloadBytes": expected_unregistered,
        "chunks": chunk_rows,
        "closed": closed,
    }


def analyze(report: Path) -> dict[str, Any]:
    root = _load_report(report)
    gpu_skin = root.get("gpuSkinSnapshot")
    if not isinstance(gpu_skin, dict):
        raise ValueError("gpuSkinSnapshot is missing")
    process_id = _u64(gpu_skin, "processId")
    process_start_filetime = _u64(
        gpu_skin, "processStartFileTime100ns"
    )
    process_start_exact = gpu_skin.get("processStartFileTime100nsExact")
    if (
        process_id <= 0
        or process_start_filetime <= 0
        or type(process_start_exact) is not str
        or process_start_exact != str(process_start_filetime)
    ):
        raise ValueError("performance report process identity is not exact")
    census = root.get("resourceResidencyCensus")
    if not isinstance(census, dict):
        raise ValueError("resourceResidencyCensus is missing")
    if census.get("contract") != "diagnostics-only-observation-v1":
        raise ValueError("unexpected census contract")
    if census.get("enabled") is not True:
        raise ValueError("census was not enabled for this process")
    if census.get("evictionAuthority") is not False:
        raise ValueError("invalid eviction-authority claim")
    if census.get("performanceComparable") is not False:
        raise ValueError("census run must not be used as an FPS comparison")
    if census.get("processPrivateCommitComparable") is not False:
        raise ValueError("census must not claim process Private/Commit closure")
    if census.get("snapshotCoverage") != (
        "known-live-steady-plus-gpu-skin-pool-peaks-v1"
    ):
        raise ValueError("unexpected census snapshot coverage")
    required_exclusions = (
        "gameDllOriginalAllocationsIncluded",
        "driverAllocatorPoolReserveIncluded",
        "transientShadowBuildCopiesIncluded",
        "retainedOldPublishedStoresIncluded",
        "resourceFieldsAreQuiescentSnapshot",
    )
    if any(census.get(name) is not False for name in required_exclusions):
        raise ValueError("census coverage exclusions are not explicit")
    if census.get("gpuSkinQueuedMissPeakIncluded") is not True:
        raise ValueError("GPU-skin queued-miss peak is missing")

    totals = census.get("totals")
    allocator = census.get("d3d9HostAllocator")
    gpu_skin_pools = census.get("gpuSkinPools")
    model = census.get("modelCache")
    buckets = census.get("buckets")
    largest = census.get("largestHostBackings")
    if not isinstance(totals, dict) or not isinstance(allocator, dict):
        raise ValueError("census totals/allocator are missing")
    if not isinstance(gpu_skin_pools, dict):
        raise ValueError("census GPU-skin pool snapshot is missing")
    if not isinstance(model, dict) or not isinstance(buckets, list):
        raise ValueError("census modelCache/buckets are missing")
    if not isinstance(largest, list):
        raise ValueError("census largestHostBackings is missing")
    allocator_chunk_contract = _analyze_chunk_diagnostics(
        allocator, totals
    )

    frame_serial = _u64(census, "frameSerial")
    lifetime_registrations = _u64(census, "lifetimeRegistrations")
    live_resources = _u64(census, "liveResources")
    if frame_serial < 300:
        raise ValueError("census did not reach the 300-frame quiet window")
    if lifetime_registrations <= 0 or live_resources <= 0:
        raise ValueError("census registration chain produced no resources")

    bucket_rows: list[dict[str, Any]] = []
    by_class: dict[str, dict[str, int]] = {}
    for raw in buckets:
        if not isinstance(raw, dict) or type(raw.get("class")) is not str:
            raise ValueError("malformed census bucket")
        row = {
            "class": raw["class"],
            "pool": _u64(raw, "pool"),
            "mapMode": _u64(raw, "mapMode"),
            "resources": _u64(raw, "resources"),
            "logicalBytes": _u64(raw, "logicalBytes"),
            "deviceAllocationBytes": _u64(raw, "deviceAllocationBytes"),
            "hostBackingLogicalBytes": _u64(
                raw, "hostBackingLogicalBytes"
            ),
            "hostMappedLogicalBytes": _u64(raw, "hostMappedLogicalBytes"),
            "duplicateHostBackingLogicalBytes": _u64(
                raw, "duplicateHostBackingLogicalBytes"
            ),
            "readLocks": _u64(raw, "readLocks"),
            "partialSubresourceWriteLocks": _u64(
                raw, "partialSubresourceWriteLocks"
            ),
            "externalDirtyCalls": _u64(raw, "externalDirtyCalls"),
            "observedCandidateHostBytes": _u64(
                raw, "observedCandidateHostBytes"
            ),
            "lazyReadbackCandidateHostBytes": _u64(
                raw, "lazyReadbackCandidateHostBytes"
            ),
        }
        bucket_rows.append(row)
        aggregate = by_class.setdefault(
            row["class"],
            {
                "resources": 0,
                "logicalBytes": 0,
                "deviceAllocationBytes": 0,
                "hostBackingLogicalBytes": 0,
                "hostMappedLogicalBytes": 0,
                "duplicateHostBackingLogicalBytes": 0,
                "observedCandidateHostBytes": 0,
                "lazyReadbackCandidateHostBytes": 0,
            },
        )
        for name in aggregate:
            aggregate[name] += row[name]

    bucket_rows.sort(
        key=lambda row: (
            row["duplicateHostBackingLogicalBytes"],
            row["hostBackingLogicalBytes"],
        ),
        reverse=True,
    )
    mirror = by_class.get("gpuSkinStaticMirror", {})
    atlas = by_class.get("gpuSkinStaticAtlas", {})
    upload = by_class.get("gpuSkinUploadPage", {})
    output = by_class.get("gpuSkinOutputPage", {})
    static_staging = by_class.get("gpuSkinStaticUploadStaging", {})

    bucket_resource_total = sum(
        int(row["resources"]) for row in bucket_rows
    )
    if bucket_resource_total != live_resources:
        raise ValueError("census live-resource bucket closure failed")
    d3d9_classes = {
        "vertexBuffer", "indexBuffer", "surface", "texture2D",
        "cubeTexture", "volumeTexture",
    }
    if not any(
        int(by_class.get(name, {}).get("resources", 0)) > 0
        for name in d3d9_classes
    ):
        raise ValueError("census observed no D3D9 resources")
    required_gpu_skin_classes = (
        "gpuSkinStaticMirror",
        "gpuSkinStaticAtlas",
        "gpuSkinUploadPage",
        "gpuSkinOutputPage",
    )
    if any(
        int(by_class.get(name, {}).get("resources", 0)) <= 0
        for name in required_gpu_skin_classes
    ):
        raise ValueError("census did not exercise all GPU-skin resource pools")
    if (
        _u64(gpu_skin_pools, "staticReadyRecords") <= 0
        or _u64(gpu_skin_pools, "staticAtlasUsedBytes") <= 0
        or _u64(gpu_skin_pools, "outputResidentBytes") <= 0
    ):
        raise ValueError("GPU-skin pool census contains no ready workload")

    return {
        "contract": "war3-resource-residency-analysis-v1",
        "sourceReport": str(report.resolve()),
        "diagnosticsOnly": True,
        "evictionAuthority": False,
        "performanceComparable": False,
        "reportProcessIdentity": {
            "processId": process_id,
            "processStartFileTime100ns": process_start_filetime,
            "processStartFileTime100nsExact": process_start_exact,
            "exactFieldsClosed": True,
        },
        "frameSerial": frame_serial,
        "quiescentWindowReached": True,
        "lifetimeRegistrations": lifetime_registrations,
        "liveResources": live_resources,
        "bucketResourceTotal": bucket_resource_total,
        "registrationChainNonEmpty": True,
        "totals": totals,
        "d3d9HostAllocator": allocator,
        "allocatorChunkContract": allocator_chunk_contract,
        "modelCopies": {
            "cacheUniquePayloadBytes": _u64(
                model, "uniqueGeosetCapacityBytes"
            ),
            "cacheAliasDuplicateBytes": _u64(
                model, "aliasDuplicateCapacityBytes"
            ),
            "shadowStorePayloadBytes": _u64(
                model, "shadowStorePayloadCapacityBytes"
            ),
            "gpuSkinMirrorResidentDuplicateBytes": int(
                mirror.get("duplicateHostBackingLogicalBytes", 0)
            ),
            "gpuSkinMirrorShareableBytes": int(
                mirror.get("observedCandidateHostBytes", 0)
            ),
            "hashContainerOverheadExcluded": bool(
                _u64(model, "hashContainerOverheadIncluded") == 0
                and _u64(
                    model, "shadowStoreHashContainerOverheadIncluded"
                ) == 0
            ),
        },
        "gpuSkinPools": {
            "staticAtlasReservedBytes": _u64(
                gpu_skin_pools, "staticAtlasReservedBytes"
            ),
            "staticAtlasUsedBytes": _u64(
                gpu_skin_pools, "staticAtlasUsedBytes"
            ),
            "staticAtlasDeviceAllocationSliceBytes": int(
                atlas.get("deviceAllocationBytes", 0)
            ),
            "staticAtlasReferencedBytes": int(
                mirror.get("logicalBytes", 0)
            ),
            "staticReadyRecords": _u64(
                gpu_skin_pools, "staticReadyRecords"
            ),
            "staticPendingRecords": _u64(
                gpu_skin_pools, "staticPendingRecords"
            ),
            "staticInvalidRecords": _u64(
                gpu_skin_pools, "staticInvalidRecords"
            ),
            "queuedStaticMissHostBytes": _u64(
                gpu_skin_pools, "queuedStaticMissHostBytes"
            ),
            "peakQueuedStaticMissHostBytes": _u64(
                gpu_skin_pools, "peakQueuedStaticMissHostBytes"
            ),
            "uploadResidentBytes": _u64(
                gpu_skin_pools, "uploadResidentBytes"
            ),
            "uploadActiveUsedBytes": _u64(
                gpu_skin_pools, "uploadActiveUsedBytes"
            ),
            "uploadPendingUsedBytes": _u64(
                gpu_skin_pools, "uploadPendingUsedBytes"
            ),
            "uploadRetiredUsedBytes": _u64(
                gpu_skin_pools, "uploadRetiredUsedBytes"
            ),
            "uploadIdleCapacityBytes": _u64(
                gpu_skin_pools, "uploadIdleCapacityBytes"
            ),
            "outputResidentBytes": _u64(
                gpu_skin_pools, "outputResidentBytes"
            ),
            "outputCursorBytes": _u64(
                gpu_skin_pools, "outputCursorBytes"
            ),
            "outputOutstandingSlices": _u64(
                gpu_skin_pools, "outputOutstandingSlices"
            ),
            "uploadPageBytes": int(
                upload.get("deviceAllocationBytes", 0)
            ),
            "outputPageBytes": int(
                output.get("deviceAllocationBytes", 0)
            ),
            "staticUploadStagingBytes": int(
                static_staging.get("hostBackingLogicalBytes", 0)
            ),
        },
        "observedCandidates": {
            "directOrShareableBytes": _u64(
                totals, "observedCandidateHostBytes"
            ),
            "lazyReadbackRequiredBytes": _u64(
                totals, "lazyReadbackCandidateHostBytes"
            ),
            "notAuthorization": True,
        },
        "snapshotLimitations": {
            "gameDllOriginalAllocationsIncluded": False,
            "driverAllocatorPoolReserveIncluded": False,
            "transientShadowBuildCopiesIncluded": False,
            "retainedOldPublishedStoresIncluded": False,
            "resourceFieldsAreQuiescentSnapshot": False,
            "privateCommitClosure": False,
        },
        "largestBuckets": bucket_rows[:16],
        "largestHostBackings": largest,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.report)
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
