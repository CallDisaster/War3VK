#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A/B：S1 terrain legacy capture on/off，量 FPS 与 ShadowCapture 归属。"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import run_quick_autotest  # noqa: E402

MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x"
HIGH = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"
ARTIFACTS = Path(__file__).resolve().parent / "artifacts"


def parse_report(path: str) -> dict:
    out: dict = {}
    if not path or not Path(path).is_file():
        return out
    t = Path(path).read_text(encoding="utf-8", errors="ignore")

    def g(k: str) -> float:
        m = re.search(rf'"{k}"\s*:\s*([0-9.]+)', t)
        return float(m.group(1)) if m else 0.0

    for k in (
        "avgFps",
        "avgFrameTimeMs",
        "avgMainThreadCpuMs",
        "avgTrackedActiveCpuMs",
        "avgUntrackedActiveCpuMs",
        "cpuCoveragePct",
    ):
        out[k] = g(k)
    keys = {}
    for m in re.finditer(
        r'"path"\s*:\s*"([^"]+)"[^}]*?"avgCpuMs"\s*:\s*([0-9.]+)', t
    ):
        path_name, avg = m.group(1), float(m.group(2))
        for w in (
            "ShadowCapture",
            "Shadow/DrawTime/Capture",
            "War3SemanticScene/Populate",
            "Other/Untracked",
        ):
            if w in path_name:
                keys[w] = max(keys.get(w, 0.0), avg)
    out["keys"] = keys
    return out


def main() -> int:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    base_env = {
        "DXVK_WAR3_POINT_LIGHTS": "0",
        "DXVK_WAR3_POINT_SHADOW": "0",
        "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
        "DXVK_WAR3_TEST_POINT_LIGHT": "0",
    }
    plans = [
        ("low_s1_on", MAP, {"DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE": "1"}),
        ("low_s1_off", MAP, {"DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE": "0"}),
        ("high_s1_on", HIGH, {"DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE": "1"}),
        ("high_s1_off", HIGH, {"DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE": "0"}),
    ]
    rows = []
    deploy = True
    for name, map_path, extra in plans:
        env = dict(base_env)
        env.update(extra)
        print(f"\n========== {name} ==========", flush=True)
        r = run_quick_autotest(
            map_path=map_path,
            sample_duration_sec=22,
            use_isolated_desktop=False,
            windowed=False,
            deploy_d3d9_before_launch=deploy,
            env_overrides_json=json.dumps(env, ensure_ascii=False),
            enforce_video_baseline=False,
        )
        deploy = False
        rep = r.get("report") or {}
        path = rep.get("path") or r.get("reportPath")
        parsed = parse_report(path or "")
        row = {
            "name": name,
            "ok": bool(r.get("ok")),
            "stage": r.get("stage"),
            "reportPath": path,
            **parsed,
        }
        for k in (
            "avgFps",
            "avgFrameTimeMs",
            "avgMainThreadCpuMs",
            "avgTrackedActiveCpuMs",
            "avgUntrackedActiveCpuMs",
            "cpuCoveragePct",
        ):
            if rep.get(k) is not None:
                try:
                    row[k] = float(rep[k])
                except Exception:
                    pass
        rows.append(row)
        print(
            f"  ok={row['ok']} fps={row.get('avgFps', 0):.2f} "
            f"mt={row.get('avgMainThreadCpuMs', 0):.3f} "
            f"sc={row.get('keys', {}).get('ShadowCapture', 0):.3f} "
            f"pop={row.get('keys', {}).get('War3SemanticScene/Populate', 0):.3f}",
            flush=True,
        )

    out = ARTIFACTS / "s1_ab_perf_20260709.json"
    out.write_text(json.dumps(rows, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nWrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
