"""One isolated WarVK A→B→A same-process canary, no player deployment.

Uses two editor-saved-map-derived MPQ copies with JASS ChangeLevel timers.
The runtime instance, desktop and job are owned by existing AutoTest. A
successful process launch or CPU ledger alone is never counted as acceptance.
The player's d3d9.dll is absent and must stay absent (checked, never created).

Proxy mode (PROXY_ARM set by run_v12202_final_gate.py --mode proxy): the same
product DLL with its device in the x64 render host. The in-process renderer's
shadow lifecycle is not in this process, so the proxy arm judges the same
transition from the data layer's own account (the status file's / control
plane's proxyMode section): the session generation must rise strictly over
A -> B -> A, each map must reach world frames with shadow casters, every
session reset must reach the host, and the transport must end clean.
What needs the host's per-generation account (retired resources) is recorded
as not applicable.
"""

from __future__ import annotations

import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import time

import contextlib

import war3_autotest_mcp as war3
import x64_proxy_gate_mode as x64mode
from autotest_sessions import build_instance_layout, materialize_instance_root


ROOT = Path(__file__).resolve().parents[1]
PLAYER = Path("E:/Work/Warcraft III")
SOURCE_MAP_A = PLAYER / "Maps/Test/WorldEditTestMap.w3x"
SOURCE_MAP_B = PLAYER / "Maps/Test/ShadowPressure.w3x"
MAP_DIR = ROOT / "AutoTest/artifacts/v122_visual_api_20260914/cross-map-20260925"
MAP_A = MAP_DIR / "A.w3x"
MAP_B = MAP_DIR / "B.w3x"
DLL = ROOT / "build32/src/d3d9/d3d9.dll"
ARTIFACT_ROOT = ROOT / "AutoTest/artifacts"
RUN_ID = "crossmap-20260925-d"
SESSION_ID = "maptest"
EXPECTED = {
    PLAYER / "Game.dll": "E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A",
    SOURCE_MAP_A: "692E92F43BAC99EF8ECED7833E137B22475914F592DA7F78FCC24B54C221E95F",
    SOURCE_MAP_B: "926822FC233C41431397499F6A922AB988C052BA0CA2E7EB14CB2B0581F5F4D8",
    MAP_A: "410436ED826DEC572FC7F0E51FE214DFA75F975A11530593000D214AC6681A02",
    MAP_B: "B730265584CB9B91F211112EB2535D2AD7989A9C24A1E685D38D0F3D7A911DDB",
    DLL: "1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF",
}
MONITOR_KEYS = (
    "shadowMapEpoch", "shadowMapResetRequestedSerial", "shadowMapResetAppliedSerial",
    "shadowMapTransitionState", "shadowMapProducerReady", "shadowMapCompletedRetireSerial",
    "shadowMapQuarantinedRetireSerial", "shadowRetiredSessionCount",
    "shadowRetiredSessionEntryCount", "shadowRetiredSessionOldestRetireSerial",
    "shadowRetiredSessionCollectedCount", "shadowRetiredSessionAllocatorBytes",
    "shadowRetiredSessionCachedGpuLogicalBytes", "shadowRetiredSessionCpuOwnedBytes",
    "queueSubmittedSerial", "queueCompletedSerial", "shadowReplayCasterCount",
    "shadowReplayDrawnCasterCount", "semanticSceneShadowMapDrawnCasters",
    "semanticSceneReceiverHasCompleteShadowMap", "semanticSceneShadowMapExecutedThisFrame",
)
PHASE_EPOCH = {"A-first": 1, "B": 2, "A-return": 3}
# None: the in-process arm. A x64_proxy_gate_mode.ProxyArm: the proxy arm.
PROXY_ARM = None


