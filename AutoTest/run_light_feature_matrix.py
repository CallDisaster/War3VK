#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""点光 / 点阴影 / 体积光特性矩阵 + 双图基线护栏。

灯光 / 软件光追验收矩阵：
  1) 默认发行档 dual_perf（高压 ≥85 / 低压 ≥120）
  2) 低压地图上跑 11 组 light/volumetric/ray 配置
     （含 512/1024/2048 点阴影与软件光追 A0/A1）
  3) JSON 落盘 AutoTest/artifacts/light_feature_matrix_<ts>.json
  4) 同步写 latest_light_baseline.json（仅 baseline 配置）

用法（Windows 前台，不要 isolated desktop）::

    py AutoTest\\run_light_feature_matrix.py
    py AutoTest\\run_light_feature_matrix.py --skip-dual
    py AutoTest\\run_light_feature_matrix.py --only volumetric_only

环境变量通过 run_quick_autotest(env_overrides_json=...) 注入，不改源码默认。
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import (  # noqa: E402
    DEFAULT_WAR3_DIR,
    STATE,
    run_quick_autotest,
)

HIGH_MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"
LOW_MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x"
ARTIFACTS = Path(__file__).resolve().parent / "artifacts"
WAR3_DIR = Path(DEFAULT_WAR3_DIR)
CRASH_DIR = WAR3_DIR / "WarVK" / "Crash"
DBWIN_BUFFER_LIMIT = 4000
DBWIN_RETAIN_PATTERNS = (
    "DXVK PointShadow:",
    "DXVK War3Volumetric:",
    "DXVK War3HybridRay:",
    "RuntimeBenchmark",
    "exception",
    "crash",
    "fatal",
    "device lost",
)

