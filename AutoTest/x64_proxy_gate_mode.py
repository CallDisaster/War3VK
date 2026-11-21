"""Proxy mode for the real-game gates: the same product d3d9.dll, its device in the x64 render host.

Shared by the gate scripts that take `--mode inprocess|proxy` (default inprocess):
run_v12202_final_gate.py (cross-map A->B->A, Walk, life-and-death pressure),
run_weather_canary_20261010.py and run_startup_diag_gate.py.

The in-process arm of a gate keeps its own environment, assertions and outputs.
What both arms share from here is the player-directory guard: the player's
E:/Work/Warcraft III/d3d9.dll is absent and must stay absent - it is never
created, copied or restored - and each arm pins the sandbox DLL it actually
loads to the hash of the DLL under test.

The proxy arm adds:
  - the product's proxy-mode environment (WARVK_D3D9_FRONT and the transport
    profile the probes call "product", run_x64_game_proxy_probe.py PROFILES);
  - front end and host copied into the gate's own output folder, hashed;
  - the game's stdout/stderr captured (war3_autotest_mcp.game_stdio_capture),
    where the front end prints its session account when the device goes;
  - a natural exit before the usual stop (war3_autotest_mcp.natural_exit_owned_game),
    so that account and the host's are written at all;
  - evidence parsing and the transport-level acceptance every proxy arm shares.

Anything a gate cannot yet measure in proxy mode is recorded as NOT_APPLICABLE
with its reason - never as a pass.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
PLAYER_DLL = Path("E:/Work/Warcraft III/d3d9.dll")
FROZEN_HOST = ROOT / "build64_render_host_phase52_20261117_a/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe"
MODES = ("inprocess", "proxy")
NOT_APPLICABLE = "不适用"
HOST_LOST_EXIT = 0xE0574B01
# The product transport profile (run_x64_game_proxy_probe.py PROFILES["product"]).
PRODUCT_TRANSPORT = dict(
    WARVK_D3D9_BATCH_DRAWS="1", WARVK_D3D9_ASYNC_BATCH="1", WARVK_D3D9_ASYNC_EVENTS="1",
    WARVK_D3D9_STREAM="1", WARVK_D3D9_FIXED_BATCH="1", WARVK_D3D9_COMMAND_RING="1",
    WARVK_D3D9_X86_BACKING_KIB="32768", WARVK_D3D9_UPLOAD_WINDOW_KIB="1024")
ACCOUNT_LINE = re.compile(r"^D3D9 ([A-Za-z0-9-]+): (\{.*\})\s*$")


def not_applicable(reason: str) -> Dict[str, Any]:
    return {"status": NOT_APPLICABLE, "reason": reason}


def add_mode_arguments(parser) -> None:
    parser.add_argument("--mode", choices=MODES, default="inprocess",
                        help="inprocess (default, the shipped arm) or proxy (x64 render host)")
    parser.add_argument("--front", type=Path,
                        help="proxy mode: the x86 D3D9 front end DLL (tools/render_host/d3d9_proxy.cpp)")
    parser.add_argument("--host", type=Path, default=FROZEN_HOST,
                        help="proxy mode: the x64 warvk-d3d9-host-dev.exe")
    parser.add_argument("--proxy-env", action="append", default=[], metavar="NAME=VALUE",
                        help="proxy mode: a WARVK_D3D9_* transport switch on top of the product profile (repeatable)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def identity(path: Path) -> Dict[str, Any]:
    path = Path(path)
    return {"path": str(path), "size": path.stat().st_size, "sha256": sha256(path)}


def player_dll_state() -> Dict[str, Any]:
    """The player's DLL is only looked at: absent is the expected state."""
    if PLAYER_DLL.exists():
        return {"path": str(PLAYER_DLL), "exists": True, **identity(PLAYER_DLL)}
    return {"path": str(PLAYER_DLL), "exists": False}


