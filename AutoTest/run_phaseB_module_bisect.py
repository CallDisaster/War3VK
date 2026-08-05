#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase B: high-pressure module bisect to locate unscoped main-thread CPU."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import run_quick_autotest  # noqa: E402

MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"
OUT = Path(__file__).resolve().parent / "artifacts" / "phaseB_module_bisect_20260709.json"

CASES = [
    {"name": "full", "profile": "", "disable_modules": "", "env": {}},
    {
        "name": "no_semantic",
        "profile": "",
        "disable_modules": "semantic.data",
        "env": {},
    },
    {
        "name": "no_capture",
        "profile": "",
        "disable_modules": "shadow.capture",
        "env": {},
    },
    {
        "name": "no_sem_cap",
        "profile": "",
        "disable_modules": "semantic.data,shadow.capture",
        "env": {},
    },
    {
        "name": "no_shadow",
        "profile": "",
        "disable_modules": "shadow",
        "env": {},
    },
    {"name": "dxvk_only", "profile": "dxvk_only", "disable_modules": "", "env": {}},
]


def main() -> int:
    results = []
    for case in CASES:
        name = case["name"]
        print(
            f"CASE {name} profile={case.get('profile')!r} "
            f"disable={case.get('disable_modules')!r}",
            flush=True,
        )
        kwargs = {
            "map_path": MAP,
            "sample_duration_sec": 15,
            "use_isolated_desktop": False,
            "deploy_d3d9_before_launch": False,
            "profile": case.get("profile") or "",
            "disable_modules": case.get("disable_modules") or "",
        }
        env = case.get("env") or {}
        if env:
            kwargs["env_overrides_json"] = json.dumps(env)
        r = run_quick_autotest(**kwargs)
        rep = r.get("report") or {}
        row = {
            "name": name,
            "ok": r.get("ok"),
            "stage": r.get("stage"),
            "avgFps": rep.get("avgFps"),
            "avgFrameTimeMs": rep.get("avgFrameTimeMs"),
            "avgMainThreadCpuMs": rep.get("avgMainThreadCpuMs"),
            "avgProcessCpuMs": rep.get("avgProcessCpuMs"),
            "avgGpuTimeMs": rep.get("avgGpuTimeMs"),
            "avgUntrackedActiveCpuMs": rep.get("avgUntrackedActiveCpuMs"),
            "avgTrackedActiveCpuMs": rep.get("avgTrackedActiveCpuMs"),
            "reportPath": (rep.get("reportPath") or r.get("reportPath")),
        }
        print(json.dumps(row, ensure_ascii=False), flush=True)
        results.append(row)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")
    print("WROTE", OUT, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