# 特性矩阵：全部在低压地图跑，避免高压场景把“灯成本”和“地图压力”缠在一起。
# 点光配置依赖 DXVK_WAR3_TEST_POINT_LIGHT 注入测试灯。
FEATURE_CASES: List[Dict[str, Any]] = [
    {
        "name": "baseline",
        "desc": "CSM on, 点光/体积光全关",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "0",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "0",
        },
        "fps_vs_baseline_min": 0.0,  # 自身即基线
        "main_thread_delta_max_ms": 0.5,
    },
    {
        "name": "volumetric_only",
        "desc": "体积光产品安全档 divisor=4 samples=16",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "0",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "1",
            "DXVK_WAR3_VOLUMETRIC_RES_DIVISOR": "4",
            "DXVK_WAR3_VOLUMETRIC_SAMPLES": "16",
            "DXVK_WAR3_TEST_POINT_LIGHT": "0",
        },
        "fps_vs_baseline_min": 0.92,
        "main_thread_delta_max_ms": 0.4,
    },
    {
        "name": "volumetric_max_quality",
        "desc": "体积光上帝视角阴影柱可见性门 divisor=4 samples=16",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "0",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "1",
            "DXVK_WAR3_VOLUMETRIC_INTENSITY": "1.5",
            "DXVK_WAR3_VOLUMETRIC_DENSITY": "2.0",
            "DXVK_WAR3_VOLUMETRIC_WEIGHT": "2.25",
            "DXVK_WAR3_VOLUMETRIC_MAX_RAY": "1.0",
            "DXVK_WAR3_VOLUMETRIC_HEIGHT_FOG": "0.50",
            # 这是上帝视角阴影柱的相干可见性 preset，不是把每个滑杆
            # 拉到最大。height=2 会把介质压到地表，fadeFar=1 会让短的
            # 俯视射线更晚显现；contrast exponent 则由 weight 直接控制。
            "DXVK_WAR3_VOLUMETRIC_FADE_NEAR": "0.0",
            "DXVK_WAR3_VOLUMETRIC_FADE_FAR": "0.55",
            "DXVK_WAR3_VOLUMETRIC_EXTINCTION": "0.08",
            "DXVK_WAR3_VOLUMETRIC_UNSHADOWED": "0.35",
            "DXVK_WAR3_VOLUMETRIC_RES_DIVISOR": "4",
            "DXVK_WAR3_VOLUMETRIC_SAMPLES": "16",
            "DXVK_WAR3_TEST_POINT_LIGHT": "0",
        },
        "fps_vs_baseline_min": 0.82,
        "main_thread_delta_max_ms": 0.8,
    },
    {
        "name": "point_lights_only",
        "desc": "1 盏测试点光，无 cube 阴影",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.95,
        "main_thread_delta_max_ms": 0.4,
    },
    {
        "name": "point_lights_shadow_perf_512",
        "desc": "1 盏点光 + cube shadow 512 / 每帧六面（低成本性能对照）",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "1",
            "DXVK_WAR3_POINT_SHADOW_RESOLUTION": "512",
            "DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW_MAX_FACES": "6",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.85,
        "main_thread_delta_max_ms": 1.0,
    },
    {
        "name": "point_lights_shadow",
        "desc": "1 盏点光 + cube shadow 1024 / 每帧六面（产品默认正确性）",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "1",
            "DXVK_WAR3_POINT_SHADOW_RESOLUTION": "1024",
            "DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW_MAX_FACES": "6",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.82,
        "main_thread_delta_max_ms": 1.0,
    },
    {
        "name": "point_lights_shadow_ultra_2048",
        "desc": "1 盏点光 + cube shadow 2048 / 每帧六面（Ultra 质量门）",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "1",
            "DXVK_WAR3_POINT_SHADOW_RESOLUTION": "2048",
            "DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW_MAX_FACES": "6",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.75,
        "main_thread_delta_max_ms": 1.0,
    },
    {
        "name": "point_ray_a0_linear",
        "desc": "1 盏点光 + 软件光追 A0 线性短射线，关闭 cube/Hi-Z",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_POINT_RAY_SHADOW": "1",
            "DXVK_WAR3_POINT_RAY_SHADOW_HIZ": "0",
            "DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_RAY_SHADOW_STEPS": "12",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.88,
        "main_thread_delta_max_ms": 0.5,
    },
    {
        "name": "point_ray_a1_hiz",
        "desc": "1 盏点光 + 软件光追 A1 半分辨率 Hi-Z，关闭 cube",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_POINT_RAY_SHADOW": "1",
            "DXVK_WAR3_POINT_RAY_SHADOW_HIZ": "1",
            "DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_RAY_SHADOW_HIZ_VISITS": "24",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.88,
        "main_thread_delta_max_ms": 0.5,
    },
    {
        "name": "volumetric_plus_point",
        "desc": "体积光 + 点光散射，无 point cube",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "1",
            "DXVK_WAR3_VOLUMETRIC_RES_DIVISOR": "4",
            "DXVK_WAR3_VOLUMETRIC_SAMPLES": "16",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.90,
        "main_thread_delta_max_ms": 0.6,
    },
    {
        "name": "volumetric_plus_point_shadow",
        "desc": "体积光 + 点光散射 + 完整 cube 遮挡（防穿墙）",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "1",
            "DXVK_WAR3_POINT_SHADOW_RESOLUTION": "1024",
            "DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW_MAX_FACES": "6",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "1",
            "DXVK_WAR3_VOLUMETRIC_RES_DIVISOR": "4",
            "DXVK_WAR3_VOLUMETRIC_SAMPLES": "16",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
        },
        "fps_vs_baseline_min": 0.78,
        "main_thread_delta_max_ms": 1.2,
    },
]


def _perf(r: Dict[str, Any]) -> Dict[str, Any]:
    rep = r.get("report") or {}
    return {
        "ok": bool(r.get("ok")),
        "stage": r.get("stage"),
        "avgFps": float(rep.get("avgFps") or 0.0),
        "avgFrameTimeMs": float(rep.get("avgFrameTimeMs") or 0.0),
        "avgGpuTimeMs": float(rep.get("avgGpuTimeMs") or 0.0),
        "avgMainThreadCpuMs": float(rep.get("avgMainThreadCpuMs") or 0.0),
        "avgProcessCpuMs": float(rep.get("avgProcessCpuMs") or 0.0),
        "frameCount": int(rep.get("frameCount") or 0),
        "reportPath": (
            rep.get("reportPath")
            or rep.get("path")
            or rep.get("latestReportPath")
            or r.get("reportPath")
        ),
        "screenshot": r.get("screenshot"),
    }


def _int_or_zero(value: Any) -> int:
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _control_plane_pipe_pid(screenshot: Dict[str, Any]) -> tuple[str, int]:
    details = screenshot.get("details") if isinstance(screenshot, dict) else {}
    if not isinstance(details, dict):
        details = {}
    pipe_name = str(details.get("pipeName", "") or "")
    match = re.search(r"War3ControlPlane_(\d+)$", pipe_name)
    return pipe_name, _int_or_zero(match.group(1) if match else 0)