def require_player_dll_absent(stage: str) -> Dict[str, Any]:
    state = player_dll_state()
    if state["exists"]:
        raise RuntimeError(f"player d3d9.dll must stay absent ({stage}): {PLAYER_DLL}")
    return state


def pe_bits(path: Path) -> int:
    data = Path(path).read_bytes()[:4096]
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[offset:offset + 4] != b"PE\0\0":
        raise ValueError(f"not a PE image: {path}")
    machine = struct.unpack_from("<H", data, offset + 4)[0]
    return {0x14C: 32, 0x8664: 64}.get(machine, 0)


class ProxyArm:
    """The proxy arm's front end and host, staged in the gate's own folder."""

    def __init__(self, front: Path, host: Path, staging: Path, extra_env: Optional[List[str]] = None):
        if front is None or host is None:
            raise ValueError("--mode proxy needs --front and --host")
        self.extra_env: Dict[str, str] = {}
        for item in extra_env or []:
            name, separator, value = item.partition("=")
            if not separator or not value or not name.startswith("WARVK_D3D9_"):
                raise ValueError(f"--proxy-env takes WARVK_D3D9_NAME=VALUE: {item}")
            self.extra_env[name] = value
        front, host = Path(front).resolve(), Path(host).resolve()
        if not front.is_file() or not host.is_file():
            raise FileNotFoundError(f"front end or host missing: {front} {host}")
        if pe_bits(front) != 32 or pe_bits(host) != 64:
            raise ValueError("an x86 front end and an x64 host are required")
        self.staging = Path(staging)
        self.staging.mkdir(parents=True, exist_ok=False)
        (self.staging / "logs").mkdir()
        self.front = self.staging / "front.dll"
        self.host = self.staging / "host.exe"
        shutil.copy2(front, self.front)
        shutil.copy2(host, self.host)
        self.sources = {"front": identity(front), "host": identity(host)}
        self.staged = {"front": identity(self.front), "host": identity(self.host)}
        if (self.sources["front"]["sha256"] != self.staged["front"]["sha256"]
                or self.sources["host"]["sha256"] != self.staged["host"]["sha256"]):
            raise RuntimeError("front end / host copy mismatch")
        self.host_log = self.staging / "host.log"

    def env(self, data_log_dir: Optional[Path] = None) -> Dict[str, str]:
        """What the game process gets on top of the gate's own overrides."""
        env = {
            "WARVK_D3D9_FRONT": str(self.front),
            "WARVK_D3D9_HOST_EXE": str(self.host),
            "WARVK_D3D9_HOST_LOG": str(self.host_log),
            "WARVK_D3D9_GAME": "sandbox-isolated-v1",
            "WARVK_D3D9_PROFILE": "presenter-isolated-v1",
            "WARVK_D3D9_ANNOTATIONS": "1",
            # The natural exit's stock EndGame (game.end_for_exit_test).
            "DXVK_WAR3_INTERNAL_EXIT_TEST": "1",
            **PRODUCT_TRANSPORT,
            **self.extra_env,
        }
        if data_log_dir is not None:
            env["DXVK_LOG_PATH"] = str(data_log_dir)
        return env

    def receipt(self) -> Dict[str, Any]:
        return {"sources": self.sources, "staged": self.staged, "hostLog": str(self.host_log),
                "transportExtra": dict(self.extra_env)}


def _json_or_none(text: str) -> Optional[dict]:
    try:
        value = json.loads(text)
        return value if isinstance(value, dict) else None
    except ValueError:
        return None


def _read(path: Path) -> str:
    try:
        return Path(path).read_text("utf-8", errors="replace")
    except OSError:
        return ""


