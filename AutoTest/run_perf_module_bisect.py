#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""模块禁用 A/B：定位 50FPS 与 Untracked 的真实来源。"""
from __future__ import annotations

import json
import re
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import run_quick_autotest  # noqa: E402

MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x"
ARTIFACTS = Path(__file__).resolve().parent / "artifacts"

CASES = [
    {"name": "full_default", "env": {}},
    {
        "name": "no_semantic",
        "env": {"DXVK_WAR3_DISABLE": "semantic.data"},
    },
    {
        "name": "no_shadow_capture",
        "env": {"DXVK_WAR3_DISABLE": "shadow.capture"},
    },
    {
        "name": "no_semantic_no_capture",
        "env": {"DXVK_WAR3_DISABLE": "semantic.data,shadow.capture"},
    },
    {
        "name": "no_shadow_stack",
        "env": {"DXVK_WAR3_DISABLE": "shadow"},
    },
    {
        "name": "no_render_interference",
        "env": {"DXVK_WAR3_DISABLE": "render"},
    },
    {
        "name": "dxvk_only",
        "env": {"DXVK_WAR3_PROFILE": "dxvk_only"},
    },
]


def summarize(path: str) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    if not path:
        return out
    p = Path(path)
    # Autotest may return Windows path; also try Log dir by basename
    candidates = [p]
    if p.name:
        candidates.append(Path("/mnt/e/Work/War3/WarVK/Log") / p.name)
    text = None
    for c in candidates:
        try:
            if c.is_file():
                text = c.read_text(encoding="utf-8", errors="ignore")
                out["reportPath"] = str(c)
                break
        except Exception:
            pass
    if not text:
        return out

    def g(k: str) -> float:
        m = re.search(rf'"{k}"\s*:\s*([0-9.]+)', text)
        return float(m.group(1)) if m else 0.0

    for k in (
        "avgFps",
        "avgFrameTimeMs",
        "avgGpuTimeMs",
        "avgMainThreadCpuMs",
        "avgProcessCpuMs",
        "avgTrackedActiveCpuMs",
        "avgUntrackedActiveCpuMs",
        "avgIdleWaitCpuMs",
        "cpuCoveragePct",
    ):
        out[k] = g(k)

    keys = {
        "Populate": 0.0,
        "Shadow/Main": 0.0,
        "ShadowMap": 0.0,
        "OutsideMainLoop/Tracked": 0.0,
        "CaptureLiveState": 0.0,
        "Registries": 0.0,
        "PostFX": 0.0,
    }
    for m in re.finditer(
        r'"name"\s*:\s*"([^"]+)"\s*,\s*"path"\s*:\s*"([^"]+)"[^}]*?"avgSelfCpuMs"\s*:\s*([0-9.]+)',
        text,
    ):
        name, path_s, avg = m.group(1), m.group(2), float(m.group(3))
        for k in list(keys.keys()):
            if k in name or k in path_s:
                keys[k] = max(keys[k], avg)
    out["keys"] = keys
    return out


def main() -> int:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    rows: List[Dict[str, Any]] = []
    deploy = True
    for case in CASES:
        print(f"\n========== {case['name']} ==========", flush=True)
        env = dict(case["env"])
        # keep lights off for clean bisect
        env.setdefault("DXVK_WAR3_POINT_LIGHTS", "0")
        env.setdefault("DXVK_WAR3_POINT_SHADOW", "0")
        env.setdefault("DXVK_WAR3_VOLUMETRIC_LIGHT", "0")
        env.setdefault("DXVK_WAR3_TEST_POINT_LIGHT", "0")
        r = run_quick_autotest(
            map_path=MAP,
            sample_duration_sec=22,
            use_isolated_desktop=False,
            windowed=False,
            deploy_d3d9_before_launch=deploy,
            env_overrides_json=json.dumps(env, ensure_ascii=False),
            enforce_video_baseline=False,
        )
        deploy = False
        rep = r.get("report") or {}
        path = rep.get("path") or r.get("reportPath") or ""
        sm = summarize(path)
        row = {
            "name": case["name"],
            "env": env,
            "ok": bool(r.get("ok")),
            "stage": r.get("stage"),
            **sm,
        }
        # prefer report dict if present
        for k in ("avgFps", "avgFrameTimeMs", "avgMainThreadCpuMs"):
            if rep.get(k) is not None:
                try:
                    row[k] = float(rep[k])
                except Exception:
                    pass
        rows.append(row)
        print(
            f"  ok={row['ok']} fps={row.get('avgFps',0):.2f} "
            f"mt={row.get('avgMainThreadCpuMs',0):.3f} "
            f"tracked={row.get('avgTrackedActiveCpuMs',0):.3f} "
            f"untracked={row.get('avgUntrackedActiveCpuMs',0):.3f} "
            f"idle={row.get('avgIdleWaitCpuMs',0):.3f} "
            f"keys={row.get('keys')}",
            flush=True,
        )

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = ARTIFACTS / f"perf_module_bisect_{ts}.json"
    out.write_text(json.dumps(rows, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nWrote {out}", flush=True)

    # ranking
    ok_rows = [x for x in rows if x.get("ok") and x.get("avgFps")]
    ok_rows.sort(key=lambda x: x["avgFps"], reverse=True)
    print("\nFPS ranking:", flush=True)
    for x in ok_rows:
        print(f"  {x['avgFps']:7.2f}  {x['name']}", flush=True)
    return 0 if all(x.get("ok") for x in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
