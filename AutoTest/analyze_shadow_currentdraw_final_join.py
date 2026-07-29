#!/usr/bin/env python3
"""Join upstream CurrentDraw contracts to final S11 shadow casters.

This analyzer is intentionally object-centric.  It answers whether a final
caster part disappeared because Warcraft III stopped submitting the object's
draw contract, or because WarVK rejected/lost it between CurrentDraw and the
final shadow replay choke point.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument(
        "--rawcode",
        type=lambda value: int(value, 0),
        default=0x68666F6F,
        help="Object rawcode as decimal or 0x... (default: hfoo).",
    )
    parser.add_argument("--stage", type=int, default=11)
    parser.add_argument("--top", type=int, default=80)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def final_part_key(record: dict[str, Any]) -> str:
    fields = (
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
        "alphaTestEnabled",
    )
    return "|".join(str(record.get(field, 0)) for field in fields)


def current_draw_key(record: dict[str, Any]) -> str:
    fields = (
        "renderablePart",
        "meshPayloadPtr",
        "layerIndex",
        "payloadWordF0",
        "payloadWord104",
        "payloadWord108",
        "payloadWord11C",
        "payloadWord48",
        "stream1Ptr",
        "stream1Stride",
    )
    return "|".join(str(record.get(field, 0)) for field in fields)


def summarize_final(record: dict[str, Any]) -> dict[str, Any]:
    fields = (
        "index",
        "jHandle",
        "rawcode",
        "indexCount",
        "numVertices",
        "positionStride",
        "positionFormat",
        "vertexBlendEnabled",
        "vertexBlendIndexed",
        "vertexBlendCount",
        "alphaTestEnabled",
        "paletteIndex",
        "boundsCenter",
        "boundsRadius",
        "backingHash",
        "contentHash",
        "positionStorageGeneration",
        "indexStorageGeneration",
    )
    return {field: record.get(field) for field in fields}


def summarize_current(record: dict[str, Any]) -> dict[str, Any]:
    fields = (
        "index",
        "jHandle",
        "rawcode",
        "known",
        "sceneNode",
        "renderablePart",
        "meshPayloadPtr",
        "layerIndex",
        "payloadWordF0",
        "payloadWord104",
        "payloadWord108",
        "payloadWord11C",
        "payloadWord48",
        "stream1Ptr",
        "stream1Stride",
        "capturedPaletteCount",
        "frameTag",
        "visibleFrameSerial",
        "renderFrameIndex",
        "captureSerial",
    )
    return {field: record.get(field) for field in fields}


def counter_delta(
    current: Counter[str], previous: Counter[str]
) -> tuple[list[str], list[str]]:
    return (
        list((current - previous).elements()),
        list((previous - current).elements()),
    )


def main() -> int:
    args = parse_args()
    final_by_frame: dict[int, dict[int, list[dict[str, Any]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    current_by_frame: dict[int, dict[int, list[dict[str, Any]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    frame_stats: dict[int, dict[str, Any]] = {}
    object_names: dict[int, dict[str, Any]] = {}
    parse_errors: list[dict[str, Any]] = []

    with args.trace.open("r", encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            if "shadow" not in line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as error:
                parse_errors.append(
                    {
                        "line": line_number,
                        "column": error.colno,
                        "message": error.msg,
                    }
                )
                continue
            event_type = event.get("type")
            serial = int(event.get("frameSerial", 0))
            if serial <= 0:
                continue
            if event_type == "shadowFinalCasterFrame":
                frame_stats[serial] = event
                continue
            if int(event.get("rawcode", 0)) != args.rawcode:
                continue
            handle = int(event.get("jHandle", 0))
            if handle == 0:
                continue
            if (
                event_type == "shadowFinalCasterRecord"
                and int(event.get("stage", -1)) == args.stage
            ):
                final_by_frame[serial][handle].append(event)
            elif event_type == "shadowPoseFullTraceCurrentDraw":
                current_by_frame[serial][handle].append(event)
            elif event_type == "shadowPoseFullTraceObject":
                object_names[handle] = {
                    "jHandle": handle,
                    "modelPath": event.get("modelPath", ""),
                    "kind": event.get("kind"),
                    "runtimeModelPtr": event.get("runtimeModelPtr"),
                }

    serials = sorted(
        set(frame_stats) | set(final_by_frame) | set(current_by_frame)
    )
    handles = sorted(
        {
            handle
            for by_handle in (
                list(final_by_frame.values())
                + list(current_by_frame.values())
            )
            for handle in by_handle
        }
    )
    previous_final: dict[int, Counter[str]] = {}
    previous_current: dict[int, Counter[str]] = {}
    rows: list[dict[str, Any]] = []
    anomalies: list[dict[str, Any]] = []
    current_join_observed_handles = {
        handle
        for by_handle in current_by_frame.values()
        for handle, records_for_handle in by_handle.items()
        if records_for_handle
    }

    for serial in serials:
        frame_row: dict[str, Any] = {
            "frameSerial": serial,
            "finalFrameCount": int(
                frame_stats.get(serial, {}).get("finalDrawCount", 0)
            ),
            "objects": [],
        }
        for handle in handles:
            final_records = final_by_frame.get(serial, {}).get(handle, [])
            current_records = current_by_frame.get(serial, {}).get(handle, [])
            final_counter = Counter(final_part_key(row) for row in final_records)
            current_counter = Counter(
                current_draw_key(row) for row in current_records
            )
            prev_final = previous_final.get(handle, Counter())
            prev_current = previous_current.get(handle, Counter())
            final_added, final_removed = counter_delta(
                final_counter, prev_final
            )
            current_added, current_removed = counter_delta(
                current_counter, prev_current
            )
            if (
                final_records
                or current_records
                or prev_final
                or prev_current
            ):
                object_row = {
                    "jHandle": handle,
                    "finalPartCount": sum(final_counter.values()),
                    "currentDrawCount": sum(current_counter.values()),
                    "finalAddedCount": len(final_added),
                    "finalRemovedCount": len(final_removed),
                    "currentAddedCount": len(current_added),
                    "currentRemovedCount": len(current_removed),
                    "finalAddedPartKeys": final_added,
                    "finalRemovedPartKeys": final_removed,
                    "currentAddedKeys": current_added,
                    "currentRemovedKeys": current_removed,
                }
                frame_row["objects"].append(object_row)
                if final_removed:
                    upstream_persisted = (
                        sum(current_counter.values()) > 0
                        and len(current_removed) == 0
                    )
                    if upstream_persisted:
                        classification = "WarVKDownstreamGap"
                    elif current_counter or prev_current:
                        classification = "UpstreamDrawChangedOrMissing"
                    else:
                        # CurrentDraw records can legitimately lack the object
                        # identity that is attached later in the semantic
                        # pipeline.  Absence from this handle join is therefore
                        # not evidence that Warcraft III omitted the draw.
                        classification = "CurrentDrawHandleJoinUnavailable"
                    anomalies.append(
                        {
                            "frameSerial": serial,
                            **object_row,
                            "upstreamCurrentDrawPersisted": upstream_persisted,
                            "classification": classification,
                            "object": object_names.get(handle, {}),
                            "finalRecords": [
                                summarize_final(row) for row in final_records
                            ],
                            "currentDrawRecords": [
                                summarize_current(row)
                                for row in current_records
                            ],
                        }
                    )
            previous_final[handle] = final_counter
            previous_current[handle] = current_counter
        rows.append(frame_row)

    anomalies.sort(
        key=lambda row: (
            row["classification"] == "WarVKDownstreamGap",
            row["finalRemovedCount"],
            row["currentDrawCount"],
        ),
        reverse=True,
    )
    counts_by_handle: dict[int, dict[int, int]] = defaultdict(dict)
    for frame in rows:
        serial = int(frame["frameSerial"])
        for object_row in frame["objects"]:
            counts_by_handle[int(object_row["jHandle"])][serial] = int(
                object_row["finalPartCount"]
            )

    continuity: list[dict[str, Any]] = []
    for handle in handles:
        observed = counts_by_handle.get(handle, {})
        positive_serials = [
            serial for serial, count in observed.items() if count > 0
        ]
        if not positive_serials:
            continue
        first_positive = min(positive_serials)
        zero_frames = 0
        zero_runs = 0
        current_zero_run = 0
        max_zero_run = 0
        transitions = 0
        previous_count: int | None = None
        for serial in serials:
            if serial < first_positive:
                continue
            count = int(observed.get(serial, 0))
            if previous_count is not None and count != previous_count:
                transitions += 1
            previous_count = count
            if count == 0:
                zero_frames += 1
                current_zero_run += 1
                if current_zero_run == 1:
                    zero_runs += 1
                max_zero_run = max(max_zero_run, current_zero_run)
            else:
                current_zero_run = 0
        continuity.append(
            {
                "jHandle": handle,
                "firstPositiveFrameSerial": first_positive,
                "zeroFramesAfterFirstPositive": zero_frames,
                "zeroRunCount": zero_runs,
                "maxZeroRunFrames": max_zero_run,
                "partCountTransitionCount": transitions,
                "currentDrawHandleJoinEverObserved": (
                    handle in current_join_observed_handles
                ),
            }
        )

    first_all_positive = 0
    first_positive_by_handle = {
        row["jHandle"]: row["firstPositiveFrameSerial"]
        for row in continuity
    }
    if len(first_positive_by_handle) == len(handles) and handles:
        first_all_positive = max(first_positive_by_handle.values())
    synchronized_absence_serials: list[int] = []
    if first_all_positive:
        for serial in serials:
            if serial < first_all_positive:
                continue
            if all(
                counts_by_handle.get(handle, {}).get(serial, 0) == 0
                for handle in handles
            ):
                synchronized_absence_serials.append(serial)

    classification_counts = Counter(
        row["classification"] for row in anomalies
    )
    output = args.output or args.trace.with_name(
        args.trace.stem + "_currentdraw_final_join.json"
    )
    result = {
        "trace": str(args.trace),
        "rawcode": args.rawcode,
        "rawcodeBigEndian": args.rawcode.to_bytes(4, "big").decode(
            "latin-1", errors="replace"
        ),
        "stage": args.stage,
        "frameCount": len(serials),
        "handleCount": len(handles),
        "parseErrors": parse_errors,
        "anomalyCount": len(anomalies),
        "downstreamGapCount": sum(
            row["classification"] == "WarVKDownstreamGap"
            for row in anomalies
        ),
        "classificationCounts": dict(classification_counts),
        "continuity": continuity,
        "synchronizedFullAbsenceFrameCount": len(
            synchronized_absence_serials
        ),
        "synchronizedFullAbsenceFrameSerials": (
            synchronized_absence_serials[:256]
        ),
        "top": anomalies[: args.top],
        "allFrames": rows,
    }
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "output": str(output),
                "frameCount": result["frameCount"],
                "handleCount": result["handleCount"],
                "anomalyCount": result["anomalyCount"],
                "downstreamGapCount": result["downstreamGapCount"],
                "top": result["top"][:5],
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
