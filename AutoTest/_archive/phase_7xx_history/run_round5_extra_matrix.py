#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
第五轮补充：性能上限探索（60s）
用于判断 150 FPS 目标在当前实现下是否可达。
"""

from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List

from war3_autotest_mcp import run_quick_autotest


ROOT = Path(__file__).resolve().parents[1]
CONFIG_H = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
OUT_DIR = ROOT / "AutoTest" / "artifacts" / "round5_matrix"
OUT_DIR.mkdir(parents=True, exist_ok=True)


@dataclass
class Combo:
    name: str
    desc: str
    flags: Dict[str, Any]


def _replace_inline_constant(text: str, key: str, value: Any) -> str:
    repl_value = ("true" if value else "false") if isinstance(value, bool) else str(value)
    pattern = re.compile(
        rf"(inline\s+constexpr\s+(?:bool|uint32_t|int32_t|int|float)\s+{re.escape(key)}\s*=\s*)([^;]+)(;)"
    )
    new_text, n = pattern.subn(lambda m: f"{m.group(1)}{repl_value}{m.group(3)}", text, count=1)
    if n == 0:
        raise RuntimeError(f"未找到常量: {key}")
    return new_text


def apply_flags(flags: Dict[str, Any]) -> None:
    text = CONFIG_H.read_text(encoding="utf-8")
    for k, v in flags.items():
        text = _replace_inline_constant(text, k, v)
    CONFIG_H.write_text(text, encoding="utf-8")


def run_ninja() -> Dict[str, Any]:
    p = subprocess.run(["ninja", "-C", "build32"], cwd=str(ROOT), capture_output=True, text=True)
    return {"ok": p.returncode == 0, "returncode": p.returncode}


def run_autotest() -> Dict[str, Any]:
    return run_quick_autotest(
        war3_dir=r"E:\Work\War3",
        map_path=r"E:\Work\War3\Maps\光影测试.w3x",
        ready_timeout_sec=180,
        sample_duration_sec=60,
        windowed=False,
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
        section_top_n=30,
        avoid_focus_on_stop=True,
    )


def compact_result(res: Dict[str, Any]) -> Dict[str, Any]:
    report = res.get("report", {}) if isinstance(res, dict) else {}
    return {
        "ok": bool(res.get("ok", False)),
        "stage": res.get("stage"),
        "reportPath": report.get("reportPath"),
        "avgFps": report.get("avgFps"),
        "avgFrameTimeMs": report.get("avgFrameTimeMs"),
        "avgGpuTimeMs": report.get("avgGpuTimeMs"),
        "avgMainThreadCpuMs": report.get("avgMainThreadCpuMs"),
        "avgProcessCpuMs": report.get("avgProcessCpuMs"),
        "warnings": res.get("warnings", []),
    }


def main() -> int:
    original = CONFIG_H.read_text(encoding="utf-8")

    base = {
        "kNativeMainLoopCoverageAnalysisMode": False,
        "kNativeQueueTakeoverEnabled": True,
        "kNativeQueueTakeoverConservativeEnabled": False,
        "kNativeDispatchLocalContextMergeEnabled": False,
        "kNativeDispatchTagStageCacheEnabled": True,
        "kNativeQueueTakeoverUseNativeTransparentFlush": True,
        "kShadowAdaptiveMapUpdateEnabled": True,
        "kNativeRenderQueueDiagnosticStatsEnabled": False,
    }

    combos: List[Combo] = [
        Combo("E0_best_so_far", "当前最佳基线（性能模式）", {**base}),
        Combo(
            "E1_disable_shadow_capture_mode1",
            "关闭 mode1 ShadowCapture（上限探索）",
            {**base, "kNativeShadowDisableShadowCaptureWhenMode1": True},
        ),
        Combo(
            "E2_disable_shadow_receiver_outline_mode1",
            "关闭 mode1 War3ShadowReceiver + Outline（上限探索）",
            {
                **base,
                "kNativeShadowDisableShadowCaptureWhenMode1": True,
                "kNativeShadowDisableWar3ShadowReceiverWhenMode1": True,
                "kNativeShadowDisableOutlineWhenMode1": True,
            },
        ),
        Combo(
            "E3_native_shadow_mode0",
            "切回 NativeShadowMode=0（上限探索）",
            {**base, "kNativeShadowDefaultMode": 0},
        ),
    ]

    results: List[Dict[str, Any]] = []
    try:
        for c in combos:
            print(f"[round5-extra] {c.name} {c.desc}")
            apply_flags(c.flags)
            build = run_ninja()
            row = {"name": c.name, "desc": c.desc, "flags": c.flags, "build": build}
            if build["ok"]:
                row["autotest"] = compact_result(run_autotest())
            else:
                row["autotest"] = {"ok": False, "stage": "build_failed"}
            results.append(row)
            print(json.dumps({
                "name": c.name,
                "build_ok": build["ok"],
                "autotest_ok": row["autotest"].get("ok"),
                "avgFps": row["autotest"].get("avgFps"),
                "reportPath": row["autotest"].get("reportPath"),
            }, ensure_ascii=False))
    finally:
        # 不在这里决定最佳落盘，统一由主流程回写。
        CONFIG_H.write_text(original, encoding="utf-8")

    out = OUT_DIR / "round5_extra_matrix_results.json"
    out.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[round5-extra] wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