def _exact_pid_crash_artifacts(pid: int, started_epoch: float) -> List[Dict[str, Any]]:
    if pid <= 0 or not CRASH_DIR.is_dir():
        return []
    pid_pattern = re.compile(rf"(?:^|[_-])pid{pid}(?:[_\-.]|$)", re.IGNORECASE)
    rows: List[Dict[str, Any]] = []
    for path in CRASH_DIR.iterdir():
        try:
            stat = path.stat()
        except OSError:
            continue
        if not path.is_file() or not pid_pattern.search(path.name):
            continue
        if stat.st_mtime + 1.0 < float(started_epoch):
            continue
        rows.append(
            {
                "path": str(path),
                "size": int(stat.st_size),
                "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(),
            }
        )
    return sorted(rows, key=lambda row: str(row["path"]))


def _report_section_rows(report: Dict[str, Any]) -> List[Dict[str, Any]]:
    breakdown = report.get("sectionBreakdown") if isinstance(report, dict) else {}
    if not isinstance(breakdown, dict):
        return []
    dedup: Dict[str, Dict[str, Any]] = {}
    for key in ("topBySelfCpu", "topByInclusiveCpu", "topByGpu"):
        rows = breakdown.get(key, [])
        if not isinstance(rows, list):
            continue
        for row in rows:
            if not isinstance(row, dict):
                continue
            path = str(row.get("path", "") or "")
            name = str(row.get("name", "") or "")
            identity = path or name
            if not identity:
                continue
            previous = dedup.get(identity)
            if previous is None or _int_or_zero(row.get("calls")) > _int_or_zero(previous.get("calls")):
                dedup[identity] = {
                    "name": name,
                    "path": path,
                    "calls": _int_or_zero(row.get("calls")),
                    "callsPerFrame": float(row.get("callsPerFrame", 0.0) or 0.0),
                    "avgCpuMs": float(row.get("avgCpuMs", 0.0) or 0.0),
                    "avgSelfCpuMs": float(row.get("avgSelfCpuMs", 0.0) or 0.0),
                    "avgGpuMs": float(row.get("avgGpuMs", 0.0) or 0.0),
                }
    return list(dedup.values())


def _section_matches(rows: List[Dict[str, Any]], name: str) -> List[Dict[str, Any]]:
    needle = name.casefold()
    return [
        row
        for row in rows
        if _int_or_zero(row.get("calls")) > 0
        and (
            str(row.get("name", "") or "").casefold() == needle
            or str(row.get("path", "") or "").casefold().endswith("/" + needle)
        )
    ]


