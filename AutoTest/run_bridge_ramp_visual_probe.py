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


def main() -> int:
    args = _parse_args()
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
            for index in range(capture_count):
                if not worker.is_alive():
                    break
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
                frames.append(
                    {
                        "index": index,
                        "capturedAtMs": int(time.time() * 1000),
                        "ok": bool(captured.get("ok")),
                        "output": str(captured.get("output", "") or ""),
                        "elapsedSec": captured.get("elapsedSec", 0.0),
                        "error": str(captured.get("error", "") or ""),
                        "exactFrame": exact,
                    }
                )
                time.sleep(max(0.0, args.capture_interval_sec))

        worker.join()
    finally:
        if int(war3.STATE.war3_pid or 0) > 0:
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
