#!/usr/bin/env python3
# -*- coding: utf-8 -*-
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
    ("p3", {"DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD": "3"}),
    ("p4", {"DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD": "4"}),
    ("p6", {"DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD": "6"}),
    ("p8", {"DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD": "8"}),
    ("s1off", {"DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE": "0"}),
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
        "avgMainThreadCpuMs",
        "avgTrackedActiveCpuMs",
        "avgUntrackedActiveCpuMs",
        "avgGpuTimeMs",
    ):
        out[k] = g(k)
    sc = 0.0
    for m in re.finditer(
        r'"path"\s*:\s*"ShadowCapture"[^}]*?"avgCpuMs"\s*:\s*([0-9.]+)', t
    ):
        sc = max(sc, float(m.group(1)))
    out["shadowCaptureMs"] = sc
    return out


def main() -> int:
    deploy = True
    rows = []
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
        for k, v in list(parsed.items()):
            if rep.get(k) is not None:
                try:
                    parsed[k] = float(rep[k])
                except Exception:
                    pass
        row = {"name": name, "ok": bool(r.get("ok")), "reportPath": path, **parsed}
        rows.append(row)
        print(
            f"  ok={row['ok']} fps={row.get('avgFps',0):.2f} "
            f"frame={row.get('avgFrameTimeMs',0):.3f} "
            f"mt={row.get('avgMainThreadCpuMs',0):.3f} "
            f"tracked={row.get('avgTrackedActiveCpuMs',0):.3f} "
            f"untracked={row.get('avgUntrackedActiveCpuMs',0):.3f} "
            f"sc={row.get('shadowCaptureMs',0):.3f}",
            flush=True,
        )
    out = Path(__file__).resolve().parent / "artifacts" / "s1_period_ab2.json"
    out.write_text(json.dumps(rows, indent=2), encoding="utf-8")
    print(f"\nWrote {out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