def collect_evidence(staging: Path, data_log_dirs: List[Path], instance_root: Optional[Path]) -> Dict[str, Any]:
    """The proxy session's own accounts: front end (game.stderr/stdout),
    hosts (host.log*), data layer (War3_d3d9.log), the host-lost report."""
    staging = Path(staging)
    evidence: Dict[str, Any] = {"stdioCaptured": (staging / "game.stderr").is_file()}
    accounts: Dict[str, List[dict]] = {}
    game_lines = 0
    unsupported: set = set()
    other: List[str] = []
    for line in _read(staging / "game.stderr").splitlines():
        if line.startswith("D3D9 game: "):
            game_lines += 1
            if line.startswith("D3D9 game: unsupported "):
                unsupported.add(line[len("D3D9 game: unsupported "):][:120])
            continue
        match = ACCOUNT_LINE.match(line)
        row = _json_or_none(match.group(2)) if match else None
        if row is not None:
            accounts.setdefault(match.group(1), []).append(row)
        elif line.strip() and len(other) < 40:
            other.append(line[:240])
    evidence["frontAccounts"] = accounts
    evidence["frontTraceLines"] = game_lines
    evidence["unsupported"] = sorted(unsupported)
    evidence["frontOtherLines"] = other
    evidence["sessions"] = [row for row in (_json_or_none(line) for line in _read(staging / "game.stdout").splitlines()
                                            if line.startswith("{")) if row and row.get("schema") == "warvk-d3d9-proxy/v1"]
    hosts = []
    logs = sorted(staging.glob("host.log*"), key=lambda p: int(p.suffix[1:]) if p.suffix[1:].isdigit() else 0)
    for log in logs:
        named: Dict[str, dict] = {}
        bare: Dict[str, dict] = {}
        exit_lines = []
        for line in _read(log).splitlines():
            if line.startswith("{"):
                row = _json_or_none(line)
                if row is not None:
                    bare[str(row.get("schema"))] = row
                continue
            match = ACCOUNT_LINE.match(line)
            if match:
                row = _json_or_none(match.group(2))
                if row is not None:
                    named[match.group(1)] = row
            elif "exit-intent" in line or "host failure" in line or "host transport" in line:
                exit_lines.append(line[:200])
        main = bare.get("warvk-d3d9-host/v1") or {}
        hosts.append({"log": log.name, "main": {k: main.get(k) for k in (
            "ok", "accepted", "rejected", "draws", "submitted", "liveResources", "normalTeardown") if k in main},
            "accounts": {k: named[k] for k in ("annotation-host", "scene-effects-host", "shadow-host", "reset-host")
                         if k in named},
            "exitIntent": exit_lines})
    evidence["hosts"] = hosts
    data_lines: List[str] = []
    for directory in data_log_dirs:
        for log in sorted(Path(directory).glob("*_d3d9.log")):
            data_lines += [line.strip()[:400] for line in _read(log).splitlines() if "WarVK proxy mode:" in line]
    evidence["dataLayer"] = {
        "sessionResetLines": [l for l in data_lines if "session reset generation=" in l],
        "casterLines": [l for l in data_lines if "WarVK proxy mode: casters frame=" in l][-6:],
        "closingLine": next((l for l in reversed(data_lines) if "WarVK proxy mode: frames=" in l), None),
        "firstBeforeUi": next((l for l in data_lines if "first BeforeUi" in l), None),
        "lines": len(data_lines)}
    report = Path(instance_root) / "WarVK/Log/render-host-lost.txt" if instance_root else None
    evidence["hostLostReport"] = bool(report and report.is_file())
    if evidence["hostLostReport"]:
        evidence["hostLostReportText"] = _read(report)[:2000]
        shutil.copy2(report, staging / "render-host-lost.txt")
    return evidence


def last(accounts: Dict[str, List[dict]], name: str) -> dict:
    rows = accounts.get(name) or []
    return rows[-1] if rows else {}


