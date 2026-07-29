#!/usr/bin/env python3
"""Analyze the same-DLL A1/B1/B2/A2 semantic stats hot-path gate.

A collects the legacy exact dynamic-unit-evidence diagnostic after every
successful skinned append. B is the production default: rendering and all
caster counters remain active, but that diagnostic-only CUnit SafeRead chain
is deferred unless explicitly requested. Foreground contention is filtered by
MAD and conclusions use paired relative changes rather than absolute FPS.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import statistics
from typing import Any

import war3_autotest_mcp as autotest
from analyze_shadow_metadata_ab import (
    column_values,
    delta_pct,
    series_stats,
)


def _average(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def _find_section(report: dict[str, Any], suffix: str) -> dict[str, Any]:
    for entry in report.get("sections", []):
        if str(entry.get("path", "")).endswith(suffix):
            return entry
    return {}


def load_run(path: pathlib.Path) -> dict[str, Any]:
    artifact = json.loads(path.read_text(encoding="utf-8"))
    result = artifact.get("result", artifact)
    report_summary = result.get("report", {})
    report_path = pathlib.Path(report_summary["reportPath"])
    report = autotest._extract_json_object(
        report_path.read_text(encoding="utf-8", errors="ignore")
    )
    budget = report.get("shadowBudgetSummary", {})
    workload = {
        name: _average(
            column_values(report, "workloadSeries", "workloadSeriesColumns", name)
        )
        for name in (
            "capturedDrawCount",
            "replayCasterCount",
            "semanticSceneSubmitted",
            "shadowMapDrawnCasters",
        )
    }
    paths = {
        "DirectGrouped": "/Populate/DirectGrouped",
        "BuildEligible": "/Populate/DirectGrouped/BuildEligible",
        "Submit": "/Populate/DirectGrouped/Submit",
        "ShadowCapture": "ShadowCapture",
        "ShadowMap": "/ShadowReceiver/ShadowMap",
    }
    return {
        "artifact": str(path),
        "report": str(report_path),
        "ok": bool(artifact.get("ok") and result.get("ok")),
        "belowNormal": artifact.get("priorityOverride", {}).get("priority")
        == "BELOW_NORMAL",
        "frameCount": int(report.get("frameCount", 0)),
        "frameWallMs": series_stats(
            column_values(report, "frameSeries", "frameSeriesColumns", "frameWallMs")
        ),
        "mainCpuMs": series_stats(
            column_values(report, "frameSeries", "frameSeriesColumns", "mainCpuMs")
        ),
        "processCpuMs": series_stats(
            column_values(report, "frameSeries", "frameSeriesColumns", "processCpuMs")
        ),
        "workloadPerFrame": workload,
        "sections": {
            name: float(_find_section(report, suffix).get("avgCpuMs", 0.0))
            for name, suffix in paths.items()
        },
        "integrity": {
            "framesObserved": int(budget.get("framesObserved", 0)),
            "framesIncomplete": int(budget.get("framesIncomplete", 0)),
            "framesBudgetExceeded": int(budget.get("framesBudgetExceeded", 0)),
            "blockerFinalLeak": int(
                budget.get("shadowMetadataBlockerFinalLeakCount", 0)
            ),
        },
    }


def compare(optimized: dict[str, Any], exact: dict[str, Any]) -> dict[str, Any]:
    return {
        "frameWallP50Pct": delta_pct(
            optimized["frameWallMs"]["p50"], exact["frameWallMs"]["p50"]
        ),
        "frameWallP95Pct": delta_pct(
            optimized["frameWallMs"]["p95"], exact["frameWallMs"]["p95"]
        ),
        "mainCpuP50Pct": delta_pct(
            optimized["mainCpuMs"]["p50"], exact["mainCpuMs"]["p50"]
        ),
        "mainCpuP95Pct": delta_pct(
            optimized["mainCpuMs"]["p95"], exact["mainCpuMs"]["p95"]
        ),
        "capturedDrawWorkloadPct": delta_pct(
            optimized["workloadPerFrame"]["capturedDrawCount"],
            exact["workloadPerFrame"]["capturedDrawCount"],
        ),
        "semanticSubmittedWorkloadPct": delta_pct(
            optimized["workloadPerFrame"]["semanticSceneSubmitted"],
            exact["workloadPerFrame"]["semanticSceneSubmitted"],
        ),
        "directGroupedPct": delta_pct(
            optimized["sections"]["DirectGrouped"],
            exact["sections"]["DirectGrouped"],
        ),
        "submitPct": delta_pct(
            optimized["sections"]["Submit"], exact["sections"]["Submit"]
        ),
        "shadowCapturePct": delta_pct(
            optimized["sections"]["ShadowCapture"],
            exact["sections"]["ShadowCapture"],
        ),
    }


def _pair_mean(pairs: list[dict[str, Any]], key: str) -> float | None:
    values = [float(pair[key]) for pair in pairs if pair.get(key) is not None]
    return round(statistics.fmean(values), 4) if values else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("a1", type=pathlib.Path, help="legacy exact diagnostic")
    parser.add_argument("b1", type=pathlib.Path, help="optimized deferred diagnostic")
    parser.add_argument("b2", type=pathlib.Path, help="optimized deferred diagnostic")
    parser.add_argument("a2", type=pathlib.Path, help="legacy exact diagnostic")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    runs = {
        "A1": load_run(args.a1),
        "B1": load_run(args.b1),
        "B2": load_run(args.b2),
        "A2": load_run(args.a2),
    }
    pairs = [compare(runs["B1"], runs["A1"]), compare(runs["B2"], runs["A2"])]
    mean = {key: _pair_mean(pairs, key) for key in pairs[0]}
    workload_keys = ("capturedDrawWorkloadPct", "semanticSubmittedWorkloadPct")
    output = {
        "contract": (
            "same DLL A1/B1/B2/A2; A=exact diagnostic SafeRead, "
            "B=production deferred diagnostic; negative delta is faster"
        ),
        "runs": runs,
        "pairComparisonsPct": {"B1_vs_A1": pairs[0], "B2_vs_A2": pairs[1]},
        "meanPairDeltaPct": mean,
        "gate": {
            "workloadMatchedWithin5Pct": all(
                pair.get(key) is not None and abs(float(pair[key])) <= 5.0
                for pair in pairs
                for key in workload_keys
            ),
            "p50ImprovedAtLeast8Pct": (
                mean.get("mainCpuP50Pct") is not None
                and float(mean["mainCpuP50Pct"]) <= -8.0
            ),
            "p95NotRegressedOver5Pct": (
                mean.get("mainCpuP95Pct") is not None
                and float(mean["mainCpuP95Pct"]) <= 5.0
            ),
            "integrity": all(
                run["ok"]
                and run["belowNormal"]
                and run["integrity"]["framesIncomplete"] == 0
                and run["integrity"]["framesBudgetExceeded"] == 0
                and run["integrity"]["blockerFinalLeak"] == 0
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
