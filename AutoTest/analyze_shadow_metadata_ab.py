#!/usr/bin/env python3
"""Analyze a same-DLL A1/B1/B2/A2 shadow-metadata performance gate.

A enables the safe current-frame metadata channel. B disables only the
metadata master gate. The script deliberately reports relative changes and
workload-normalized section cost because foreground load makes absolute FPS an
unreliable signal on the development machine.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import statistics
from typing import Any, Iterable

import war3_autotest_mcp as autotest


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    pos = (len(ordered) - 1) * p
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return ordered[lo]
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def mad_filter(values: Iterable[float], scale: float = 4.5) -> list[float]:
    finite = [float(value) for value in values if math.isfinite(float(value))]
    if len(finite) < 5:
        return finite
    median = statistics.median(finite)
    deviations = [abs(value - median) for value in finite]
    mad = statistics.median(deviations)
    if mad <= 1.0e-9:
        return finite
    robust_sigma = 1.4826 * mad
    limit = scale * robust_sigma
    filtered = [value for value in finite if abs(value - median) <= limit]
    return filtered if len(filtered) >= max(5, len(finite) // 2) else finite


def series_stats(values: Iterable[float]) -> dict[str, float | int]:
    raw = [float(value) for value in values if math.isfinite(float(value))]
    filtered = mad_filter(raw)
    return {
        "samples": len(raw),
        "kept": len(filtered),
        "mean": round(statistics.fmean(filtered), 6) if filtered else 0.0,
        "p50": round(percentile(filtered, 0.50), 6),
        "p95": round(percentile(filtered, 0.95), 6),
        "mad": round(
            statistics.median(
                [abs(value - statistics.median(filtered)) for value in filtered]
            ),
            6,
        )
        if filtered
        else 0.0,
    }


def column_values(report: dict[str, Any], series: str, columns: str,
                  name: str) -> list[float]:
    names = report.get(columns, [])
    if name not in names:
        return []
    index = names.index(name)
    return [float(row[index]) for row in report.get(series, []) if len(row) > index]


def section(report: dict[str, Any], path: str) -> dict[str, Any]:
    for entry in report.get("sections", []):
        if entry.get("path") == path:
            return entry
    return {}


def hotspot(report: dict[str, Any], name: str) -> dict[str, Any]:
    for entry in report.get("semanticPerfHotspots", []):
        if entry.get("name") == name:
            return entry
    return {}


def average(values: Iterable[float]) -> float:
    finite = [float(value) for value in values if math.isfinite(float(value))]
    return statistics.fmean(finite) if finite else 0.0


def load_run(artifact_path: pathlib.Path) -> dict[str, Any]:
    artifact = json.loads(artifact_path.read_text(encoding="utf-8"))
    report_path = pathlib.Path(artifact["result"]["report"]["reportPath"])
    report = autotest._extract_json_object(
        report_path.read_text(encoding="utf-8", errors="ignore")
    )
    budget = report.get("shadowBudgetSummary", {})
    frames = max(int(report.get("frameCount", 0)), 1)
    captured = average(
        column_values(
            report, "workloadSeries", "workloadSeriesColumns", "capturedDrawCount"
        )
    )
    replay = average(
        column_values(
            report, "workloadSeries", "workloadSeriesColumns", "replayCasterCount"
        )
    )
    direct = average(
        column_values(
            report,
            "workloadSeries",
            "workloadSeriesColumns",
            "semanticSceneSubmitted",
        )
    )
    shadow_capture = section(report, "ShadowCapture")
    gates = section(report, "ShadowCapture/Gates")
    post_gate = section(report, "ShadowCapture/PostGate")
    metadata = hotspot(report, "MetadataCapture")
    main_avg = float(report.get("avgMainThreadCpuMs", 0.0))
    capture_avg = float(shadow_capture.get("avgCpuMs", 0.0))
    metadata_frame_us = series_stats(
        column_values(
            report,
            "workloadSeries",
            "workloadSeriesColumns",
            "shadowMetadataCaptureUs",
        )
    )
    observed = max(int(budget.get("framesObserved", frames)), 1)
    metadata_applied_per_frame = (
        float(budget.get("shadowMetadataAppliedCount", 0)) / observed
    )
    base_captured = max(0.0, captured - metadata_applied_per_frame)
    return {
        "artifact": str(artifact_path),
        "report": str(report_path),
        "ok": bool(artifact.get("ok")),
        "belowNormal": artifact.get("priorityOverride", {}).get("priority")
        == "BELOW_NORMAL",
        "frameCount": frames,
        "frameWallMs": series_stats(
            column_values(report, "frameSeries", "frameSeriesColumns", "frameWallMs")
        ),
        "mainCpuMs": series_stats(
            column_values(report, "frameSeries", "frameSeriesColumns", "mainCpuMs")
        ),
        "processCpuMs": series_stats(
            column_values(report, "frameSeries", "frameSeriesColumns", "processCpuMs")
        ),
        "avgMainCpuMs": main_avg,
        "sections": {
            "ShadowCapture": capture_avg,
            "Gates": float(gates.get("avgCpuMs", 0.0)),
            "PostGate": float(post_gate.get("avgCpuMs", 0.0)),
        },
        "metadataSelfMsPerActiveFrame": float(metadata.get("avgUsPerCall", 0.0))
        / 1000.0,
        "metadataFrameUs": metadata_frame_us,
        "metadataShareOfMainPct": (
            float(metadata_frame_us["p50"]) / 10.0 / main_avg
            if main_avg > 0.0
            else 0.0
        ),
        "workloadPerFrame": {
            "capturedDraw": captured,
            "metadataApplied": metadata_applied_per_frame,
            "baseCapturedDraw": base_captured,
            "replayCaster": replay,
            "semanticSubmitted": direct,
        },
        "shadowCaptureUsPerCapturedDraw": (
            capture_avg * 1000.0 / captured if captured > 0.0 else 0.0
        ),
        "metadataP50UsPerRestoredAlphaCaster": (
            float(metadata_frame_us["p50"]) / metadata_applied_per_frame
            if metadata_applied_per_frame > 0.0
            else 0.0
        ),
        "integrity": {
            "framesObserved": budget.get("framesObserved", 0),
            "framesIncomplete": budget.get("framesIncomplete", 0),
            "framesBudgetExceeded": budget.get("framesBudgetExceeded", 0),
            "metadataClassified": budget.get("shadowMetadataClassifiedCount", 0),
            "metadataCaptured": budget.get("shadowMetadataCapturedCount", 0),
            "metadataApplied": budget.get("shadowMetadataAppliedCount", 0),
            "metadataRejectedByReason": budget.get(
                "shadowMetadataRejectedByReasonCount", 0
            ),
            "metadataRejectedOpaque": budget.get(
                "shadowMetadataRejectedOpaqueCount", 0
            ),
            "blockerFinalLeak": budget.get("shadowMetadataBlockerFinalLeakCount", 0),
        },
    }


def delta_pct(a: float, b: float) -> float | None:
    if abs(b) <= 1.0e-12:
        return None
    return round((a - b) * 100.0 / b, 4)


def compare(a: dict[str, Any], b: dict[str, Any]) -> dict[str, Any]:
    restored_alpha = a["workloadPerFrame"]["metadataApplied"]
    incremental_capture_us = (
        (a["sections"]["ShadowCapture"] - b["sections"]["ShadowCapture"])
        * 1000.0
    )
    return {
        "frameWallP50Pct": delta_pct(a["frameWallMs"]["p50"], b["frameWallMs"]["p50"]),
        "frameWallP95Pct": delta_pct(a["frameWallMs"]["p95"], b["frameWallMs"]["p95"]),
        "mainCpuMeanPct": delta_pct(a["avgMainCpuMs"], b["avgMainCpuMs"]),
        "mainCpuP50Pct": delta_pct(a["mainCpuMs"]["p50"], b["mainCpuMs"]["p50"]),
        "mainCpuP95Pct": delta_pct(a["mainCpuMs"]["p95"], b["mainCpuMs"]["p95"]),
        "shadowCapturePct": delta_pct(
            a["sections"]["ShadowCapture"], b["sections"]["ShadowCapture"]
        ),
        "shadowCapturePerDrawPct": delta_pct(
            a["shadowCaptureUsPerCapturedDraw"],
            b["shadowCaptureUsPerCapturedDraw"],
        ),
        "baseCapturedDrawWorkloadPct": delta_pct(
            a["workloadPerFrame"]["baseCapturedDraw"],
            b["workloadPerFrame"]["baseCapturedDraw"],
        ),
        "rawCapturedDrawIncreasePct": delta_pct(
            a["workloadPerFrame"]["capturedDraw"],
            b["workloadPerFrame"]["capturedDraw"],
        ),
        "metadataP50Us": round(float(a["metadataFrameUs"]["p50"]), 4),
        "metadataP95Us": round(float(a["metadataFrameUs"]["p95"]), 4),
        "metadataP50ShareOfMainPct": round(float(a["metadataShareOfMainPct"]), 4),
        "incrementalShadowCaptureUsPerRestoredAlphaCaster": round(
            incremental_capture_us / restored_alpha, 4
        )
        if restored_alpha > 0.0
        else None,
    }


def mean_pair_delta(comparisons: list[dict[str, Any]], key: str) -> float | None:
    values = [entry[key] for entry in comparisons if entry.get(key) is not None]
    return round(statistics.fmean(values), 4) if values else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("a1", type=pathlib.Path)
    parser.add_argument("b1", type=pathlib.Path)
    parser.add_argument("b2", type=pathlib.Path)
    parser.add_argument("a2", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    runs = {
        "A1": load_run(args.a1),
        "B1": load_run(args.b1),
        "B2": load_run(args.b2),
        "A2": load_run(args.a2),
    }
    comparisons = [compare(runs["A1"], runs["B1"]), compare(runs["A2"], runs["B2"])]
    keys = list(comparisons[0].keys())
    mean_deltas = {key: mean_pair_delta(comparisons, key) for key in keys}
    output = {
        "contract": "same DLL A1/B1/B2/A2; A=metadata on, B=metadata off",
        "runs": runs,
        "pairComparisonsPct": {"A1_vs_B1": comparisons[0], "A2_vs_B2": comparisons[1]},
        "meanPairDeltaPct": mean_deltas,
        "gate": {
            "workloadMatchedWithin5Pct": all(
                abs(value) <= 5.0
                for comparison in comparisons
                for key, value in comparison.items()
                if key == "baseCapturedDrawWorkloadPct" and value is not None
            ),
            "wallP50Within5Pct": abs(mean_deltas.get("frameWallP50Pct") or 0.0) <= 5.0,
            "wallP95Within5Pct": abs(mean_deltas.get("frameWallP95Pct") or 0.0) <= 5.0,
            "integrity": all(
                run["ok"]
                and run["belowNormal"]
                and run["integrity"]["framesIncomplete"] == 0
                and run["integrity"]["framesBudgetExceeded"] == 0
                for run in runs.values()
            ),
        },
    }
    text = json.dumps(output, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