def proxy_acceptance(evidence: Dict[str, Any], natural_exit: Optional[Dict[str, Any]],
                     world: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """The transport-level checks every proxy arm shares (research note §3.1 item 5),
    plus the world-frame evidence of gameplay (a posted key proves nothing)."""
    accounts = evidence.get("frontAccounts") or {}
    terminals = accounts.get("terminal") or []
    hosts = evidence.get("hosts") or []
    device_host = hosts[-1] if hosts else {}
    annotation = last(accounts, "annotation-proxy")
    effects = (device_host.get("accounts") or {}).get("scene-effects-host") or {}
    shadow = (device_host.get("accounts") or {}).get("shadow-host") or {}
    main = device_host.get("main") or {}
    # A Present while the window no longer matches the back buffer (a resize
    # awaiting Reset) returns d9::PresentSkipped (render_host/d3d9_wire.h:131-134:
    # "the transport stays healthy"), which the host's accepted/rejected tally
    # still counts as rejected (d3d9_host_main.cpp:1219). Those skips are the
    # designed resize path; any other rejection fails the check.
    skipped = int(((device_host.get("accounts") or {}).get("reset-host") or {}).get("presentsSkipped") or 0)
    rejected = main.get("rejected")
    checks = {
        "stdioCaptured": evidence.get("stdioCaptured") is True,
        "gameExitedByItself": bool(natural_exit and natural_exit.get("exited")),
        "terminalsClean": bool(terminals) and all(t.get("kind") == "none" and t.get("published") == t.get("applied")
                                                  for t in terminals),
        "hostsReturnedNormally": bool(hosts) and all(any("strategy=normal-return code=0" in l for l in h["exitIntent"])
                                                     for h in hosts),
        "hostRejectedZero": isinstance(rejected, int) and rejected - skipped == 0
                            and main.get("normalTeardown") is True
                            and main.get("liveResources") == 0,
        "insertionsRefusedZero": effects.get("insertionsRefused") == 0,
        "castersRefusedZero": shadow.get("castersRefused") == 0 and shadow.get("replayRejected", 0) == 0,
        "sessionResetsRefusedZero": annotation.get("sessionResetsRefused") == 0,
        "x86VulkanAbsent": bool(evidence.get("sessions")) and all(s.get("x86VulkanAbsent") is True
                                                                 for s in evidence["sessions"]),
        "noHostLostReport": evidence.get("hostLostReport") is False,
    }
    if world is not None:
        checks["worldFramesAdvanced"] = bool(world.get("advanced"))
    return {"ok": all(checks.values()), "checks": checks,
            "values": {"terminals": terminals[-1:] if terminals else [], "annotation": annotation,
                       "sceneEffects": {k: effects.get(k) for k in ("beforeUiRuns", "insertionsRefused")},
                       "shadow": {k: shadow.get(k) for k in ("casterRecords", "runs", "castersRefused", "replayRejected")},
                       "host": main, "hostPresentsSkipped": skipped}}


def proxy_status(status: Dict[str, Any]) -> Dict[str, Any]:
    """The control plane's / status file's proxyMode section (N1), or {}."""
    value = (status or {}).get("proxyMode")
    return value if isinstance(value, dict) else {}


def world_progress(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Gameplay readiness from evidence: the data layer's world scenes and
    BeforeUi insertions must advance between the first and the last sample."""
    frames = [(int((r.get("frame") or {}).get("worldScenes", 0) or 0),
               int((r.get("frame") or {}).get("insertions", 0) or 0)) for r in rows if r]
    if len(frames) < 3:
        return {"advanced": False, "samples": len(frames)}
    # Also over the last third: a game stuck on a screen without the world
    # (e.g. "press any key to continue") after some world frames fails here.
    late = frames[len(frames) * 2 // 3:]
    late_advanced = len(late) >= 2 and late[-1][0] > late[0][0]
    return {"advanced": frames[-1][0] > frames[0][0] and frames[-1][1] > frames[0][1] and late_advanced,
            "lateAdvanced": late_advanced,
            "samples": len(frames), "worldScenes": [frames[0][0], frames[-1][0]],
            "insertions": [frames[0][1], frames[-1][1]], "lateWorldScenes": [late[0][0], late[-1][0]]}