def proxy_row(status: dict) -> dict:
    """The proxy arm's per-sample telemetry (proxyMode, N1)."""
    proxy = x64mode.proxy_status(status)
    frame = dict(proxy.get("frame") or {})
    last = dict((proxy.get("casters") or {}).get("lastFrame") or {})
    return {"sessionGeneration": frame.get("sessionGeneration"), "frames": frame.get("frames"),
            "worldScenes": frame.get("worldScenes"), "insertions": frame.get("insertions"),
            "refused": frame.get("refused"), "castersSent": last.get("sent"),
            "units": last.get("units"), "deviceLost": proxy.get("deviceLost"),
            "present": bool(proxy)}


def proxy_verdict(receipt: dict, rows: list, evidence: dict, natural_exit: dict) -> dict:
    """A -> B -> A judged from the data layer's account (see the docstring)."""
    phases = {}
    for name in ("A-first", "B", "A-return"):
        part = [row["proxy"] for row in rows if row["phase"] == name and row.get("proxy", {}).get("present")]
        generations = [int(p.get("sessionGeneration") or 0) for p in part]
        insertions = [int(p.get("insertions") or 0) for p in part]
        tail = part[-3:]
        phases[name] = {
            "sampleCount": len(part),
            "uniqueFrameCount": len({int(p.get("frames") or 0) for p in part}),
            "generationTail": generations[-1] if generations else None,
            "generationMin": min(generations, default=None),
            "insertionsAdvanced": len(insertions) >= 2 and insertions[-1] > insertions[0],
            "castersSentMax": max((int(p.get("castersSent") or 0) for p in part), default=0),
            "castersSentTail": [int(p.get("castersSent") or 0) for p in tail],
            "refusedMax": max((int(p.get("refused") or 0) for p in part), default=0),
        }
    tails = [phases[n]["generationTail"] for n in ("A-first", "B", "A-return")]
    # The generation each phase began with: the next map change's reset can
    # land before that phase's JASS marker, so a phase's tail may already be
    # the next map's (2026-11-17 r1: tails [2, 4, 4], entries [0, 2, 4]).
    entries = [phases[n]["generationMin"] for n in ("A-first", "B", "A-return")]
    annotation = x64mode.last(evidence.get("frontAccounts") or {}, "annotation-proxy")
    reset_lines = (evidence.get("dataLayer") or {}).get("sessionResetLines") or []
    # The front end sends the newest pending reset ahead of the next Present
    # and a later generation replaces an unsent one (d3d9_connection.h
    # requestSessionReset), so Sent < Requested is normal (coalesced, or a reset
    # at exit with no Present after it). What must hold: every generation the
    # data layer had while frames were still presented reached the host.
    present_rows = [row["proxy"] for row in rows if row.get("proxy", {}).get("present")]
    framed = [int(p.get("sessionGeneration") or 0) for i, p in enumerate(present_rows)
              if any(int(q.get("worldScenes") or 0) > int(p.get("worldScenes") or 0) for q in present_rows[i + 1:])]
    hosts = evidence.get("hosts") or []
    host_annotation = ((hosts[-1].get("accounts") or {}).get("annotation-host") or {}) if hosts else {}
    host_generation = host_annotation.get("sessionGeneration")
    checks = {
        "sameProcess": bool(receipt.get("sameProcess")),
        "processStillAliveAtEnd": bool(receipt.get("processStillAliveAtEnd")),
        "resolution": bool((receipt.get("resolution") or {}).get("ok")),
        "sessionGenerationAdvanced": all(isinstance(g, int) for g in entries) and entries[0] < entries[1] < entries[2]
                                     and all(phases[n]["sampleCount"] >= 5 for n in phases),
        "sessionResetLogged": len(reset_lines) >= 2,
        "sessionResetsReachedHost": bool(annotation) and annotation.get("sessionResetsRefused") == 0
                                    and int(annotation.get("sessionResetsSent") or 0) >= 1
                                    and isinstance(host_generation, int) and bool(framed)
                                    and host_generation >= max(framed),
        "runtimeStateGate": all(row["sampleCount"] >= 5 and row["uniqueFrameCount"] >= 2
                                and row["insertionsAdvanced"] and row["castersSentMax"] > 0
                                and row["refusedMax"] == 0 for row in phases.values()),
        "telemetryErrors": receipt.get("telemetryErrors", 99) < 20,
        "noNewGpuEvents": not receipt.get("newGpuEvents"),
        "noGpuIncidents": not receipt.get("gpuIncidents"),
        "screenshots": len(receipt.get("screenshots") or []) == 3
                       and all(x.get("ok") and x.get("generationAdvancedForPhase") for x in receipt["screenshots"]),
    }
    acceptance = x64mode.proxy_acceptance(evidence, natural_exit, x64mode.world_progress(
        [{"frame": {"worldScenes": row["proxy"].get("worldScenes"), "insertions": row["proxy"].get("insertions")}}
         for row in rows if row.get("proxy", {}).get("present")]))
    checks.update({"proxy." + k: v for k, v in acceptance["checks"].items()})
    return {"accepted": all(checks.values()), "checks": checks, "phases": phases,
            "generationTails": tails, "generationEntries": entries, "sessionResetLines": reset_lines[:8],
            "annotation": annotation, "hostAnnotation": host_annotation,
            "generationWithFramesAfterMax": max(framed, default=None), "proxyAcceptance": acceptance,
            "notApplicable": {
                "retirementCoverage": x64mode.not_applicable(
                    "retired shadow sessions live in the x64 host; its per-generation account (N3) "
                    "is not written yet"),
                "mapEpochExact": x64mode.not_applicable(
                    "shadowMapEpoch is the in-process renderer's; the proxy arm asserts a strictly "
                    "rising session generation instead (a reset may advance it by more than one)"),
                "queueSerials": x64mode.not_applicable(
                    "queue serials are the host's; the front end's terminal published==applied "
                    "covers the transport")}}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def write_json_new(path: Path, value: object) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False,
                  default=str)


