#!/usr/bin/env python3
"""Visible-desktop A-B-B-A gate for an existing directional PCF setting.

This reuses the locked-sun/internal-framebuffer contract from the directional
alpha gate. It never deploys a DLL or uses an isolated desktop. Baseline and
candidate rounds always run in fresh War3 processes with hashed alpha disabled.
"""

from __future__ import annotations

import argparse
import json
from datetime import datetime
from pathlib import Path
from typing import Any, Dict

from run_shadow_alpha_coverage_abba import (
    DEFAULT_MAP,
    aggregate,
    existing_war3_pids,
    launch_round,
    war3,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--war3-dir", type=Path, default=Path(r"E:\Work\War3"))
    parser.add_argument("--map", dest="map_path", type=Path, default=DEFAULT_MAP)
    parser.add_argument("--sun-steps", type=int, default=16)
    parser.add_argument("--settle-sec", type=float, default=0.18)
    parser.add_argument("--baseline-kernel", type=int, default=2)
    parser.add_argument("--baseline-radius", type=float, default=0.70)
    parser.add_argument("--candidate-kernel", type=int, required=True)
    parser.add_argument("--candidate-radius", type=float, required=True)
    parser.add_argument("--target-x", type=float, default=0.0)
    parser.add_argument("--target-y", type=float, default=0.0)
    parser.add_argument("--target-distance", type=float, default=1200.0)
    parser.add_argument("--angle-of-attack", type=float, default=328.0)
    parser.add_argument("--rotation", type=float, default=90.0)
    parser.add_argument("--far-z", type=float, default=3000.0)
    parser.add_argument("--full-map-visibility", action="store_true")
    args = parser.parse_args()

    if not args.map_path.is_file():
        raise SystemExit(f"map not found: {args.map_path}")
    if any(war3._pid_alive(pid) for pid in existing_war3_pids()):
        raise SystemExit("refusing to start: an existing War3 process is alive")
    for name, value in (
        ("baseline-kernel", args.baseline_kernel),
        ("candidate-kernel", args.candidate_kernel),
    ):
        if value < 0 or value > 3:
            raise SystemExit(f"{name} must be in [0,3]")
    for name, value in (
        ("baseline-radius", args.baseline_radius),
        ("candidate-radius", args.candidate_radius),
    ):
        if not (0.0 <= value <= 6.0):
            raise SystemExit(f"{name} must be finite and in [0,6]")

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    artifact_dir = war3.ARTIFACT_ROOT / "shadow_filter_abba" / stamp
    artifact_dir.mkdir(parents=True, exist_ok=True)
    sun_steps = max(8, min(64, int(args.sun_steps)))
    settle_sec = max(0.05, min(1.0, float(args.settle_sec)))
    result: Dict[str, Any] = {
        "map": str(args.map_path),
        "mapSha256": war3.sha256_file(args.map_path),
        "war3Dir": str(args.war3_dir),
        "sunSteps": sun_steps,
        "settleSec": settle_sec,
        "baseline": {
            "kernel": args.baseline_kernel,
            "radius": args.baseline_radius,
        },
        "candidate": {
            "kernel": args.candidate_kernel,
            "radius": args.candidate_radius,
        },
        "camera": {
            "targetX": float(args.target_x),
            "targetY": float(args.target_y),
            "targetDistance": float(args.target_distance),
            "angleOfAttack": float(args.angle_of_attack),
            "rotation": float(args.rotation),
            "farZ": float(args.far_z),
        },
        "fullMapVisibility": bool(args.full_map_visibility),
        "sequence": ["baseline-a", "candidate-b1", "candidate-b2", "baseline-a2"],
        "rounds": [],
    }

    for label, candidate_enabled in (
        ("baseline-a", False),
        ("candidate-b1", True),
        ("candidate-b2", True),
        ("baseline-a2", False),
    ):
        kernel = args.candidate_kernel if candidate_enabled else args.baseline_kernel
        radius = args.candidate_radius if candidate_enabled else args.baseline_radius
        row = launch_round(
            war3_dir=args.war3_dir,
            map_path=args.map_path,
            artifact_dir=artifact_dir,
            label=label,
            hashed=False,
            sun_steps=sun_steps,
            settle_sec=settle_sec,
            environment_overrides={
                "DXVK_WAR3_SHADOW_ALPHA_HASH": "0",
                "DXVK_WAR3_SHADOW_PCF_KERNEL": str(kernel),
                "DXVK_WAR3_SHADOW_PCF": f"{radius:.6f}",
            },
            candidate_enabled=candidate_enabled,
            camera_config=result["camera"],
            full_map_visibility=result["fullMapVisibility"],
        )
        result["rounds"].append(row)
        (artifact_dir / "partial_result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        if bool(row.get("deviceLost")):
            result["stoppedAfterFirstDeviceLost"] = True
            break

    result["aggregate"] = aggregate(result["rounds"])
    result["ok"] = bool(result["aggregate"].get("roundsComplete"))
    result_path = artifact_dir / "shadow_filter_abba_result.json"
    result_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps({
        "ok": result["ok"],
        "artifactDir": str(artifact_dir),
        "resultPath": str(result_path),
        "aggregate": result["aggregate"],
        "rounds": [
            {
                "label": row.get("label"),
                "ok": row.get("ok"),
                "deviceLost": row.get("deviceLost"),
                "metrics": row.get("metrics"),
            }
            for row in result["rounds"]
        ],
    }, ensure_ascii=False, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
