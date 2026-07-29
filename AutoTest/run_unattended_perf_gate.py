#!/usr/bin/env python3
"""Run one isolated War3 perf gate without relying on a long-lived MCP process.

The launch, retained native process HANDLE, liveness checks, priority override,
report export, and cleanup all live in this single fresh Python process.  This
is important for isolated desktops, where a weak OpenProcess/tasklist probe can
otherwise mistake an inaccessible process for a dead one.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import threading
import time
import traceback
from datetime import datetime
from pathlib import Path
from typing import Any, Dict

import war3_autotest_mcp as war3


BELOW_NORMAL_PRIORITY_CLASS = 0x00004000
PROCESS_SET_INFORMATION = 0x0200


def _lower_owned_process_priority() -> Dict[str, Any]:
    """Lower only the exact process identified by STATE's retained witness."""
    pid = int(war3.STATE.war3_pid or 0)
    witness = war3.STATE.retained_native_process
    if pid <= 0 or witness is None:
        return {"ok": False, "retry": True, "error": "launch state not published"}

    expected = dict(witness.snapshot() or {})
    if expected.get("available") is not True:
        return {"ok": False, "retry": True, "error": "native witness unavailable"}

    kernel32 = war3._KERNEL32
    kernel32.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.SetPriorityClass.restype = wintypes.BOOL
    kernel32.GetPriorityClass.argtypes = [wintypes.HANDLE]
    kernel32.GetPriorityClass.restype = wintypes.DWORD

    access = PROCESS_SET_INFORMATION | war3.PROCESS_QUERY_LIMITED_INFORMATION
    handle = kernel32.OpenProcess(access, False, pid)
    handle_value = war3._native_handle_value(handle)
    if handle_value <= 0:
        return {
            "ok": False,
            "retry": True,
            "error": f"OpenProcess failed: {int(ctypes.get_last_error())}",
        }

    try:
        actual = war3._native_process_binding_from_handle(handle_value)
        exact = bool(
            actual.get("ok") is True
            and int(actual.get("pid", 0)) == int(expected.get("pid", 0))
            and int(actual.get("creationEpochMs", 0))
            == int(expected.get("creationEpochMs", 0))
            and war3._canonical_process_path(actual.get("canonicalExePath", ""))
            == war3._canonical_process_path(expected.get("canonicalExePath", ""))
        )
        if not exact:
            return {
                "ok": False,
                "retry": False,
                "error": "priority target identity mismatch",
                "expected": expected,
                "actual": actual,
            }

        changed = bool(
            kernel32.SetPriorityClass(
                wintypes.HANDLE(handle_value),
                BELOW_NORMAL_PRIORITY_CLASS,
            )
        )
        observed = int(
            kernel32.GetPriorityClass(wintypes.HANDLE(handle_value)) or 0
        )
        return {
            "ok": changed and observed == BELOW_NORMAL_PRIORITY_CLASS,
            "retry": False,
            "pid": pid,
            "priority": "BELOW_NORMAL",
            "observedClass": observed,
            "win32Error": 0 if changed else int(ctypes.get_last_error()),
        }
    finally:
        kernel32.CloseHandle(wintypes.HANDLE(handle_value))


