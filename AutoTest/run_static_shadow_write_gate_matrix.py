#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
静态阴影写入端五轮矩阵：
R1 观测 -> R2 owner-aware -> R3 补漏 -> R4 callback 兜底 -> R5 生产固化

每轮流程：
1) 修改配置
2) ninja -C build32
3) run_quick_autotest (2K 全屏)
4) sync_all_debug
5) 保存产物并汇总

支持失败回滚顺序：
1) 回滚 Round4 callback 黑名单
2) 回滚 Round3 unknown-owner 条件
3) 不回滚 Round2 owner-aware 主干
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

from war3_autotest_mcp import run_quick_autotest, sync_all_debug


ROOT = Path(__file__).resolve().parents[1]
CONFIG_H = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
ARTIFACT_ROOT = ROOT / "AutoTest" / "artifacts" / "static_shadow_write_gate_matrix"
RUN_DIR = ARTIFACT_ROOT / datetime.now().strftime("%Y%m%d_%H%M%S")
RUN_DIR.mkdir(parents=True, exist_ok=True)

DEFAULT_WAR3_DIR = os.getenv("WAR3_DIR", r"E:\Work\War3")
DEFAULT_MAP_PATH = os.getenv("WAR3_MAP_PATH", r"E:\Work\War3\Maps\光影测试.w3x")


@dataclass
class RoundSpec:
    idx: int
    name: str
    desc: str
    flags: Dict[str, Any]
    repeats: int = 1
    use_callback_blocklist: bool = False


def _env_flag(name: str, default: bool) -> bool:
    raw = os.getenv(name, "").strip().lower()
    if not raw:
        return default
    return raw in ("1", "true", "yes", "on", "y")


def _parse_callback_rvas(raw: str) -> List[int]:
    if not raw.strip():
        return []
    out: List[int] = []
    for token in re.split(r"[\s,;]+", raw.strip()):
        if not token:
            continue
        try:
            value = int(token, 0)
        except ValueError:
            continue
        if 0 < value <= 0xFFFFFFFF:
            out.append(value)
    # 去重并保序
    seen = set()
    dedup: List[int] = []
    for v in out:
        if v in seen:
            continue
        seen.add(v)
        dedup.append(v)
    return dedup


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


def _replace_callback_rva_array(text: str, rvas: List[int]) -> str:
    pattern = re.compile(
        r"(inline\s+constexpr\s+uint32_t\s+kNativeShadowBlockedCallbackRvas\[\]\s*=\s*\{)(.*?)(\};)",
        re.S,
    )

    def _sub(m: re.Match[str]) -> str:
        if rvas:
            body = "\n" + "".join(f"    0x{v:08X}u,\n" for v in rvas)
        else:
            body = "\n    // empty\n"
        return f"{m.group(1)}{body}{m.group(3)}"

    new_text, n = pattern.subn(_sub, text, count=1)
    if n == 0:
        raise RuntimeError("未找到数组: kNativeShadowBlockedCallbackRvas")
    return new_text


def _apply_config_flags(base_text: str, flags: Dict[str, Any], callback_rvas: List[int]) -> str:
    text = base_text
    for k, v in flags.items():
        text = _replace_inline_constant(text, k, v)
    text = _replace_callback_rva_array(text, callback_rvas)
    return text


