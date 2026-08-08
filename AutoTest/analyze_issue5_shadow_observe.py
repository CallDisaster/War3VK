#!/usr/bin/env python3
"""Analyze a non-mutating Issue #5 shadow-culling perf recording.

The game must have been launched with both observer modes set to 1 and the
performance recorder must cover at least 10,000 Presents.  This tool never
launches, focuses, reprioritizes, or stops Warcraft III.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import war3_autotest_mcp as war3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?", default="")
    parser.add_argument("--war3-dir", default=r"E:\Work\War3")
    parser.add_argument("--output", default="")
    parser.add_argument(
        "--require-admission-ready",
        action="store_true",
        help="Return exit code 2 unless every Observe admission gate closes.",
    )
    return parser.parse_args()


def analyze_report(report: Path) -> dict:
    content = report.read_text(encoding="utf-8", errors="ignore")
    data = war3._extract_json_object(content)
    if not data:
        raise ValueError("无法从性能报告提取 const data JSON")
    return {
        "reportPath": str(report.resolve()),
        "attachOnlyAnalysis": True,
        "neverLaunchesOrStopsGame": True,
        "avgFps": float(data.get("avgFps", 0.0) or 0.0),
        "avgFrameTimeMs": float(data.get("avgFrameTimeMs", 0.0) or 0.0),
        "avgGpuTimeMs": float(data.get("avgGpuTimeMs", 0.0) or 0.0),
        "shadowCullObserveSummary":
            war3._extract_shadow_cull_observe_summary(data),
    }


def main() -> int:
    args = parse_args()
    report = Path(args.report) if args.report else war3._find_latest_report(
        Path(args.war3_dir)
    )
    if report is None or not report.is_file():
        raise SystemExit("未找到性能报告；请指定 HTML 或先完成一次录制")

    result = analyze_report(report)
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)

    ready = bool(
        result["shadowCullObserveSummary"]["gate"][
            "consumeAdmissionReady"
        ]
    )
    return 0 if ready or not args.require_admission_ready else 2


if __name__ == "__main__":
    raise SystemExit(main())
