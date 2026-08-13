#!/usr/bin/env python3
"""Visible-desktop A-B-B-A gate for directional alpha-cutout shadow coverage.

The runner owns every War3 process it launches. It uses only the internal test
pipe after startup, fixes the camera and steps a locked sun through the same
sequence for hard-cutoff and hashed-coverage processes. Every recorded image
must be paired with a newly published directional shadow map.
It never enables an isolated desktop and never deploys a DLL.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List, Sequence, Tuple

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import war3_autotest_mcp as war3


DEFAULT_MAP = Path(r"E:\Work\War3\Maps\(4)TurtleRock~3.w3x")


def existing_war3_pids() -> List[int]:
    completed = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq war3.exe", "/FO", "CSV", "/NH"],
        capture_output=True,
        text=True,
        timeout=8,
        check=False,
    )
    pids: List[int] = []
    for row in csv.reader(io.StringIO(completed.stdout or "")):
        if len(row) >= 2 and row[0].strip().lower() == "war3.exe":
            try:
                pids.append(int(row[1]))
            except ValueError:
                pass
    return pids


def percentile(values: Sequence[float], q: float) -> float:
    if not values:
        return 0.0
    return float(np.percentile(np.asarray(values, dtype=np.float64), q))


def crop_luma(path: Path) -> np.ndarray:
    image = Image.open(path).convert("L")
    array = np.asarray(image, dtype=np.float32)
    height, width = array.shape
    top = max(0, int(round(height * 0.045)))
    bottom = max(top + 1, int(round(height * 0.72)))
    left = max(0, int(round(width * 0.02)))
    right = max(left + 1, int(round(width * 0.98)))
    return array[top:bottom, left:right]


def edge_mask(image: np.ndarray, threshold: float = 24.0) -> np.ndarray:
    gradient = np.zeros_like(image, dtype=np.float32)
    gradient[:, 1:] += np.abs(image[:, 1:] - image[:, :-1])
    gradient[1:, :] += np.abs(image[1:, :] - image[:-1, :])
    return gradient >= float(threshold)


def overlap_for_shift(
    first: np.ndarray, second: np.ndarray, dx: int, dy: int
) -> Tuple[np.ndarray, np.ndarray]:
    height, width = first.shape
    ax0 = max(0, dx)
    ax1 = min(width, width + dx)
    bx0 = max(0, -dx)
    bx1 = min(width, width - dx)
    ay0 = max(0, dy)
    ay1 = min(height, height + dy)
    by0 = max(0, -dy)
    by1 = min(height, height - dy)
    return first[ay0:ay1, ax0:ax1], second[by0:by1, bx0:bx1]


def aligned_pair_metrics(first: np.ndarray, second: np.ndarray) -> Dict[str, Any]:
    best: Dict[str, Any] | None = None
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            a, b = overlap_for_shift(first, second, dx, dy)
            if not a.size or a.shape != b.shape:
                continue
            ea = edge_mask(a)
            eb = edge_mask(b)
            edge_union = np.logical_or(ea, eb)
            changed = np.abs(a - b) >= 12.0
            residual = (
                float(np.count_nonzero(np.logical_and(changed, edge_union))) /
                float(max(1, np.count_nonzero(edge_union)))
            )
            mean_abs = float(np.mean(np.abs(a - b)))
            score = residual + mean_abs * 1.0e-4
            if best is None or score < float(best["score"]):
                best = {
                    "score": score,
                    "dx": dx,
                    "dy": dy,
                    "edgeToggleFraction": residual,
                    "meanAbsLuma": mean_abs,
                }
    return best or {
        "score": 1.0,
        "dx": 0,
        "dy": 0,
        "edgeToggleFraction": 1.0,
        "meanAbsLuma": 255.0,
    }


def sequence_metrics(paths: Sequence[Path]) -> Dict[str, Any]:
    frames = [crop_luma(path) for path in paths]
    pairs = [
        aligned_pair_metrics(frames[index - 1], frames[index])
        for index in range(1, len(frames))
    ]
    toggles = [float(row["edgeToggleFraction"]) for row in pairs]
    coverage = [float(np.count_nonzero(frame < 250.0)) / frame.size for frame in frames]
    edge_lengths = [float(np.count_nonzero(edge_mask(frame))) for frame in frames]
    return {
        "frameCount": len(frames),
        "pairCount": len(pairs),
        "pairs": pairs,
        "edgeToggleMean": float(np.mean(toggles)) if toggles else 0.0,
        "edgeToggleP95": percentile(toggles, 95.0),
        "darkCoverageMean": float(np.mean(coverage)) if coverage else 0.0,
        "darkCoverageMin": float(np.min(coverage)) if coverage else 0.0,
        "edgeLengthMean": float(np.mean(edge_lengths)) if edge_lengths else 0.0,
    }


def invoke(pid: int, root: Path, command: str, payload: Dict[str, Any]) -> Dict[str, Any]:
    return war3._invoke_internal_test_request(
        pid=pid,
        war3_dir=root,
        command=command,
        payload=payload,
        timeout_sec=6.0,
    )


def launch_round(
    war3_dir: Path,
    map_path: Path,
    artifact_dir: Path,
    label: str,
    hashed: bool,
    sun_steps: int,
    settle_sec: float,
    environment_overrides: Dict[str, str] | None = None,
    candidate_enabled: bool | None = None,
    pause_simulation: bool = False,
    camera_config: Dict[str, float] | None = None,
    full_map_visibility: bool = False,
    publication_timeout_sec: float = 4.0,
) -> Dict[str, Any]:
    camera = {
        "targetX": 0.0,
        "targetY": 0.0,
        "targetDistance": 1200.0,
        "angleOfAttack": 328.0,
        "rotation": 90.0,
        "fieldOfView": 70.0,
        "farZ": 3000.0,
        "roll": 0.0,
        "zOffset": 0.0,
        "duration": 0.0,
        "quickPosition": True,
    }
    if camera_config:
        camera.update(camera_config)
    env = {
        "DXVK_WAR3_SHADOW_ALPHA_HASH": "1" if hashed else "0",
        "DXVK_WAR3_SHADOW_TAA_MODE": "0",
        "DXVK_WAR3_CSM_CONTINUITY_TRACE": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC": str(
            max(20, int(math.ceil(sun_steps * settle_sec + 12.0)))
        ),
    }
    if environment_overrides:
        env.update(environment_overrides)
    events_before = war3._query_windows_gpu_events()
    event_keys = {war3._gpu_event_identity(row) for row in events_before}
    log_offsets = war3._snapshot_log_offsets(war3_dir)
    launch = war3._launch_suite_map_until_ready(
        war3_dir=str(war3_dir),
        requested_map_path=str(map_path),
        allow_fallback_to_default_test_map=False,
        ready_timeout_sec=240,
        ready_allow_fallback=False,
        ready_require_game_started_for_fallback=True,
        ready_fallback_min_elapsed_sec=20,
        ready_fallback_min_cpu_sec=1.0,
        launch_kwargs={
            "launcher_mode": war3.YDWE_LAUNCHER_MODE_DIRECT,
            "ydwe_root": "",
            "windowed": True,
            "use_isolated_desktop": False,
            "desktop_name": "",
            "opengl": False,
            "auto_perf_record": True,
            "auto_perf_export_sec": max(24, int(sun_steps * settle_sec + 16.0)),
            "deploy_d3d9_before_launch": False,
            "build_d3d9_path": "build32/src/d3d9/d3d9.dll",
            "enforce_video_baseline": False,
            "render_log": False,
            "profile": "full_default",
            "disable_modules": "",
            "env_overrides_json": json.dumps(env),
            "extra_args": "",
        },
        startup_input_actions=[
            {"type": "sleep", "ms": 10000},
            {"type": "key", "vk": 0x20, "holdMs": 80},
            {"type": "sleep", "ms": 8000},
            {"type": "key", "vk": 0x20, "holdMs": 80},
            {"type": "sleep", "ms": 8000},
            {"type": "key", "vk": 0x20, "holdMs": 80},
            {"type": "sleep", "ms": 8000},
            {"type": "key", "vk": 0x20, "holdMs": 80},
            {"type": "sleep", "ms": 1200},
        ],
    )
    row: Dict[str, Any] = {
        "label": label,
        "hashed": bool(hashed),
        "candidateEnabled": (
            bool(hashed) if candidate_enabled is None else bool(candidate_enabled)
        ),
        "camera": camera,
        "fullMapVisibility": bool(full_map_visibility),
        "environment": env,
        "launch": launch,
        "frames": [],
    }
    if not launch.get("ok"):
        row.update(ok=False, stage="launch")
        return row

    pid = int(launch["pid"])
    owned = True
    root = Path(str(dict(launch.get("launch", {}) or {}).get("instanceRoot", war3_dir)))
    round_dir = artifact_dir / label
    round_dir.mkdir(parents=True, exist_ok=True)
    camera_before: Dict[str, Any] = {}
    failure = ""
    try:
        camera_before = invoke(pid, root, "camera.snapshot", {})
        setup_commands = [("shadow.debug_mode", {"mode": 2})]
        if full_map_visibility:
            setup_commands.insert(0, ("visibility.full_map", {"enabled": True}))
        setup_commands.append(("camera.apply", camera))
        if pause_simulation:
            setup_commands.insert(1, ("game.pause", {"paused": True}))
        for command, payload in setup_commands:
            reply = invoke(pid, root, command, payload)
            row.setdefault("setup", []).append(
                {"command": command, "ok": bool(reply.get("ok")), "reply": reply}
            )
            if not reply.get("ok"):
                failure = f"setup failed: {command}"
                break
        time.sleep(0.5)

        preflight_path = round_dir / "_preflight.bmp"
        preflight = war3._capture_final_frame_via_internal_test_api(
            pid=pid,
            war3_dir=root,
            output_path=preflight_path,
            timeout_sec=8.0,
        )
        row["preflightCapture"] = preflight
        previous_render_serial = int(
            dict(dict(preflight.get("details", {}) or {}).get("result", {}) or {})
            .get("shadowMapRenderSerial", 0)
            or 0
        )
        if not preflight.get("ok") or previous_render_serial <= 0:
            failure = "preflight did not observe a published shadow map"

        for index in range(sun_steps):
            if failure:
                break
            if not war3._pid_alive(pid):
                failure = "owned War3 process exited during capture"
                break
            time01 = 0.49 + 0.02 * index / max(1, sun_steps - 1)
            sun = invoke(
                pid, root, "shadow.lock_sun", {"enabled": True, "time01": time01}
            )
            if not sun.get("ok"):
                failure = "sun lock failed"
                break
            output = round_dir / f"{index:03d}_{time01:.6f}.bmp"
            deadline = time.monotonic() + max(
                0.5, min(10.0, float(publication_timeout_sec))
            )
            capture: Dict[str, Any] = {}
            observed_render_serial = previous_render_serial
            publication_polls = 0
            time.sleep(max(0.05, settle_sec))
            while time.monotonic() < deadline:
                publication_polls += 1
                capture = war3._capture_final_frame_via_internal_test_api(
                    pid=pid,
                    war3_dir=root,
                    output_path=output,
                    timeout_sec=8.0,
                )
                observed_render_serial = int(
                    dict(
                        dict(capture.get("details", {}) or {}).get("result", {})
                        or {}
                    ).get("shadowMapRenderSerial", 0)
                    or 0
                )
                if capture.get("ok") and observed_render_serial > previous_render_serial:
                    break
                time.sleep(0.05)
            status = war3._read_runtime_status_best_effort(pid) or {}
            row["frames"].append(
                {
                    "index": index,
                    "time01": time01,
                    "capture": capture,
                    "publicationPolls": publication_polls,
                    "previousShadowMapRenderSerial": previous_render_serial,
                    "observedShadowMapRenderSerial": observed_render_serial,
                    "output": str(output),
                    "runtimeStatus": status,
                }
            )
            if not capture.get("ok"):
                failure = "internal framebuffer capture failed"
                break
            if observed_render_serial <= previous_render_serial:
                failure = "shadow map publication timed out after sun step"
                break
            previous_render_serial = observed_render_serial
            if war3._runtime_status_device_lost(status):
                failure = "runtime status reported device lost"
                break
    finally:
        if war3._pid_alive(pid):
            invoke(pid, root, "shadow.debug_mode", {"mode": 0})
            invoke(pid, root, "shadow.lock_sun", {"enabled": False, "time01": 0.5})
            if pause_simulation:
                invoke(pid, root, "game.pause", {"paused": False})
            if full_map_visibility:
                invoke(pid, root, "visibility.full_map", {"enabled": False})
            snapshot = dict(camera_before.get("result", {}) or {})
            if snapshot:
                snapshot["duration"] = 0.0
                snapshot["quickPosition"] = True
                invoke(pid, root, "camera.apply", snapshot)
        if owned:
            row["stop"] = war3.stop_war3(
                pid=pid,
                graceful_wait_sec=8 if not failure else 2,
                force=True,
                avoid_foreground_switch=True,
            )

    new_events = [
        event
        for event in war3._query_windows_gpu_events()
        if war3._gpu_event_identity(event) not in event_keys
    ]
    log_summary = war3._read_runtime_log_summary(war3_dir, log_offsets=log_offsets)
    paths = [
        Path(frame["output"])
        for frame in row["frames"]
        if bool(dict(frame.get("capture", {}) or {}).get("ok"))
    ]
    row["metrics"] = sequence_metrics(paths) if len(paths) >= 2 else {}
    render_serials = [
        int(
            dict(
                dict(dict(frame.get("capture", {}) or {}).get("details", {}) or {})
                .get("result", {})
                or {}
            ).get("shadowMapRenderSerial", 0)
            or 0
        )
        for frame in row["frames"]
    ]
    row["shadowMapRenderSerials"] = render_serials
    row["shadowMapPublicationAdvanced"] = bool(
        len(render_serials) == sun_steps
        and all(serial > 0 for serial in render_serials)
        and all(
            render_serials[index] > render_serials[index - 1]
            for index in range(1, len(render_serials))
        )
    )
    if not failure and not row["shadowMapPublicationAdvanced"]:
        failure = "shadow map publication did not advance for every sun step"
    row["newGpuEvents"] = new_events
    row["logSummary"] = log_summary
    device_lost = bool(
        new_events
        or int(dict(log_summary.get("keywordCounts", {}) or {}).get("deviceLost", 0) or 0)
        or "device lost" in failure.lower()
    )
    row.update(
        ok=(
            not failure
            and not device_lost
            and len(paths) == sun_steps
            and row["shadowMapPublicationAdvanced"]
        ),
        stage="done" if not failure and not device_lost else "failed",
        failureReason=failure,
        deviceLost=device_lost,
        artifactDir=str(round_dir),
    )
    return row


def aggregate(rounds: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    def selected(candidate_enabled: bool, field: str) -> List[float]:
        return [
            float(dict(row.get("metrics", {}) or {}).get(field, 0.0) or 0.0)
            for row in rounds
            if bool(row.get("ok"))
            and bool(row.get("candidateEnabled", row.get("hashed")))
            == candidate_enabled
        ]

    off_toggle = selected(False, "edgeToggleP95")
    on_toggle = selected(True, "edgeToggleP95")
    off_coverage = selected(False, "darkCoverageMean")
    on_coverage = selected(True, "darkCoverageMean")
    off_edges = selected(False, "edgeLengthMean")
    on_edges = selected(True, "edgeLengthMean")
    off_toggle_mean = float(np.mean(off_toggle)) if off_toggle else 0.0
    on_toggle_mean = float(np.mean(on_toggle)) if on_toggle else 0.0
    coverage_ratio = (
        float(np.mean(on_coverage)) / float(np.mean(off_coverage))
        if on_coverage and off_coverage and float(np.mean(off_coverage)) > 0.0
        else 0.0
    )
    edge_ratio = (
        float(np.mean(on_edges)) / float(np.mean(off_edges))
        if on_edges and off_edges and float(np.mean(off_edges)) > 0.0
        else 0.0
    )
    improvement = (
        1.0 - on_toggle_mean / off_toggle_mean
        if off_toggle_mean > 0.0
        else 0.0
    )
    all_ok = len(rounds) == 4 and all(bool(row.get("ok")) for row in rounds)
    quality_passed = bool(
        all_ok
        and improvement >= 0.40
        and coverage_ratio >= 0.99
        and 0.99 <= edge_ratio <= 1.15
    )
    return {
        "roundsComplete": all_ok,
        "offEdgeToggleP95Mean": off_toggle_mean,
        "onEdgeToggleP95Mean": on_toggle_mean,
        "edgeToggleImprovement": improvement,
        "coverageRatio": coverage_ratio,
        "edgeLengthRatio": edge_ratio,
        "qualityPassed": quality_passed,
        "promotionAllowed": False,
        "promotionReason": (
            "quality image gate passed; CPU/GPU and long stability gates remain"
            if quality_passed
            else "image gate did not satisfy every objective threshold"
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--war3-dir", type=Path, default=Path(r"E:\Work\War3"))
    parser.add_argument("--map", dest="map_path", type=Path, default=DEFAULT_MAP)
    parser.add_argument("--sun-steps", type=int, default=24)
    parser.add_argument("--settle-sec", type=float, default=0.18)
    parser.add_argument("--target-x", type=float, default=0.0)
    parser.add_argument("--target-y", type=float, default=0.0)
    parser.add_argument("--target-distance", type=float, default=1200.0)
    parser.add_argument("--angle-of-attack", type=float, default=328.0)
    parser.add_argument("--rotation", type=float, default=90.0)
    parser.add_argument("--far-z", type=float, default=3000.0)
    parser.add_argument("--full-map-visibility", action="store_true")
    parser.add_argument(
        "--single-round",
        choices=("off", "on"),
        default="",
        help="Run one diagnostic round instead of the default A-B-B-A gate.",
    )
    args = parser.parse_args()

    if not args.map_path.is_file():
        raise SystemExit(f"map not found: {args.map_path}")
    if any(war3._pid_alive(pid) for pid in existing_war3_pids()):
        raise SystemExit("refusing to start: an existing War3 process is alive")

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    artifact_dir = war3.ARTIFACT_ROOT / "shadow_alpha_coverage_abba" / stamp
    artifact_dir.mkdir(parents=True, exist_ok=True)
    result: Dict[str, Any] = {
        "map": str(args.map_path),
        "mapSha256": war3.sha256_file(args.map_path),
        "war3Dir": str(args.war3_dir),
        "sunSteps": max(8, min(64, int(args.sun_steps))),
        "settleSec": max(0.05, min(1.0, float(args.settle_sec))),
        "camera": {
            "targetX": float(args.target_x),
            "targetY": float(args.target_y),
            "targetDistance": float(args.target_distance),
            "angleOfAttack": float(args.angle_of_attack),
            "rotation": float(args.rotation),
            "farZ": float(args.far_z),
        },
        "fullMapVisibility": bool(args.full_map_visibility),
        "sequence": (
            [f"single-{args.single_round}"]
            if args.single_round
            else ["off-a", "on-b1", "on-b2", "off-a2"]
        ),
        "rounds": [],
    }
    round_modes = (
        [(f"single-{args.single_round}", args.single_round == "on")]
        if args.single_round
        else [("off-a", False), ("on-b1", True),
              ("on-b2", True), ("off-a2", False)]
    )
    for label, hashed in round_modes:
        row = launch_round(
            war3_dir=args.war3_dir,
            map_path=args.map_path,
            artifact_dir=artifact_dir,
            label=label,
            hashed=hashed,
            sun_steps=result["sunSteps"],
            settle_sec=result["settleSec"],
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
    result["ok"] = (
        bool(result["rounds"]) and bool(result["rounds"][0].get("ok"))
        if args.single_round
        else bool(result["aggregate"].get("roundsComplete"))
    )
    result_path = artifact_dir / "shadow_alpha_coverage_abba_result.json"
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
