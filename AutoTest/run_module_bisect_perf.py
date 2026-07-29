#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Foreground module bisect: S1 / capture / semantic cost isolation."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any, Dict

sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import run_quick_autotest  # noqa: E402

MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x"
BASE = {
    "DXVK_WAR3_POINT_LIGHTS": "0",
    "DXVK_WAR3_POINT_SHADOW": "0",
    "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
    "DXVK_WAR3_TEST_POINT_LIGHT": "0",
}
CASES = [
    ("A_baseline", {}),
    ("B_s1_off", {"DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE": "0"}),
    ("C_no_capture", {"DXVK_WAR3_DISABLE_SHADOW_CAPTURE": "1"}),
    ("D_no_sem", {"DXVK_WAR3_DISABLE": "semantic.data"}),
    ("E_no_sem_no_cap", {
        "DXVK_WAR3_DISABLE": "semantic.data",
        "DXVK_WAR3_DISABLE_SHADOW_CAPTURE": "1",
    }),
]


def parse_report(path: str) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    if not path or not Path(path).is_file():
        return out
    t = Path(path).read_text(encoding="utf-8", errors="ignore")

    def g(k: str) -> float:
        m = re.search(rf'"{k}"\s*:\s*([0-9.]+)', t)
        return float(m.group(1)) if m else 0.0

    for k in (
        "avgFps",
        "avgFrameTimeMs",
        "avgGpuTimeMs",
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
        path_s, cpu = m.group(1), float(m.group(2))
        for w in (
            "ShadowCapture",
            "War3SemanticScene/Populate",
            "Shadow/Main",
            "ShadowMap",
            "Other/UntrackedActive",
        ):
            if w in path_s:
                keys[w] = max(keys.get(w, 0.0), cpu)
    out["keySections"] = keys
    return out


def main() -> int:
    deploy = True
    results = []
    for name, extra in CASES:
        env = dict(BASE)
        env.update(extra)
        print(f"\n========== {name} ==========", flush=True)
        r = run_quick_autotest(
            map_path=MAP,
            sample_duration_sec=18,
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
            "reportPath": path,
            **parsed,
        }
        for k in (
            "avgFps",
            "avgFrameTimeMs",
            "avgGpuTimeMs",
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
        results.append(row)
        print(
            f"  ok={row['ok']} fps={row.get('avgFps', 0):.2f} "
            f"frame={row.get('avgFrameTimeMs', 0):.3f} "
            f"mt={row.get('avgMainThreadCpuMs', 0):.3f} "
            f"tracked={row.get('avgTrackedActiveCpuMs', 0):.3f} "
            f"untracked={row.get('avgUntrackedActiveCpuMs', 0):.3f} "
            f"keys={json.dumps(row.get('keySections') or {}, ensure_ascii=False)}",
            flush=True,
        )
    out = Path(__file__).resolve().parent / "artifacts" / "module_bisect_perf.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"\nWrote {out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
