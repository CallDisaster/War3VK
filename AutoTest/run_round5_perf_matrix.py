#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
第五轮：性能与稳定性矩阵（60s）

目标：
1) 在不改渲染语义前提下筛选最优配置；
2) 每个组合都执行编译 + 60s AutoTest；
3) 记录崩溃/异常关键词，输出可回滚的最佳候选。
"""

from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List

from war3_autotest_mcp import run_quick_autotest, sync_all_debug


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
    if isinstance(value, bool):
        repl_value = "true" if value else "false"
    else:
        repl_value = str(value)

    pattern = re.compile(
        rf"(inline\s+constexpr\s+(?:bool|uint32_t|int32_t|int|float)\s+{re.escape(key)}\s*=\s*)([^;]+)(;)"
    )

    def _sub(m: re.Match[str]) -> str:
        return f"{m.group(1)}{repl_value}{m.group(3)}"

    new_text, n = pattern.subn(_sub, text, count=1)
    if n == 0:
        raise RuntimeError(f"未找到常量: {key}")
    return new_text


def apply_flags(flags: Dict[str, Any]) -> None:
    text = CONFIG_H.read_text(encoding="utf-8")
    for k, v in flags.items():
        text = _replace_inline_constant(text, k, v)
    CONFIG_H.write_text(text, encoding="utf-8")


def run_ninja() -> Dict[str, Any]:
    p = subprocess.run(
        ["ninja", "-C", "build32"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
    )
    return {
        "ok": p.returncode == 0,
        "returncode": p.returncode,
        "stdout": p.stdout[-5000:],
        "stderr": p.stderr[-5000:],
    }


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
        section_top_n=40,
        avoid_focus_on_stop=True,
    )


def collect_debug_signals() -> Dict[str, Any]:
    data = sync_all_debug(
        event_limit=200,
        include_dbwin_events=True,
        include_log_files=True,
        include_perf_reports=False,
        tail_lines=240,
        contains="",
    )
    text_parts: List[str] = []
    if isinstance(data, dict):
        for key in ("events", "logFiles"):
            val = data.get(key)
            if isinstance(val, list):
                for row in val:
                    text_parts.append(str(row))
            elif isinstance(val, dict):
                text_parts.append(json.dumps(val, ensure_ascii=False))
            elif val is not None:
                text_parts.append(str(val))
    full_text = "\n".join(text_parts).lower()
    keywords = [
        "fatal",
        "access_violation",
        "0xc0000005",
        "error 132",
        "assert",
        "device lost",
        "hung",
        "deadlock",
    ]
    hits = [k for k in keywords if k in full_text]
    return {"hits": hits, "rawSize": len(full_text)}


def compact_result(res: Dict[str, Any], debug_sig: Dict[str, Any]) -> Dict[str, Any]:
    report = res.get("report", {}) if isinstance(res, dict) else {}
    shot = res.get("screenshotSize", {}) if isinstance(res, dict) else {}
    return {
        "ok": bool(res.get("ok", False)),
        "stage": res.get("stage"),
        "reportPath": report.get("reportPath"),
        "avgFps": report.get("avgFps"),
        "avgFrameTimeMs": report.get("avgFrameTimeMs"),
        "avgGpuTimeMs": report.get("avgGpuTimeMs"),
        "avgMainThreadCpuMs": report.get("avgMainThreadCpuMs"),
        "avgProcessCpuMs": report.get("avgProcessCpuMs"),
        "activeFrameTimeMs": report.get("activeFrameTimeMs"),
        "avgTrackedActiveCpuMs": report.get("avgTrackedActiveCpuMs"),
        "avgUntrackedActiveCpuMs": report.get("avgUntrackedActiveCpuMs"),
        "cpuCoveragePct": report.get("cpuCoveragePct"),
        "jank16": report.get("jank16"),
        "jank33": report.get("jank33"),
        "screenshotSize": shot,
        "warnings": res.get("warnings", []),
        "debugKeywordHits": debug_sig.get("hits", []),
        "debugRawSize": debug_sig.get("rawSize", 0),
    }


def pick_best(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    ok_rows = [
        r
        for r in results
        if r.get("build", {}).get("ok")
        and r.get("autotest", {}).get("ok")
        and not r.get("autotest", {}).get("debugKeywordHits")
    ]
    if not ok_rows:
        return {"ok": False, "reason": "无可用稳定组合"}

    ok_rows.sort(
        key=lambda r: (
            float(r["autotest"].get("avgFps") or 0.0),
            -float(r["autotest"].get("avgFrameTimeMs") or 1e9),
        ),
        reverse=True,
    )
    best = ok_rows[0]
    return {
        "ok": True,
        "name": best.get("name"),
        "desc": best.get("desc"),
        "avgFps": best.get("autotest", {}).get("avgFps"),
        "avgFrameTimeMs": best.get("autotest", {}).get("avgFrameTimeMs"),
        "reportPath": best.get("autotest", {}).get("reportPath"),
        "flags": best.get("flags", {}),
    }


def main() -> int:
    original = CONFIG_H.read_text(encoding="utf-8")

    # 说明：
    # - C0 保留“当前配置”作为对照；
    # - C1~C8 在“性能模式（关闭 coverage analysis）”下比较策略差异。
    perf_base = {
        "kNativeMainLoopCoverageAnalysisMode": False,
        "kNativeQueueTakeoverEnabled": True,
        "kNativeQueueTakeoverConservativeEnabled": False,
        "kNativeDispatchLocalContextMergeEnabled": True,
        "kNativeDispatchTagStageCacheEnabled": True,
        "kNativeQueueTakeoverUseNativeTransparentFlush": True,
        "kShadowAdaptiveMapUpdateEnabled": True,
        "kNativeRenderQueueDiagnosticStatsEnabled": False,
    }

    combos: List[Combo] = [
        Combo(
            name="C0_current_analysis_on",
            desc="当前基线（分析模式 + 现有策略）",
            flags={
                "kNativeMainLoopCoverageAnalysisMode": True,
                "kNativeQueueTakeoverEnabled": True,
                "kNativeQueueTakeoverConservativeEnabled": False,
                "kNativeDispatchLocalContextMergeEnabled": True,
                "kNativeDispatchTagStageCacheEnabled": True,
                "kNativeQueueTakeoverUseNativeTransparentFlush": True,
                "kShadowAdaptiveMapUpdateEnabled": True,
                "kNativeRenderQueueDiagnosticStatsEnabled": False,
            },
        ),
        Combo(
            name="C1_perf_base_full_takeover",
            desc="性能模式 + Full Takeover（默认优化组）",
            flags={**perf_base},
        ),
        Combo(
            name="C2_perf_full_no_local_merge",
            desc="性能模式 + Full Takeover + 关闭 LocalMerge",
            flags={**perf_base, "kNativeDispatchLocalContextMergeEnabled": False},
        ),
        Combo(
            name="C3_perf_full_no_tag_cache",
            desc="性能模式 + Full Takeover + 关闭 TagStageCache",
            flags={**perf_base, "kNativeDispatchTagStageCacheEnabled": False},
        ),
        Combo(
            name="C4_perf_conservative_takeover",
            desc="性能模式 + Conservative Takeover",
            flags={
                **perf_base,
                "kNativeQueueTakeoverEnabled": False,
                "kNativeQueueTakeoverConservativeEnabled": True,
            },
        ),
        Combo(
            name="C5_perf_full_no_native_trans_flush",
            desc="性能模式 + Full Takeover + 透明队列不走原生 Flush",
            flags={**perf_base, "kNativeQueueTakeoverUseNativeTransparentFlush": False},
        ),
        Combo(
            name="C6_perf_full_no_skip_empty_flush",
            desc="性能模式 + Full Takeover + 不跳过空 Flush",
            flags={**perf_base, "kNativePatchSkipFlushWhenQueueEmpty": False},
        ),
        Combo(
            name="C7_perf_full_shadow_adaptive_off",
            desc="性能模式 + Full Takeover + 关闭 ShadowMap 自适应隔帧",
            flags={**perf_base, "kShadowAdaptiveMapUpdateEnabled": False},
        ),
        Combo(
            name="C8_perf_full_sort_check_wide",
            desc="性能模式 + Full Takeover + 扩大有序性预检上限",
            flags={**perf_base, "kNativeQueueSkipSortCheckMaxCount": 8192},
        ),
    ]

    results: List[Dict[str, Any]] = []
    try:
        for combo in combos:
            print(f"[round5] running {combo.name}: {combo.desc}")
            apply_flags(combo.flags)
            build = run_ninja()
            row: Dict[str, Any] = {
                "name": combo.name,
                "desc": combo.desc,
                "flags": combo.flags,
                "build": build,
            }
            if build["ok"]:
                run = run_autotest()
                debug_sig = collect_debug_signals()
                row["autotest"] = compact_result(run, debug_sig)
            else:
                row["autotest"] = {
                    "ok": False,
                    "stage": "build_failed",
                    "debugKeywordHits": [],
                }
            results.append(row)
            print(
                json.dumps(
                    {
                        "name": combo.name,
                        "build_ok": build["ok"],
                        "autotest_ok": row["autotest"].get("ok", False),
                        "avgFps": row["autotest"].get("avgFps"),
                        "avgFrameTimeMs": row["autotest"].get("avgFrameTimeMs"),
                        "reportPath": row["autotest"].get("reportPath"),
                        "debugHits": row["autotest"].get("debugKeywordHits"),
                    },
                    ensure_ascii=False,
                )
            )
    finally:
        # 默认恢复到最佳候选；若无候选则恢复原始文本。
        best = pick_best(results)
        if best.get("ok"):
            print(f"[round5] apply best flags: {best.get('name')}")
            apply_flags(best.get("flags", {}))
        else:
            CONFIG_H.write_text(original, encoding="utf-8")

    summary = {
        "results": results,
        "best": pick_best(results),
        "targetFps": 150.0,
        "hitTarget": (
            pick_best(results).get("ok")
            and float(pick_best(results).get("avgFps") or 0.0) >= 150.0
        ),
    }

    out_json = OUT_DIR / "round5_matrix_results.json"
    out_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[round5] wrote {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

