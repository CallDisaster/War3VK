#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
夜间连续执行：
1. City 优先，失败自动回退到光影测试.w3x；
2. 先跑三轮稳定性，再跑三轮压力，再跑三轮低压基准回归；
3. 全部结果统一落盘，不以中途人工汇总为停点。
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

from war3_autotest_mcp import (
    DEFAULT_CITY_MAP,
    DEFAULT_TEST_MAP,
    run_city_shadow_pressure_suite,
    run_city_shadow_stability_suite,
    run_quick_autotest,
)


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "AutoTest" / "artifacts" / "overnight_shadow_plan" / datetime.now().strftime("%Y%m%d_%H%M%S")
OUT_DIR.mkdir(parents=True, exist_ok=True)
LOG_PATH = OUT_DIR / "runner.log"
JSON_PATH = OUT_DIR / "result.json"


def log(msg: str) -> None:
    line = f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    with LOG_PATH.open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def save_json(obj: Dict[str, Any]) -> None:
    JSON_PATH.write_text(json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8")


def run_build() -> Dict[str, Any]:
    p = subprocess.run(
        ["ninja", "-C", "build32"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    return {
        "ok": p.returncode == 0,
        "returncode": p.returncode,
        "stdoutTail": p.stdout[-4000:],
        "stderrTail": p.stderr[-4000:],
    }


def compact_stability(res: Dict[str, Any]) -> Dict[str, Any]:
    report = dict(res.get("report", {}) or {})
    return {
        "ok": bool(res.get("ok")),
        "stage": res.get("stage"),
        "requestedMapPath": res.get("requestedMapPath"),
        "actualMapPath": res.get("actualMapPath"),
        "fallbackUsed": res.get("fallbackUsed"),
        "flickerSuspect": res.get("flickerSuspect"),
        "lowPitchMissingShadowSuspect": res.get("lowPitchMissingShadowSuspect"),
        "reportPath": report.get("reportPath"),
        "avgFps": report.get("avgFps"),
        "shadowBudgetSummary": res.get("shadowBudgetSummary"),
        "logSummary": res.get("logSummary"),
    }


def compact_pressure(res: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "ok": bool(res.get("ok")),
        "requestedMapPath": res.get("requestedMapPath"),
        "actualMapPath": res.get("actualMapPath"),
        "fallbackUsed": res.get("fallbackUsed"),
        "passed": res.get("passed"),
        "total": res.get("total"),
        "rounds": [
            {
                "round": row.get("round"),
                "ok": row.get("ok"),
                "actualMapPath": row.get("actualMapPath"),
                "fallbackUsed": row.get("fallbackUsed"),
                "reportPath": dict(row.get("report", {}) or {}).get("reportPath"),
                "avgFps": dict(row.get("report", {}) or {}).get("avgFps"),
                "shadowBudgetSummary": row.get("shadowBudgetSummary"),
                "logSummary": row.get("logSummary"),
            }
            for row in list(res.get("rounds", []) or [])
        ],
    }


def compact_quick(res: Dict[str, Any]) -> Dict[str, Any]:
    report = dict(res.get("report", {}) or {})
    return {
        "ok": bool(res.get("ok")),
        "stage": res.get("stage"),
        "reportPath": report.get("reportPath"),
        "avgFps": report.get("avgFps"),
        "avgFrameTimeMs": report.get("avgFrameTimeMs"),
        "avgGpuTimeMs": report.get("avgGpuTimeMs"),
        "avgMainThreadCpuMs": report.get("avgMainThreadCpuMs"),
        "shadowBudgetSummary": report.get("shadowBudgetSummary"),
        "logSummary": report.get("logSummary"),
    }


def main() -> int:
    result: Dict[str, Any] = {
        "startedAt": datetime.now().isoformat(timespec="seconds"),
        "cityRequestedMap": str(DEFAULT_CITY_MAP),
        "fallbackMap": str(DEFAULT_TEST_MAP),
        "nightlyPolicy": {
            "cityFirstThenFallback": True,
            "doNotStopForInterimSummary": True,
            "stabilityBeforePressureBeforeLowPressureRegression": True,
        },
        "build": {},
        "stabilityRuns": [],
        "pressure": {},
        "lowPressureRuns": [],
        "gitSavepoint": {
            "attempted": False,
            "status": "deferred",
            "reason": "dirty worktree; auto commit not forced by runner",
        },
    }
    save_json(result)

    log("build32 编译开始")
    build = run_build()
    result["build"] = build
    save_json(result)
    if not build.get("ok"):
        log("build32 编译失败，runner 终止")
        result["finishedAt"] = datetime.now().isoformat(timespec="seconds")
        result["ok"] = False
        save_json(result)
        return 1
    log("build32 编译通过")

    for idx in range(3):
        log(f"稳定性轮次 {idx + 1}/3 开始")
        res = run_city_shadow_stability_suite(
            war3_dir=r"E:\Work\War3",
            map_path=str(DEFAULT_CITY_MAP),
            allow_fallback_to_default_test_map=True,
            ready_timeout_sec=180,
            settle_sec=60,
            sequence_frame_count=5,
            sequence_interval_sec=0.25,
            use_isolated_desktop=True,
            desktop_name=f"War3NightlyStability_{idx + 1}",
            profile="full_analysis",
            windowed=True,
            sample_duration_sec=6,
            deploy_d3d9_before_launch=True,
            build_d3d9_path=r"build32/src/d3d9/d3d9.dll",
        )
        compact = compact_stability(res)
        result["stabilityRuns"].append(compact)
        save_json(result)
        log(
            f"稳定性轮次 {idx + 1}/3 结束 "
            f"ok={compact.get('ok')} map={compact.get('actualMapPath')} fallback={compact.get('fallbackUsed')}"
        )
        if not compact.get("ok"):
            result["finishedAt"] = datetime.now().isoformat(timespec="seconds")
            result["ok"] = False
            save_json(result)
            return 2

    log("三轮稳定性通过，开始三轮压力测试")
    pressure = run_city_shadow_pressure_suite(
        war3_dir=r"E:\Work\War3",
        map_path=str(DEFAULT_CITY_MAP),
        allow_fallback_to_default_test_map=True,
        rounds=3,
        duration_sec=600,
        sample_interval_sec=30,
        ready_timeout_sec=180,
        use_isolated_desktop=True,
        desktop_name="War3NightlyPressure",
        profile="full_default",
        windowed=True,
        deploy_d3d9_before_launch=True,
        build_d3d9_path=r"build32/src/d3d9/d3d9.dll",
    )
    result["pressure"] = compact_pressure(pressure)
    save_json(result)
    log(
        f"压力测试结束 ok={result['pressure'].get('ok')} "
        f"passed={result['pressure'].get('passed')}/{result['pressure'].get('total')}"
    )
    if not result["pressure"].get("ok"):
        result["finishedAt"] = datetime.now().isoformat(timespec="seconds")
        result["ok"] = False
        save_json(result)
        return 3

    log("压力签收通过，开始三轮低压基准回归")
    for idx in range(3):
        res = run_quick_autotest(
            war3_dir=r"E:\Work\War3",
            map_path=str(DEFAULT_TEST_MAP),
            ready_timeout_sec=150,
            sample_duration_sec=18,
            windowed=True,
            use_isolated_desktop=True,
            desktop_name=f"War3NightlyQuick_{idx + 1}",
            opengl=False,
            auto_perf_record=True,
            auto_perf_export_sec=8,
            deploy_d3d9_before_launch=True,
            build_d3d9_path=r"build32/src/d3d9/d3d9.dll",
            enforce_video_baseline=True,
            baseline_width=2560,
            baseline_height=1440,
            baseline_refresh_rate=59,
            include_sections_in_report=True,
            section_top_n=40,
            avoid_focus_on_stop=True,
            profile="full_default",
        )
        compact = compact_quick(res)
        result["lowPressureRuns"].append(compact)
        save_json(result)
        log(f"低压回归 {idx + 1}/3 结束 ok={compact.get('ok')} avgFps={compact.get('avgFps')}")
        if not compact.get("ok"):
            result["finishedAt"] = datetime.now().isoformat(timespec="seconds")
            result["ok"] = False
            save_json(result)
            return 4

    result["finishedAt"] = datetime.now().isoformat(timespec="seconds")
    result["ok"] = True
    save_json(result)
    log("夜间 runner 阶段性链路完成：稳定性 -> 压力 -> 低压回归 全通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