def _cleanup_owned_process() -> Dict[str, Any]:
    pid = int(war3.STATE.war3_pid or 0)
    if pid <= 0:
        return {"ok": True, "skipped": True, "reason": "no owned process"}
    return war3.stop_war3(
        pid=pid,
        graceful_wait_sec=3,
        force=True,
        avoid_foreground_switch=True,
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--war3-dir", default=r"E:\Work\War3")
    parser.add_argument(
        "--map",
        default=r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x",
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
    parser.add_argument("--sample-sec", type=int, default=45)
    parser.add_argument("--ready-timeout-sec", type=int, default=180)
    parser.add_argument("--hot-shadow-timeout-sec", type=int, default=180)
    parser.add_argument("--perf-level", type=int, default=1)
    parser.add_argument("--profile", default="full_default")
    parser.add_argument("--scenario", default="night_unattended_gate")
    parser.add_argument("--desktop", default="")
    parser.add_argument("--no-hot-shadow", action="store_true")
    parser.add_argument("--extra-env-json", default="{}")
    parser.add_argument("--section-top-n", type=int, default=300)
    parser.add_argument("--output", default="")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    try:
        extra_env = json.loads(args.extra_env_json)
        if not isinstance(extra_env, dict):
            raise ValueError("--extra-env-json must decode to an object")
    except Exception as exc:
        raise SystemExit(f"invalid --extra-env-json: {exc}") from exc

    extra_env.setdefault("DXVK_WAR3_PERF_LEVEL", str(max(0, args.perf_level)))
    desktop = args.desktop or f"War3CodexNight_{int(time.time())}"
    output = (
        Path(args.output)
        if args.output
        else Path(__file__).resolve().parent
        / "artifacts"
        / f"unattended_gate_{datetime.now():%Y%m%d_%H%M%S}.json"
    )
    output.parent.mkdir(parents=True, exist_ok=True)

    result_box: Dict[str, Any] = {}
    priority_box: Dict[str, Any] = {
        "ok": False,
        "error": "process did not publish a retained witness",
    }

    def run_gate() -> None:
        try:
            result_box["result"] = war3.run_quick_autotest(
                war3_dir=args.war3_dir,
                map_path=args.map,
                ready_timeout_sec=max(1, args.ready_timeout_sec),
                sample_duration_sec=max(3, args.sample_sec),
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
                section_top_n=max(1, args.section_top_n),
                avoid_focus_on_stop=True,
                profile=args.profile,
                env_overrides_json=json.dumps(extra_env, ensure_ascii=False),
                scenario_name=args.scenario,
                require_control_plane_ready=True,
                require_hot_shadow_frame=not args.no_hot_shadow,
                hot_shadow_timeout_sec=max(1, args.hot_shadow_timeout_sec),
            )
        except BaseException as exc:
            result_box["exception"] = repr(exc)
            result_box["traceback"] = traceback.format_exc()

    worker = threading.Thread(target=run_gate, name="war3-perf-gate")
    started_at = time.time()
    worker.start()
    cleanup: Dict[str, Any] = {"ok": True, "skipped": True}
    try:
        priority_deadline = time.time() + max(60, args.ready_timeout_sec)
        while worker.is_alive() and time.time() < priority_deadline:
            attempt = _lower_owned_process_priority()
            if attempt.get("ok"):
                priority_box = attempt
                break
            if not attempt.get("retry", False):
                priority_box = attempt
                cleanup = _cleanup_owned_process()
                break
            time.sleep(0.05)

        if worker.is_alive() and not priority_box.get("ok"):
            cleanup = _cleanup_owned_process()

        worker.join()
    finally:
        if int(war3.STATE.war3_pid or 0) > 0:
            cleanup = _cleanup_owned_process()

    payload = {
        "ok": bool(
            priority_box.get("ok")
            and isinstance(result_box.get("result"), dict)
            and result_box["result"].get("ok")
        ),
        "startedAt": datetime.fromtimestamp(started_at).isoformat(),
        "elapsedSec": round(time.time() - started_at, 3),
        "desktop": desktop,
        "priorityOverride": priority_box,
        "cleanup": cleanup,
        **result_box,
    }
    output.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, default=str),
        encoding="utf-8",
    )
    print(str(output))
    print(json.dumps({
        "ok": payload["ok"],
        "elapsedSec": payload["elapsedSec"],
        "priorityOverride": priority_box,
        "stage": (
            result_box.get("result", {}).get("stage")
            if isinstance(result_box.get("result"), dict)
            else "exception"
        ),
    }, ensure_ascii=False))
    return 0 if payload["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
