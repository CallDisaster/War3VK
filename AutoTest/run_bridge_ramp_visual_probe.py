#!/usr/bin/env python3
"""Capture exact-frame bridge/ramp shadow state on an isolated desktop.

The render-thread frame-capture response carries the Stage1 matrix/caster/
receiver contract for the same backbuffer that is written to each BMP.  This
avoids correlating a moving-camera screenshot with a later control-plane poll.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import threading
import time
import traceback
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

import run_unattended_perf_gate as gate
import war3_autotest_mcp as war3


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--war3-dir", default=r"E:\Work\War3")
    parser.add_argument(
        "--map",
        default=r"E:\Work\War3\Maps\ShadowTest\光影测试(桥斜坡).w3x",
    )
    parser.add_argument(
        "--build",
        default=str(
            Path(__file__).resolve().parents[1]
            / "build32"
            / "src"
            / "d3d9"
            / "d3d9.dll"
        ),
    )
    parser.add_argument("--label", default="original_world")
    parser.add_argument("--force-identity-world", action="store_true")
    parser.add_argument("--disable-s1-persistent", action="store_true")
    parser.add_argument("--disable-shadow-taa", action="store_true")
    parser.add_argument("--shadow-debug-mode", type=int, default=0)
    parser.add_argument(
        "--lock-sun-time",
        type=float,
        default=None,
        help=(
            "Lock the renderer sun to this normalized day time while leaving "
            "the scripted camera and map simulation running."
        ),
    )
    parser.add_argument(
        "--freeze-scripted-camera",
        action="store_true",
        help=(
            "Invoke the stock PauseGame(boolean) native through the internal "
            "test API before capture. Rendering continues while the map timer "
            "and scripted camera remain frozen."
        ),
    )
    parser.add_argument(
        "--skip-stage13-witness",
        action="store_true",
        help=(
            "Start exact capture as soon as the shadow receiver is ready. "
            "Use for unit/destructible pressure maps with no Stage13 cohort."
        ),
    )
    parser.add_argument(
        "--camera-angle-deg",
        type=float,
        default=None,
        help=(
            "Continuously force CAMERA_FIELD_ANGLE_OF_ATTACK through the "
            "stock SetCameraField native while map simulation stays live."
        ),
    )
    parser.add_argument(
        "--camera-target-x",
        type=float,
        default=None,
        help="Continuously pin the local camera target X through SetCameraPosition.",
    )
    parser.add_argument(
        "--camera-target-y",
        type=float,
        default=None,
        help="Continuously pin the local camera target Y through SetCameraPosition.",
    )
    parser.add_argument("--camera-distance", type=float, default=1650.0)
    parser.add_argument("--camera-rotation-deg", type=float, default=90.0)
    parser.add_argument("--camera-fov-deg", type=float, default=70.0)
    parser.add_argument("--camera-far-z", type=float, default=5000.0)
    parser.add_argument(
        "--camera-pan-x-amplitude",
        type=float,
        default=0.0,
        help=(
            "Apply a deterministic sine-wave target-X pan around the fixed "
            "camera target, indexed by captured frame."
        ),
    )
    parser.add_argument(
        "--camera-pan-y-amplitude",
        type=float,
        default=0.0,
        help=(
            "Apply a deterministic sine-wave target-Y pan around the fixed "
            "camera target, indexed by captured frame."
        ),
    )
    parser.add_argument(
        "--camera-pan-period-captures",
        type=int,
        default=120,
        help="Capture count per complete deterministic fixed-camera pan cycle.",
    )
    parser.add_argument("--extra-env-json", default="{}")
    parser.add_argument("--capture-count", type=int, default=48)
    parser.add_argument("--capture-interval-sec", type=float, default=0.20)
    parser.add_argument(
        "--capture-retain-count",
        type=int,
        default=0,
        help=(
            "Keep only this many recent non-trigger BMPs on disk (0 keeps "
            "all captures). Trigger-adjacent frames are pinned and never "
            "count against this rolling limit."
        ),
    )
    parser.add_argument(
        "--temporal-dark-trigger-pixels",
        type=int,
        default=0,
        help=(
            "Opt-in online tear trigger: pin a capture when the largest "
            "newly-dark connected component reaches this many analysis "
            "pixels. 0 disables online image analysis."
        ),
    )
    parser.add_argument(
        "--temporal-dark-trigger-threshold",
        type=float,
        default=24.0,
        help="Minimum globally-normalized luma drop for the online trigger.",
    )
    parser.add_argument(
        "--temporal-dark-trigger-width",
        type=int,
        default=640,
        help="Reduced scene width used by the online temporal trigger.",
    )
    parser.add_argument(
        "--temporal-dark-trigger-scene-ratio",
        type=float,
        default=0.60,
        help="Top-of-frame scene fraction analyzed; excludes the lower UI.",
    )
    parser.add_argument(
        "--max-trigger-events",
        type=int,
        default=1,
        help="Maximum number of image events whose evidence is pinned.",
    )
    parser.add_argument(
        "--stop-after-trigger-post-captures",
        type=int,
        default=0,
        help=(
            "Stop the capture loop after this many post-trigger exact "
            "frames (0 continues for the requested duration)."
        ),
    )
    parser.add_argument(
        "--trace-ring-segment-sec",
        type=float,
        default=0.0,
        help=(
            "Opt-in rolling full-caster trace segment duration. Must be at "
            "least 2 seconds; 0 disables trace-ring control."
        ),
    )
    parser.add_argument(
        "--trace-ring-retain-segments",
        type=int,
        default=2,
        help="Recent non-trigger trace segments retained on disk.",
    )
    parser.add_argument(
        "--trace-ring-max-pose-records", type=int, default=64
    )
    parser.add_argument(
        "--trace-ring-max-shadow-object-records", type=int, default=128
    )
    parser.add_argument(
        "--trace-ring-max-current-draw-records", type=int, default=256
    )
    parser.add_argument(
        "--trace-ring-caster-sample-bytes",
        type=int,
        default=256,
        help=(
            "Final-caster source bytes hashed per record in trace-ring mode."
        ),
    )
    parser.add_argument("--sample-sec", type=int, default=50)
    parser.add_argument("--ready-timeout-sec", type=int, default=180)
    parser.add_argument(
        "--wait-for-shadow-trace-sec",
        type=float,
        default=0.0,
        help=(
            "Before the first screenshot, wait for this run's JSONL trace to "
            "contain a shadowFinalCasterFrame event. This guarantees that "
            "every captured shadowFrameSerial can be joined back to final "
            "caster data."
        ),
    )
    parser.add_argument("--output", default="")
    return parser.parse_args()


def _wait_for_final_caster_trace(
    log_dir: Path,
    started_at: float,
    timeout_sec: float,
    worker: threading.Thread,
) -> Dict[str, Any]:
    if timeout_sec <= 0.0:
        return {
            "ok": True,
            "skipped": True,
            "reason": "not requested",
        }

    marker = b'"type":"shadowFinalCasterFrame"'
    serial_pattern = re.compile(br'"frameSerial":([0-9]+)')
    deadline = time.time() + timeout_sec
    last_error = ""
    candidate_path: Path | None = None
    while worker.is_alive() and time.time() < deadline:
        try:
            candidates = [
                path
                for path in log_dir.glob("shadow_pose_full_trace_*.jsonl")
                if path.stat().st_mtime >= started_at - 1.0
            ]
            if candidates:
                candidate_path = max(
                    candidates,
                    key=lambda path: path.stat().st_mtime,
                )
                size = candidate_path.stat().st_size
                if size > 0:
                    with candidate_path.open("rb") as stream:
                        prefix = stream.read(min(size, 8 * 1024 * 1024))
                    marker_at = prefix.find(marker)
                    if marker_at >= 0:
                        match = serial_pattern.search(
                            prefix[marker_at : marker_at + 512]
                        )
                        return {
                            "ok": True,
                            "skipped": False,
                            "path": str(candidate_path),
                            "sizeAtWitness": size,
                            "firstFinalCasterFrameSerial": (
                                int(match.group(1)) if match else 0
                            ),
                            "waitedSec": round(
                                time.time() - started_at,
                                3,
                            ),
                        }
        except OSError as exc:
            # The writer may be rotating/opening the file between glob and
            # read. Keep polling the same bounded diagnostic window.
            last_error = repr(exc)
        time.sleep(0.05)

    return {
        "ok": False,
        "skipped": False,
        "path": str(candidate_path) if candidate_path else "",
        "waitedSec": round(time.time() - started_at, 3),
        "error": last_error or "final caster trace witness timed out",
    }


def _analyze_pairwise_dark_trigger(
    previous_path: Path,
    current_path: Path,
    *,
    target_width: int,
    scene_ratio: float,
    luma_threshold: float,
) -> Dict[str, Any]:
    """Find a large new dark component without retaining a full sequence.

    The fixed-camera long probe only needs an online sentinel, not the more
    expensive temporal-median analysis used for the final artifact.  Removing
    the median frame-wide luma delta makes the sentinel insensitive to a slow
    sun/exposure step while preserving a localized triangular shadow.
    """
    try:
        import cv2
        import numpy as np
    except Exception as exc:
        return {
            "ok": False,
            "error": f"online temporal trigger dependencies unavailable: {exc}",
        }

    previous = cv2.imread(str(previous_path), cv2.IMREAD_GRAYSCALE)
    current = cv2.imread(str(current_path), cv2.IMREAD_GRAYSCALE)
    if previous is None or current is None:
        return {
            "ok": False,
            "error": "online temporal trigger could not read one or both BMPs",
            "previous": str(previous_path),
            "current": str(current_path),
        }
    if previous.shape != current.shape:
        return {
            "ok": False,
            "error": "online temporal trigger frame sizes differ",
            "previousShape": list(previous.shape),
            "currentShape": list(current.shape),
        }

    source_height, source_width = current.shape[:2]
    scene_height = max(
        1,
        min(source_height, int(round(source_height * scene_ratio))),
    )
    analysis_width = max(64, min(int(target_width), source_width))
    analysis_height = max(
        1,
        int(round(scene_height * analysis_width / max(source_width, 1))),
    )
    previous_scene = cv2.resize(
        previous[:scene_height, :],
        (analysis_width, analysis_height),
        interpolation=cv2.INTER_AREA,
    ).astype(np.float32)
    current_scene = cv2.resize(
        current[:scene_height, :],
        (analysis_width, analysis_height),
        interpolation=cv2.INTER_AREA,
    ).astype(np.float32)

    dark_delta = previous_scene - current_scene
    global_delta = float(np.median(dark_delta))
    normalized = dark_delta - global_delta
    raw_mask = normalized >= max(0.0, float(luma_threshold))
    kernel = np.ones((3, 3), dtype=np.uint8)
    clean = cv2.morphologyEx(
        raw_mask.astype(np.uint8), cv2.MORPH_OPEN, kernel
    )
    clean = cv2.morphologyEx(clean, cv2.MORPH_CLOSE, kernel)
    component_count, _, stats, _ = cv2.connectedComponentsWithStats(
        clean, connectivity=8
    )
    largest_area = 0
    analysis_bounds = [0, 0, 0, 0]
    if component_count > 1:
        areas = stats[1:, cv2.CC_STAT_AREA]
        largest_index = 1 + int(np.argmax(areas))
        largest_area = int(stats[largest_index, cv2.CC_STAT_AREA])
        analysis_bounds = [
            int(stats[largest_index, cv2.CC_STAT_LEFT]),
            int(stats[largest_index, cv2.CC_STAT_TOP]),
            int(stats[largest_index, cv2.CC_STAT_WIDTH]),
            int(stats[largest_index, cv2.CC_STAT_HEIGHT]),
        ]
    source_scale = float(source_width) / float(max(analysis_width, 1))
    source_bounds = [
        int(round(value * source_scale)) for value in analysis_bounds
    ]
    return {
        "ok": True,
        "previous": str(previous_path),
        "current": str(current_path),
        "sourceSize": [int(source_width), int(source_height)],
        "analysisSize": [int(analysis_width), int(analysis_height)],
        "sceneRatio": float(scene_ratio),
        "lumaThreshold": float(luma_threshold),
        "globalLumaDelta": global_delta,
        "darkPixelCount": int(np.count_nonzero(clean)),
        "largestDarkComponent": largest_area,
        "largestBoundsAnalysis": analysis_bounds,
        "largestBoundsSourceApprox": source_bounds,
    }


def _safe_unlink_probe_artifact(path: Path, allowed_dir: Path) -> str:
    """Delete only a resolved regular file directly inside one probe dir."""
    try:
        resolved = path.resolve()
        allowed = allowed_dir.resolve()
        if resolved.parent != allowed:
            return "refused: artifact is outside the probe directory"
        if not resolved.is_file():
            return "skipped: artifact is not a regular file"
        resolved.unlink()
        return "deleted"
    except Exception as exc:
        return f"failed: {exc}"


def main() -> int:
    args = _parse_args()
    if args.capture_retain_count < 0:
        raise SystemExit("--capture-retain-count must be non-negative")
    if args.temporal_dark_trigger_pixels < 0:
        raise SystemExit("--temporal-dark-trigger-pixels must be non-negative")
    if args.temporal_dark_trigger_width < 64:
        raise SystemExit("--temporal-dark-trigger-width must be at least 64")
    if args.temporal_dark_trigger_pixels > 0:
        try:
            import cv2  # noqa: F401
            import numpy  # noqa: F401
        except Exception as exc:
            raise SystemExit(
                f"online temporal trigger requires cv2 and numpy: {exc}"
            ) from exc
    if not 0.10 <= args.temporal_dark_trigger_scene_ratio <= 1.0:
        raise SystemExit(
            "--temporal-dark-trigger-scene-ratio must be between 0.10 and 1.0"
        )
    if args.max_trigger_events < 0:
        raise SystemExit("--max-trigger-events must be non-negative")
    if args.stop_after_trigger_post_captures < 0:
        raise SystemExit(
            "--stop-after-trigger-post-captures must be non-negative"
        )
    trace_ring_enabled = args.trace_ring_segment_sec > 0.0
    if trace_ring_enabled and args.trace_ring_segment_sec < 2.0:
        raise SystemExit("--trace-ring-segment-sec must be 0 or at least 2")
    if args.trace_ring_retain_segments < 1:
        raise SystemExit("--trace-ring-retain-segments must be positive")
    for name in (
        "trace_ring_max_pose_records",
        "trace_ring_max_shadow_object_records",
        "trace_ring_max_current_draw_records",
        "trace_ring_caster_sample_bytes",
    ):
        if int(getattr(args, name)) < 0:
            raise SystemExit(f"--{name.replace('_', '-')} must be non-negative")
    if (args.camera_target_x is None) != (args.camera_target_y is None):
        raise SystemExit(
            "--camera-target-x and --camera-target-y must be supplied together"
        )
    fixed_camera_requested = args.camera_target_x is not None
    if fixed_camera_requested and args.camera_angle_deg is None:
        raise SystemExit("--camera-angle-deg is required with a fixed camera")

    def camera_request(capture_index: int = 0) -> tuple[str, Dict[str, Any]]:
        if fixed_camera_requested:
            period = max(4, int(args.camera_pan_period_captures))
            phase = (2.0 * math.pi * float(capture_index % period)) / float(
                period
            )
            pan = math.sin(phase)
            return (
                "camera.fixed",
                {
                    "targetX": (
                        args.camera_target_x
                        + args.camera_pan_x_amplitude * pan
                    ),
                    "targetY": (
                        args.camera_target_y
                        + args.camera_pan_y_amplitude * pan
                    ),
                    "targetDistance": args.camera_distance,
                    "angleDegrees": args.camera_angle_deg,
                    "rotationDegrees": args.camera_rotation_deg,
                    "fieldOfViewDegrees": args.camera_fov_deg,
                    "farZ": args.camera_far_z,
                    "rollDegrees": 0.0,
                    "zOffset": 0.0,
                },
            )
        return (
            "camera.angle_of_attack",
            {"angleDegrees": args.camera_angle_deg},
        )

    try:
        extra_env = json.loads(args.extra_env_json)
        if not isinstance(extra_env, dict):
            raise ValueError("extra env must be a JSON object")
    except Exception as exc:
        raise SystemExit(f"invalid --extra-env-json: {exc}") from exc
    war3_dir = Path(args.war3_dir)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output = (
        Path(args.output)
        if args.output
        else Path(__file__).resolve().parent
        / "artifacts"
        / f"bridge_ramp_visual_probe_{args.label}_{stamp}.json"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    frame_dir = output.with_suffix("")
    frame_dir.mkdir(parents=True, exist_ok=True)
    desktop = f"War3CodexBridgeProbe_{int(time.time())}"

    env: Dict[str, str] = {
        "DXVK_WAR3_PERF_LEVEL": "1",
        # Exact per-stage producer census is intentionally limited to this
        # diagnostic runner. It is read on the same render-thread boundary as
        # each captured final frame and remains disabled in normal gameplay.
        "DXVK_WAR3_SHADOW_STAGE_HISTOGRAM": "1",
        "DXVK_WAR3_S1_FORCE_IDENTITY_WORLD": (
            "1" if args.force_identity_world else "0"
        ),
        "DXVK_WAR3_S1_TERRAIN_PERSISTENT_GEOMETRY": (
            "0" if args.disable_s1_persistent else "1"
        ),
        "DXVK_WAR3_SHADOW_TAA": "0" if args.disable_shadow_taa else "1",
        "DXVK_WAR3_SHADOW_DEBUG": str(max(0, args.shadow_debug_mode)),
        "DXVK_WAR3_CSM_CONTINUITY_TRACE": "1",
    }
    env.update({str(key): str(value) for key, value in extra_env.items()})
    if trace_ring_enabled:
        # Manual bounded segments own tracing in this mode. Prevent a launch-
        # time unlimited trace from racing the ring controller and set the
        # hash sample budget before the DLL reads its trace environment.
        env["DXVK_WAR3_SHADOW_POSE_FULL_TRACE"] = "0"
        env["DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTER_SAMPLE_BYTES"] = str(
            max(0, int(args.trace_ring_caster_sample_bytes))
        )
    result_box: Dict[str, Any] = {}

    def run_gate() -> None:
        try:
            result_box["result"] = war3.run_quick_autotest(
                war3_dir=str(war3_dir),
                map_path=args.map,
                ready_timeout_sec=max(1, args.ready_timeout_sec),
                sample_duration_sec=max(20, args.sample_sec),
                windowed=False,
                use_isolated_desktop=True,
                desktop_name=desktop,
                auto_perf_record=True,
                record_after_game_started=True,
                auto_perf_export_sec=8,
                deploy_d3d9_before_launch=True,
                build_d3d9_path=args.build,
                enforce_video_baseline=True,
                include_sections_in_report=True,
                section_top_n=160,
                avoid_focus_on_stop=True,
                profile="full_default",
                env_overrides_json=json.dumps(env, ensure_ascii=False),
                scenario_name=f"bridge_ramp_visual_probe_{args.label}",
                require_control_plane_ready=True,
                # This map intentionally contains no semantic unit caster at
                # startup. Its relevant hot contract is Stage1 + receiver,
                # which is polled below; the generic unit/skinned gate would
                # stop an otherwise healthy run before the capture sequence.
                require_hot_shadow_frame=False,
                hot_shadow_timeout_sec=max(1, args.ready_timeout_sec),
            )
        except BaseException as exc:
            result_box["exception"] = repr(exc)
            result_box["traceback"] = traceback.format_exc()

    worker = threading.Thread(target=run_gate, name="bridge-ramp-probe")
    worker.start()
    priority: Dict[str, Any] = {
        "ok": False,
        "error": "process did not publish a retained witness",
    }
    frames: List[Dict[str, Any]] = []
    frame_rows_by_path: Dict[str, Dict[str, Any]] = {}
    rolling_capture_paths: List[Path] = []
    pinned_capture_paths: set[str] = set()
    temporal_trigger_events: List[Dict[str, Any]] = []
    capture_ring_deletions: List[Dict[str, Any]] = []
    cleanup: Dict[str, Any] = {"ok": True, "skipped": True}
    camera_freeze: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not requested",
    }
    camera_resume: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "pause was not invoked",
    }
    sun_lock: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not requested",
    }
    camera_angle: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not requested",
        "refreshAttempts": 0,
        "refreshFailures": 0,
    }
    shadow_trace_witness: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not requested",
    }
    pause_invoked = False
    started_at = time.time()
    trace_log_dir = war3_dir / "WarVK" / "Log"
    trace_ring: Dict[str, Any] = {
        "enabled": bool(trace_ring_enabled),
        "segmentSec": float(args.trace_ring_segment_sec),
        "retainSegments": int(args.trace_ring_retain_segments),
        "segments": [],
        "control": [],
        "errors": [],
    }
    trace_segment_started_at = 0.0
    trace_segment_pin_on_close = False

    def prune_trace_ring() -> None:
        segments = trace_ring["segments"]
        retained_unpinned = [
            row
            for row in segments
            if not row.get("pinned") and row.get("retained", True)
        ]
        while len(retained_unpinned) > args.trace_ring_retain_segments:
            row = retained_unpinned.pop(0)
            path_text = str(row.get("path", "") or "")
            path = Path(path_text) if path_text else Path()
            safe_name = path.name.startswith("shadow_pose_full_trace_") and (
                path.suffix.lower() == ".jsonl"
            )
            fresh = False
            try:
                fresh = path.stat().st_mtime >= started_at - 2.0
            except OSError:
                pass
            if safe_name and fresh:
                outcome = _safe_unlink_probe_artifact(path, trace_log_dir)
            else:
                outcome = "refused: trace name or run freshness check failed"
            row["retained"] = outcome != "deleted"
            row["deleteOutcome"] = outcome
            if outcome.startswith("failed") or outcome.startswith("refused"):
                trace_ring["errors"].append(outcome)

    def start_trace_segment(pid: int, *, pin_on_close: bool = False) -> bool:
        nonlocal trace_segment_started_at, trace_segment_pin_on_close
        if not trace_ring_enabled:
            return False
        max_seconds = max(
            3,
            int(
                math.ceil(
                    args.trace_ring_segment_sec
                    + max(2.0, args.wait_for_shadow_trace_sec)
                )
            ),
        )
        response = war3._control_plane_request(
            pid=pid,
            command="start_shadow_pose_full_trace",
            payload={
                "maxSeconds": max_seconds,
                "includeMatrixBytes": False,
                "maxPoseRecords": max(
                    0, int(args.trace_ring_max_pose_records)
                ),
                "maxShadowObjectRecords": max(
                    0, int(args.trace_ring_max_shadow_object_records)
                ),
                "maxCurrentDrawRecords": max(
                    0, int(args.trace_ring_max_current_draw_records)
                ),
            },
            timeout_sec=6.0,
        )
        trace_ring["control"].append(
            {
                "action": "start",
                "atMs": int(time.time() * 1000),
                "ok": bool(response.get("ok")),
                "error": str(response.get("error", "") or ""),
                "result": dict(response.get("result", {}) or {}),
            }
        )
        if not response.get("ok"):
            trace_ring["errors"].append(
                str(response.get("error", "trace segment start failed"))
            )
            return False
        trace_segment_started_at = time.time()
        trace_segment_pin_on_close = bool(pin_on_close)
        return True

    def stop_trace_segment(
        pid: int,
        *,
        reason: str,
        pin: bool = False,
    ) -> Dict[str, Any]:
        nonlocal trace_segment_started_at, trace_segment_pin_on_close
        if not trace_ring_enabled or trace_segment_started_at <= 0.0:
            return {"ok": True, "skipped": True, "reason": "not active"}
        stopped_at = time.time()
        response = war3._control_plane_request(
            pid=pid,
            command="stop_shadow_pose_full_trace",
            payload={},
            timeout_sec=6.0,
        )
        result = dict(response.get("result", {}) or {})
        original_path = str(result.get("path", "") or "")
        archived_path = original_path
        archive_outcome = "skipped: no trace path"
        if original_path:
            source = Path(original_path)
            try:
                source_resolved = source.resolve()
                log_resolved = trace_log_dir.resolve()
                fresh = source_resolved.stat().st_mtime >= started_at - 2.0
                safe_name = source_resolved.name.startswith(
                    "shadow_pose_full_trace_"
                ) and source_resolved.suffix.lower() == ".jsonl"
                if source_resolved.parent != log_resolved:
                    archive_outcome = "refused: trace is outside log directory"
                elif not safe_name or not fresh:
                    archive_outcome = (
                        "refused: trace name or run freshness check failed"
                    )
                else:
                    safe_label = re.sub(r"[^A-Za-z0-9_-]+", "_", args.label)
                    destination = source_resolved.with_name(
                        f"{source_resolved.stem}_{safe_label}_"
                        f"seg{len(trace_ring['segments']):04d}_"
                        f"{int(stopped_at * 1000)}.jsonl"
                    )
                    source_resolved.replace(destination)
                    archived_path = str(destination)
                    archive_outcome = "renamed"
                    if str(shadow_trace_witness.get("path", "")) == original_path:
                        shadow_trace_witness["originalPath"] = original_path
                        shadow_trace_witness["path"] = archived_path
                        shadow_trace_witness["renamedPath"] = archived_path
            except Exception as exc:
                archive_outcome = f"failed: {exc}"
                trace_ring["errors"].append(archive_outcome)
        row = {
            "reason": reason,
            "startedAtMs": int(trace_segment_started_at * 1000),
            "stoppedAtMs": int(stopped_at * 1000),
            "elapsedSec": round(stopped_at - trace_segment_started_at, 3),
            "ok": bool(response.get("ok")),
            "error": str(response.get("error", "") or ""),
            "path": archived_path,
            "originalPath": original_path,
            "archiveOutcome": archive_outcome,
            "frameEventsWritten": int(
                result.get("frameEventsWritten", 0) or 0
            ),
            "recordEventsWritten": int(
                result.get("recordEventsWritten", 0) or 0
            ),
            "pinned": bool(pin or trace_segment_pin_on_close),
            "retained": True,
        }
        trace_ring["segments"].append(row)
        trace_ring["control"].append(
            {
                "action": "stop",
                "atMs": int(stopped_at * 1000),
                "ok": bool(response.get("ok")),
                "error": row["error"],
                "result": result,
            }
        )
        if not response.get("ok"):
            trace_ring["errors"].append(
                str(response.get("error", "trace segment stop failed"))
            )
        trace_segment_started_at = 0.0
        trace_segment_pin_on_close = False
        prune_trace_ring()
        return row

    def prune_capture_ring() -> None:
        if args.capture_retain_count <= 0:
            return
        while True:
            unpinned = [
                path
                for path in rolling_capture_paths
                if str(path.resolve()) not in pinned_capture_paths
                and path.exists()
            ]
            if len(unpinned) <= args.capture_retain_count:
                return
            path = unpinned[0]
            outcome = _safe_unlink_probe_artifact(path, frame_dir)
            capture_ring_deletions.append(
                {"path": str(path), "outcome": outcome}
            )
            row = frame_rows_by_path.get(str(path.resolve()))
            if row is not None:
                row["retained"] = outcome != "deleted"
                row["deleteOutcome"] = outcome
            if outcome == "deleted":
                rolling_capture_paths.remove(path)
            if outcome != "deleted":
                return

    try:
        deadline = time.time() + max(60, args.ready_timeout_sec)
        while worker.is_alive() and time.time() < deadline:
            attempt = gate._lower_owned_process_priority()
            if attempt.get("ok"):
                priority = attempt
                break
            if not attempt.get("retry", False):
                priority = attempt
                break
            time.sleep(0.05)

        ready = False
        pid = int(war3.STATE.war3_pid or 0)
        while worker.is_alive() and time.time() < deadline and pid > 0:
            status = war3._control_plane_request(
                pid=pid,
                command="get_runtime_status",
                payload={},
                timeout_sec=2.0,
            )
            status_result = dict(status.get("result", {}) or {})
            runtime = dict(status_result.get("runtime", {}) or {})
            shadow = dict(status_result.get("shadow", {}) or {})
            if (
                status.get("ok")
                and bool(runtime.get("gameStarted", False))
                and int(
                    shadow.get("semanticSceneShadowMapExecutedThisFrame", 0)
                    or 0
                )
                != 0
                and int(
                    shadow.get("semanticSceneReceiverActiveStrengthMilli", 0)
                    or 0
                )
                > 0
            ):
                ready = True
                break
            time.sleep(0.10)

        if ready:
            # Wait for a bridge-heavy Stage13 frame. This avoids freezing the
            # test while the camera is over empty terrain and makes the
            # current/history/final factor captures directly comparable.
            stage13_witness: Dict[str, Any] = {}
            if not args.skip_stage13_witness:
                stage13_wait_deadline = time.time() + 20.0
                while (
                    worker.is_alive()
                    and time.time() < stage13_wait_deadline
                ):
                    witness = war3._control_plane_request(
                        pid=pid,
                        command="get_hot_shadow_probe",
                        payload={},
                        timeout_sec=2.0,
                    )
                    probe = dict(witness.get("result", {}) or {})
                    shadow_summary = dict(
                        probe.get("shadowRuntimeSummary", {}) or {}
                    )
                    stage13_witness = {
                        "stage13CaptureConsideredCount": int(
                            shadow_summary.get(
                                "stage13CaptureConsideredCount", 0
                            )
                            or 0
                        ),
                        "semanticSceneReplayDrawsCount": int(
                            shadow_summary.get(
                                "semanticSceneReplayDrawsCount", 0
                            )
                            or 0
                        ),
                    }
                    if (
                        stage13_witness[
                            "stage13CaptureConsideredCount"
                        ]
                        >= 20
                        and stage13_witness[
                            "semanticSceneReplayDrawsCount"
                        ]
                        > 0
                    ):
                        break
                    time.sleep(0.05)
            if args.lock_sun_time is not None:
                sun_lock = war3._invoke_internal_test_request(
                    pid=pid,
                    war3_dir=war3_dir,
                    command="shadow.lock_sun",
                    payload={
                        "enabled": True,
                        "time01": max(0.0, min(1.0, args.lock_sun_time)),
                    },
                    timeout_sec=6.0,
                )
                # Let the new light direction settle through CSM, current
                # visibility and history before the first exact capture.
                time.sleep(0.25)
            if args.freeze_scripted_camera:
                camera_freeze = war3._invoke_internal_test_request(
                    pid=pid,
                    war3_dir=war3_dir,
                    command="game.pause",
                    payload={"paused": True},
                    timeout_sec=6.0,
                )
                camera_freeze["stage13WitnessBeforePause"] = stage13_witness
                pause_invoked = bool(camera_freeze.get("ok"))
                # Drain the last in-flight simulation frame before correlating
                # Stage13 and shadow history on a fixed scene.
                time.sleep(0.25)
            if args.camera_angle_deg is not None:
                camera_command, camera_payload = camera_request(0)
                camera_angle = war3._invoke_internal_test_request(
                    pid=pid,
                    war3_dir=war3_dir,
                    command=camera_command,
                    payload=camera_payload,
                    timeout_sec=6.0,
                )
                camera_angle["refreshAttempts"] = 1
                camera_angle["refreshFailures"] = (
                    0 if camera_angle.get("ok") else 1
                )
                # Let view/CSM/current visibility settle after the first
                # instantaneous camera-field override.
                time.sleep(0.25)
            if trace_ring_enabled:
                start_trace_segment(pid)
            shadow_trace_witness = _wait_for_final_caster_trace(
                war3_dir / "WarVK" / "Log",
                started_at,
                max(0.0, args.wait_for_shadow_trace_sec),
                worker,
            )
            capture_count = (
                max(1, args.capture_count)
                if shadow_trace_witness.get("ok")
                else 0
            )
            previous_capture_path: Path | None = None
            previous_capture_index = -1
            first_trigger_index = -1
            trace_pin_until_index = -1
            for index in range(capture_count):
                if not worker.is_alive():
                    break
                if (
                    trace_ring_enabled
                    and trace_segment_started_at > 0.0
                    and time.time() - trace_segment_started_at
                    >= args.trace_ring_segment_sec
                ):
                    pin_segment = bool(
                        trace_segment_pin_on_close
                        or index <= trace_pin_until_index
                    )
                    stop_trace_segment(
                        pid,
                        reason="periodic-rotation",
                        pin=pin_segment,
                    )
                    start_trace_segment(
                        pid,
                        pin_on_close=index <= trace_pin_until_index,
                    )
                if args.camera_angle_deg is not None:
                    camera_command, camera_payload = camera_request(index)
                    refreshed = war3._invoke_internal_test_request(
                        pid=pid,
                        war3_dir=war3_dir,
                        command=camera_command,
                        payload=camera_payload,
                        timeout_sec=6.0,
                    )
                    camera_angle["refreshAttempts"] = int(
                        camera_angle.get("refreshAttempts", 0) or 0
                    ) + 1
                    if not refreshed.get("ok"):
                        camera_angle["refreshFailures"] = int(
                            camera_angle.get("refreshFailures", 0) or 0
                        ) + 1
                        camera_angle["lastRefreshError"] = str(
                            refreshed.get("error", "") or ""
                        )
                capture_path = frame_dir / f"{args.label}_{index:03d}.bmp"
                captured = war3._capture_final_frame_via_internal_test_api(
                    pid=pid,
                    war3_dir=war3_dir,
                    output_path=capture_path,
                    timeout_sec=8.0,
                )
                exact = dict(
                    ((captured.get("details", {}) or {}).get("result", {}) or {})
                )
                frame_row = {
                    "index": index,
                    "capturedAtMs": int(time.time() * 1000),
                    "ok": bool(captured.get("ok")),
                    "output": str(captured.get("output", "") or ""),
                    "elapsedSec": captured.get("elapsedSec", 0.0),
                    "error": str(captured.get("error", "") or ""),
                    "exactFrame": exact,
                    "retained": False,
                }
                frames.append(frame_row)
                actual_capture_path = Path(frame_row["output"])
                if frame_row["ok"] and actual_capture_path.is_file():
                    resolved_text = str(actual_capture_path.resolve())
                    frame_row["retained"] = True
                    frame_rows_by_path[resolved_text] = frame_row
                    rolling_capture_paths.append(actual_capture_path)

                    trigger_result: Dict[str, Any] = {}
                    trigger_allowed = bool(
                        args.temporal_dark_trigger_pixels > 0
                        and args.max_trigger_events > 0
                        and len(temporal_trigger_events)
                        < args.max_trigger_events
                        and previous_capture_path is not None
                        and previous_capture_path.is_file()
                    )
                    if trigger_allowed:
                        trigger_result = _analyze_pairwise_dark_trigger(
                            previous_capture_path,
                            actual_capture_path,
                            target_width=args.temporal_dark_trigger_width,
                            scene_ratio=args.temporal_dark_trigger_scene_ratio,
                            luma_threshold=(
                                args.temporal_dark_trigger_threshold
                            ),
                        )
                        frame_row["temporalTriggerAnalysis"] = trigger_result
                    triggered = bool(
                        trigger_result.get("ok")
                        and int(
                            trigger_result.get(
                                "largestDarkComponent", 0
                            )
                            or 0
                        )
                        >= args.temporal_dark_trigger_pixels
                    )
                    if triggered:
                        previous_resolved = str(
                            previous_capture_path.resolve()
                        )
                        pinned_capture_paths.update(
                            {previous_resolved, resolved_text}
                        )
                        previous_row = frame_rows_by_path.get(
                            previous_resolved
                        )
                        if previous_row is not None:
                            previous_row["triggerPinned"] = True
                        frame_row["triggerPinned"] = True
                        event = {
                            **trigger_result,
                            "eventIndex": len(temporal_trigger_events),
                            "previousCaptureIndex": previous_capture_index,
                            "captureIndex": index,
                            "triggerPixels": int(
                                args.temporal_dark_trigger_pixels
                            ),
                            "previousExactFrame": (
                                dict(previous_row.get("exactFrame", {}) or {})
                                if previous_row is not None
                                else {}
                            ),
                            "currentExactFrame": exact,
                        }
                        temporal_trigger_events.append(event)
                        if first_trigger_index < 0:
                            first_trigger_index = index
                        trace_pin_until_index = max(
                            trace_pin_until_index,
                            index
                            + max(
                                1,
                                args.stop_after_trigger_post_captures,
                            ),
                        )
                        if trace_ring_enabled:
                            # Pin the preceding closed window before stopping
                            # the active event window, then start one bounded
                            # post-event segment.
                            retained_segments = [
                                row
                                for row in trace_ring["segments"]
                                if row.get("retained", True)
                            ]
                            if retained_segments:
                                retained_segments[-1]["pinned"] = True
                            stop_trace_segment(
                                pid,
                                reason="temporal-dark-trigger",
                                pin=True,
                            )
                            prune_trace_ring()
                            start_trace_segment(pid, pin_on_close=True)

                    if (
                        first_trigger_index >= 0
                        and args.stop_after_trigger_post_captures > 0
                        and index
                        <= first_trigger_index
                        + args.stop_after_trigger_post_captures
                    ):
                        pinned_capture_paths.add(resolved_text)
                        frame_row["triggerPostPinned"] = True

                    previous_capture_path = actual_capture_path
                    previous_capture_index = index
                    prune_capture_ring()

                if (
                    first_trigger_index >= 0
                    and args.stop_after_trigger_post_captures > 0
                    and index
                    >= first_trigger_index
                    + args.stop_after_trigger_post_captures
                ):
                    break
                time.sleep(max(0.0, args.capture_interval_sec))

            if trace_ring_enabled:
                stop_trace_segment(
                    pid,
                    reason="capture-loop-complete",
                    pin=bool(trace_segment_pin_on_close),
                )

        worker.join()
    finally:
        if int(war3.STATE.war3_pid or 0) > 0:
            if trace_ring_enabled and trace_segment_started_at > 0.0:
                stop_trace_segment(
                    int(war3.STATE.war3_pid or 0),
                    reason="finally",
                    pin=bool(trace_segment_pin_on_close),
                )
            if pause_invoked:
                camera_resume = war3._invoke_internal_test_request(
                    pid=int(war3.STATE.war3_pid or 0),
                    war3_dir=war3_dir,
                    command="game.pause",
                    payload={"paused": False},
                    timeout_sec=6.0,
                )
            cleanup = gate._cleanup_owned_process()

    payload = {
        "ok": bool(
            priority.get("ok")
            and frames
            and all(row.get("ok") for row in frames)
            and (
                args.wait_for_shadow_trace_sec <= 0.0
                or shadow_trace_witness.get("ok")
            )
            and (
                args.camera_angle_deg is None
                or (
                    camera_angle.get("ok")
                    and int(camera_angle.get("refreshFailures", 0) or 0) == 0
                )
            )
            and (
                not trace_ring_enabled
                or (
                    bool(trace_ring.get("segments"))
                    and not trace_ring.get("errors")
                )
            )
            and isinstance(result_box.get("result"), dict)
            and result_box["result"].get("ok")
        ),
        "label": args.label,
        "forceIdentityWorld": bool(args.force_identity_world),
        "freezeScriptedCamera": bool(args.freeze_scripted_camera),
        "skipStage13Witness": bool(args.skip_stage13_witness),
        "cameraAngleDegrees": args.camera_angle_deg,
        "fixedCameraRequested": fixed_camera_requested,
        "cameraTargetX": args.camera_target_x,
        "cameraTargetY": args.camera_target_y,
        "cameraDistance": args.camera_distance,
        "cameraRotationDegrees": args.camera_rotation_deg,
        "cameraFieldOfViewDegrees": args.camera_fov_deg,
        "cameraFarZ": args.camera_far_z,
        "cameraPanXAmplitude": args.camera_pan_x_amplitude,
        "cameraPanYAmplitude": args.camera_pan_y_amplitude,
        "cameraPanPeriodCaptures": args.camera_pan_period_captures,
        "captureRetainCount": args.capture_retain_count,
        "captureRingDeletions": capture_ring_deletions,
        "retainedCaptureCount": sum(
            1 for row in frames if row.get("retained")
        ),
        "temporalDarkTriggerPixels": args.temporal_dark_trigger_pixels,
        "temporalDarkTriggerThreshold": (
            args.temporal_dark_trigger_threshold
        ),
        "temporalDarkTriggerWidth": args.temporal_dark_trigger_width,
        "temporalDarkTriggerSceneRatio": (
            args.temporal_dark_trigger_scene_ratio
        ),
        "temporalTriggerEvents": temporal_trigger_events,
        "traceRing": trace_ring,
        "lockSunTime": args.lock_sun_time,
        "sunLock": sun_lock,
        "cameraFreeze": camera_freeze,
        "cameraResume": camera_resume,
        "cameraAngle": camera_angle,
        "shadowTraceWitness": shadow_trace_witness,
        "environment": env,
        "desktop": desktop,
        "startedAt": datetime.fromtimestamp(started_at).isoformat(),
        "elapsedSec": round(time.time() - started_at, 3),
        "priorityOverride": priority,
        "frames": frames,
        "cleanup": cleanup,
        **result_box,
    }
    output.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, default=str),
        encoding="utf-8",
    )
    print(str(output))
    print(
        json.dumps(
            {
                "ok": payload["ok"],
                "frames": len(frames),
                "forceIdentityWorld": payload["forceIdentityWorld"],
                "priority": priority,
            },
            ensure_ascii=False,
        )
    )
    return 0 if payload["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
