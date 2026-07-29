#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""前台多轮 perf：解析 Untracked / 真实 FPS，避免 isolated desktop 假 60。"""
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
HIGH = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"
ARTIFACTS = Path(__file__).resolve().parent / "artifacts"


def parse_report(path: str) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    if not path or not Path(path).is_file():
        return out
    t = Path(path).read_text(encoding="utf-8", errors="ignore")

    def g(k: str):
        m = re.search(rf'"{k}"\s*:\s*([0-9.]+)', t)
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
        "activeFrameTimeMs",
    ):
        out[k] = g(k)

    secs = []
    for m in re.finditer(
        r'"name"\s*:\s*"([^"]+)"\s*,\s*"path"\s*:\s*"([^"]+)"[^}]*?"avgCpuMs"\s*:\s*([0-9.]+)',
        t,
    ):
        secs.append(
            {"avgCpuMs": float(m.group(3)), "name": m.group(1), "path": m.group(2)}
        )
    secs.sort(key=lambda x: x["avgCpuMs"], reverse=True)
    # 去重 path
    seen = set()
    top = []
    for s in secs:
        if s["path"] in seen:
            continue
        seen.add(s["path"])
        top.append(s)
        if len(top) >= 25:
            break
    out["topSections"] = top

    # 关键叶子
    want = (
        "War3SemanticScene/Populate",
        "ShadowCapture",
        "Shadow/Main",
        "ShadowMap",
        "PointShadow",
        "Hook_FlushAndReset",
        "Present",
        "WaitGate",
        "Other/UntrackedActive",
        "War3Pipeline/BeforeUi",
        "Direct/BuildPacket",
        "Direct/Append",
        "DrawTime",
        "Registries",
        "EndFrame",
    )
    keys = {}
    for s in secs:
        for w in want:
            if w in s["name"] or w in s["path"]:
                keys[w] = max(keys.get(w, 0.0), s["avgCpuMs"])
    out["keySections"] = keys
    return out


def main() -> int:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    rounds: List[Dict[str, Any]] = []
    deploy = True
    plans = [
        ("low_fg_1", MAP),
        ("low_fg_2", MAP),
        ("low_fg_3", MAP),
        ("high_fg_1", HIGH),
    ]
    env = {
        "DXVK_WAR3_POINT_LIGHTS": "0",
        "DXVK_WAR3_POINT_SHADOW": "0",
        "DXVK_WAR3_VOLUMETRIC_LIGHT": "0",
        "DXVK_WAR3_TEST_POINT_LIGHT": "0",
    }
    for name, map_path in plans:
        print(f"\n========== {name} ==========", flush=True)
        r = run_quick_autotest(
            map_path=map_path,
            sample_duration_sec=25,
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
            "map": map_path,
            "ok": bool(r.get("ok")),
            "stage": r.get("stage"),
            "reportPath": path,
            **parsed,
        }
        # prefer report JSON numbers if present
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
        rounds.append(row)
        print(
            f"  ok={row['ok']} fps={row.get('avgFps',0):.2f} "
            f"frame={row.get('avgFrameTimeMs',0):.3f} "
            f"mt={row.get('avgMainThreadCpuMs',0):.3f} "
            f"tracked={row.get('avgTrackedActiveCpuMs',0):.3f} "
            f"untracked={row.get('avgUntrackedActiveCpuMs',0):.3f} "
            f"cov={row.get('cpuCoveragePct',0):.1f}%",
            flush=True,
        )
        if row.get("keySections"):
            print("  keys:", json.dumps(row["keySections"], ensure_ascii=False), flush=True)
        if row.get("topSections"):
            print("  top5:", [(s["name"], round(s["avgCpuMs"], 3)) for s in row["topSections"][:5]], flush=True)

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = ARTIFACTS / f"fg_untracked_probe_{ts}.json"
    out.write_text(json.dumps(rounds, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nWrote {out}", flush=True)
    return 0 if all(x.get("ok") for x in rounds) else 1


if __name__ == "__main__":
    raise SystemExit(main())
