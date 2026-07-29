#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A2 Worker_Prepare + 点光/体积光 隔离桌面模块级对比。

以 sectionBreakdown 为主判据（isolated desktop FPS 噪声大）：
  - baseline
  - point_shadow + Worker_Prepare=1（默认）
  - point_shadow + Worker_Prepare=0（同步对照）
  - volumetric_only
"""
from __future__ import annotations

import json
import re
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import run_quick_autotest  # noqa: E402

LOW_MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x"
ARTIFACTS = Path(__file__).resolve().parent / "artifacts"

SECTION_KEYS = (
    "Shadow/Main",
    "ShadowMap",
    "PointShadow",
    "PointShadow/PrepareCpu",
    "PointShadow/WorkerPrepare",
    "War3SemanticScene/Populate",
    "VolumetricLight",
    "Other/UntrackedActive",
)

CASES: List[Dict[str, Any]] = [
    {
        "name": "baseline",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "0",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "0",
            "DXVK_WAR3_WORKER_PREPARE": "1",
        },
    },
    {
        "name": "point_shadow_worker_on",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "1",
            "DXVK_WAR3_POINT_SHADOW_RESOLUTION": "256",
            "DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW_MAX_FACES": "3",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
            "DXVK_WAR3_WORKER_PREPARE": "1",
        },
    },
    {
        "name": "point_shadow_worker_off",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW": "1",
            "DXVK_WAR3_POINT_SHADOW_RESOLUTION": "256",
            "DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS": "1",
            "DXVK_WAR3_POINT_SHADOW_MAX_FACES": "3",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
            "DXVK_WAR3_TEST_POINT_LIGHT": "1",
            "DXVK_WAR3_TEST_POINT_LIGHT_SHADOW": "0.65",
            "DXVK_WAR3_TEST_POINT_LIGHT_Z": "420",
            "DXVK_WAR3_TEST_POINT_LIGHT_RANGE": "1800",
            "DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY": "1.0",
            "DXVK_WAR3_WORKER_PREPARE": "0",
        },
    },
    {
        "name": "volumetric_only",
        "env": {
            "DXVK_WAR3_POINT_LIGHTS": "0",
            "DXVK_WAR3_POINT_SHADOW": "0",
            "DXVK_WAR3_VOLUMETRIC_LIGHT": "1",
            "DXVK_WAR3_VOLUMETRIC_RES_DIVISOR": "4",
            "DXVK_WAR3_VOLUMETRIC_SAMPLES": "24",
            "DXVK_WAR3_TEST_POINT_LIGHT": "0",
            "DXVK_WAR3_WORKER_PREPARE": "1",
        },
    },
]


def _parse_section_ms(report_path: Optional[str]) -> Dict[str, float]:
    out: Dict[str, float] = {k: 0.0 for k in SECTION_KEYS}
    if not report_path:
        return out
    p = Path(report_path)
    if not p.is_file():
        return out
    text = p.read_text(encoding="utf-8", errors="ignore")
    # HTML 报告常见：name 列 + avgCpuMs / inclusive
    for key in SECTION_KEYS:
        # 宽松匹配：节名后若干字段中的第一个浮点 ms
        pat = re.compile(
            re.escape(key) + r"[^0-9]{0,80}?([0-9]+\.[0-9]+)",
            re.IGNORECASE,
        )
        m = pat.search(text)
        if m:
            out[key] = float(m.group(1))
    return out


def main() -> int:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_path = ARTIFACTS / f"a2_worker_module_matrix_isolated_{ts}.json"
    sample_sec = 25
    rows: List[Dict[str, Any]] = []
    deploy = True

    for case in CASES:
        print(f"\n========== {case['name']} ==========", flush=True)
        r = run_quick_autotest(
            map_path=LOW_MAP,
            sample_duration_sec=sample_sec,
            use_isolated_desktop=True,
            windowed=False,
            deploy_d3d9_before_launch=deploy,
            env_overrides_json=json.dumps(case["env"], ensure_ascii=False),
            enforce_video_baseline=False,
        )
        deploy = False
        rep = r.get("report") or {}
        report_path = rep.get("path") or r.get("reportPath")
        sections = _parse_section_ms(report_path)
        row = {
            "name": case["name"],
            "ok": bool(r.get("ok")),
            "stage": r.get("stage"),
            "avgFps": float(rep.get("avgFps") or 0.0),
            "avgFrameTimeMs": float(rep.get("avgFrameTimeMs") or 0.0),
            "avgGpuTimeMs": float(rep.get("avgGpuTimeMs") or 0.0),
            "avgMainThreadCpuMs": float(rep.get("avgMainThreadCpuMs") or 0.0),
            "reportPath": report_path,
            "sections": sections,
        }
        rows.append(row)
        print(
            f"  ok={row['ok']} fps={row['avgFps']:.2f} "
            f"mt={row['avgMainThreadCpuMs']:.3f} "
            f"PointShadow={sections.get('PointShadow', 0):.3f} "
            f"WorkerPrepare={sections.get('PointShadow/WorkerPrepare', 0):.3f} "
            f"Vol={sections.get('VolumetricLight', 0):.3f}",
            flush=True,
        )

    payload = {
        "timestamp": ts,
        "sampleSec": sample_sec,
        "isolatedDesktop": True,
        "map": LOW_MAP,
        "cases": rows,
    }
    out_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nWrote {out_path}", flush=True)

    # 模块门：点阴影 CPU 不应明显爆炸；Worker_Prepare 开启时允许 Worker 有采样
    base = next((x for x in rows if x["name"] == "baseline"), None)
    p_on = next((x for x in rows if x["name"] == "point_shadow_worker_on"), None)
    p_off = next((x for x in rows if x["name"] == "point_shadow_worker_off"), None)
    vol = next((x for x in rows if x["name"] == "volumetric_only"), None)

    failed = False
    for x in rows:
        if not x.get("ok"):
            print(f"FAIL case not ok: {x['name']}", flush=True)
            failed = True

    if p_on and base:
        ps = p_on["sections"].get("PointShadow", 0.0)
        sm_delta = abs(
            p_on["sections"].get("ShadowMap", 0.0)
            - base["sections"].get("ShadowMap", 0.0)
        )
        print(
            f"GATE point_shadow: PointShadow={ps:.3f}ms ShadowMapΔ={sm_delta:.3f}ms",
            flush=True,
        )
        # 允许点阴影本身有成本，但不允许 CSM 因点光路径大幅膨胀
        if sm_delta > 1.5:
            print("FAIL ShadowMap delta too large vs baseline", flush=True)
            failed = True

    if p_on and p_off:
        print(
            f"A/B worker: on PointShadow={p_on['sections'].get('PointShadow', 0):.3f} "
            f"Worker={p_on['sections'].get('PointShadow/WorkerPrepare', 0):.3f} | "
            f"off PointShadow={p_off['sections'].get('PointShadow', 0):.3f} "
            f"Worker={p_off['sections'].get('PointShadow/WorkerPrepare', 0):.3f}",
            flush=True,
        )

    if vol and base:
        v = vol["sections"].get("VolumetricLight", 0.0)
        print(f"GATE volumetric: VolumetricLight={v:.3f}ms", flush=True)
        if v > 2.0:
            print("WARN volumetric CPU high (soft)", flush=True)

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
