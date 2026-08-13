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


PROCESS_SET_INFORMATION = 0x0200

# 性能门默认保持与 war3_autotest_mcp 启动器一致的 High。后台/隔离桌面
# 视觉正确性任务若确有需要，必须显式选择 below-normal；不能再静默把性能
# 样本降到 BELOW_NORMAL。
PROCESS_PRIORITY_CLASSES = {
    "high": (0x00000080, "HIGH"),
    "normal": (0x00000020, "NORMAL"),
    "below-normal": (0x00004000, "BELOW_NORMAL"),
}


def _set_owned_process_priority(priority_name: str) -> Dict[str, Any]:
    """Set one retained AutoTest process to an explicit, verified priority class."""
    normalized = str(priority_name or "").strip().lower()
    target = PROCESS_PRIORITY_CLASSES.get(normalized)
    if target is None:
        return {
            "ok": False,
            "retry": False,
            "error": f"unsupported process priority: {priority_name!r}",
            "supported": sorted(PROCESS_PRIORITY_CLASSES),
        }
    target_class, target_name = target

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
                target_class,
            )
        )
        observed = int(
            kernel32.GetPriorityClass(wintypes.HANDLE(handle_value)) or 0
        )
        return {
            "ok": changed and observed == target_class,
            "retry": False,
            "pid": pid,
            "priority": target_name,
            "requested": normalized,
            "observedClass": observed,
            "win32Error": 0 if changed else int(ctypes.get_last_error()),
        }
    finally:
        kernel32.CloseHandle(wintypes.HANDLE(handle_value))


def _lower_owned_process_priority() -> Dict[str, Any]:
    """兼容旧的视觉探针；性能门不得默认调用此包装。"""
    return _set_owned_process_priority("below-normal")


def _reconcile_priority_evidence(
    requested: str,
    observed: Dict[str, Any],
    result: Any,
) -> Dict[str, Any]:
    """Close a short-run witness race without weakening priority policy."""
    if observed.get("ok"):
        return observed
    if str(requested).strip().lower() != "high" or not isinstance(result, dict):
        return observed
    launch_priority = result.get("launch", {}).get("priority", {})
    if not isinstance(launch_priority, dict):
        return observed
    if not launch_priority.get("ok") or launch_priority.get("priority") != "HIGH":
        return observed
    return {
        "ok": True,
        "retry": False,
        "pid": int(launch_priority.get("pid", 0) or 0),
        "priority": "HIGH",
        "requested": "high",
        "source": "launcher-owned-process",
        "outerWitnessRace": True,
    }


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
    parser.add_argument(
        "--process-priority",
        choices=tuple(PROCESS_PRIORITY_CLASSES),
        default="high",
        help=(
            "Priority class for the owned War3 process. Performance gates default "
            "to high; below-normal is only for explicit background correctness runs."
        ),
    )
    parser.add_argument(
        "--background-idle-sleep",
        choices=("disabled", "native"),
        default="disabled",
        help=(
            "Whether the verified WM_ACTIVATEAPP background idle Sleep stays "
            "disabled for AutoTest or uses native game behavior for an A/B run."
        ),
    )
    parser.add_argument("--desktop", default="")
    parser.add_argument("--no-hot-shadow", action="store_true")
    parser.add_argument(
        "--allow-final-shadow-publication-only",
        action="store_true",
        help=(
            "Accept an exact-owner hot frame only when sealed producer, replay, "
            "four-cascade draw, identity, and receiver publication contracts all close."
        ),
    )
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
    # 由 Game.dll+0x1552E0 的精确 Hook 消除 WM_ACTIVATEAPP 的空闲 Sleep。
    # CLI 明确优先于 extra-env-json，确保报告中的性能策略可复现。
    extra_env["DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE"] = (
        "1" if args.background_idle_sleep == "disabled" else "0"
    )
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
                allow_final_shadow_publication_only=(
                    args.allow_final_shadow_publication_only
                ),
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
            attempt = _set_owned_process_priority(args.process_priority)
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

    priority_box = _reconcile_priority_evidence(
        args.process_priority,
        priority_box,
        result_box.get("result"),
    )

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
        "backgroundThrottle": {
            "environmentVariable": "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE",
            "mode": args.background_idle_sleep,
            "value": extra_env.get(
                "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE", ""
            ),
        },
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