def below_normal(pid: int) -> bool:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    handle = kernel32.OpenProcess(0x0200 | 0x1000, False, pid)
    if not handle:
        return False
    try:
        return bool(kernel32.SetPriorityClass(handle, 0x00004000))
    finally:
        kernel32.CloseHandle(handle)


def borderless_1440(pid: int) -> dict:
    """Only the AutoTest-owned isolated HWND; never changes the input desktop."""
    hwnd = war3._find_main_window_hwnd(pid)
    if not hwnd:
        return {"ok": False, "error": "owned window missing"}
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
    user32.SetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                       ctypes.c_ssize_t]
    user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
    user32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_uint]
    user32.SetWindowPos.restype = ctypes.c_int
    old = int(user32.GetWindowLongPtrW(hwnd, -16))
    style = (old & ~(0x00CF0000 | 0x00400000)) | 0x80000000 | 0x10000000
    ctypes.set_last_error(0)
    previous = int(user32.SetWindowLongPtrW(hwnd, -16, style))
    if previous == 0 and ctypes.get_last_error() != 0:
        return {"ok": False, "error": f"SetWindowLongPtrW {ctypes.get_last_error()}"}
    ok = bool(user32.SetWindowPos(hwnd, None, 0, 0, 2560, 1440,
                                  0x0020 | 0x0004 | 0x0040))
    time.sleep(1.0)
    window = war3.query_war3_window(pid=pid, wait_sec=1,
                                    require_visible=False)
    client = dict(window.get("clientRect") or {})
    return {"ok": ok and client.get("width") == 2560 and
            client.get("height") == 1440,
            "oldStyle": old, "newStyle": style, "window": window}


def phase_from_markers(temp: Path) -> str:
    if (temp / "crossmap_A_stage_1.txt").is_file():
        return "A-return"
    if (temp / "crossmap_B.txt").is_file():
        return "B"
    if (temp / "crossmap_A_stage_0.txt").is_file():
        return "A-first"
    return "startup"