def _case_evidence(
    raw: Dict[str, Any],
    *,
    case_name: str,
    started_epoch: float,
) -> Dict[str, Any]:
    launch = raw.get("launch") if isinstance(raw.get("launch"), dict) else {}
    ready = raw.get("ready") if isinstance(raw.get("ready"), dict) else {}
    stop = raw.get("stop") if isinstance(raw.get("stop"), dict) else {}
    screenshot = (
        raw.get("screenshot") if isinstance(raw.get("screenshot"), dict) else {}
    )
    report = raw.get("report") if isinstance(raw.get("report"), dict) else {}

    launch_pid = _int_or_zero(launch.get("pid"))
    ready_pid = _int_or_zero(ready.get("pid"))
    stop_pid = _int_or_zero(stop.get("pid"))
    pipe_name, pipe_pid = _control_plane_pipe_pid(screenshot)

    # launch_war3_test clears STATE.debug_events immediately before starting the
    # process. Capture before the next case can clear it, then retain only rows
    # whose DBWIN producer PID exactly equals this launch PID. Foreign War3 or
    # World Editor events are counted as excluded and are never serialized.
    with STATE.debug_lock:
        source_sequence = int(STATE.debug_seq)
        buffered_events = list(STATE.debug_events[-DBWIN_BUFFER_LIMIT:])
    exact_pid_events = [
        row for row in buffered_events if _int_or_zero(row.get("pid")) == launch_pid
    ]
    retained_events = [
        row
        for row in exact_pid_events
        if any(
            pattern.casefold() in str(row.get("msg", "") or "").casefold()
            for pattern in DBWIN_RETAIN_PATTERNS
        )
    ]
    point_rendered_events = [
        row
        for row in exact_pid_events
        if "DXVK PointShadow: Rendered!" in str(row.get("msg", "") or "")
    ]
    hybrid_roi_events = [
        row
        for row in exact_pid_events
        if (
            (match := re.search(
                r"DXVK War3HybridRay: contact ROI scheduled=(\d+)",
                str(row.get("msg", "") or ""),
            ))
            and int(match.group(1)) > 0
        )
    ]
    hybrid_a0_events = [
        row
        for row in exact_pid_events
        if "DXVK War3HybridRay: A0 receiver submitted"
        in str(row.get("msg", "") or "")
    ]
    volumetric_submit_events = [
        row
        for row in exact_pid_events
        if "DXVK War3Volumetric: composite submitted"
        in str(row.get("msg", "") or "")
    ]
    volumetric_shadow_submit_events = [
        row
        for row in volumetric_submit_events
        if (
            (match := re.search(
                r"pointShadows=(\d+)", str(row.get("msg", "") or "")
            ))
            and int(match.group(1)) > 0
        )
    ]

    report_path = str(
        report.get("reportPath")
        or report.get("path")
        or report.get("latestReportPath")
        or raw.get("reportPath")
        or ""
    )
    report_file_mtime = 0.0
    if report_path:
        try:
            report_file_mtime = Path(report_path).stat().st_mtime
        except OSError:
            report_file_mtime = 0.0
    report_file_within_case = bool(
        report_file_mtime > 0.0
        and report_file_mtime + 1.0 >= float(started_epoch)
    )
    new_perf_report = bool(
        report.get("reportType") == "perf_report"
        and report.get("newReportDetected")
        and not report.get("reportWasStale")
        and report_path
        and report_file_within_case
    )
    section_rows = _report_section_rows(report) if new_perf_report else []
    point_sections = _section_matches(section_rows, "PointShadow")
    volumetric_sections = _section_matches(section_rows, "VolumetricLight")
    point_ray_hiz_sections = _section_matches(section_rows, "PointRayHiZ")
    active_point_sections = [
        row for row in point_sections if float(row.get("avgGpuMs", 0.0) or 0.0) > 0.0
    ]
    active_volumetric_sections = [
        row
        for row in volumetric_sections
        if float(row.get("avgGpuMs", 0.0) or 0.0) > 0.0
    ]
    active_point_ray_hiz_sections = [
        row
        for row in point_ray_hiz_sections
        if float(row.get("avgGpuMs", 0.0) or 0.0) > 0.0
    ]

    execution_required = case_name.startswith(
        ("point_lights_shadow", "volumetric", "point_ray")
    )
    execution_sources: List[str] = []
    required_execution_source_count = 1
    if case_name == "volumetric_plus_point_shadow":
        # This gate proves both producers: a sampleable six-face cube and the
        # later volume effect+composite submission in the same exact process.
        required_execution_source_count = 2
        if point_rendered_events:
            execution_sources.append("exact-pid-dbwin:DXVK PointShadow: Rendered!")
        elif active_point_sections:
            execution_sources.append(
                "current-report-gpu-section:PointShadow"
            )
        if volumetric_shadow_submit_events:
            execution_sources.append(
                "exact-pid-dbwin:DXVK War3Volumetric composite "
                "submitted(pointShadows>0)"
            )
        elif active_volumetric_sections:
            execution_sources.append(
                "current-report-gpu-section:VolumetricLight"
            )
    elif case_name.startswith("point_lights_shadow"):
        # Newer production builds removed the per-frame DBWIN marker. A GPU
        # section from the fresh, case-bounded report is equivalent execution
        # evidence: the section must have calls and non-zero GPU time, so a
        # settings echo or a CPU-only early-return cannot satisfy this gate.
        if point_rendered_events:
            execution_sources.append("exact-pid-dbwin:DXVK PointShadow: Rendered!")
        elif active_point_sections:
            execution_sources.append(
                "current-report-gpu-section:PointShadow"
            )
    elif case_name.startswith("volumetric"):
        # Settings logs and the CPU-only pipeline section are not execution
        # evidence. Require the marker emitted after effect + composite draws.
        if volumetric_submit_events:
            execution_sources.append(
                "exact-pid-dbwin:DXVK War3Volumetric composite submitted"
            )
    elif case_name == "point_ray_a1_hiz":
        # PointRayHiZ/Perf begins before resource/ROI early returns. A strictly
        # positive scheduled texel count proves at least one compute dispatch.
        if hybrid_roi_events:
            execution_sources.append(
                "exact-pid-dbwin:DXVK War3HybridRay contact ROI scheduled>0"
            )
    elif case_name == "point_ray_a0_linear":
        # A0 executes inside the receiver shader and has no separate GPU
        # section. Count only the post-draw marker emitted after the receiver
        # was actually submitted; environment-setting echoes are not evidence.
        if hybrid_a0_events:
            execution_sources.append(
                "exact-pid-dbwin:DXVK War3HybridRay A0 receiver submitted"
            )

    crash_artifacts = _exact_pid_crash_artifacts(launch_pid, started_epoch)
    pre_stop_already_exited = str(stop.get("message", "") or "") == "进程已不在"
    forced_stop = bool(stop.get("forced"))
    pid_consistency = bool(
        launch_pid > 0
        and ready_pid == launch_pid
        and stop_pid == launch_pid
        and pipe_pid == launch_pid
    )
    log_summary = raw.get("logSummary") or report.get("logSummary") or {}
    keyword_counts = (
        log_summary.get("keywordCounts", {})
        if isinstance(log_summary, dict)
        else {}
    )
    device_lost_count = _int_or_zero(keyword_counts.get("deviceLost"))
    runtime_process_failure = bool(
        launch_pid <= 0
        or not ready.get("ok")
        or not screenshot.get("ok")
        or not stop.get("ok")
        or not stop.get("stopped")
        or not pid_consistency
        or pre_stop_already_exited
        or crash_artifacts
        or device_lost_count > 0
    )

    return {
        "source": "run_quick_autotest raw result + exact-launch-PID DBWIN",
        "caseStartedAt": datetime.fromtimestamp(started_epoch).isoformat(),
        "launchPid": launch_pid,
        "readyPid": ready_pid,
        "readyOk": bool(ready.get("ok")),
        "readyMode": str(ready.get("mode", "") or ""),
        "stopPid": stop_pid,
        "stopOk": bool(stop.get("ok")),
        "stopConfirmed": bool(stop.get("stopped")),
        "forcedStop": forced_stop,
        "preStopAlreadyExited": pre_stop_already_exited,
        "screenshotOk": bool(screenshot.get("ok")),
        "controlPlanePipe": pipe_name,
        "controlPlanePipePid": pipe_pid,
        "pidConsistency": pid_consistency,
        "runtimeProcessFailure": runtime_process_failure,
        "exactPidCrashArtifacts": crash_artifacts,
        "dbwin": {
            "source": "war3_autotest_mcp.STATE OutputDebugString buffer",
            "filter": "event.pid == launchPid (strict integer equality)",
            "bufferLimit": DBWIN_BUFFER_LIMIT,
            "sourceSequence": source_sequence,
            "bufferRowsObserved": len(buffered_events),
            "sourceMayBeTruncated": source_sequence > len(buffered_events),
            "exactPidRowsObserved": len(exact_pid_events),
            "foreignPidRowsExcluded": len(buffered_events) - len(exact_pid_events),
            "retainedEventPolicy": list(DBWIN_RETAIN_PATTERNS),
            "retainedExactPidRows": retained_events,
        },
        "report": {
            "reportType": str(report.get("reportType", "") or ""),
            "reportPath": report_path or None,
            "reportFileMtime": (
                datetime.fromtimestamp(report_file_mtime).isoformat()
                if report_file_mtime > 0.0
                else None
            ),
            "reportFileWithinCase": report_file_within_case,
            "pidAttribution": (
                "report is case-time-bounded but has no embedded PID; "
                "flow gate separately requires exact launch/ready/pipe/stop PID"
            ),
            "newReportDetected": bool(report.get("newReportDetected")),
            "reportWasStale": bool(report.get("reportWasStale")),
            "benchmarkFallback": bool(report.get("benchmarkFallback")),
            "pointShadowSections": point_sections,
            "activePointShadowSections": active_point_sections,
            "volumetricLightSections": volumetric_sections,
            "activeVolumetricLightSections": active_volumetric_sections,
            "pointRayHiZSections": point_ray_hiz_sections,
            "activePointRayHiZSections": active_point_ray_hiz_sections,
        },
        "controlledForcedStop": bool(
            forced_stop and stop.get("ok") and stop.get("stopped")
        ),
        "deviceLostCount": device_lost_count,
        "logSummary": log_summary,
        "executionRequired": execution_required,
        "executionRequiredSourceCount": (
            required_execution_source_count if execution_required else 0
        ),
        "executionObserved": (
            len(execution_sources) >= required_execution_source_count
            if execution_required
            else None
        ),
        "executionEvidenceSource": "; ".join(execution_sources) if execution_sources else (
            "not-required" if not execution_required else "none"
        ),
        "settingsLogsCountAsExecution": False,
    }


