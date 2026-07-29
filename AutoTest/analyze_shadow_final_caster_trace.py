#!/usr/bin/env python3
"""Join exact screenshots to the final shadow-caster JSONL trace.

The renderer emits ``shadowFinalCasterFrame`` and
``shadowFinalCasterRecord`` events at the sole replay choke point.  This tool
streams that file, compares adjacent frames, and ranks:

* frames selected by the temporal screenshot analyzer;
* large caster-set changes;
* one-frame identity/backing/content substitutions;
* malformed draw contracts and suspicious world-origin geometry.

It deliberately reports concrete caster records instead of inferring the
culprit from producer-side counters.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
from typing import Any, Iterable


RUNTIME_CADENCE_FIELDS = (
    "serial",
    "frameIndex",
    "sceneFrameSerial",
    "submittedDrawCount",
    "submittedSkinnedCount",
    "directSubmittedRecordCount",
    "directSubmittedObjectCount",
    "shadowCastersCount",
    "replayDrawsCount",
    "shadowMapDrawnCasters",
    "shadowMapSkinnedDrawnCount",
    "receiverNeedShadowMap",
    "receiverHasCompleteShadowMap",
    "receiverReuseShadowMap",
    "shadowMapExecutedThisFrame",
    "shadowMapRenderSerial",
)

RUNTIME_KEY_STAT_FIELDS = (
    "drawTimeSemanticProducerClaimedCount",
    "drawTimeSemanticProducerSubmittedCount",
    "drawTimeSemanticProducerOwnedDirectGroupedSkipCount",
    "drawTimeSemanticProducerLifecycleMergedCount",
    "semanticSceneShadowManifestPartLeaseRestoredCount",
    "semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount",
    "semanticSceneShadowManifestPartLeaseExpiredCount",
    "semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount",
    "semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount",
    "semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount",
    "semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount",
    "semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount",
    "semanticSceneShadowManifestObjectCount",
    "semanticSceneShadowManifestPartCount",
    "semanticSceneShadowManifestFreshPartCount",
    "semanticSceneShadowManifestPoseStalePartCount",
    "semanticSceneShadowManifestSliceStalePartCount",
    "semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount",
    "semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount",
    "semanticSceneShadowManifestLeaseExpiredBackingOnlyCount",
    "semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount",
    "semanticSceneShadowManifestMissingRequiredPartCount",
    "semanticSceneShadowManifestGraceUsedCount",
    "semanticSceneShadowManifestTombstoneRetiredCount",
    "semanticSceneShadowManifestObjectCoreCompleteCount",
    "semanticSceneShadowManifestObjectCoreIncompleteSkipCount",
    "semanticSceneShadowManifestPartOmittedIncompleteCoreCount",
    "semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount",
    "semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount",
    "semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount",
    "semanticSceneShadowManifestObjectCoreEpochMissingPartCount",
    "semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount",
)


def select_fields(source: dict[str, Any], fields: Iterable[str]) -> dict[str, Any]:
    return {name: source[name] for name in fields if name in source}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--probe-artifact", type=Path)
    parser.add_argument("--temporal-analysis", type=Path)
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--records-per-frame", type=int, default=12)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def load_trace(
    path: Path,
) -> tuple[
    dict[int, dict[str, Any]],
    dict[int, list[dict[str, Any]]],
    dict[int, dict[str, Any]],
    dict[int, int],
    list[dict[str, Any]],
]:
    frames: dict[int, dict[str, Any]] = {}
    records: dict[int, list[dict[str, Any]]] = defaultdict(list)
    runtime_stats: dict[int, dict[str, Any]] = {}
    frame_index_by_trace_serial: dict[int, int] = {}
    parse_errors: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            if (
                "shadowFinalCaster" not in line
                and '"type":"shadowPoseFullTraceFrame"' not in line
            ):
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as error:
                # A controlled process stop can interrupt the final JSONL
                # record after all prior frames have already been flushed.
                # Preserve the complete evidence and report the truncation
                # instead of discarding the entire trace.
                parse_errors.append(
                    {
                        "line": line_number,
                        "column": error.colno,
                        "message": error.msg,
                        "length": len(line),
                        "prefix": line[:160],
                    }
                )
                continue
            event_type = event.get("type")
            if event_type == "shadowPoseFullTraceFrame":
                cadence = event.get("cadence") or {}
                trace_serial = int(cadence.get("serial", 0))
                frame_index = int(cadence.get("frameIndex", 0))
                if trace_serial > 0 and frame_index > 0:
                    key_stats = event.get("keyStats") or {}
                    frame_index_by_trace_serial[trace_serial] = frame_index
                    runtime_stats[frame_index] = {
                        "cadence": select_fields(
                            cadence, RUNTIME_CADENCE_FIELDS
                        ),
                        "keyStats": select_fields(
                            key_stats, RUNTIME_KEY_STAT_FIELDS
                        ),
                    }
                continue
            serial = int(event.get("frameSerial", 0))
            if serial <= 0:
                continue
            if event_type == "shadowFinalCasterFrame":
                frames[serial] = event
            elif event_type == "shadowFinalCasterRecord":
                records[serial].append(event)
    return (
        frames,
        records,
        runtime_stats,
        frame_index_by_trace_serial,
        parse_errors,
    )


def load_capture_join(
    probe_path: Path | None,
    temporal_path: Path | None,
    frame_index_by_trace_serial: dict[int, int],
) -> tuple[dict[int, dict[str, Any]], list[dict[str, Any]]]:
    if probe_path is None:
        return {}, []
    probe = json.loads(probe_path.read_text(encoding="utf-8"))
    temporal_by_index: dict[int, dict[str, Any]] = {}
    if temporal_path is not None:
        temporal = json.loads(temporal_path.read_text(encoding="utf-8"))
        temporal_by_index = {
            int(row["index"]): row for row in temporal.get("all", [])
        }

    result: dict[int, dict[str, Any]] = {}
    unmapped: list[dict[str, Any]] = []
    for frame in probe.get("frames", []):
        exact = frame.get("exactFrame") or {}
        trace_serial = int(exact.get("shadowFrameSerial", 0))
        if trace_serial <= 0:
            continue
        index = int(frame.get("index", -1))
        final_caster_frame_serial = int(
            frame_index_by_trace_serial.get(trace_serial, 0)
        )
        joined = {
            "captureIndex": index,
            "capturePath": frame.get("output", ""),
            "traceSerial": trace_serial,
            "mappedFinalCasterFrameSerial": final_caster_frame_serial,
            "capturedShadowCasterCount": exact.get("shadowCasterCount"),
            "capturedReplayDrawCount": exact.get("shadowReplayDrawCount"),
            "capturedReplayBackingHash": exact.get("replayBackingHash"),
        }
        if index in temporal_by_index:
            joined["temporal"] = temporal_by_index[index]
        if final_caster_frame_serial <= 0:
            unmapped.append(joined)
            continue
        result[final_caster_frame_serial] = joined
    return result, unmapped


def multiset_delta(
    current: Iterable[str], previous: Iterable[str]
) -> tuple[list[str], list[str]]:
    current_counter = Counter(current)
    previous_counter = Counter(previous)
    added = list((current_counter - previous_counter).elements())
    removed = list((previous_counter - current_counter).elements())
    return added, removed


def stable_identity_key(record: dict[str, Any]) -> str:
    """Identity independent of per-frame dynamic-ring slice offsets.

    Anonymous terrain/legacy draws may not carry a widget handle.  Their
    shape tuple is still useful as a multiset key, whereas firstIndex and
    vertexOffset rotate every frame and must never be treated as identity.
    """

    fields = (
        "stage",
        "category",
        "batchTag",
        "objectKind",
        "rawcode",
        "jHandle",
        "batchHandle",
        "indexed",
        "topology",
        "indexCount",
        "vertexCount",
        "numVertices",
        "positionStride",
        "positionOffset",
        "positionFormat",
        "indexType",
        "vertexBlendEnabled",
        "vertexBlendIndexed",
        "vertexBlendCount",
    )
    return "|".join(str(record.get(field, 0)) for field in fields)


def part_layer_key(record: dict[str, Any]) -> str:
    """Stable per-part identity for Stage11 continuity analysis.

    The primary contract is handle + renderablePart + layer.  Batch/rawcode
    are only fallbacks for records whose JASS handle is unavailable.  A null
    renderablePart is never promoted into same-part continuity evidence.
    """

    if "renderablePart" not in record or "layerIndex" not in record:
        return ""
    object_token = int(record.get("jHandle", 0) or 0)
    if object_token == 0:
        object_token = int(record.get("batchHandle", 0) or 0)
    if object_token == 0:
        object_token = int(record.get("rawcode", 0) or 0)
    part = str(record.get("renderablePart", "0x0") or "0x0")
    # Stage1 terrain and legacy Stage10/13 records deliberately have no
    # renderable-part identity.  Grouping every null pointer under one key
    # turns unrelated draws into a same-part mixed-representation false
    # positive.  Representation continuity is meaningful only for an exact
    # non-null logical part; anonymous records remain covered by the separate
    # identity/content/temporal gates.
    if is_zero_hex(part):
        return ""
    return "|".join(
        (
            str(record.get("stage", -1)),
            str(object_token),
            part,
            str(record.get("layerIndex", 0)),
        )
    )


def representation_signature(record: dict[str, Any]) -> str:
    """Shadow-side representation of one exact logical part.

    This deliberately excludes rotating buffer addresses, offsets and palette
    allocation indices.  A change here means the same part crossed between
    materially different geometry/skinning/alpha contracts, which is the
    producer-mixing failure that presents as tree squares or unit stutter.
    """

    gpu_skin = record.get("gpuSkin") or {}
    fields = (
        record.get("batchTag", 0),
        record.get("indexed", 0),
        record.get("topology", 0),
        record.get("positionStride", 0),
        record.get("positionOffset", 0),
        record.get("positionFormat", 0),
        record.get("vertexBlendEnabled", 0),
        record.get("vertexBlendIndexed", 0),
        record.get("vertexBlendCount", 0),
        record.get("alphaTestEnabled", 0),
        record.get("alphaBlendEnabled", 0),
        record.get("uvStride", 0),
        record.get("uvOffset", 0),
        record.get("uvFormat", 0),
        record.get("uvBinding", 0),
        gpu_skin.get("valid", 0),
        gpu_skin.get("outputFormat", 0),
    )
    return "|".join(str(value) for value in fields)


def alpha_representation_signature(record: dict[str, Any]) -> str:
    """Alpha/cutout state for the same-part transition gate."""

    fields = (
        record.get("alphaTestEnabled", 0),
        record.get("alphaBlendEnabled", 0),
        record.get("alphaPayloadComplete", 0),
        record.get("uvStride", 0),
        record.get("uvOffset", 0),
        record.get("uvFormat", 0),
        record.get("uvBinding", 0),
    )
    return "|".join(str(value) for value in fields)


def is_zero_hex(value: Any) -> bool:
    try:
        return int(str(value), 0) == 0
    except (TypeError, ValueError):
        return True


def stage11_alpha_payload_gap(record: dict[str, Any], frame_serial: int) -> bool:
    if "alphaPayloadComplete" not in record:
        return False
    if int(record.get("stage", -1)) != 11:
        return False
    if not int(record.get("alphaTestEnabled", 0)):
        return False
    alpha_frame = int(record.get("alphaMetadataFrameSerial", 0))
    # Final-caster frameSerial is the post-rotate frame index, while the
    # metadata producer records the scene/persistent serial consumed by that
    # rotate.  The established cadence contract is therefore either equal
    # (same-boundary emit) or exactly one lower (post-rotate emit).
    valid_source_frames = {frame_serial, max(frame_serial - 1, 0)}
    return (
        not int(record.get("alphaPayloadComplete", 0))
        or alpha_frame not in valid_source_frames
        or (
            is_zero_hex(record.get("metadataKeyHash", "0x0"))
            and is_zero_hex(record.get("exactGeometryKeyHash", "0x0"))
        )
    )


def blocker_leaked_to_final_casters(record: dict[str, Any]) -> bool:
    reason = int(record.get("metadataBlockerReason", 0))
    return (
        bool(int(record.get("pathBlocker", 0)))
        or bool(int(record.get("pathBlockerGeometryMarker", 0)))
        or reason in (1, 2, 3, 4)
    )


def full_domain_masked_anonymous_small_marker(record: dict[str, Any]) -> bool:
    """Detect the exact-index representation that masked LOSBlocker.mdl.

    A CPU-opaque IB can force the renderer to copy a full 16K-vertex backing
    page even though a six-index draw references at most six unique vertices.
    The explicit blocker flags are absent in the regression, so the old
    blockerLeak metric was a stable false negative.
    """
    index_count = int(record.get("indexCount", 0))
    world_lane = (
        int(record.get("stage", -1)) == 11
        or int(record.get("category", 0)) == 3
        or int(record.get("batchTag", -1)) == 1
    )
    return (
        world_lane
        and int(record.get("rawcode", 0)) == 0
        and int(record.get("jHandle", 0)) == 0
        and int(record.get("batchHandle", 0)) == 0
        and int(record.get("objectKind", 0)) in (0, 1, 3)
        and not int(record.get("unitIdentityProven", 0))
        and bool(int(record.get("indexed", 0)))
        and int(record.get("topology", -1)) == 3
        and 3 <= index_count <= 8
        and not int(record.get("actualIndexDomainKnown", 0))
        and bool(int(record.get("fullVertexDomainFallback", 0)))
        and not int(record.get("vertexBlendEnabled", 0))
        and not int(record.get("vertexBlendIndexed", 0))
        and not int(record.get("alphaTestEnabled", 0))
        and not int(record.get("alphaBlendEnabled", 0))
    )


def grace_resurrected_anonymous_small_marker(record: dict[str, Any]) -> bool:
    """Detect the generic 4v/6i Grace form of the same LOS blocker.

    The exact full-domain representation can be rejected correctly while an
    older generic CurrentDraw record for the same part is restored during an
    exact-producer hole.  That regression has a distinct and stable contract:
    an anonymous Stage11 world draw, GraceOneFrame lifecycle, compact 12-byte
    positions, and the legacy indexed vertex-blend interpretation.  Requiring
    every field avoids confusing legitimate Stage13 4v/6i alpha geometry with
    the blocker resurrection.
    """
    vertex_count = int(
        record.get("numVertices", 0) or record.get("vertexCount", 0)
    )
    index_count = int(record.get("indexCount", 0))
    world_lane = (
        int(record.get("stage", -1)) == 11
        or int(record.get("category", 0)) == 3
        or int(record.get("batchTag", -1)) == 1
    )
    return (
        world_lane
        and int(record.get("rawcode", 0)) == 0
        and int(record.get("jHandle", 0)) == 0
        and int(record.get("batchHandle", 0)) == 0
        and int(record.get("objectKind", 0)) in (0, 1, 3)
        and not int(record.get("unitIdentityProven", 0))
        and bool(int(record.get("indexed", 0)))
        and int(record.get("topology", -1)) == 3
        and 3 <= index_count <= 8
        and 3 <= vertex_count <= 8
        and int(record.get("partLifecycleState", -1)) == 2
        and int(record.get("positionStride", 0)) == 12
        and bool(int(record.get("vertexBlendEnabled", 0)))
        and bool(int(record.get("vertexBlendIndexed", 0)))
        and not int(record.get("alphaTestEnabled", 0))
        and not int(record.get("alphaBlendEnabled", 0))
        and not int(record.get("fullVertexDomainFallback", 0))
    )


def has_strong_object_identity(record: dict[str, Any]) -> bool:
    """Return whether backing continuity is meaningful for this draw.

    Terrain/S10 legacy draws commonly rotate through dynamic-ring allocations,
    so a backing change is expected even when the logical caster is stable.
    S11 draws normally carry a widget/batch/rawcode identity and are the lane
    where an unexpected backing swap is useful evidence.
    """

    return int(record.get("stage", -1)) == 11 and any(
        int(record.get(field, 0))
        for field in ("jHandle", "batchHandle", "rawcode")
    )


def record_contract_score(record: dict[str, Any]) -> tuple[float, list[str]]:
    score = 0.0
    reasons: list[str] = []
    flags = int(record.get("validationFlags", 0))
    if flags:
        score += 1000.0
        reasons.append(f"validationFlags=0x{flags:x}")

    stride = int(record.get("positionStride", 0))
    offset = int(record.get("positionOffset", 0))
    position_size = int(record.get("positionBufferSize", 0))
    vertices = int(
        record.get("numVertices", 0) or record.get("vertexCount", 0)
    )
    if stride > 0 and vertices > 0:
        required = offset + vertices * stride
        if required > position_size:
            score += 750.0
            reasons.append(
                f"positionRange={required}>bufferSize={position_size}"
            )

    if int(record.get("indexed", 0)):
        index_size = int(record.get("indexBufferSize", 0))
        index_count = int(record.get("indexCount", 0))
        first_index = int(record.get("firstIndex", 0))
        index_type = int(record.get("indexType", 0))
        bytes_per_index = 2 if index_type == 0 else 4
        required = (first_index + index_count) * bytes_per_index
        if required > index_size:
            score += 750.0
            reasons.append(
                f"indexRange={required}>bufferSize={index_size}"
            )

    radius = float(record.get("boundsRadius", 0.0))
    translation = record.get("worldTranslation") or [0.0, 0.0, 0.0]
    xyz = [float(value) for value in translation[:3]]
    if not all(math.isfinite(value) for value in xyz + [radius]):
        score += 1000.0
        reasons.append("nonFiniteTransformOrBounds")
    if radius > 5000.0:
        score += min(radius / 10.0, 500.0)
        reasons.append(f"hugeBoundsRadius={radius:.1f}")
    stage = int(record.get("stage", -1))
    object_kind = int(record.get("objectKind", 0))
    identity_bearing_lane = stage in (10, 11, 12) or object_kind != 0
    bounds = record.get("boundsCenter") or [0.0, 0.0, 0.0]
    bounds_xyz = [float(value) for value in bounds[:3]]
    bounds_near_origin = (
        len(bounds_xyz) >= 2
        and abs(bounds_xyz[0]) <= 64.0
        and abs(bounds_xyz[1]) <= 64.0
    )
    if (
        identity_bearing_lane
        and abs(xyz[0]) <= 64.0
        and abs(xyz[1]) <= 64.0
        and bounds_near_origin
        and (vertices >= 1000 or radius >= 500.0)
    ):
        # Identity-world skinned snapshots legitimately contain world-space
        # vertices and local bounds around (0,0).  Treating every such draw as
        # suspicious drowned the actual giant-geometry signal in false
        # positives.  Origin anchoring matters only when the affected geometry
        # is itself abnormally large.
        score += 340.0
        reasons.append("largeGeometryAnchoredNearOrigin")
    if max(abs(value) for value in xyz) > 100000.0:
        score += 500.0
        reasons.append("extremeWorldTranslation")
    if int(record.get("positionBuffer", "0x0"), 16) == 0:
        score += 1000.0
        reasons.append("nullPositionBuffer")
    if stage11_alpha_payload_gap(record, int(record.get("frameSerial", 0))):
        score += 750.0
        reasons.append("stage11AlphaPayloadGap")
    if blocker_leaked_to_final_casters(record):
        score += 1000.0
        reasons.append("blockerLeakedToFinalCaster")
    if full_domain_masked_anonymous_small_marker(record):
        score += 1000.0
        reasons.append("fullDomainMaskedAnonymousSmallMarker")
    if grace_resurrected_anonymous_small_marker(record):
        score += 1000.0
        reasons.append("graceResurrectedAnonymousSmallMarker")
    return score, reasons


def summarize_record(
    record: dict[str, Any], score: float, reasons: list[str]
) -> dict[str, Any]:
    keys = (
        "index",
        "stage",
        "category",
        "batchTag",
        "objectKind",
        "rawcode",
        "jHandle",
        "batchHandle",
        "renderablePart",
        "layerIndex",
        "pathBlocker",
        "pathBlockerGeometryMarker",
        "indexed",
        "topology",
        "indexCount",
        "firstIndex",
        "vertexOffset",
        "vertexCount",
        "firstVertex",
        "minVertexIndex",
        "numVertices",
        "positionStride",
        "positionOffset",
        "positionFormat",
        "positionBufferSize",
        "positionStorageGeneration",
        "indexBufferSize",
        "indexStorageGeneration",
        "vertexBlendEnabled",
        "vertexBlendIndexed",
        "vertexBlendCount",
        "alphaTestEnabled",
        "alphaBlendEnabled",
        "uvStride",
        "uvOffset",
        "uvFormat",
        "uvBinding",
        "alphaPayloadComplete",
        "alphaMetadataFrameSerial",
        "metadataBlockerReason",
        "partLifecycleState",
        "metadataKeyHash",
        "boundsCenter",
        "boundsRadius",
        "worldTranslation",
        "identityHash",
        "backingHash",
        "contentHash",
        "positionSampleHash",
        "indexSampleHash",
        "positionStoragePtr",
        "positionBuffer",
        "indexStoragePtr",
        "indexBuffer",
        "validationFlags",
        "gpuSkin",
    )
    result = {key: record.get(key) for key in keys}
    result["suspicionScore"] = round(score, 3)
    result["reasons"] = reasons
    return result


def main() -> int:
    args = parse_args()
    (
        frames,
        records,
        runtime_stats,
        frame_index_by_trace_serial,
        parse_errors,
    ) = load_trace(args.trace)
    if not frames:
        raise SystemExit("trace contains no shadowFinalCasterFrame events")
    captures, unmapped_captures = load_capture_join(
        args.probe_artifact,
        args.temporal_analysis,
        frame_index_by_trace_serial,
    )

    serials = sorted(frames)
    rows: list[dict[str, Any]] = []
    previous_records: list[dict[str, Any]] = []
    previous_serial = 0
    for serial in serials:
        frame = frames[serial]
        current_records = records.get(serial, [])
        current_ids = [stable_identity_key(row) for row in current_records]
        previous_ids = [
            stable_identity_key(row) for row in previous_records
        ]
        added, removed = multiset_delta(current_ids, previous_ids)

        previous_by_identity: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for record in previous_records:
            previous_by_identity[stable_identity_key(record)].append(record)
        current_representations_by_part: dict[str, set[str]] = defaultdict(set)
        previous_representations_by_part: dict[str, set[str]] = defaultdict(set)
        current_alpha_by_part: dict[str, set[str]] = defaultdict(set)
        previous_alpha_by_part: dict[str, set[str]] = defaultdict(set)
        for record in current_records:
            part_key = part_layer_key(record)
            if part_key:
                current_representations_by_part[part_key].add(
                    representation_signature(record)
                )
                current_alpha_by_part[part_key].add(
                    alpha_representation_signature(record)
                )
        for record in previous_records:
            part_key = part_layer_key(record)
            if part_key:
                previous_representations_by_part[part_key].add(
                    representation_signature(record)
                )
                previous_alpha_by_part[part_key].add(
                    alpha_representation_signature(record)
                )
        representation_transition_keys = {
            key
            for key in (
                current_representations_by_part.keys()
                & previous_representations_by_part.keys()
            )
            if current_representations_by_part[key]
            != previous_representations_by_part[key]
        }
        alpha_transition_keys = {
            key
            for key in current_alpha_by_part.keys() & previous_alpha_by_part.keys()
            if current_alpha_by_part[key] != previous_alpha_by_part[key]
        }
        mixed_representation_keys = {
            key
            for key, signatures in current_representations_by_part.items()
            if len(signatures) > 1
        }
        mixed_alpha_keys = {
            key
            for key, signatures in current_alpha_by_part.items()
            if len(signatures) > 1
        }
        backing_changes = 0
        content_changes = 0
        backing_generation_changes = 0
        alpha_payload_gaps = 0
        blocker_leaks = 0
        full_domain_masked_marker_leaks = 0
        grace_resurrected_marker_leaks = 0
        previous_by_part = {
            key: record
            for record in previous_records
            if (key := part_layer_key(record))
        }
        record_rank: list[tuple[float, list[str], dict[str, Any]]] = []
        for record in current_records:
            identity = stable_identity_key(record)
            score, reasons = record_contract_score(record)
            previous_matches = previous_by_identity.get(identity, [])
            if previous_matches:
                if has_strong_object_identity(record) and all(
                    old.get("backingHash") != record.get("backingHash")
                    for old in previous_matches
                ):
                    backing_changes += 1
                    score += 120.0
                    reasons.append("backingChangedForStableIdentity")
                if all(
                    old.get("contentHash") != record.get("contentHash")
                    for old in previous_matches
                ):
                    content_changes += 1
            elif previous_serial:
                score += 35.0
                reasons.append("identityAddedThisFrame")
            part_key = part_layer_key(record)
            previous_part = previous_by_part.get(part_key) if part_key else None
            if part_key in representation_transition_keys:
                score += 650.0
                reasons.append("samePartRepresentationTransition")
            if part_key in mixed_representation_keys:
                score += 1000.0
                reasons.append("mixedRepresentationsForSamePart")
            if part_key in alpha_transition_keys:
                score += 750.0
                reasons.append("samePartAlphaRepresentationTransition")
            if part_key in mixed_alpha_keys:
                score += 1000.0
                reasons.append("mixedAlphaRepresentationsForSamePart")
            if previous_part is not None and (
                previous_part.get("positionStorageGeneration")
                != record.get("positionStorageGeneration")
                or previous_part.get("indexStorageGeneration")
                != record.get("indexStorageGeneration")
            ):
                backing_generation_changes += 1
                score += 90.0
                reasons.append("backingGenerationChangedForPart")
            if stage11_alpha_payload_gap(record, serial):
                alpha_payload_gaps += 1
            if blocker_leaked_to_final_casters(record):
                blocker_leaks += 1
            if full_domain_masked_anonymous_small_marker(record):
                full_domain_masked_marker_leaks += 1
            if grace_resurrected_anonymous_small_marker(record):
                grace_resurrected_marker_leaks += 1
            record_rank.append((score, reasons, record))

        current_count = int(frame.get("finalDrawCount", len(current_records)))
        previous_count = (
            int(frames[previous_serial].get("finalDrawCount", len(previous_records)))
            if previous_serial
            else current_count
        )
        count_delta = current_count - previous_count
        count_delta_ratio = (
            abs(count_delta) / max(previous_count, 1)
            if previous_serial
            else 0.0
        )
        hard_score = count_delta_ratio * 500.0
        hard_score += min(len(added) + len(removed), 100) * 2.0
        hard_score += min(backing_changes, 100) * 5.0
        hard_score += min(backing_generation_changes, 100) * 5.0
        hard_score += len(representation_transition_keys) * 650.0
        hard_score += len(mixed_representation_keys) * 1000.0
        hard_score += len(alpha_transition_keys) * 750.0
        hard_score += len(mixed_alpha_keys) * 1000.0
        hard_score += alpha_payload_gaps * 750.0
        hard_score += blocker_leaks * 1000.0
        hard_score += full_domain_masked_marker_leaks * 1000.0
        hard_score += grace_resurrected_marker_leaks * 1000.0
        hard_score += int(frame.get("validationRejectCandidateCount", 0)) * 1000.0

        capture = captures.get(serial)
        temporal_area = 0
        if capture and capture.get("temporal"):
            temporal_area = int(
                capture["temporal"].get("largestDarkComponent", 0)
            )
            hard_score += temporal_area * 10.0
        analysis_warmup_transition = bool(
            capture and int(capture.get("captureIndex", -1)) == 0
        )
        ranking_temporal_area = (
            0 if analysis_warmup_transition else temporal_area
        )
        if analysis_warmup_transition:
            # Capture zero has no preceding exact screenshot.  Its temporal
            # mask and the initial fallback-only -> full-scene caster
            # publication are startup alignment, not a one-frame anomaly.
            hard_score = 0.0

        record_rank.sort(key=lambda item: item[0], reverse=True)
        suspicious_records = [
            summarize_record(record, score, reasons)
            for score, reasons, record in record_rank[
                : max(1, args.records_per_frame)
            ]
            if score > 0.0
        ]
        rows.append(
            {
                "frameSerial": serial,
                "previousFrameSerial": previous_serial,
                "hardScore": round(hard_score, 3),
                "finalDrawCount": current_count,
                "countDelta": count_delta,
                "countDeltaRatio": count_delta_ratio,
                "identityAddedCount": len(added),
                "identityRemovedCount": len(removed),
                "backingChangeCount": backing_changes,
                "backingGenerationChangeCount": backing_generation_changes,
                "contentChangeCount": content_changes,
                "samePartRepresentationTransitionCount": len(
                    representation_transition_keys
                ),
                "samePartRepresentationTransitionKeys": sorted(
                    representation_transition_keys
                )[:128],
                "mixedRepresentationPartCount": len(
                    mixed_representation_keys
                ),
                "mixedRepresentationPartKeys": sorted(
                    mixed_representation_keys
                )[:128],
                "samePartAlphaRepresentationTransitionCount": len(
                    alpha_transition_keys
                ),
                "samePartAlphaRepresentationTransitionKeys": sorted(
                    alpha_transition_keys
                )[:128],
                "mixedAlphaRepresentationPartCount": len(mixed_alpha_keys),
                "mixedAlphaRepresentationPartKeys": sorted(
                    mixed_alpha_keys
                )[:128],
                "alphaPayloadGapCount": alpha_payload_gaps,
                "blockerLeakCount": blocker_leaks,
                "fullDomainMaskedMarkerLeakCount": (
                    full_domain_masked_marker_leaks
                ),
                "graceResurrectedMarkerLeakCount": (
                    grace_resurrected_marker_leaks
                ),
                "explainedPartDisappearanceCount": 0,
                "explainedPartDisappearanceKeys": [],
                "explainedPartDisappearanceReasons": {},
                "unexplainedPartDisappearanceCount": 0,
                "unexplainedPartDisappearanceKeys": [],
                "validationRejectCandidateCount": int(
                    frame.get("validationRejectCandidateCount", 0)
                ),
                "identityXor": frame.get("identityXor"),
                "identitySum": frame.get("identitySum"),
                "backingXor": frame.get("backingXor"),
                "contentXor": frame.get("contentXor"),
                "stageCounts": frame.get("stageCounts"),
                "categoryCounts": frame.get("categoryCounts"),
                "capture": capture,
                "temporalLargestDarkComponent": temporal_area,
                "rankingTemporalLargestDarkComponent": ranking_temporal_area,
                "analysisWarmupTransition": analysis_warmup_transition,
                "addedStableIdentityKeys": added[:64],
                "removedStableIdentityKeys": removed[:64],
                "suspiciousRecords": suspicious_records,
                "runtimeStats": runtime_stats.get(serial, {}),
            }
        )
        previous_records = current_records
        previous_serial = serial

    # A part present in both adjacent complete final-caster frames but absent
    # in the middle is a one-frame continuity hole.  Report it by the exact
    # handle/part/layer key instead of treating it as an object-wide count dip.
    part_sets = {
        serial: {
            key
            for record in records.get(serial, [])
            if (key := part_layer_key(record))
        }
        for serial in serials
    }
    row_by_serial = {int(row["frameSerial"]): row for row in rows}
    explained_part_disappearance_total = 0
    unexplained_part_disappearance_total = 0
    for index in range(1, len(serials) - 1):
        previous_key_set = part_sets[serials[index - 1]]
        current_key_set = part_sets[serials[index]]
        next_key_set = part_sets[serials[index + 1]]
        gaps = sorted((previous_key_set & next_key_set) - current_key_set)
        if not gaps:
            continue
        row = row_by_serial[serials[index]]
        key_stats = dict(
            (row.get("runtimeStats", {}) or {}).get("keyStats", {}) or {}
        )
        reason_budgets = [
            (
                "MissingRequiredPart",
                int(
                    key_stats.get(
                        "semanticSceneShadowManifestMissingRequiredPartCount",
                        0,
                    )
                    or 0
                ),
            ),
            (
                "RetiredAfterAuthoritativeAbsence",
                int(
                    key_stats.get(
                        "semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount",
                        0,
                    )
                    or 0
                ),
            ),
            (
                "TombstoneRetired",
                int(
                    key_stats.get(
                        "semanticSceneShadowManifestTombstoneRetiredCount",
                        0,
                    )
                    or 0
                ),
            ),
        ]
        explained: list[str] = []
        unexplained = list(gaps)
        explained_reasons: dict[str, int] = {}
        for reason, budget in reason_budgets:
            if budget <= 0 or not unexplained:
                continue
            take = min(budget, len(unexplained))
            explained.extend(unexplained[:take])
            del unexplained[:take]
            explained_reasons[reason] = take

        row["explainedPartDisappearanceCount"] = len(explained)
        row["explainedPartDisappearanceKeys"] = explained[:128]
        row["explainedPartDisappearanceReasons"] = explained_reasons
        row["unexplainedPartDisappearanceCount"] = len(unexplained)
        row["unexplainedPartDisappearanceKeys"] = unexplained[:128]
        if unexplained:
            row["hardScore"] = round(
                float(row["hardScore"]) + len(unexplained) * 900.0, 3
            )
        explained_part_disappearance_total += len(explained)
        unexplained_part_disappearance_total += len(unexplained)

    ranked = sorted(
        rows,
        key=lambda row: (
            row["hardScore"],
            row["rankingTemporalLargestDarkComponent"],
            row["countDeltaRatio"],
        ),
        reverse=True,
    )
    output = args.output or args.trace.with_name(
        args.trace.stem + "_final_caster_analysis.json"
    )
    result = {
        "trace": str(args.trace),
        "probeArtifact": str(args.probe_artifact)
        if args.probe_artifact
        else "",
        "temporalAnalysis": str(args.temporal_analysis)
        if args.temporal_analysis
        else "",
        "frameCount": len(frames),
        "recordCount": sum(len(value) for value in records.values()),
        "explainedPartDisappearanceCount": (
            explained_part_disappearance_total
        ),
        "unexplainedPartDisappearanceCount": (
            unexplained_part_disappearance_total
        ),
        "samePartRepresentationTransitionCount": sum(
            int(row["samePartRepresentationTransitionCount"]) for row in rows
        ),
        "mixedRepresentationPartCount": sum(
            int(row["mixedRepresentationPartCount"]) for row in rows
        ),
        "samePartAlphaRepresentationTransitionCount": sum(
            int(row["samePartAlphaRepresentationTransitionCount"])
            for row in rows
        ),
        "mixedAlphaRepresentationPartCount": sum(
            int(row["mixedAlphaRepresentationPartCount"]) for row in rows
        ),
        "alphaPayloadGapCount": sum(
            int(row["alphaPayloadGapCount"]) for row in rows
        ),
        "blockerLeakCount": sum(int(row["blockerLeakCount"]) for row in rows),
        "fullDomainMaskedMarkerLeakCount": sum(
            int(row["fullDomainMaskedMarkerLeakCount"]) for row in rows
        ),
        "graceResurrectedMarkerLeakCount": sum(
            int(row["graceResurrectedMarkerLeakCount"]) for row in rows
        ),
        "captureJoinCount": sum(1 for serial in serials if serial in captures),
        "captureTraceMissCount": len(unmapped_captures)
        + sum(1 for serial in captures if serial not in frames),
        "captureTraceUnmappedSerials": [
            int(row.get("traceSerial", 0)) for row in unmapped_captures
        ][:256],
        "captureTraceMissingFinalCasterFrameSerials": [
            serial for serial in sorted(captures) if serial not in frames
        ][:256],
        "captureTraceMissingFrameSerials": [
            int(row.get("traceSerial", 0)) for row in unmapped_captures
        ][:256]
        + [
            serial for serial in sorted(captures) if serial not in frames
        ][:256],
        "captureTraceCoveragePct": round(
            100.0
            * sum(1 for serial in captures if serial in frames)
            / max(len(captures) + len(unmapped_captures), 1),
            3,
        ),
        "traceSerialDomainContract": (
            "exact capture shadowFrameSerial -> "
            "shadowPoseFullTraceFrame.cadence.serial -> cadence.frameIndex -> "
            "shadowFinalCasterFrame.frameSerial"
        ),
        "parseErrors": parse_errors,
        "rankingContract": {
            "temporalComponentWeight": 10.0,
            "countDeltaRatioWeight": 500.0,
            "validationCandidateWeight": 1000.0,
            "backingChangeWeight": 5.0,
            "samePartRepresentationTransitionWeight": 650.0,
            "mixedRepresentationPartWeight": 1000.0,
            "samePartAlphaRepresentationTransitionWeight": 750.0,
            "mixedAlphaRepresentationPartWeight": 1000.0,
            "captureZeroExcludedAsWarmup": True,
        },
        "top": ranked[: max(1, args.top)],
        "allFrames": rows,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "output": str(output),
                "frameCount": result["frameCount"],
                "recordCount": result["recordCount"],
                "top": result["top"][:3],
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
