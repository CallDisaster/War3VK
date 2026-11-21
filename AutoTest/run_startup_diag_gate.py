"""Isolated AutoTest gate for the startup error dialog and the VRAM preset.

Restored from the 1.22.03 release run (receipt
AutoTest/artifacts/v12203-final-startup-20261112/startup-diag.json); the scenario
behaviour is that run's, unchanged. What was judged by reading the receipt is
now written down as `classification` (docs/research/2026-11-17-x64-proxy-real-game-gates.md §1.2).

Scenarios (one instance at a time, sandbox only, player files never touched):
  fail-quiet   : DXVK_FILTER_DEVICE_NAME matches nothing; isolated desktop => no dialog.
                 Expect startup-error.txt with site=Direct3DCreate9 kind=NoCompatibleDevice,
                 then the game's own DirectX box, then the game exits.
  fail-dialog  : same, plus DXVK_WAR3_STARTUP_DIALOG=force. Expect a #32770 dialog owned by
                 war3.exe on the isolated desktop; read its text, close it.
  ok-auto      : normal start; expect WarVK\\Log\\gpu.txt with tier=... source=auto.
  ok-low       : DXVK_WAR3_VRAM_PRESET=low; expect tier=low csmResolutionCap=2048.

The player's d3d9.dll is absent and must stay absent (checked before and after,
never created). The sandbox instance loads the DLL under test, pinned to the
hash given with --sha (the build receipt's).

--mode proxy: the same scenarios with the product's device in the x64 render
host. The report site (startup-error.txt) and the VRAM preset line are not
produced by the proxy-mode startup yet (N6 / N6b in the research note), so all
four verdicts are "不适用" with that reason; what the game and the front end do
is still recorded.

usage: run_startup_diag_gate.py --run-id ID --dll PATH --sha HASH [--only a,b] [--mode proxy --front F --host H]
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import sys
import time
from ctypes import wintypes

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "AutoTest"))
import war3_autotest_mcp as war3  # noqa: E402
import x64_proxy_gate_mode as x64mode  # noqa: E402

SANDBOX = Path("E:/Work/War3_AutoTestSandbox")
PLAYER = Path("E:/Work/Warcraft III")
MAP = ROOT / "AutoTest/artifacts/stormtide_skin_20261111/stormtide-skins.w3x"
ARTIFACTS = ROOT / "AutoTest/artifacts"
PINNED = {
    MAP: "87401BBBC16C91BF48CC646BA0C858EF1209A60BD6E78607C0C6B8DF58265F85",
    PLAYER / "Game.dll": "E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A",
    SANDBOX / "Game.dll": "E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A",
}
BASE_ENV = {
    "DXVK_WAR3_INTERNAL_TEST_API": "1",
    "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE": "1",
    "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE": "1",
    "DXVK_WAR3_PERF_RECORD_ON_START": "0",
    "DISABLE_VULKAN_OBS_CAPTURE": "1",
}
SCENARIOS = [
    ("fail-quiet", {"DXVK_FILTER_DEVICE_NAME": "__warvk_no_such_gpu__"}),
    ("fail-dialog", {"DXVK_FILTER_DEVICE_NAME": "__warvk_no_such_gpu__", "DXVK_WAR3_STARTUP_DIALOG": "force"}),
    ("ok-auto", {}),
    ("ok-low", {"DXVK_WAR3_VRAM_PRESET": "low"}),
]
PROXY_REASON = {
    "fail-quiet": "the proxy-mode startup does not call ReportStartupFailure yet (N6): no startup-error.txt site",
    "fail-dialog": "the proxy-mode startup does not call ReportStartupFailure yet (N6): no WarVK dialog",
    "ok-auto": "proxy mode does not apply the VRAM preset (war3_proxy_frame.cpp, N6b): no 'WarVK GPU:' line",
    "ok-low": "proxy mode does not apply the VRAM preset (war3_proxy_frame.cpp, N6b): no 'WarVK GPU:' line",
}

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.IsWindow.argtypes = [wintypes.HWND]
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as s:
        for b in iter(lambda: s.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest().upper()


def window_text(hwnd) -> str:
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 2)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def class_name(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 255)
    return buf.value


def dialog_texts(hwnd):
    texts = []

    def cb(child, _):
        t = window_text(child)
        if t:
            texts.append({"class": class_name(child), "text": t})
        return True

    user32.EnumChildWindows(hwnd, WNDENUMPROC(cb), 0)
    return texts


def find_dialog(pid: int):
    for hwnd in war3._enumerate_pid_windows(pid):
        if class_name(hwnd) == "#32770":
            return hwnd
    return None


def read_text(path: Path) -> str:
    try:
        return path.read_bytes().decode("utf-8-sig", errors="replace")
    except OSError:
        return ""


def run_one(name: str, extra: dict, receipt: dict, args, arm) -> None:
    session = f"sd-{name}"
    env = dict(BASE_ENV)
    env.update(extra)
    row = {"scenario": name, "env": extra}
    receipt["scenarios"].append(row)
    launch = None
    staging = None
    if arm is not None:
        staging = arm.staging / name
        staging.mkdir()
        env.update(arm.env())
        env["WARVK_D3D9_HOST_LOG"] = str(staging / "host.log")
    try:
        import contextlib
        with (war3.game_stdio_capture(staging) if staging is not None else contextlib.nullcontext()):
            launch = war3.launch_war3_instance(
                sandbox_root=str(SANDBOX), map_path=str(MAP), run_id=args.run_id, session_id=session,
                artifact_root=str(ARTIFACTS), windowed=True, use_isolated_desktop=True,
                deploy_d3d9_before_launch=True, build_d3d9_path=str(args.dll), profile="full_default",
                env_overrides_json=json.dumps(env))
        row["launch"] = {k: launch.get(k) for k in ("ok", "pid", "instanceRoot", "dllSha256", "error")}
        row["desktop"] = launch.get("desktop")
        row["instanceDllPinned"] = str(launch.get("dllSha256", "")).upper() == args.sha
        print(name, "launch", json.dumps(row["launch"], ensure_ascii=False), flush=True)
        if not launch.get("ok"):
            return
        pid = int(launch["pid"])
        inst = Path(launch["instanceRoot"])
        err_file = inst / "WarVK" / "Log" / "startup-error.txt"
        gpu_file = inst / "WarVK" / "Log" / "gpu.txt"
        began = time.monotonic()
        if name.startswith("fail"):
            dialog = None
            while time.monotonic() - began < 45:
                alive = war3._pid_alive(pid)
                if name == "fail-dialog" and dialog is None and alive:
                    dialog = find_dialog(pid)
                    if dialog:
                        row["dialog"] = {"title": window_text(dialog), "children": dialog_texts(dialog),
                                         "elapsedSec": round(time.monotonic() - began, 2)}
                        time.sleep(1.0)
                        row["dialogStillThere"] = bool(user32.IsWindow(dialog))
                        user32.PostMessageW(dialog, 0x0010, 0, 0)  # WM_CLOSE (= OK for MB_OK)
                        row["dialogClosedAtSec"] = round(time.monotonic() - began, 2)
                if not alive:
                    row["exitedAtSec"] = round(time.monotonic() - began, 2)
                    break
                if name == "fail-quiet" and err_file.is_file() and "reportSeenAtSec" not in row:
                    row["reportSeenAtSec"] = round(time.monotonic() - began, 2)
                if name == "fail-dialog" and dialog and time.monotonic() - began > row.get("dialogClosedAtSec", 0) + 15:
                    break
                if name == "fail-quiet" and "reportSeenAtSec" in row and time.monotonic() - began > row["reportSeenAtSec"] + 15:
                    break
                if arm is not None and name == "fail-quiet" and time.monotonic() - began > 20 and find_dialog(pid):
                    break  # proxy mode: no report is written; the game's own box is the next thing
                time.sleep(0.25)
            # After our report/dialog, Game.dll shows its own error box. Read it, press OK,
            # and record whether the process then exits by itself.
            game_box = None
            t_box = time.monotonic()
            while war3._pid_alive(pid) and time.monotonic() - t_box < 20:
                for h in war3._enumerate_pid_windows(pid):
                    if class_name(h) == "#32770" and window_text(h) == "WarVK 无法启动" and int(h) != int(dialog or 0):
                        row["extraWarVkDialogs"] = row.get("extraWarVkDialogs", 0) + 1
                        user32.PostMessageW(h, 0x0010, 0, 0)
                        dialog = h
                    if class_name(h) == "#32770" and window_text(h) != "WarVK 无法启动":
                        game_box = h
                        break
                if game_box:
                    break
                time.sleep(0.25)
            if game_box:
                row["gameErrorBox"] = {"title": window_text(game_box), "children": dialog_texts(game_box)}
                user32.PostMessageW(game_box, 0x0010, 0, 0)  # WM_CLOSE
                t_exit = time.monotonic()
                while war3._pid_alive(pid) and time.monotonic() - t_exit < 20:
                    time.sleep(0.25)
                row["exitAfterGameBoxSec"] = None if war3._pid_alive(pid) else round(time.monotonic() - t_exit, 2)
            row["aliveAtEnd"] = war3._pid_alive(pid)
            if row["aliveAtEnd"]:
                row["windowsAtEnd"] = [{"class": class_name(h), "title": window_text(h)}
                                       for h in war3._enumerate_pid_windows(pid)]
            row["report"] = read_text(err_file)
        else:
            ready = war3.wait_for_game_ready(timeout_sec=120, session_id=session, allow_fallback=False,
                                             auto_continue_loading=True, require_control_plane_for_continue=True,
                                             continue_max_pulses=2)
            row["ready"] = {k: ready.get(k) for k in ("ok", "error", "stage")}
            print(name, "ready", json.dumps(row["ready"], ensure_ascii=False, default=str), flush=True)
            row["gpu"] = read_text(gpu_file)
            row["startupErrorPresent"] = err_file.is_file()
            if arm is not None:
                reply = war3._control_plane_request(pid=pid, command="get_runtime_status", payload={},
                                                    timeout_sec=2.0)
                row["proxyStatus"] = x64mode.proxy_status(dict(reply.get("result") or {}))
                row["naturalExit"] = war3.natural_exit_owned_game(
                    pid, inst, str((launch.get("desktop") or {}).get("name", "")))
        logs = {}
        art = Path(launch.get("artifactDir") or inst)
        for p in list(art.glob("*_d3d9.log")) + list((inst / "WarVK" / "Log").glob("*")):
            if p.is_file() and p.stat().st_size < 4 << 20:
                txt = read_text(p)
                hits = [ln for ln in txt.splitlines()
                        if any(k in ln for k in ("WarVK GPU", "WarVK startup failure", "Skipping:",
                                                 "VRAM preset", "Failed to", "Direct3DCreate9", "CreateDevice",
                                                 "WarVK proxy mode"))]
                if hits:
                    logs[p.name] = hits[:40]
        dbwin = art / "dbwin.jsonl"
        if dbwin.is_file():
            hits = [ln[:400] for ln in read_text(dbwin).splitlines()
                    if "csm" in ln.lower() and ("resolution" in ln.lower() or "4096" in ln or "2048" in ln)]
            if hits:
                logs["dbwin.jsonl"] = hits[:12]
        row["logHits"] = logs
    finally:
        if launch and launch.get("ok"):
            row["stop"] = war3.stop_war3(session_id=session, graceful_wait_sec=5, force=True,
                                         avoid_foreground_switch=True).get("ok")
        if staging is not None:
            row["proxyEvidence"] = x64mode.collect_evidence(
                staging, [Path(launch.get("artifactDir"))] if launch and launch.get("artifactDir") else [],
                Path(launch["instanceRoot"]) if launch and launch.get("instanceRoot") else None)
        print(name, "done", json.dumps({k: row.get(k) for k in ("exitedAtSec", "aliveAtEnd", "reportSeenAtSec",
                                                                "dialogClosedAtSec", "exitAfterGameBoxSec", "stop")}),
              flush=True)


def classify(row: dict) -> dict:
    """The 1.22.03 receipt's criteria (research note §1.2), in-process arm."""
    name = row["scenario"]
    report = row.get("report") or ""
    gpu = row.get("gpu") or ""
    checks = {"launched": bool((row.get("launch") or {}).get("ok")),
              "instanceDllPinned": row.get("instanceDllPinned") is True}
    if name == "fail-quiet":
        checks.update(reportSite="site=Direct3DCreate9 kind=NoCompatibleDevice" in report,
                      reportSeen=row.get("reportSeenAtSec") is not None,
                      noWarVkDialog=not row.get("dialog") and not row.get("extraWarVkDialogs"),
                      gameErrorBox=bool(row.get("gameErrorBox")),
                      exited=row.get("aliveAtEnd") is False)
    elif name == "fail-dialog":
        checks.update(warvkDialog=(row.get("dialog") or {}).get("title") == "WarVK 无法启动",
                      reportSite="site=Direct3DCreate9 kind=NoCompatibleDevice" in report,
                      exited=row.get("aliveAtEnd") is False)
    elif name == "ok-auto":
        checks.update(ready=bool((row.get("ready") or {}).get("ok")),
                      gpuLine="WarVK GPU:" in gpu and "source=auto" in gpu and "csmResolutionCap=" in gpu,
                      noStartupError=row.get("startupErrorPresent") is False)
    elif name == "ok-low":
        checks.update(ready=bool((row.get("ready") or {}).get("ok")),
                      gpuLine="tier=low" in gpu and "csmResolutionCap=2048" in gpu,
                      noStartupError=row.get("startupErrorPresent") is False)
    return {"ok": all(checks.values()), "checks": checks}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--sha", required=True, help="the build receipt's hash of --dll")
    parser.add_argument("--only", default="", help="comma-separated scenario names")
    x64mode.add_mode_arguments(parser)
    args = parser.parse_args()
    args.dll = args.dll.resolve()
    args.sha = args.sha.upper()
    only = set(filter(None, args.only.split(",")))
    player_before = x64mode.player_dll_guard("before")
    for p, want in PINNED.items():
        if sha256(p) != want:
            raise RuntimeError(f"identity mismatch: {p}")
    if not args.dll.is_file() or sha256(args.dll) != args.sha:
        raise RuntimeError(f"DLL under test differs from --sha: {args.dll}")
    for key in tuple(os.environ):
        if key.startswith(("DXVK_WAR3_", "DXVK_RUNTIME_", "DXVK_FILTER_")):
            os.environ.pop(key)
    pre = war3.preflight_instance_pool(sandbox_root=str(SANDBOX), map_path=str(MAP), instance_count=1,
                                       run_id=args.run_id, session_prefix="sd", artifact_root=str(ARTIFACTS))
    print("preflight", json.dumps({k: pre.get(k) for k in ("ok", "warnings", "error")}, ensure_ascii=False), flush=True)
    if not pre.get("ok") or pre.get("warnings"):
        raise RuntimeError(f"instance preflight: {pre}")
    out = ARTIFACTS / args.run_id
    if out.exists():
        raise RuntimeError("run artifact already exists")
    arm = x64mode.ProxyArm(args.front, args.host, out / "proxy") if args.mode == "proxy" else None
    receipt = {"mode": args.mode, "dll": str(args.dll), "dllSha256": args.sha, "scenarios": [],
               "globalInputUsed": False, "playerDllBefore": player_before,
               **({"proxyArm": arm.receipt()} if arm is not None else {})}
    try:
        for name, extra in SCENARIOS:
            if only and name not in only:
                continue
            run_one(name, extra, receipt, args, arm)
    finally:
        receipt["sourceUnchanged"] = all(sha256(p) == h for p, h in PINNED.items())
        receipt["playerDllAfter"] = x64mode.player_dll_state()
        verdicts = {}
        for row in receipt["scenarios"]:
            if arm is None:
                verdicts[row["scenario"]] = classify(row)
            else:
                verdicts[row["scenario"]] = x64mode.not_applicable(PROXY_REASON[row["scenario"]])
        receipt["verdicts"] = verdicts
        measured = [v for v in verdicts.values() if "ok" in v]
        receipt["ok"] = (bool(measured) and all(v["ok"] for v in measured) and receipt["sourceUnchanged"]
                         and x64mode.player_dll_unchanged(player_before, receipt["playerDllAfter"]))
        out.mkdir(parents=True, exist_ok=True)
        with (out / "startup-diag.json").open("x", encoding="utf-8") as s:
            json.dump(receipt, s, ensure_ascii=False, indent=2, default=str)
        print("verdicts", json.dumps(verdicts, ensure_ascii=False), flush=True)
        print("sourceUnchanged", receipt["sourceUnchanged"], "playerDllUnchanged",
              x64mode.player_dll_unchanged(player_before, receipt["playerDllAfter"]), flush=True)
    if arm is not None:
        return 0  # nothing measurable yet; see the verdicts
    return 0 if receipt["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
