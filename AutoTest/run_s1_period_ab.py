#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import run_quick_autotest  # noqa: E402

MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x"
base = {
    "DXVK_WAR3_POINT_LIGHTS": "0",
    "DXVK_WAR3_POINT_SHADOW": "0",
    "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
    "DXVK_WAR3_TEST_POINT_LIGHT": "0",
}
rows = []
for period in (2, 3, 4):
    env = dict(base)
    env["DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD"] = str(period)
    name = f"period_{period}"
    print(f"==== {name} ====", flush=True)
    r = run_quick_autotest(
        map_path=MAP,
        sample_duration_sec=18,
        use_isolated_desktop=False,
        windowed=False,
        deploy_d3d9_before_launch=False,
        env_overrides_json=json.dumps(env),
        enforce_video_baseline=False,
    )
    rep = r.get("report") or {}
    row = {
        "name": name,
        "period": period,
        "ok": bool(r.get("ok")),
        "avgFps": float(rep.get("avgFps") or 0),
        "avgMainThreadCpuMs": float(rep.get("avgMainThreadCpuMs") or 0),
        "avgTrackedActiveCpuMs": float(rep.get("avgTrackedActiveCpuMs") or 0),
        "avgUntrackedActiveCpuMs": float(rep.get("avgUntrackedActiveCpuMs") or 0),
        "cpuCoveragePct": float(rep.get("cpuCoveragePct") or 0),
    }
    rows.append(row)
    print(
        f"  fps={row['avgFps']:.2f} mt={row['avgMainThreadCpuMs']:.3f} "
        f"tr={row['avgTrackedActiveCpuMs']:.3f} ut={row['avgUntrackedActiveCpuMs']:.3f}",
        flush=True,
    )

out = Path(__file__).resolve().parent / "artifacts" / "s1_period_ab_20260709.json"
out.write_text(json.dumps(rows, indent=2), encoding="utf-8")
print("Wrote", out)
