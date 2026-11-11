"""Isolated, single-process WarVK Walk JAPI/hook probe for the copied test map.

The player map and DLL are read-only inputs. AutoTest owns a content-addressed
instance, isolated desktop, game process and its teardown. No global input.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import time

import war3_autotest_mcp as war3


ROOT = Path(__file__).resolve().parents[1]
PLAYER = Path("E:/Work/Warcraft III")
MAP = PLAYER / "Maps/Test/WorldEditTestMap.w3x"
PROBE_MAP = ROOT / "AutoTest/artifacts/v122_visual_api_20260914/unit-walk-probe-20260925/unit-walk-probe.w3x"
DLL = ROOT / "build32/src/d3d9/d3d9.dll"
ARTIFACTS = ROOT / "AutoTest/artifacts"
RUN_ID = "unit-walk-probe-20260925-d"
SESSION_ID = "walk"
PINNED = {
    MAP: "AB4402A0EB07176B9858707A1BE3929586281781EE95626792F459642C56C3D4",
    PROBE_MAP: "DA8C842BB5FE22D0F3AEAAAFDCA7DA8C3FA7604A9F10FA800A1E2BEA900EC0BC",
    DLL: "1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF",
    PLAYER / "d3d9.dll": "FF3B7BD521F95935DB204EF2FEBAB828BDE85BA743E509292B9FA9F91B05F003",
    PLAYER / "Game.dll": "E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def check_inputs() -> None:
    for path, expected in PINNED.items():
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"identity mismatch: {path}")
    for name in ("War3.exe", "ninja.exe", "cc1plus.exe"):
        if any(str(row.get("exeName", "")).lower() == name.lower()
               for row in war3._snapshot_process_entries()):
            raise RuntimeError(f"process conflict: {name}")


def main() -> int:
    check_inputs()
    for key in tuple(os.environ):
        if key.startswith("DXVK_WAR3_") or key.startswith("DXVK_RUNTIME_"):
            os.environ.pop(key)
    preflight = war3.preflight_instance_pool(
        sandbox_root=str(PLAYER), map_path=str(PROBE_MAP), instance_count=1,
        run_id=RUN_ID, session_prefix=SESSION_ID, artifact_root=str(ARTIFACTS),
    )
    if not preflight.get("ok") or preflight.get("warnings"):
        raise RuntimeError(f"instance preflight: {preflight}")
    target = ARTIFACTS / RUN_ID / SESSION_ID
    if target.exists():
        raise RuntimeError("output already exists")
    launch = None
    receipt: dict = {"inputIdentity": {str(p): h for p, h in PINNED.items()},
                     "globalInputUsed": False, "snapshots": [],
                     "captures": [],
                     "runtimeAcceptance": False}
    try:
        launch = war3.launch_war3_instance(
            sandbox_root=str(PLAYER), map_path=str(PROBE_MAP), run_id=RUN_ID,
            session_id=SESSION_ID, artifact_root=str(ARTIFACTS),
            windowed=True, use_isolated_desktop=True,
            deploy_d3d9_before_launch=True, build_d3d9_path=str(DLL),
            profile="full_default",
            env_overrides_json=json.dumps({
                "DXVK_WAR3_INTERNAL_TEST_API": "1",
                "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE": "1",
                "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE": "1",
                "DXVK_WAR3_PERF_RECORD_ON_START": "0",
                "DISABLE_VULKAN_OBS_CAPTURE": "1",
            }),
        )
        receipt["launch"] = {k: launch.get(k) for k in (
            "ok", "pid", "sessionId", "instanceRoot", "artifactDir",
            "mapSha256", "dllSha256", "desktop", "envOverrides", "error")}
        if not launch.get("ok") or not launch.get("desktop", {}).get("ok"):
            raise RuntimeError(f"isolated launch failed: {launch.get('error')}")
        if str(launch.get("dllSha256", "")).upper() != PINNED[DLL]:
            raise RuntimeError("instance DLL identity mismatch")
        ready = war3.wait_for_game_ready(
            timeout_sec=100, session_id=SESSION_ID, allow_fallback=False,
            auto_continue_loading=True, require_control_plane_for_continue=True,
            continue_max_pulses=2,
        )
        receipt["ready"] = {k: ready.get(k) for k in (
            "ok", "pid", "runtimeStatus", "error", "source", "stage")}
        if not ready.get("ok"):
            raise RuntimeError(f"game ready failed: {ready.get('error')}")
        pid = int(launch["pid"])
        instance = Path(launch["instanceRoot"])
        began = time.monotonic()
        captured_manual = False
        captured_restored = False
        while time.monotonic() - began < 39.0:
            if not war3._pid_alive(pid):
                raise RuntimeError("owned game exited before observation completed")
            row = war3._invoke_internal_test_request(
                pid, instance, "unit.walk.snapshot", timeout_sec=6)
            if not row.get("ok"):
                raise RuntimeError(f"walk snapshot failed: {row.get('error')}")
            receipt["snapshots"].append({"elapsedSec": round(time.monotonic() - began, 3),
                                          **row.get("result", {})})
            elapsed = time.monotonic() - began
            label = None
            if 31.0 <= elapsed < 33.5 and not captured_manual:
                label = "manual-stand-while-moving"
                captured_manual = True
            elif elapsed >= 35.0 and not captured_restored:
                label = "auto-walk-restored"
                captured_restored = True
            if label is not None:
                path = target / f"{label}.bmp"
                capture = war3._request_internal_frame_capture(
                    pid, path, instance, timeout_sec=12)
                receipt["captures"].append({
                    "label": label, "elapsedSec": round(elapsed, 3),
                    "ok": capture.get("ok"), "error": capture.get("error"),
                    "path": str(path),
                    "size": path.stat().st_size if path.is_file() else 0,
                    "sha256": sha256(path) if path.is_file() else "",
                })
            time.sleep(1.5)
        latest = receipt["snapshots"][-1]
        receipt["classification"] = {
            "jassRequestReachedDll": latest["setterCalls"] > 0,
            "jassRequestAccepted": latest["setterAccepted"] > 0,
            "autoWalkRequested": latest["autoRequestMatched"] > 0,
            "autoWalkSuppressed": latest["autoRequestSuppressed"] > 0,
            "restored": latest["disabledUnitCount"] == 0,
            "pixelOrAnimationVisualProof": False,
        }
        print(json.dumps(receipt["classification"], ensure_ascii=False), flush=True)
        return 0
    finally:
        if launch and launch.get("ok"):
            receipt["stop"] = war3.stop_war3(
                session_id=SESSION_ID, graceful_wait_sec=5, force=True,
                avoid_foreground_switch=True)
        receipt["sourceIdentityAfter"] = {str(p): sha256(p) for p in PINNED}
        receipt["sourceUnchanged"] = all(
            receipt["sourceIdentityAfter"][str(p)] == h for p, h in PINNED.items())
        target.mkdir(parents=True, exist_ok=True)
        with (target / "walk-probe.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2, allow_nan=False)


if __name__ == "__main__":
    raise SystemExit(main())
