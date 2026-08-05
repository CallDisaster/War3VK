#!/usr/bin/env python3
"""Attach-only A->B->A shadow lifecycle recorder.

The user owns every map transition. This process never launches, focuses,
reprioritizes, pauses, or terminates Warcraft III. It only polls WarVK's
control pipe, asks the existing low-disk evidence ring to retain transition
segments, and captures bounded screenshots.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict

import war3_autotest_mcp as war3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--attach-pid", type=int, required=True)
    parser.add_argument("--war3-dir", default=r"E:\Work\Warcraft III")
    parser.add_argument("--duration-sec", type=float, default=900.0)
    parser.add_argument("--poll-sec", type=float, default=0.25)
    parser.add_argument("--screenshots", type=int, default=160)
    parser.add_argument("--output-dir", default="")
    parser.add_argument(
        "--retain-file",
        default="",
        help="Touch this file to pin the current low-disk evidence window.",
    )
    return parser.parse_args()


def gpu_events() -> str:
    script = (
        "Get-WinEvent -FilterHashtable @{LogName='System'; Id=153,4101} "
        "-MaxEvents 64 -ErrorAction SilentlyContinue | "
        "Select-Object TimeCreated,Id,ProviderName,LevelDisplayName,Message | "
        "ConvertTo-Json -Depth 3"
    )
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    return result.stdout if result.returncode == 0 else result.stderr


def request(pid: int, command: str, payload: Dict[str, Any] | None = None) -> Dict[str, Any]:
    return war3._control_plane_request(
        pid=pid, command=command, payload=payload or {}, timeout_sec=2.0
    )


def main() -> int:
    args = parse_args()
    pid = int(args.attach_pid)
    if pid <= 0 or not war3._pid_alive(pid):
        raise SystemExit(f"attach-only target is not alive: {pid}")
    if args.duration_sec <= 0.0 or args.poll_sec < 0.05:
        raise SystemExit("duration must be positive and poll-sec >= 0.05")

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    root = Path(args.output_dir) if args.output_dir else (
        Path(__file__).resolve().parent / "artifacts" / "cross_map" / stamp
    )
    shots = root / "screenshots"
    root.mkdir(parents=True, exist_ok=True)
    shots.mkdir(parents=True, exist_ok=True)
    (root / "gpu_events_before.json.txt").write_text(
        gpu_events(), encoding="utf-8"
    )

    initial = request(pid, "get_runtime_status")
    initial_status = dict((initial.get("result") or {}).get("runtimeStatus") or {})
    initial_shadow = dict(initial_status.get("shadow") or {})
    collector_was_attached = bool(
        int(initial_shadow.get("shadowEvidenceCollectorAttached", 0) or 0)
    )
    request(pid, "set_shadow_evidence_collector", {"attached": True})

    sample_path = root / "runtime_samples.jsonl"
    transition_path = root / "transitions.jsonl"
    start = time.monotonic()
    deadline = start + float(args.duration_sec)
    shot_interval = float(args.duration_sec) / max(1, int(args.screenshots))
    next_shot = start
    shot_count = 0
    sample_count = 0
    last_epoch = int(initial_shadow.get("shadowMapEpoch", 0) or 0)
    retain_path = Path(args.retain_file) if args.retain_file else None
    retain_mtime = retain_path.stat().st_mtime_ns if retain_path and retain_path.exists() else 0
    last_status: Dict[str, Any] = initial_status

    try:
        with sample_path.open("w", encoding="utf-8") as samples, transition_path.open(
            "w", encoding="utf-8"
        ) as transitions:
            while time.monotonic() < deadline and war3._pid_alive(pid):
                now = time.monotonic()
                reply = request(pid, "get_runtime_status")
                status = dict((reply.get("result") or {}).get("runtimeStatus") or {})
                if status:
                    last_status = status
                    shadow = dict(status.get("shadow") or {})
                    row = {
                        "elapsedSec": round(now - start, 3),
                        "timestampMs": int(status.get("timestampMs", 0) or 0),
                        "frameIndex": int(status.get("frameIndex", 0) or 0),
                        "shadow": shadow,
                    }
                    samples.write(json.dumps(row, ensure_ascii=False) + "\n")
                    samples.flush()
                    sample_count += 1
                    epoch = int(shadow.get("shadowMapEpoch", 0) or 0)
                    if epoch and epoch != last_epoch:
                        retained = request(pid, "retain_shadow_evidence")
                        transitions.write(
                            json.dumps(
                                {
                                    "elapsedSec": round(now - start, 3),
                                    "oldEpoch": last_epoch,
                                    "newEpoch": epoch,
                                    "status": shadow,
                                    "retention": retained,
                                },
                                ensure_ascii=False,
                            )
                            + "\n"
                        )
                        transitions.flush()
                        last_epoch = epoch

                if retain_path and retain_path.exists():
                    current_mtime = retain_path.stat().st_mtime_ns
                    if current_mtime != retain_mtime:
                        retain_mtime = current_mtime
                        request(pid, "retain_shadow_evidence")

                if shot_count < int(args.screenshots) and now >= next_shot:
                    output = shots / f"frame_{shot_count:04d}.png"
                    war3.capture_war3_screenshot(
                        output_path=str(output),
                        pid=pid,
                        war3_dir=str(args.war3_dir),
                        prefer_internal=True,
                        timeout_sec=4,
                        fallback_to_window_capture=False,
                    )
                    shot_count += 1
                    next_shot += shot_interval
                time.sleep(float(args.poll_sec))
    finally:
        if not collector_was_attached and war3._pid_alive(pid):
            request(pid, "set_shadow_evidence_collector", {"attached": False})
        (root / "gpu_events_after.json.txt").write_text(
            gpu_events(), encoding="utf-8"
        )

    summary = {
        "attachOnly": True,
        "pid": pid,
        "durationSec": round(time.monotonic() - start, 3),
        "sampleCount": sample_count,
        "screenshotCount": shot_count,
        "lastStatus": last_status,
        "processStillAlive": bool(war3._pid_alive(pid)),
        "neverLaunchesOrStopsGame": True,
    }
    (root / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if summary["processStillAlive"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