def main() -> int:
    player_dll_before = x64mode.player_dll_guard("before")
    for path, expected in EXPECTED.items():
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"input identity mismatch: {path}")
    if any(str(row.get("exeName", "")).lower() in
           {"war3.exe", "ninja.exe", "cc1plus.exe"}
           for row in war3._snapshot_process_entries()):
        raise RuntimeError("game/build process conflict")
    output = ARTIFACT_ROOT / RUN_ID
    # The proxy arm staged its front end and host in <run>/proxy beforehand.
    if output.exists() and not (PROXY_ARM is not None and
                                [x.name for x in output.iterdir()] == ["proxy"]):
        raise RuntimeError("run artifact already exists")
    preflight = war3.preflight_instance_pool(
        sandbox_root=str(PLAYER), map_path=str(MAP_A), instance_count=1,
        run_id=RUN_ID, session_prefix=SESSION_ID,
        artifact_root=str(ARTIFACT_ROOT),
    )
    if not preflight.get("ok") or preflight.get("warnings"):
        raise RuntimeError(f"instance preflight: {preflight}")
    layout = build_instance_layout(PLAYER, ARTIFACT_ROOT, RUN_ID, SESSION_ID)
    materialized = materialize_instance_root(layout)
    if not materialized.get("ok"):
        raise RuntimeError(f"instance materialization: {materialized}")
    maps = layout.instance_root / "Maps/AutoTest/WarVKCrossMap20260925"
    maps.mkdir(parents=True, exist_ok=False)
    for source, name in ((MAP_A, "A.w3x"), (MAP_B, "B.w3x")):
        target = maps / name
        with source.open("rb") as original, target.open("xb") as copied:
            shutil.copyfileobj(original, copied)
        if sha256(target) != EXPECTED[source]:
            raise RuntimeError(f"instance map copy differs: {name}")

    receipt: dict = {
        "transitionCanaryAccepted": False, "releaseAuthorized": False,
        "globalInputUsed": False,
        "inputIdentity": {str(path): value for path, value in EXPECTED.items()},
        "materialize": materialized, "preflight": preflight,
        "phases": [], "screenshots": [], "samples": 0,
        "sampleCountsByPhase": {}, "telemetryErrors": 0,
        "telemetryErrorExamples": [],
        "mode": "inprocess" if PROXY_ARM is None else "proxy",
        "playerDllBefore": player_dll_before,
    }
    overrides = {
        "DXVK_WAR3_INTERNAL_TEST_API": "1",
        "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE": "1",
        "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE": "1",
        "DXVK_WAR3_PERF_MONITOR": "1",
        "DXVK_WAR3_PERF_RECORD_ON_START": "0",
        "DXVK_WAR3_PERF_RECORD_ON_GAME_START": "0",
        "DXVK_WAR3_PERF_AUTO_EXPORT_SEC": "0",
        "DISABLE_VULKAN_OBS_CAPTURE": "1",
    }
    if PROXY_ARM is not None:
        overrides.update(PROXY_ARM.env())
        receipt["proxyArm"] = PROXY_ARM.receipt()
    launched = False
    samples_path = layout.artifact_dir / "runtime-samples.jsonl"
    try:
        gpu_before = war3._query_windows_gpu_events()
        receipt["gpuEventsBefore"] = [war3._gpu_event_identity(x) for x in gpu_before]
        with (war3.game_stdio_capture(PROXY_ARM.staging) if PROXY_ARM is not None
              else contextlib.nullcontext()):
            launch = war3.launch_war3_instance(
                sandbox_root=str(PLAYER), map_path=str(MAP_A), run_id=RUN_ID,
                session_id=SESSION_ID, artifact_root=str(ARTIFACT_ROOT),
                windowed=True, use_isolated_desktop=True,
                deploy_d3d9_before_launch=True, build_d3d9_path=str(DLL),
                profile="full_default", reuse_existing_root=True,
                env_overrides_json=json.dumps(overrides),
            )
        launched = bool(launch.get("ok"))
        receipt["launch"] = {k: launch.get(k) for k in (
            "ok", "pid", "instanceRoot", "mapSha256", "dllSha256",
            "desktop", "error", "envOverrides")}
        if not launched or not launch.get("desktop", {}).get("ok"):
            raise RuntimeError(f"isolated launch failed: {launch.get('error')}")
        pid = int(launch["pid"])
        receipt["belowNormal"] = below_normal(pid)
        if str(launch.get("dllSha256", "")).upper() != EXPECTED[DLL]:
            raise RuntimeError("instance DLL SHA mismatch")
        ready = war3.wait_for_game_ready(
            timeout_sec=120, session_id=SESSION_ID,
            allow_fallback=False, auto_continue_loading=False,
            require_control_plane_for_continue=True, continue_max_pulses=2)
        receipt["ready"] = {k: ready.get(k) for k in (
            "ok", "pid", "error", "source", "stage")}
        if not ready.get("ok"):
            raise RuntimeError(f"initial map not ready: {ready.get('error')}")
        receipt["resolution"] = borderless_1440(pid)

        phase = "startup"
        began = time.monotonic()
        # Proxy arm: a map load through the transport is much slower (T4
        # extra-map probe, 2026-11-17); the in-process deadline is unchanged.
        deadline = began + (125.0 if PROXY_ARM is None else 300.0)
        stable_return_started = None
        taken: set[str] = set()
        temp = layout.instance_root / "WarVK/Temp"
        last_fresh_at = time.monotonic()
        last_timestamp_ms = 0
        proxy_rows_seen: list = []
        with samples_path.open("x", encoding="utf-8") as samples:
            while time.monotonic() < deadline:
                if not war3._pid_alive(pid):
                    raise RuntimeError("owned game exited during A-B-A")
                current = phase_from_markers(temp)
                if current != phase:
                    phase = current
                    receipt["phases"].append({"phase": phase,
                                              "elapsedSec": round(time.monotonic()-began, 3)})
                    print(f"phase={phase} pid={pid}", flush=True)
                    if phase == "A-return":
                        stable_return_started = time.monotonic()
                status = {}
                telemetry_source = "periodic-file"
                file_status = {}
                file_age_ms = None
                status_file = temp / "runtime_status.json"
                if status_file.is_file():
                    try:
                        candidate = json.loads(status_file.read_text(
                            encoding="utf-8"))
                        age = int(time.time() * 1000) - int(candidate.get(
                            "timestampMs", 0) or 0)
                        if 0 <= age <= 12000:
                            file_status = candidate
                            file_age_ms = age
                    except (OSError, ValueError, TypeError):
                        pass
                if file_status:
                    status = file_status
                if PROXY_ARM is not None and not status:
                    # Proxy arm: the control plane answers while the status
                    # file is between refreshes (or the game is loading).
                    reply = war3._control_plane_request(
                        pid=pid, command="get_runtime_status", payload={}, timeout_sec=2.0)
                    if reply.get("ok") and x64mode.proxy_status(dict(reply.get("result") or {})):
                        status = dict(reply.get("result") or {})
                        telemetry_source = "control-plane"
                if status:
                    shadow = dict(status.get("shadow") or {})
                    file_shadow = dict(file_status.get("shadow") or {})
                    for key in MONITOR_KEYS:
                        if key not in shadow and key in file_shadow:
                            shadow[key] = file_shadow[key]
                    row = {
                        "elapsedSec": round(time.monotonic()-began, 3),
                        "phase": phase, "pid": pid,
                        "frameIndex": status.get("frameIndex"),
                        "timestampMs": status.get("timestampMs"),
                        "telemetrySource": telemetry_source,
                        "fileFrameIndex": file_status.get("frameIndex"),
                        "fileAgeMs": file_age_ms,
                        "runtime": status.get("runtime"),
                        "perf": status.get("perf"),
                        "monitor": {key: shadow.get(key) for key in MONITOR_KEYS},
                    }
                    if PROXY_ARM is not None:
                        row["proxy"] = proxy_row(status)
                    samples.write(json.dumps(row, ensure_ascii=False) + "\n")
                    samples.flush()
                    receipt["samples"] += 1
                    receipt["sampleCountsByPhase"][phase] = (
                        receipt["sampleCountsByPhase"].get(phase, 0) + 1)
                    stamp = int(status.get("timestampMs", 0) or 0)
                    if stamp > last_timestamp_ms:
                        last_timestamp_ms = stamp
                        last_fresh_at = time.monotonic()
                else:
                    receipt["telemetryErrors"] += 1
                    if len(receipt["telemetryErrorExamples"]) < 5:
                        receipt["telemetryErrorExamples"].append({
                            "phase": phase, "error": "fresh periodic status unavailable"})
                if time.monotonic() - last_fresh_at > 15.0:
                    raise RuntimeError("periodic status stalled for >15 seconds")
                if (PROXY_ARM is not None and status and phase in PHASE_EPOCH
                        and phase not in taken):
                    # Proxy arm: the image must come from the new map - the
                    # session generation the phase began with (the JASS marker
                    # follows the map change, so the reset precedes it), still
                    # current, above every earlier phase's entry generation;
                    # and world frames with casters since the phase began. The
                    # next map change's reset can land before that phase's
                    # marker, so tails are not compared.
                    entered = next(x["elapsedSec"] for x in receipt["phases"]
                                   if x["phase"] == phase)
                    current = row["proxy"]
                    entry_generation = {}
                    for r in proxy_rows_seen:
                        if r["phase"] in PHASE_EPOCH:
                            entry_generation.setdefault(
                                r["phase"], int((r.get("proxy") or {}).get("sessionGeneration") or 0))
                    at_entry = next((r["proxy"] for r in proxy_rows_seen if r["phase"] == phase), current)
                    generation = int(current.get("sessionGeneration") or 0)
                    this_entry = entry_generation.get(phase, generation)
                    earlier = [g for name, g in entry_generation.items() if name != phase]
                    fresh = generation == this_entry and (
                        phase == "A-first" or this_entry > max(earlier, default=-1))
                    advanced = int(current.get("insertions") or 0) - int(at_entry.get("insertions") or 0) >= 30
                    if (time.monotonic()-began >= entered + 5.0 and fresh and advanced
                            and int(current.get("castersSent") or 0) > 0):
                        screenshot = layout.artifact_dir / f"{phase}.png"
                        shot = war3.capture_war3_screenshot(
                            output_path=str(screenshot), pid=pid,
                            war3_dir=str(layout.instance_root),
                            prefer_internal=True, timeout_sec=8,
                            fallback_to_window_capture=False)
                        receipt["screenshots"].append({
                            "phase": phase, "ok": shot.get("ok"),
                            "sessionGeneration": generation,
                            "generationAdvancedForPhase": fresh,
                            "insertions": current.get("insertions"),
                            "statusFrameIndex": status.get("frameIndex"),
                            "error": shot.get("error"), "path": str(screenshot),
                            "size": screenshot.stat().st_size if screenshot.exists() else 0,
                            "sha256": sha256(screenshot) if screenshot.exists() else "",
                        })
                        taken.add(phase)
                if status and PROXY_ARM is not None:
                    proxy_rows_seen.append(row)
                if PROXY_ARM is None and status and phase in PHASE_EPOCH and phase not in taken:
                    entered = next(x["elapsedSec"] for x in receipt["phases"]
                                   if x["phase"] == phase)
                    # JASS markers precede ChangeLevel completion. An image
                    # from the old map must never satisfy a new-map gate.
                    expected_epoch = PHASE_EPOCH[phase]
                    observed_epoch = int(shadow.get("shadowMapEpoch") or 0)
                    ready_shadow = bool(
                        int(shadow.get("shadowMapProducerReady") or 0) and
                        int(shadow.get("semanticSceneReceiverHasCompleteShadowMap") or 0) and
                        int(shadow.get("shadowReplayDrawnCasterCount") or 0))
                    if (time.monotonic()-began >= entered + 5.0 and
                            observed_epoch == expected_epoch and ready_shadow):
                        screenshot = layout.artifact_dir / f"{phase}.png"
                        shot = war3.capture_war3_screenshot(
                            output_path=str(screenshot), pid=pid,
                            war3_dir=str(layout.instance_root),
                            prefer_internal=True, timeout_sec=8,
                            fallback_to_window_capture=False)
                        receipt["screenshots"].append({
                            "phase": phase, "ok": shot.get("ok"),
                            "expectedEpoch": expected_epoch,
                            "observedEpoch": observed_epoch,
                            "statusFrameIndex": status.get("frameIndex"),
                            "error": shot.get("error"), "path": str(screenshot),
                            "size": screenshot.stat().st_size if screenshot.exists() else 0,
                            "sha256": sha256(screenshot) if screenshot.exists() else "",
                        })
                        taken.add(phase)
                if stable_return_started and time.monotonic()-stable_return_started >= 30.0:
                    break
                time.sleep(0.5)
        receipt["sameProcess"] = all(x["phase"] in
                                     [p["phase"] for p in receipt["phases"]]
                                     for x in ({"phase": "A-first"}, {"phase": "B"},
                                               {"phase": "A-return"}))
        receipt["processStillAliveAtEnd"] = war3._pid_alive(pid)
        if PROXY_ARM is not None:
            # The game ends by itself so the front end and the hosts write
            # their accounts (x64_proxy_gate_mode); stop_war3 stays the fallback.
            receipt["naturalExit"] = war3.natural_exit_owned_game(
                pid, layout.instance_root, str(launch.get("desktop", {}).get("name", "")))
        receipt["gpuEventsAfter"] = [war3._gpu_event_identity(x)
                                     for x in war3._query_windows_gpu_events()]
        receipt["newGpuEvents"] = sorted(set(receipt["gpuEventsAfter"])
                                          - set(receipt["gpuEventsBefore"]))
        receipt["gpuIncidents"] = [str(path) for path in
                                   (layout.instance_root / "WarVK/Log").glob("gpu_incident_*.json")]
        reports = sorted((layout.instance_root / "WarVK/Log").glob(
            "war3_perf_report*.html"))
        receipt["perfReports"] = [{"path": str(path), "size": path.stat().st_size,
                                   "sha256": sha256(path)} for path in reports]
        rows = [json.loads(line) for line in samples_path.read_text(
            encoding="utf-8").splitlines() if line]
        phase_summary = {}
        for name in ("A-first", "B", "A-return"):
            all_part = [row for row in rows if row["phase"] == name]
            epochs = [int(row["monitor"].get("shadowMapEpoch") or 0)
                      for row in all_part]
            part = [row for row in all_part if
                    int(row["monitor"].get("shadowMapEpoch") or 0) ==
                    PHASE_EPOCH[name]]
            by_frame = {int(row["frameIndex"] or 0): row for row in part
                        if int(row["frameIndex"] or 0) > 0}
            tail = list(by_frame.values())[-2:]
            phase_summary[name] = {
                "sampleCount": len(part), "allSampleCount": len(all_part),
                "uniqueFrameCount": len(by_frame),
                "settledEpoch": PHASE_EPOCH[name] if part else 0,
                "epochMin": min(epochs, default=0),
                "epochMax": max(epochs, default=0),
                "receiverCompleteTail": sum(int(row["monitor"].get(
                    "semanticSceneReceiverHasCompleteShadowMap") or 0)
                    for row in tail),
                "drawnCasterMax": max((int(row["monitor"].get(
                    "shadowReplayDrawnCasterCount") or 0) for row in part),
                    default=0),
                "producerReadyTail": sum(int(row["monitor"].get(
                    "shadowMapProducerReady") or 0) for row in tail),
                "retiredCountMax": max((int(row["monitor"].get(
                    "shadowRetiredSessionCount") or 0) for row in all_part),
                    default=0),
                "collectedMax": max((int(row["monitor"].get(
                    "shadowRetiredSessionCollectedCount") or 0) for row in all_part),
                    default=0),
                "resetRequestedMax": max((int(row["monitor"].get(
                    "shadowMapResetRequestedSerial") or 0) for row in all_part),
                    default=0),
                "resetAppliedMax": max((int(row["monitor"].get(
                    "shadowMapResetAppliedSerial") or 0) for row in all_part),
                    default=0),
            }
        receipt["phaseSummary"] = phase_summary
        epoch_timeline = []
        for row in rows:
            epoch = int(row["monitor"].get("shadowMapEpoch") or 0)
            if epoch and (not epoch_timeline or epoch != epoch_timeline[-1]):
                epoch_timeline.append(epoch)
        receipt["epochTimeline"] = epoch_timeline
        if PROXY_ARM is not None:
            evidence = x64mode.collect_evidence(
                PROXY_ARM.staging, [layout.artifact_dir], layout.instance_root)
            receipt["proxyEvidence"] = evidence
            receipt["proxyVerdict"] = verdict = proxy_verdict(
                receipt, rows, evidence, receipt.get("naturalExit") or {})
            receipt["transitionCanaryAccepted"] = verdict["accepted"]
            print(json.dumps({"mode": "proxy", "checks": verdict["checks"],
                              "generationTails": verdict["generationTails"],
                              "transitionCanaryAccepted": verdict["accepted"]},
                             ensure_ascii=False), flush=True)
            return 0 if verdict["accepted"] else 2
        receipt["mapEpochAdvanced"] = (
            epoch_timeline == [1, 2, 3] and
            all(phase_summary[name]["sampleCount"] >= 5 for name in PHASE_EPOCH))
        receipt["resetSerialAdvanced"] = (
            0 < phase_summary["B"]["resetRequestedMax"] <=
            phase_summary["B"]["resetAppliedMax"] <
            phase_summary["A-return"]["resetAppliedMax"] and
            phase_summary["A-return"]["resetRequestedMax"] <=
            phase_summary["A-return"]["resetAppliedMax"])
        receipt["retirementCoverage"] = (
            any(row["retiredCountMax"] > 0 or row["collectedMax"] > 0
                for row in phase_summary.values()))
        receipt["runtimeStateGate"] = all(
            row["sampleCount"] >= 5 and row["uniqueFrameCount"] >= 2 and
            row["receiverCompleteTail"] == 2 and row["producerReadyTail"] == 2 and
            row["drawnCasterMax"] > 0 for row in phase_summary.values())
        receipt["transitionCanaryAccepted"] = bool(receipt["sameProcess"] and
                                   receipt["processStillAliveAtEnd"] and
                                   receipt["resolution"].get("ok") and
                                   receipt["runtimeStateGate"] and
                                   receipt["mapEpochAdvanced"] and
                                   receipt["resetSerialAdvanced"] and
                                   receipt["retirementCoverage"] and
                                   receipt["telemetryErrors"] < 20 and
                                   not receipt["newGpuEvents"] and
                                   not receipt["gpuIncidents"] and
                                   len(receipt["screenshots"]) == 3 and
                                   all(x["ok"] and
                                       x["observedEpoch"] == x["expectedEpoch"]
                                       for x in receipt["screenshots"]))
        print(json.dumps({k: receipt[k] for k in
                          ("sameProcess", "processStillAliveAtEnd", "samples",
                           "sampleCountsByPhase", "telemetryErrors",
                           "mapEpochAdvanced", "resetSerialAdvanced",
                           "retirementCoverage", "runtimeStateGate",
                           "newGpuEvents", "gpuIncidents",
                           "transitionCanaryAccepted")},
                         ensure_ascii=False), flush=True)
        return 0 if receipt["transitionCanaryAccepted"] else 2
    finally:
        if launched:
            receipt["stop"] = war3.stop_war3(
                session_id=SESSION_ID, graceful_wait_sec=5,
                force=True, avoid_foreground_switch=True)
        receipt["sourceIdentityAfter"] = {str(path): sha256(path)
                                           for path in EXPECTED}
        receipt["sourceUnchanged"] = all(
            receipt["sourceIdentityAfter"][str(path)] == value
            for path, value in EXPECTED.items())
        receipt["playerDllAfter"] = x64mode.player_dll_state()
        receipt["playerDllUnchanged"] = x64mode.player_dll_unchanged(player_dll_before, receipt["playerDllAfter"])
        write_json_new(layout.artifact_dir / "cross-map-result.json", receipt)


if __name__ == "__main__":
    raise SystemExit(main())