def _run_ninja() -> Dict[str, Any]:
    p = subprocess.run(
        ["ninja", "-C", "build32"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
    )
    return {
        "ok": p.returncode == 0,
        "returncode": p.returncode,
        "stdout": p.stdout[-8000:],
        "stderr": p.stderr[-8000:],
    }


def _run_autotest() -> Dict[str, Any]:
    return run_quick_autotest(
        war3_dir=DEFAULT_WAR3_DIR,
        map_path=DEFAULT_MAP_PATH,
        ready_timeout_sec=180,
        sample_duration_sec=18,
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


def _run_sync_debug() -> Dict[str, Any]:
    return sync_all_debug(
        war3_dir=DEFAULT_WAR3_DIR,
        event_limit=400,
        include_dbwin_events=True,
        include_log_files=True,
        include_perf_reports=True,
        perf_report_count=1,
        tail_lines=240,
        contains="",
    )


def _compact_autotest_result(res: Dict[str, Any]) -> Dict[str, Any]:
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
        "cpuCoveragePct": report.get("cpuCoveragePct"),
        "jank16": report.get("jank16"),
        "jank33": report.get("jank33"),
        "screenshotSize": shot,
        "warnings": res.get("warnings", []),
    }


def _write_json(path: Path, data: Dict[str, Any]) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")


def _copy_round_artifacts(
    round_dir: Path,
    idx: int,
    run_idx: int,
    autotest_raw: Dict[str, Any],
    sync_debug_raw: Dict[str, Any],
) -> Dict[str, str]:
    paths: Dict[str, str] = {}

    sync_name = (
        f"sync_all_debug_round{idx}.json"
        if run_idx == 0
        else f"sync_all_debug_round{idx}_run{run_idx + 1}.json"
    )
    sync_path = round_dir / sync_name
    _write_json(sync_path, sync_debug_raw)
    paths["syncDebug"] = str(sync_path)

    shot_path = str(autotest_raw.get("screenshot", {}).get("output", "") or "")
    if shot_path:
        src = Path(shot_path)
        if src.exists():
            shot_name = (
                f"screenshot_round{idx}.png"
                if run_idx == 0
                else f"screenshot_round{idx}_run{run_idx + 1}.png"
            )
            dst = round_dir / shot_name
            shutil.copy2(src, dst)
            paths["screenshot"] = str(dst)

    report_path = str(autotest_raw.get("report", {}).get("reportPath", "") or "")
    perf_path_file = round_dir / "perf_report_path.txt"
    if run_idx == 0:
        perf_path_file.write_text(report_path + "\n", encoding="utf-8")
    else:
        with perf_path_file.open("a", encoding="utf-8") as f:
            f.write(report_path + "\n")
    paths["perfReportPathFile"] = str(perf_path_file)
    return paths


def _baseline_compare(base: Dict[str, Any], cur: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = {"hasBaseline": False}
    if not base or not cur:
        return out
    base_fps = base.get("avgFps")
    cur_fps = cur.get("avgFps")
    base_cpu = base.get("avgMainThreadCpuMs")
    cur_cpu = cur.get("avgMainThreadCpuMs")
    if not isinstance(base_fps, (int, float)) or not isinstance(cur_fps, (int, float)):
        return out
    out["hasBaseline"] = True
    out["fpsDeltaPct"] = ((float(cur_fps) - float(base_fps)) / max(0.001, float(base_fps))) * 100.0
    if isinstance(base_cpu, (int, float)) and isinstance(cur_cpu, (int, float)):
        out["mainThreadCpuDeltaMs"] = float(cur_cpu) - float(base_cpu)
    out["passFpsGuard"] = float(out["fpsDeltaPct"]) >= -3.0
    cpu_delta = float(out.get("mainThreadCpuDeltaMs", 0.0))
    out["passMainThreadCpuGuard"] = cpu_delta <= 0.2
    return out


def main() -> int:
    original = CONFIG_H.read_text(encoding="utf-8")
    current_text = original

    callback_rvas = _parse_callback_rvas(os.getenv("STATIC_SHADOW_BLOCK_CALLBACK_RVAS", ""))
    force_round4 = _env_flag("STATIC_SHADOW_FORCE_ROUND4", False)

    base_flags = {
        "kNativeShadowDefaultMode": 1,
        "kNativeShadowRegisterImageHookEnabled": True,
        "kNativeShadowRegisterImageStatsLogging": False,
        "kNativeShadowRegisterImageVerboseLogging": False,
        "kNativeShadowRegisterSourceStatsLogging": False,
        "kNativeShadowRegisterSourceVerboseLogging": False,
        "kNativeShadowListAHookEnabled": False,
        "kNativeShadowListBHookEnabled": False,
        "kNativeShadowUpdateWriteHookEnabled": False,
        "kNativeShadowUpdateStatsLogging": False,
        "kNativeShadowBlockUpdateByCallbackEnabled": False,
    }

    rounds: List[RoundSpec] = [
        RoundSpec(
            idx=1,
            name="R1_observability",
            desc="来源精确识别+统计，不改行为（strict=off）",
            flags={
                **base_flags,
                "kNativeShadowRegisterSourceStatsLogging": True,
                "kNativeShadowRegisterPolicyStrictMode1": False,
                "kNativeShadowRegisterOwnerKindFilterEnabled": False,
                "kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled": False,
            },
        ),
        RoundSpec(
            idx=2,
            name="R2_owner_aware_v1",
            desc="启用 owner-aware 主干（strict=on，unknown-owner 兜底暂关）",
            flags={
                **base_flags,
                "kNativeShadowRegisterSourceStatsLogging": True,
                "kNativeShadowRegisterPolicyStrictMode1": True,
                "kNativeShadowRegisterOwnerKindFilterEnabled": True,
                "kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled": False,
            },
        ),
        RoundSpec(
            idx=3,
            name="R3_source_fill_and_unknown_owner_gate",
            desc="补齐 unknown-owner(type+key) 兜底条件",
            flags={
                **base_flags,
                "kNativeShadowRegisterSourceStatsLogging": True,
                "kNativeShadowRegisterPolicyStrictMode1": True,
                "kNativeShadowRegisterOwnerKindFilterEnabled": True,
                "kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled": True,
            },
        ),
        RoundSpec(
            idx=4,
            name="R4_callback_fallback",
            desc="仅残留时启用 ShadowUpdate callback 黑名单兜底",
            flags={
                **base_flags,
                "kNativeShadowRegisterSourceStatsLogging": True,
                "kNativeShadowRegisterPolicyStrictMode1": True,
                "kNativeShadowRegisterOwnerKindFilterEnabled": True,
                "kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled": True,
                "kNativeShadowUpdateWriteHookEnabled": True,
                "kNativeShadowUpdateStatsLogging": True,
                "kNativeShadowBlockUpdateByCallbackEnabled": True,
            },
            use_callback_blocklist=True,
        ),
        RoundSpec(
            idx=5,
            name="R5_production_hardening",
            desc="关闭 verbose/stats，保留 owner-aware 主干并双轮复测",
            flags={
                **base_flags,
                "kNativeShadowRegisterPolicyStrictMode1": True,
                "kNativeShadowRegisterOwnerKindFilterEnabled": True,
                "kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled": True,
            },
            repeats=2,
        ),
    ]

    results: List[Dict[str, Any]] = []
    autotest_summary: List[Dict[str, Any]] = []
    baseline: Dict[str, Any] = {}

    try:
        for r in rounds:
            if r.idx == 4:
                if not callback_rvas and not force_round4:
                    skip_row = {
                        "idx": r.idx,
                        "name": r.name,
                        "desc": r.desc,
                        "skipped": True,
                        "skipReason": "未提供 callback 黑名单且未设置 STATIC_SHADOW_FORCE_ROUND4=1",
                    }
                    results.append(skip_row)
                    print(json.dumps({"round": r.name, "skipped": True}, ensure_ascii=False))
                    continue
                if r.use_callback_blocklist and not callback_rvas and force_round4:
                    # 强制执行 Round4 但无黑名单时，仅启用统计不启用按回调拦截。
                    r.flags["kNativeShadowBlockUpdateByCallbackEnabled"] = False

            callbacks = callback_rvas if r.use_callback_blocklist else []
            current_text = _apply_config_flags(current_text, r.flags, callbacks)
            CONFIG_H.write_text(current_text, encoding="utf-8")

            round_dir = RUN_DIR / f"round{r.idx}_{r.name}"
            round_dir.mkdir(parents=True, exist_ok=True)

            build = _run_ninja()
            row: Dict[str, Any] = {
                "idx": r.idx,
                "name": r.name,
                "desc": r.desc,
                "flags": r.flags,
                "callbacks": [f"0x{v:08X}" for v in callbacks],
                "build": build,
                "runs": [],
                "skipped": False,
            }

            if build["ok"]:
                for run_idx in range(max(1, int(r.repeats))):
                    autotest_raw = _run_autotest()
                    sync_raw = _run_sync_debug()
                    artifacts = _copy_round_artifacts(
                        round_dir,
                        r.idx,
                        run_idx,
                        autotest_raw,
                        sync_raw,
                    )
                    compact = _compact_autotest_result(autotest_raw)
                    compact["artifacts"] = artifacts
                    row["runs"].append(compact)

                if row["runs"]:
                    row["autotest"] = row["runs"][0]
                    row["autotestRepeat"] = row["runs"][1:] if len(row["runs"]) > 1 else []
                else:
                    row["autotest"] = {"ok": False, "stage": "no_run"}
                    row["autotestRepeat"] = []
            else:
                row["autotest"] = {"ok": False, "stage": "build_failed"}
                row["autotestRepeat"] = []

            # 失败回滚顺序：Round4 callback -> Round3 unknown-owner。
            rollback_actions: List[str] = []
            round_ok = bool(row.get("build", {}).get("ok")) and bool(
                row.get("autotest", {}).get("ok")
            )
            if not round_ok:
                rollback_text = CONFIG_H.read_text(encoding="utf-8")
                if r.idx >= 4:
                    rollback_text = _apply_config_flags(
                        rollback_text,
                        {
                            "kNativeShadowUpdateWriteHookEnabled": False,
                            "kNativeShadowBlockUpdateByCallbackEnabled": False,
                        },
                        [],
                    )
                    rollback_actions.append("rollback_round4_callback_blacklist")
                if r.idx >= 3:
                    rollback_text = _replace_inline_constant(
                        rollback_text,
                        "kNativeShadowRegisterUnknownOwnerTypeKeyBlockEnabled",
                        False,
                    )
                    rollback_actions.append("rollback_round3_unknown_owner_gate")
                if rollback_actions:
                    CONFIG_H.write_text(rollback_text, encoding="utf-8")
                    rollback_build = _run_ninja()
                    row["rollback"] = {
                        "actions": rollback_actions,
                        "build": rollback_build,
                    }
                    current_text = CONFIG_H.read_text(encoding="utf-8")

            if r.idx == 1 and row.get("autotest", {}).get("ok"):
                baseline = row["autotest"]

            row["baselineCompare"] = _baseline_compare(baseline, row.get("autotest", {}))
            results.append(row)

            autotest_summary.append(
                {
                    "idx": r.idx,
                    "name": r.name,
                    "ok": row.get("autotest", {}).get("ok", False),
                    "stage": row.get("autotest", {}).get("stage"),
                    "avgFps": row.get("autotest", {}).get("avgFps"),
                    "avgMainThreadCpuMs": row.get("autotest", {}).get("avgMainThreadCpuMs"),
                    "reportPath": row.get("autotest", {}).get("reportPath"),
                    "baselineCompare": row.get("baselineCompare", {}),
                    "rollbackActions": row.get("rollback", {}).get("actions", []),
                }
            )

            print(
                json.dumps(
                    {
                        "round": r.name,
                        "build_ok": build["ok"],
                        "autotest_ok": row.get("autotest", {}).get("ok", False),
                        "avgFps": row.get("autotest", {}).get("avgFps"),
                        "avgMainThreadCpuMs": row.get("autotest", {}).get("avgMainThreadCpuMs"),
                        "reportPath": row.get("autotest", {}).get("reportPath"),
                        "rollback": row.get("rollback", {}).get("actions", []),
                    },
                    ensure_ascii=False,
                )
            )
    finally:
        # 矩阵结束后恢复原始配置，避免无人值守期间残留中间态。
        CONFIG_H.write_text(original, encoding="utf-8")

    matrix_json = RUN_DIR / "matrix_results.json"
    summary_json = RUN_DIR / "autotest_summary.json"
    _write_json(matrix_json, {"runDir": str(RUN_DIR), "results": results})
    _write_json(summary_json, {"runDir": str(RUN_DIR), "summary": autotest_summary})

    print(f"[static-shadow] wrote {matrix_json}")
    print(f"[static-shadow] wrote {summary_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