def _run_case(
    *,
    map_path: str,
    sample_sec: int,
    env: Dict[str, str],
    deploy: bool,
    isolated: bool,
) -> Dict[str, Any]:
    return run_quick_autotest(
        war3_dir=str(WAR3_DIR),
        map_path=map_path,
        sample_duration_sec=sample_sec,
        use_isolated_desktop=isolated,
        windowed=isolated,
        deploy_d3d9_before_launch=deploy,
        env_overrides_json=json.dumps(env, ensure_ascii=False),
        enforce_video_baseline=True,
        include_sections_in_report=True,
        section_top_n=200,
    )


def main() -> int:
    global WAR3_DIR
    ap = argparse.ArgumentParser(description="Light/volumetric feature matrix")
    ap.add_argument(
        "--war3-dir",
        default=str(WAR3_DIR),
        help="Warcraft III runtime directory (default: AutoTest sandbox)",
    )
    ap.add_argument("--skip-dual", action="store_true", help="跳过双图基线")
    ap.add_argument("--only", default="", help="只跑指定特性 case 名")
    ap.add_argument("--sample-sec", type=int, default=30)
    ap.add_argument(
        "--isolated", action="store_true",
        help=("隔离桌面画质/稳定性取证；自动跳过 dual_perf，"
              "且绝不使用隔离 FPS 判性能"),
    )
    ap.add_argument(
        "--deploy-once",
        action="store_true",
        default=True,
        help="仅第一轮部署 d3d9.dll（默认）",
    )
    args = ap.parse_args()
    WAR3_DIR = Path(args.war3_dir).resolve()
    if args.isolated:
        args.skip_dual = True

    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_path = ARTIFACTS / f"light_feature_matrix_{ts}.json"

    result: Dict[str, Any] = {
        "timestamp": ts,
        "sampleSec": args.sample_sec,
        "isolatedDesktop": bool(args.isolated),
        "performanceJudgement": not bool(args.isolated),
        "war3Dir": str(WAR3_DIR),
        "dual": {},
        "featureCases": [],
        "gates": {},
    }

    deploy_next = True
    dual_pass = True

    if not args.skip_dual:
        print("\n========== DUAL PERF BASELINE ==========")
        for label, map_path, gate in (
            ("high", HIGH_MAP, 85.0),
            ("low", LOW_MAP, 120.0),
        ):
            print(f"--- {label}: {map_path}")
            case_started_epoch = time.time()
            r = _run_case(
                map_path=map_path,
                sample_sec=args.sample_sec,
                env={
                    "DXVK_WAR3_POINT_LIGHTS": "0",
                    "DXVK_WAR3_POINT_SHADOW": "0",
                    "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
                    "DXVK_WAR3_TEST_POINT_LIGHT": "0",
                },
                deploy=deploy_next,
                isolated=False,
            )
            deploy_next = False
            p = _perf(r)
            evidence = _case_evidence(
                r,
                case_name=f"dual_{label}",
                started_epoch=case_started_epoch,
            )
            p["evidence"] = evidence
            p["flowPass"] = bool(not evidence["runtimeProcessFailure"])
            p["gateFps"] = gate
            p["pass"] = bool(p["ok"] and p["flowPass"] and p["avgFps"] >= gate)
            dual_pass = dual_pass and p["pass"]
            result["dual"][label] = p
            print(
                f"  ok={p['ok']} avgFps={p['avgFps']:.3f} "
                f"mainCpu={p['avgMainThreadCpuMs']:.3f} "
                f"gate>={gate}: {'PASS' if p['pass'] else 'FAIL'}"
            )
            time.sleep(1.0)

    only = (args.only or "").strip()
    cases = FEATURE_CASES
    if only:
        cases = [c for c in FEATURE_CASES if c["name"] == only]
        if not cases:
            print(f"未知 case: {only}")
            return 2

    print("\n========== FEATURE MATRIX (low map) ==========")
    baseline_fps: Optional[float] = None
    baseline_main: Optional[float] = None
    matrix_pass = True

    for case in cases:
        name = case["name"]
        print(f"--- {name}: {case['desc']}")
        case_started_epoch = time.time()
        r = _run_case(
            map_path=LOW_MAP,
            sample_sec=args.sample_sec,
            env=case["env"],
            deploy=deploy_next,
            isolated=bool(args.isolated),
        )
        deploy_next = False
        p = _perf(r)
        evidence = _case_evidence(
            r,
            case_name=name,
            started_epoch=case_started_epoch,
        )
        flow_pass = bool(not evidence["runtimeProcessFailure"])
        entry = {
            "name": name,
            "desc": case["desc"],
            "env": case["env"],
            "metrics": p,
            "evidence": evidence,
            "flowPass": flow_pass,
            "executionRequired": evidence["executionRequired"],
            "executionObserved": evidence["executionObserved"],
            "executionEvidenceSource": evidence["executionEvidenceSource"],
            "fpsVsBaselineMin": case["fps_vs_baseline_min"],
            "mainThreadDeltaMaxMs": case["main_thread_delta_max_ms"],
        }

        if args.isolated:
            entry["qualityOnly"] = True
            entry["performanceGateSkipped"] = (
                "isolated desktop FPS is not a release metric"
            )
            base_pass = flow_pass
        elif name == "baseline":
            baseline_fps = p["avgFps"] if p["ok"] else None
            baseline_main = p["avgMainThreadCpuMs"] if p["ok"] else None
            base_pass = bool(p["ok"] and flow_pass and (baseline_fps or 0) > 0)
        else:
            ok_ratio = True
            ok_cpu = True
            if baseline_fps and baseline_fps > 0 and p["ok"]:
                ratio = p["avgFps"] / baseline_fps
                entry["fpsRatio"] = ratio
                ok_ratio = ratio >= float(case["fps_vs_baseline_min"])
            else:
                entry["fpsRatio"] = None
                ok_ratio = False
            if baseline_main is not None and p["ok"]:
                delta = p["avgMainThreadCpuMs"] - baseline_main
                entry["mainThreadDeltaMs"] = delta
                ok_cpu = delta <= float(case["main_thread_delta_max_ms"]) + 1e-3
            else:
                entry["mainThreadDeltaMs"] = None
                ok_cpu = False
            base_pass = bool(flow_pass and ok_ratio and ok_cpu)

        execution_gate_pass = bool(
            not evidence["executionRequired"] or evidence["executionObserved"]
        )
        entry["executionGatePass"] = execution_gate_pass
        entry["pass"] = bool(base_pass and execution_gate_pass)

        matrix_pass = matrix_pass and bool(entry.get("pass"))
        result["featureCases"].append(entry)
        print(
            f"  ok={p['ok']} avgFps={p['avgFps']:.3f} "
            f"mainCpu={p['avgMainThreadCpuMs']:.3f} "
            f"pass={entry.get('pass')}"
        )
        time.sleep(1.0)

    result["gates"] = {
        "dualPass": dual_pass if not args.skip_dual else None,
        "matrixPass": matrix_pass,
        "overallPass": (dual_pass if not args.skip_dual else True) and matrix_pass,
        "performanceJudgement": not bool(args.isolated),
    }

    out_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    # baseline 指标单独缓存，供后续相对比较
    for entry in result["featureCases"]:
        if (
            entry["name"] == "baseline"
            and entry.get("flowPass")
            and entry["metrics"].get("ok")
        ):
            (ARTIFACTS / "latest_light_baseline.json").write_text(
                json.dumps(
                    {
                        "timestamp": ts,
                        "source": str(out_path),
                        "metrics": entry["metrics"],
                        "dual": result.get("dual"),
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
            break

    print("\n========== SUMMARY ==========")
    print(f"  output: {out_path}")
    print(f"  dualPass={result['gates']['dualPass']}")
    print(f"  matrixPass={result['gates']['matrixPass']}")
    print(f"  overallPass={result['gates']['overallPass']}")
    return 0 if result["gates"]["overallPass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
