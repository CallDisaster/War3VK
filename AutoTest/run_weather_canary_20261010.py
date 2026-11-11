"""Prepare/check/run an isolated real-JASS weather fixture.

Preparation creates only new files beneath this night-specific artifact tree.
Run requires an explicit candidate DLL hash, successful pjass receipt and the
existing AutoTest sandbox. No player installation writes or global input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "AutoTest/artifacts/weather-night-20261010"
SOURCE_MAP = Path("E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x")
SOURCE_MAP_SHA = "692E92F43BAC99EF8ECED7833E137B22475914F592DA7F78FCC24B54C221E95F"
SANDBOX = Path("E:/Work/War3_AutoTestSandbox")
PLAYER_DLL = Path("E:/Work/Warcraft III/d3d9.dll")
PLAYER_GAME = Path("E:/Work/Warcraft III/Game.dll")
API = ROOT / "WarVK/jass/warvk_api.j"
HARNESS = ROOT / "AutoTest/weather_canary_20261010.j"
YDWE = Path("E:/Work/War3/YDWE1.32.13 - MemoryHack")
PS32 = Path("C:/Windows/SysWOW64/WindowsPowerShell/v1.0/powershell.exe")
PJASS = YDWE / "compiler/pjass/pjass-latest.exe"
COMMON = YDWE / "compiler/jass/24/common.j"
BLIZZARD = YDWE / "compiler/jass/24/blizzard.j"
STORM = YDWE / "bin/StormLib.dll"
RECEIPT = re.compile(
    r"WEATHER_CANARY_STAGE=(\d+);checks=(\d+);errors=(\d+);id=(-?\d+);time=([-+\d.eE]+)"
)
QUERY_DIAGNOSTIC = re.compile(
    r'WEATHER_QUERY_DIAGNOSTIC=scientific:([-+\d.eE]+);decimal:([-+\d.eE]+);playRaw:([^"\r\n]*)'
)
STAGE_COUNT = 15
EXPECTED_CHECKS = (9, 10, 11, 12, 13, 14, 20, 21, 23, 25, 28, 31, 32, 34, 35)
CAPTURES = {0: "baseline", 1: "held-snow-only", 2: "held-haze-only",
            3: "held-low-snow-only", 4: "held-full-weather", 5: "paused",
            6: "seek-paused", 7: "resumed", 8: "disabled", 9: "reenabled",
            11: "timeline-interpolating", 12: "timeline-full", 13: "destroyed"}
PARAM_FIELDS = ("snowfall", "windAngle", "windSpeed", "gust", "haze", "lowSnow")
HELD_PARAMS = {
    1: (0.35, 350.0, 40.0, 0.10, 0.0, 0.0),
    2: (0.0, 350.0, 40.0, 0.10, 0.65, 0.0),
    3: (0.0, 90.0, 100.0, 0.20, 0.0, 0.80),
    4: (1.0, 10.0, 420.0, 0.80, 0.75, 0.80),
}

# Derived from the audited local StormLib helper. All outputs must belong to
# this invocation's CreateNew artifact directory; the source MPQ is read-only.
MPQ_HELPER = r'''
param([string]$InputMap,[string]$OutputRoot,[string]$ReplacementScript)
$ErrorActionPreference='Stop'
if ([IntPtr]::Size -ne 4) { throw '32-bit PowerShell required' }
$source=(Resolve-Path -LiteralPath $InputMap).Path
$out=(Resolve-Path -LiteralPath $OutputRoot).Path
$allowed=[IO.Path]::GetFullPath('@ARTIFACT_ROOT@')+'\'
if (-not ($out+'\').StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw 'Output escaped artifact root' }
for ($checkDir=[IO.DirectoryInfo]$out; $null -ne $checkDir; $checkDir=$checkDir.Parent) {
  if (($checkDir.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse output refused' }
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WeatherStorm {
 const string Dll=@"E:\Work\War3\YDWE1.32.13 - MemoryHack\bin\StormLib.dll";
 [DllImport(Dll,CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.I1)]
 public static extern bool SFileOpenArchive(string p,uint pr,uint fl,out IntPtr a);
 [DllImport(Dll,SetLastError=true)] [return:MarshalAs(UnmanagedType.I1)]
 public static extern bool SFileOpenFileEx(IntPtr a,[MarshalAs(UnmanagedType.LPStr)] string n,uint s,out IntPtr f);
 [DllImport(Dll)] public static extern uint SFileGetFileSize(IntPtr f,out uint high);
 [DllImport(Dll)] [return:MarshalAs(UnmanagedType.I1)]
 public static extern bool SFileReadFile(IntPtr f,byte[] b,uint sz,out uint read,IntPtr o);
 [DllImport(Dll)] [return:MarshalAs(UnmanagedType.I1)] public static extern bool SFileCloseFile(IntPtr f);
 [DllImport(Dll)] [return:MarshalAs(UnmanagedType.I1)] public static extern bool SFileCloseArchive(IntPtr a);
 [DllImport(Dll,CharSet=CharSet.Unicode)] [return:MarshalAs(UnmanagedType.I1)]
 public static extern bool SFileAddFileEx(IntPtr a,string src,[MarshalAs(UnmanagedType.LPStr)] string n,uint fl,uint c,uint nc);
}
'@
function Read-WeatherScript([string]$map,[string]$target) {
 $a=[IntPtr]::Zero; $f=[IntPtr]::Zero; $entry='war3map.j'
 try {
  if (-not [WeatherStorm]::SFileOpenArchive($map,0,0x100,[ref]$a)) { throw 'Readonly MPQ open failed' }
  $missing=[IntPtr]::Zero
  if ([WeatherStorm]::SFileOpenFileEx($a,'__WEATHER_CANARY_MUST_NOT_EXIST__',0,[ref]$missing) -or $missing -ne [IntPtr]::Zero) { throw 'StormLib bool ABI check failed' }
  if (-not [WeatherStorm]::SFileOpenFileEx($a,$entry,0,[ref]$f)) {
   $entry='scripts\war3map.j'
   if (-not [WeatherStorm]::SFileOpenFileEx($a,$entry,0,[ref]$f)) { throw 'Canonical script missing' }
  }
  [uint32]$high=0; [uint32]$read=0
  $size=[WeatherStorm]::SFileGetFileSize($f,[ref]$high)
  if ($high -ne 0 -or $size -gt 16777216) { throw 'Script size rejected' }
  $bytes=New-Object byte[] $size
  if (-not [WeatherStorm]::SFileReadFile($f,$bytes,$size,[ref]$read,[IntPtr]::Zero) -or $read -ne $size) { throw 'Incomplete script read' }
  $s=[IO.File]::Open($target,[IO.FileMode]::CreateNew)
  try {$s.Write($bytes,0,$bytes.Length)} finally {$s.Dispose()}
  return $entry
 } finally {
  if ($f -ne [IntPtr]::Zero) {[void][WeatherStorm]::SFileCloseFile($f)}
  if ($a -ne [IntPtr]::Zero) {[void][WeatherStorm]::SFileCloseArchive($a)}
 }
}
$entry=Read-WeatherScript $source (Join-Path $out 'original-war3map.j')
$replacement=(Resolve-Path -LiteralPath $ReplacementScript).Path
if (-not $replacement.StartsWith($out+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Replacement escaped invocation root' }
$target=Join-Path $out 'weather-canary.w3x'
if (Test-Path -LiteralPath $target) { throw 'Output map exists' }
[IO.File]::Copy($source,$target,$false)
$archive=[IntPtr]::Zero
try {
 if (-not [WeatherStorm]::SFileOpenArchive($target,0,0,[ref]$archive)) { throw 'Copied MPQ open failed' }
 if (-not [WeatherStorm]::SFileAddFileEx($archive,$replacement,$entry,[uint32]2147484160,2,2)) { throw 'Copied script replacement failed' }
} finally {if ($archive -ne [IntPtr]::Zero) {[void][WeatherStorm]::SFileCloseArchive($archive)}}
$readbackEntry=Read-WeatherScript $target (Join-Path $out 'readback-war3map.j')
if ($entry -cne $readbackEntry) {throw 'Canonical entry changed'}
Write-Output ('entry='+$entry)
'''


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def identity(path: Path) -> dict:
    return {"path": str(path), "size": path.stat().st_size, "sha256": sha256(path)}


def resize_owned_isolated_client(war3, launch: dict) -> dict:
    """Make only this isolated HWND borderless, never the user's desktop."""
    import ctypes
    from ctypes import wintypes
    if (not launch.get("ok") or not launch.get("desktop", {}).get("ok")
            or not launch.get("desktop", {}).get("name")
            or int(launch.get("desktop", {}).get("handle", 0) or 0) <= 0):
        raise RuntimeError("owned isolated desktop receipt required for resize")
    desktop_handle = int(launch["desktop"]["handle"])
    pid = int(launch["pid"])
    root = Path(launch["instanceRoot"]).resolve()
    if not root.is_relative_to((SANDBOX / "_AutoTestInstances").resolve()):
        raise RuntimeError("owned resize root escaped sandbox")
    hwnd = war3._wait_for_main_window_hwnd(pid, timeout_sec=8.0, require_visible=True)
    if not hwnd:
        raise RuntimeError("owned isolated window not found")
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    owner = wintypes.DWORD()
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    thread_id = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if not thread_id or owner.value != pid:
        raise RuntimeError("resize HWND is not owned by this session PID")
    user32.GetUserObjectInformationW.argtypes = [wintypes.HANDLE, ctypes.c_int,
        wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    user32.GetUserObjectInformationW.restype = wintypes.BOOL
    user32.OpenInputDesktop.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    user32.OpenInputDesktop.restype = wintypes.HANDLE
    user32.CloseDesktop.argtypes = [wintypes.HANDLE]
    user32.CloseDesktop.restype = wintypes.BOOL

    def desktop_name(handle) -> str:
        if not handle:
            raise RuntimeError("desktop handle unavailable")
        needed = wintypes.DWORD()
        ctypes.set_last_error(0)
        queried = user32.GetUserObjectInformationW(handle, 2, None, 0, ctypes.byref(needed))
        error = ctypes.get_last_error()
        if not queried and error != 122:  # Expected ERROR_INSUFFICIENT_BUFFER.
            raise ctypes.WinError(error)
        if needed.value < ctypes.sizeof(ctypes.c_wchar) or needed.value > 65536:
            raise RuntimeError("invalid desktop name buffer requirement")
        name = ctypes.create_unicode_buffer(
            (needed.value + ctypes.sizeof(ctypes.c_wchar) - 1) // ctypes.sizeof(ctypes.c_wchar))
        if not user32.GetUserObjectInformationW(handle, 2, name,
                ctypes.sizeof(name), ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not name.value:
            raise RuntimeError("empty desktop name")
        return name.value

    # The launcher retains this owned desktop capability. GetThreadDesktop
    # cannot reliably query the foreign game thread; membership is instead
    # proven by enumerating this exact retained HDESK below. Do not close it.
    actual_desktop = desktop_name(desktop_handle)
    expected_desktop = str(launch["desktop"]["name"])
    if actual_desktop.casefold() != expected_desktop.casefold():
        raise RuntimeError("retained desktop name differs from isolated launch")
    input_handle = user32.OpenInputDesktop(0, False, 0x0001)  # DESKTOP_READOBJECTS
    if not input_handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        input_desktop = desktop_name(input_handle)
    finally:
        if not user32.CloseDesktop(input_handle):
            raise ctypes.WinError(ctypes.get_last_error())
    if actual_desktop.casefold() == input_desktop.casefold():
        raise RuntimeError("owned resize refused on the interactive input desktop")
    enum_callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumDesktopWindows.argtypes = [wintypes.HANDLE, enum_callback_type, wintypes.LPARAM]
    user32.EnumDesktopWindows.restype = wintypes.BOOL
    matches = []
    callback_errors = []

    @enum_callback_type
    def enum_owned_window(candidate, _parameter):
        try:
            if int(candidate or 0) == int(hwnd):
                candidate_owner = wintypes.DWORD()
                candidate_thread = user32.GetWindowThreadProcessId(
                    candidate, ctypes.byref(candidate_owner))
                matches.append({"hwnd": int(candidate), "pid": int(candidate_owner.value),
                    "threadId": int(candidate_thread)})
        except Exception as error:
            callback_errors.append(str(error))
        # Never short-circuit: False would be indistinguishable from an API
        # failure and would prevent detection of duplicate target matches.
        return True

    ctypes.set_last_error(0)
    enum_succeeded = user32.EnumDesktopWindows(desktop_handle, enum_owned_window, 0)
    if not enum_succeeded:
        error = ctypes.get_last_error()
        if error:
            raise ctypes.WinError(error)
        raise RuntimeError("retained desktop window enumeration failed")
    if callback_errors:
        raise RuntimeError(f"retained desktop window enumeration callback failed: {callback_errors}")
    expected_match = {"hwnd": int(hwnd), "pid": pid, "threadId": int(thread_id)}
    if matches != [expected_match]:
        raise RuntimeError("owned HWND/PID/TID membership not proven on retained isolated desktop")
    user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.GetWindowLongW.restype = wintypes.LONG
    user32.SetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.LONG]
    user32.SetWindowLongW.restype = wintypes.LONG
    before = {}
    for index, name, remove, add in ((-16, "style", 0x00C40000, 0x80000000),
                                    (-20, "exStyle", 0x00000300, 0)):
        ctypes.set_last_error(0)
        old_result = user32.GetWindowLongW(hwnd, index)
        if old_result == 0 and ctypes.get_last_error():
            raise ctypes.WinError(ctypes.get_last_error())
        old = int(old_result) & 0xffffffff
        before[name] = old
        value = (old & ~remove) | add
        ctypes.set_last_error(0)
        result = user32.SetWindowLongW(hwnd, index, ctypes.c_long(value).value)
        if result == 0 and ctypes.get_last_error():
            raise ctypes.WinError(ctypes.get_last_error())
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user32.SetWindowPos.restype = wintypes.BOOL
    if not user32.SetWindowPos(hwnd, None, 0, 0, 2560, 1440, 0x0034):
        raise ctypes.WinError(ctypes.get_last_error())
    # FRAMECHANGED | NOZORDER | NOACTIVATE above; no input or desktop switch.
    result = war3._resize_window_client_native(pid, 2560, 1440, x=0, y=0)
    if not result.get("ok") or int(result.get("hwnd", 0)) != int(hwnd):
        raise RuntimeError("owned resize helper failed or selected a different HWND")
    user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.GetClientRect.restype = wintypes.BOOL
    client = wintypes.RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(client)):
        raise ctypes.WinError(ctypes.get_last_error())
    actual_dimensions = [int(client.right - client.left), int(client.bottom - client.top)]
    if actual_dimensions != [2560, 1440]:
        raise RuntimeError(f"owned client resize mismatch: {actual_dimensions}")
    return {**result, "pid": pid, "hwnd": int(hwnd), "beforeStyles": before,
        "threadId": int(thread_id), "actualLaunchDesktop": actual_desktop,
        "ownedLaunchDesktopHandle": desktop_handle,
        "desktopMembership": {"proof": "EnumDesktopWindows", "enumSucceeded": True,
                              "targetMatches": matches},
        "expectedLaunchDesktop": expected_desktop, "inputDesktopAtResize": input_desktop,
        "inputDesktopHandleClosed": True, "actualClientDimensions": actual_dimensions,
        "borderlessOwnedWindow": True, "globalInputUsed": False,
        "registryWritePerformed": False}


def write_new(path: Path, content: str) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(content)


def split_globals(text: str) -> tuple[str, str]:
    text = text.replace("\r\n", "\n")
    match = re.match(r"globals\n(.*?)^endglobals\n", text, re.M | re.S)
    if not match:
        raise ValueError("one leading globals block required")
    return match.group(1), text[match.end():]


def artifact_dir(name: str) -> Path:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,60}", name):
        raise ValueError("invalid artifact name")
    path = ARTIFACTS / name
    resolved = path.resolve()
    if not resolved.is_relative_to(ARTIFACTS.resolve()):
        raise ValueError("artifact path escaped root")
    # The PS helper and the AutoTest session layer independently reject reparse.
    for parent in (path, *path.parents):
        if parent.exists() and parent.is_symlink():
            raise ValueError(f"symlink artifact path refused: {parent}")
    return path


def prepare(args: argparse.Namespace, suite=None) -> int:
    out = artifact_dir(args.name)
    harness = suite.HARNESS if suite else HARNESS
    for dependency in (SOURCE_MAP, API, harness, PS32, STORM, PJASS, COMMON, BLIZZARD):
        if not dependency.is_file():
            raise FileNotFoundError(dependency)
    before = identity(SOURCE_MAP)
    if before["sha256"] != args.expected_map_sha.upper():
        raise ValueError("source map identity differs from explicit pin")
    ag, ab = split_globals(API.read_text(encoding="utf-8-sig"))
    hg, hb = split_globals(harness.read_text(encoding="utf-8-sig"))
    names = re.findall(r"^function (\w+) takes", ab + hb, re.M)
    if len(names) != len(set(names)) or names.count("main") != 1 or names.count("config") != 1:
        raise ValueError("merged JASS function ambiguity")
    out.mkdir(parents=True, exist_ok=False)
    script = out / "war3map.j"
    write_new(script, "globals\n" + ag + hg + "endglobals\n" + ab + hb)
    helper = out / "weather-mpq-copy.ps1"
    write_new(helper, MPQ_HELPER.replace("@ARTIFACT_ROOT@", str(ARTIFACTS).replace("'", "''")))
    copied = subprocess.run([str(PS32), "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(helper), "-InputMap", str(SOURCE_MAP), "-OutputRoot", str(out),
        "-ReplacementScript", str(script)], capture_output=True, text=True, timeout=60)
    write_new(out / "mpq-copy.txt", copied.stdout + copied.stderr)
    if copied.returncode:
        raise RuntimeError("MPQ copy failed; preserve new artifact and inspect mpq-copy.txt")
    if (out / "readback-war3map.j").read_bytes() != script.read_bytes():
        raise RuntimeError("exact MPQ script readback failed")
    if identity(SOURCE_MAP) != before:
        raise RuntimeError("source map changed during readonly preparation")
    receipt = {"sourceMap": before, "map": identity(out / "weather-canary.w3x"),
        "originalScript": identity(out / "original-war3map.j"), "script": identity(script),
        "api": identity(API), "harness": identity(harness),
        "wrapperCount": len(re.findall(r"^function (\w+) takes", ab, re.M)),
        "tools": {str(p): identity(p) for p in (PS32, STORM, PJASS, COMMON, BLIZZARD)},
        "exactReadback": True, "sourceMapUnchanged": True,
        "originalGameplayTriggersIncluded": False, "jassSyntaxChecked": False,
        "gameLaunched": False, "globalInputUsed": False}
    if suite:
        receipt["suiteContract"] = identity(Path(suite.__file__))
    receipt["suite"] = "stress" if suite else "functional"
    write_new(out / "prepare.json", json.dumps(receipt, indent=2, ensure_ascii=False))
    print(json.dumps({"preparedMap": receipt["map"], "jassSyntaxChecked": False}, ensure_ascii=False))
    return 0


def check_jass(args: argparse.Namespace) -> int:
    out = artifact_dir(args.name)
    prepared = json.loads((out / "prepare.json").read_text(encoding="utf-8"))
    for name in ("map", "script", "api", "harness"):
        row = prepared[name]
        if identity(Path(row["path"])) != row:
            raise RuntimeError(f"prepared identity drift: {name}")
    for path, row in prepared["tools"].items():
        if identity(Path(path)) != row:
            raise RuntimeError(f"tool identity drift: {path}")
    checked = subprocess.run([str(PJASS), str(COMMON), str(BLIZZARD), str(out / "war3map.j")],
                             capture_output=True, text=True, timeout=30)
    write_new(out / "pjass.txt", checked.stdout + checked.stderr)
    write_new(out / "jass-check.json", json.dumps({"exitCode": checked.returncode,
        "script": identity(out / "war3map.j"), "tools": prepared["tools"],
        "realJassVmExecuted": False}, indent=2))
    if checked.returncode:
        raise RuntimeError("pjass rejected canary; inspect pjass.txt")
    print("pjass passed; real JASS VM remains unexecuted")
    return 0


def stage_receipts(instance: Path) -> list[dict]:
    result = []
    for path in sorted((instance / "WarVK/Temp").glob("weather_canary_stage_*.txt")):
        text = path.read_bytes().decode("utf-8", errors="replace")
        match = RECEIPT.search(text)
        if not match:
            continue
        stage, checks, errors, weather_id, seconds = match.groups()
        result.append({"stage": int(stage), "checks": int(checks), "errors": int(errors),
            "id": int(weather_id), "timeSeconds": float(seconds), "path": str(path),
            "sha256": sha256(path), "raw": text})
        diagnostic = QUERY_DIAGNOSTIC.search(text)
        if diagnostic:
            scientific, decimal, raw_query = diagnostic.groups()
            result[-1]["queryDiagnostic"] = {
                "stockS2RScientific": float(scientific),
                "stockS2RDecimal": float(decimal), "playRaw": raw_query.strip()}
        tiny = re.search(r'WEATHER_TINY_QUERY_RAW=([^"\r\n]*)', text)
        if tiny:
            result[-1]["tinyQueryRaw"] = tiny.group(1).strip()
    return sorted(result, key=lambda row: row["stage"])


def image_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(32)
    if header.startswith(b"BM") and len(header) >= 26:
        width, height = struct.unpack_from("<ii", header, 18)
        declared_size = struct.unpack_from("<I", header, 2)[0]
        if width == 0 or height == 0 or declared_size < 54 or path.stat().st_size < declared_size:
            raise RuntimeError("incomplete or invalid BMP screenshot")
        return abs(width), abs(height)
    if header.startswith(b"\x89PNG\r\n\x1a\n") and len(header) >= 24:
        return struct.unpack_from(">II", header, 16)
    raise RuntimeError("unsupported or truncated screenshot")


def frame_alignment(result: dict) -> dict:
    runtime_frame = int(result.get("runtime", {}).get("frameSerial", 0))
    frozen = result.get("frozenInput", {})
    frozen_frame = int(frozen.get("frameSerial", 0))
    answer = {"queryFrameSerial": runtime_frame, "frozenInputFrameSerial": frozen_frame,
        "runtimeInterpretation": "owner live state, not render input",
        "frozenInputKind": frozen.get("observationKind", "unavailable")}
    for name in ("render", "volumetric"):
        value = result.get(name, {})
        observed = int(value.get("frameSerial", 0))
        matching = frozen.get("observationKind") == "render-consumed-value-snapshot"
        matching = matching and observed > 0 and observed == frozen_frame and all(
            int(value.get(field, 0)) > 0
            and int(value.get(field, 0)) == int(frozen.get(field, 0))
            for field in ("mapEpoch", "deviceEpoch"))
        if name == "render":
            matching = matching and int(value.get("id", 0)) == int(frozen.get("id", 0))
            matching = matching and abs(float(value.get("timeSeconds", 0))
                                      - float(frozen.get("timeSeconds", 0))) < 1.0e-5
        answer[name] = {"frameSerial": observed,
            "sameFrozenFrame": matching,
            "liveQueryLagFrames": runtime_frame - observed
                if observed > 0 and runtime_frame > 0 else None,
            "interpretation": "matches frozen render input" if matching
                else "unmatched frozen frame/identity; not joint-frame evidence"}
    return answer


def expected_params(stage: int, seconds: float) -> dict:
    if stage in HELD_PARAMS:
        return dict(zip(PARAM_FIELDS, HELD_PARAMS[stage]))
    if stage in (5, 6, 7, 9, 12):
        return dict(zip(PARAM_FIELDS, HELD_PARAMS[4]))
    if stage == 11:
        fraction = min(max(seconds / 6.0, 0.0), 1.0)
        light = (0.15, 350.0, 40.0, 0.10, 0.0, 0.0)
        full = HELD_PARAMS[4]
        result = {key: start + (end - start) * fraction
                  for key, start, end in zip(PARAM_FIELDS, light, full)}
        result["windAngle"] = (350.0 + 20.0 * fraction) % 360.0
        return result
    return {}


def phase_matches(observation: dict, stage: int, stages: dict) -> bool:
    frozen = observation.get("frozenInput", {})
    start = stages.get(stage)
    if (not start or frozen.get("enabled") is not True or int(start["id"]) <= 0
            or frozen.get("observationKind") != "render-consumed-value-snapshot"
            or int(frozen.get("id", 0)) != start["id"]
            or frozen.get("paused") is not (stage in (5, 6))):
        return False
    seconds = float(frozen.get("timeSeconds", float("nan")))
    if not math.isfinite(seconds):
        return False
    lower = max(0.0, start["timeSeconds"] - 0.10)
    upper = lower + (0.20 if stage in (5, 6) else 8.75)
    if stage == 11:
        upper = 6.0
    if not lower <= seconds < upper:
        return False
    expected = expected_params(stage, seconds)
    actual = frozen.get("params", {})
    if not expected:
        return False
    for field, reference in expected.items():
        value = float(actual.get(field, float("nan")))
        difference = abs(value - reference)
        if field == "windAngle" and math.isfinite(value):
            difference = abs((value - reference + 180.0) % 360.0 - 180.0)
        if not math.isfinite(value) or difference > 0.001:
            return False
    return True


def active_capture_matches(observation: dict, stage: int, stages: dict) -> bool:
    if not phase_matches(observation, stage, stages):
        return False
    aligned = frame_alignment(observation)
    if not aligned["render"]["sameFrozenFrame"]:
        return False
    params = observation["frozenInput"]["params"]
    snow = params["snowfall"] > 0
    draw = observation.get("render", {})
    if draw.get("commandsRecorded") is not snow:
        return False
    if draw.get("stage") != ("commands-recorded" if snow else "no-snowfall"):
        return False
    medium = params["haze"] > 0 or params["lowSnow"] > 0
    volume = observation.get("volumetric", {})
    if volume.get("compositeSubmitted") is not medium:
        return False
    if medium and (not aligned["volumetric"]["sameFrozenFrame"]
            or volume.get("stage") != "composite-submitted"):
        return False
    return True


def classify(receipt: dict) -> dict:
    stage_rows = receipt.get("stageReceipts", [])
    stages = {row["stage"]: row for row in stage_rows}
    samples = receipt.get("snapshots", [])
    def during(stage: int) -> list[dict]:
        return [row for row in samples if row.get("jassStage") == stage
                and row.get("stageAtStatusCompletion") == stage]
    def recorded(stage: int) -> bool:
        for row in during(stage):
            obs = row["observation"]
            # Derived alignments are descriptive output, never trusted proof.
            aligned = frame_alignment(obs)
            draw = obs.get("render", {})
            if (draw.get("commandsRecorded") is True
                    and draw.get("stage") == "commands-recorded"
                    and int(draw.get("capacity", 0)) == 16384
                    and abs(int(draw.get("particleCount", 0)) -
                            int(16384 * obs.get("frozenInput", {}).get("params", {}).get("snowfall", 0))) <= 1
                    and int(draw.get("particleCount", 0)) > 0
                    and aligned["render"]["sameFrozenFrame"]
                    and phase_matches(obs, stage, stages)):
                return True
        return False
    def volume(stage: int) -> bool:
        for row in during(stage):
            obs = row["observation"]
            aligned = frame_alignment(obs)
            if (obs.get("volumetric", {}).get("compositeSubmitted")
                    and aligned["volumetric"]["sameFrozenFrame"]
                    and obs.get("volumetric", {}).get("stage") == "composite-submitted"
                    and 0 < int(obs.get("volumetric", {}).get("resourceBytes", 0)) <= 128 * 1024 * 1024
                    and aligned["render"]["sameFrozenFrame"] and phase_matches(obs, stage, stages)):
                return True
        return False
    def stable_clock(stage: int, enabled: bool, paused: bool | None) -> bool:
        start = stages.get(stage)
        if not start or int(start["id"]) <= 0:
            return False
        times = []
        for row in during(stage):
            state = row["observation"].get("runtime", {})
            seconds = float(state.get("timeSeconds", float("nan")))
            if (state.get("observationKind") != "owner-control-live-state-not-render-input"
                    or int(state.get("id", 0)) != start["id"]
                    or state.get("enabled") is not enabled
                    or (paused is not None and state.get("paused") is not paused)
                    or not math.isfinite(seconds)
                    or abs(seconds - start["timeSeconds"]) >= 0.08):
                continue
            times.append(seconds)
        return len(times) >= 2 and max(times) - min(times) < 0.08
    def no_snow(stage: int) -> bool:
        return any(phase_matches(row["observation"], stage, stages)
            and frame_alignment(row["observation"])["render"]["sameFrozenFrame"]
            and row["observation"]["render"].get("commandsRecorded") is False
            and row["observation"]["render"].get("stage") == "no-snowfall"
            and row["observation"]["render"].get("particleCount") == 0
            for row in during(stage))
    def progressing(stage: int) -> bool:
        values = [row["observation"]["frozenInput"] for row in during(stage)
            if phase_matches(row["observation"], stage, stages)
            and frame_alignment(row["observation"])["render"]["sameFrozenFrame"]]
        return any(int(b["frameSerial"]) > int(a["frameSerial"])
            and float(b["timeSeconds"]) - float(a["timeSeconds"]) > 0.25
            for a, b in zip(values, values[1:]))
    def off_observed(stage: int, obs: dict) -> bool:
        state, draw, volume = (obs.get(name, {}) for name in ("runtime", "render", "volumetric"))
        frozen = obs.get("frozenInput", {})
        expected_id = 0 if stage in (13, 14) else stages.get(stage, {}).get("id", -1)
        base = (state.get("observationKind") == "owner-control-live-state-not-render-input"
            and state.get("enabled") is False and state.get("id") == expected_id
            and draw.get("commandsRecorded") is False and draw.get("id") == 0
            and draw.get("stage") == "disabled" and draw.get("particleCount") == 0
            and frozen.get("enabled") is False and frozen.get("id") == 0
            and volume.get("compositeSubmitted") is False)
        if not base:
            return False
        if stage == 0:
            return True  # No weather draw has yet established frame identity.
        prior = 7 if stage == 8 else 9 if stage == 10 else 12
        previous_frames = [int(row["observation"]["frozenInput"].get("frameSerial", 0))
            for row in during(prior) if phase_matches(row["observation"], prior, stages)]
        return (bool(previous_frames) and int(draw.get("frameSerial", 0)) > max(previous_frames)
            and volume.get("stage") == "disabled"
            and int(volume.get("frameSerial", 0)) >= int(draw["frameSerial"])
            and all(int(draw.get(field, 0)) > 0 and draw[field] == volume.get(field)
                    for field in ("mapEpoch", "deviceEpoch")))
    def render_off(stage: int) -> bool:
        return any(off_observed(stage, row["observation"]) for row in during(stage))
    def capture_bracket(row: dict) -> bool:
        stage = row.get("stage")
        if row.get("stageAtRequest") != stage or row.get("stageAtCompletion") != stage:
            return False
        before, after = row.get("beforeCaptureObservation", {}), row.get("afterCaptureObservation", {})
        if stage in (0, 8, 13):
            return off_observed(stage, before) and off_observed(stage, after)
        if not (active_capture_matches(before, stage, stages)
                and active_capture_matches(after, stage, stages)):
            return False
        ba, aa = frame_alignment(before), frame_alignment(after)
        if not (ba["render"]["sameFrozenFrame"] and aa["render"]["sameFrozenFrame"]):
            return False
        bf, af = before["frozenInput"], after["frozenInput"]
        return (af["frameSerial"] > bf["frameSerial"]
            and all(af.get(field) == bf.get(field) for field in ("id", "mapEpoch", "deviceEpoch")))
    captures = receipt.get("captures", [])
    capture_phases = {(row.get("stage"), row.get("label")) for row in captures}
    hashes = {row.get("stage"): row.get("sha256") for row in captures}
    jass_complete = len(stage_rows) == STAGE_COUNT and set(stages) == set(range(STAGE_COUNT))
    query = stages.get(11, {}).get("queryDiagnostic", {})
    raw_time = query.get("playRaw", "")
    try:
        fixed_time = float(raw_time)
        query_consistent = (bool(re.fullmatch(r"\d+(?:\.\d+)?", raw_time))
            and math.isfinite(fixed_time) and 0 <= fixed_time < 0.08
            and abs(query.get("stockS2RScientific", float("nan")) - 9.73) < 0.001
            and query.get("stockS2RDecimal") == 0.0
            and abs(stages[11]["timeSeconds"] - fixed_time) < 0.08)
    except (ValueError, TypeError, KeyError):
        query_consistent = False
    tiny_raw = stages.get(6, {}).get("tinyQueryRaw", "")
    try:
        tiny_consistent = (bool(re.fullmatch(r"\d+(?:\.\d+)?", tiny_raw))
            and abs(float(tiny_raw) - 0.0000973) < 0.000001)
    except (ValueError, TypeError):
        tiny_consistent = False
    return {"allJassStagesObserved": jass_complete,
        "jassChecksPassed": bool(stages.get(14)) and stages[14]["errors"] == 0
            and jass_complete
            and all(row["errors"] == 0 and row["checks"] == EXPECTED_CHECKS[row["stage"]]
                and row["id"] > 0 and row["id"] == stages[0]["id"]
                and math.isfinite(row["timeSeconds"]) and row["timeSeconds"] >= 0
                for row in stage_rows),
        "immediateJassTimeQueryConsistent": query_consistent,
        "tinyPausedJassTimeQueryConsistent": tiny_consistent,
        "snowOnlyCommandsRecordedAtFrozenInput": recorded(1),
        "snowOnlyMediumNotSubmitted": any(phase_matches(row["observation"], 1, stages)
            and row["observation"].get("volumetric", {}).get("compositeSubmitted") is False
            for row in during(1)),
        "hazeOnlyVolumeAtFrozenInput": volume(2) and no_snow(2),
        "lowSnowOnlyVolumeAtFrozenInput": volume(3) and no_snow(3),
        "fullWeatherAtFrozenInput": recorded(4) and volume(4),
        "pausedClockStable": stable_clock(5, True, True),
        "pausedRenderInputObserved": recorded(5),
        "seekPausedRenderInputObserved": recorded(6),
        "resumedClockProgressAtFrozenInput": recorded(7) and progressing(7),
        "disabledClockStable": stable_clock(8, False, None),
        "disabledRenderAndVolumeObserved": render_off(8),
        "reenabledClockProgressAtFrozenInput": recorded(9) and progressing(9),
        "timelineInterpolationAtFrozenInput": recorded(11) and progressing(11),
        "timelineFullAtFrozenInput": recorded(12) and volume(12),
        "destroyedRuntimeObserved": any(row["observation"]["runtime"]["id"] == 0
            and row["observation"]["runtime"].get("enabled") is False
            and row["observation"]["runtime"].get("observationKind")
                == "owner-control-live-state-not-render-input" for row in during(13) + during(14)),
        "destroyedRenderAndVolumeObserved": render_off(13) and render_off(14),
        "allCaptures2560x1440": len(captures) == len(CAPTURES)
            and capture_phases == set(CAPTURES.items())
            and all(row.get("ok") and row.get("dimensions") == [2560, 1440]
                    and int(row.get("size", 0)) > 4096
                    and re.fullmatch(r"[0-9A-Fa-f]{64}", row.get("sha256", ""))
                    and row.get("stageAtCompletion") == row.get("stage") for row in captures),
        "capturePhasesBracketed": bool(captures) and all(capture_bracket(row) for row in captures),
        "weatherImagesDifferFromBaseline": bool(hashes.get(0))
            and all(hashes.get(stage) and hashes[stage] != hashes[0] for stage in (1, 2, 4)),
        "captureExactWeatherFrameCertified": False,
        "gpuCompletionCertified": False, "pixelOrArtisticAcceptance": False}


def collect_health(war3, session_id: str, instance: Path, pid: int) -> dict:
    """Read only this owned session's bounded event history and incident files.

    No-event is not GPU completion/visual proof; older evicted debug messages
    remain outside this collection's coverage.
    """
    cursor, events, ignored = 0, [], 0
    truncated = False
    for page_index in range(8):
        page = war3.get_runtime_events(since_id=cursor, limit=1000, session_id=session_id)
        if not page.get("ok"):
            return {"covered": False, "reason": "owned runtime events unavailable",
                    "detail": page, "gpuCorrectnessCertified": False}
        rows = page.get("events", [])
        for row in rows:
            if int(row.get("pid", 0)) == pid:
                events.append(row)
            else:
                ignored += 1
        next_cursor = int(page.get("latestId", cursor))
        if len(rows) < 1000:
            break
        if next_cursor <= cursor:
            truncated = True
            break
        cursor = next_cursor
        truncated = page_index == 7
    failure_pattern = re.compile(r"vk_error_device_lost|device[ -]lost|read_invalid|gpu[ -]fault", re.I)
    failure_rows = [row for row in events if failure_pattern.search(str(row.get("msg", "")))]
    incidents = [identity(path) for path in sorted((instance / "WarVK/Log").glob("gpu_incident_*.json"))]
    return {"covered": True, "scope": "this owned pid/session only",
        "events": events, "eventCount": len(events), "otherPidEventsIgnored": ignored,
        "collectionTruncated": truncated,
        "earlierEvictedDebugHistoryCovered": False,
        "gpuFailureKeywordEvents": failure_rows, "gpuIncidentFiles": incidents,
        "noRecordedGpuFailureInCollectedEvidence": not failure_rows and not incidents,
        "gpuCorrectnessCertified": False}


def save_final_run_receipt(out: Path, receipt: dict, protected: dict,
                          dimensions: list[int], winreg, registry_key: str) -> None:
    """Persist cleanup/identity evidence even when the read-only registry query fails.

    This helper never writes or restores registry values. Failure verdicts are
    raised only after the new run.json contains their exact details.
    """
    after = {}
    for path in protected:
        try:
            after[path] = sha256(Path(path))
        except OSError as identity_error:
            after[path] = {"error": str(identity_error)}
    receipt["protectedInputsAfter"] = after
    receipt["protectedInputsUnchanged"] = after == protected
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry_key) as key:
            receipt["videoRegistryAfter"] = [int(winreg.QueryValueEx(key, name)[0])
                for name in ("reswidth", "resheight")]
        receipt["videoRegistryReadAfter"] = {"ok": True}
        receipt["videoRegistryChanged"] = receipt["videoRegistryAfter"] != dimensions
    except Exception as registry_error:
        receipt["videoRegistryReadAfter"] = {"ok": False, "error": str(registry_error),
            "registryWritePerformed": False, "restorationAttempted": False}
        receipt["videoRegistryAfter"] = None
        receipt["videoRegistryChanged"] = None  # Unknown is not 'unchanged'.
    cleanup_failed = "stop" in receipt and not receipt["stop"].get("ok")
    registry_failed = not receipt["videoRegistryReadAfter"]["ok"]
    if (cleanup_failed or not receipt["protectedInputsUnchanged"]
            or registry_failed or receipt["videoRegistryChanged"]):
        receipt["runtimeAcceptance"] = False
    write_new(out / "run.json", json.dumps(receipt, indent=2, ensure_ascii=False, allow_nan=False))
    if cleanup_failed:
        raise RuntimeError("owned session cleanup failed; inspect run.json and restore this session")
    if not receipt["protectedInputsUnchanged"]:
        raise RuntimeError("protected player input changed; inspect exact identity receipt")
    if registry_failed:
        raise RuntimeError("video registry verification unavailable; failure preserved in run.json")
    if receipt["videoRegistryChanged"]:
        raise RuntimeError("video registry drifted during owned run; no automatic overwrite performed")
    if receipt.get("healthEvidence", {}).get("noRecordedGpuFailureInCollectedEvidence") is False:
        raise RuntimeError("owned session recorded GPU-failure evidence; inspect run.json")


def run(args: argparse.Namespace, suite=None) -> int:
    # Lazy import: --prepare/--check-jass do not initialize game control tools.
    import war3_autotest_mcp as war3
    import winreg
    out = artifact_dir(args.name)
    prepared = json.loads((out / "prepare.json").read_text(encoding="utf-8"))
    checked = json.loads((out / "jass-check.json").read_text(encoding="utf-8"))
    if (out / "run.json").exists() or (out / "captures").exists() or (out / "game").exists():
        raise RuntimeError("run artifacts already exist; use a new canary name")
    if checked["exitCode"] != 0 or checked["script"] != prepared["script"]:
        raise RuntimeError("a successful exact-script pjass receipt is required")
    if not args.dll or not re.fullmatch(r"[0-9a-fA-F]{64}", args.expected_dll_sha or ""):
        raise ValueError("--run requires --dll and --expected-dll-sha")
    dll = Path(args.dll).resolve()
    if sha256(dll) != args.expected_dll_sha.upper():
        raise RuntimeError("candidate DLL differs from explicit hash")
    for name in ("sourceMap", "map", "script", "api", "harness"):
        row = prepared[name]
        if identity(Path(row["path"])) != row:
            raise RuntimeError(f"input drift before launch: {name}")
    if prepared.get("suite", "functional") != ("stress" if suite else "functional"):
        raise RuntimeError("prepared suite differs from requested suite")
    if suite and prepared.get("suiteContract") != identity(Path(suite.__file__)):
        raise RuntimeError("stress classifier drifted after preparation")
    read_stages = suite.stage_receipts if suite else stage_receipts
    capture_labels = suite.CAPTURES if suite else CAPTURES
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, war3.WAR3_VIDEO_REG_KEY) as key:
        dimensions = [int(winreg.QueryValueEx(key, name)[0]) for name in ("reswidth", "resheight")]
    if dimensions != [2560, 1440] and not args.resize_owned_client:
        raise RuntimeError(f"requires existing 2560x1440 video config; observed {dimensions}")
    for name in ("War3.exe", "ninja.exe", "cc1plus.exe"):
        if any(str(row.get("exeName", "")).lower() == name.lower()
               for row in war3._snapshot_process_entries()):
            raise RuntimeError(f"process conflict: {name}")
    protected = {str(p): sha256(p) for p in (SOURCE_MAP, PLAYER_DLL, PLAYER_GAME)}
    run_id = "weather-20261010-" + args.name
    session_id = "weather-" + args.name
    game_root = out / "game"
    preflight = war3.preflight_instance_pool(sandbox_root=str(SANDBOX),
        map_path=prepared["map"]["path"], instance_count=1, run_id=run_id,
        session_prefix=session_id, artifact_root=str(game_root))
    if not preflight.get("ok") or preflight.get("warnings"):
        raise RuntimeError(f"instance preflight rejected: {preflight}")
    # Temporary process environment only; restore it immediately after launch.
    old_env = {key: value for key, value in os.environ.items()
               if key.startswith(("DXVK_WAR3_", "DXVK_RUNTIME_"))}
    launch = None
    instance = None
    pid = 0
    receipt = {"preparedMap": prepared["map"], "candidateDll": identity(dll),
        "runnerAtLaunch": identity(Path(__file__)),
        "suite": prepared.get("suite", "functional"),
        "protectedInputs": protected, "globalInputUsed": False,
        "videoRegistryBefore": dimensions,
        "videoRegistryChanged": False, "snapshots": [], "captures": [],
        "stageReceipts": [], "runtimeAcceptance": False}
    try:
        for key in old_env:
            os.environ.pop(key, None)
        try:
            launch = war3.launch_war3_instance(sandbox_root=str(SANDBOX),
                map_path=prepared["map"]["path"], run_id=run_id, session_id=session_id,
                artifact_root=str(game_root), windowed=True, use_isolated_desktop=True,
                deploy_d3d9_before_launch=True, build_d3d9_path=str(dll), profile="full_default",
                env_overrides_json=json.dumps({"DXVK_WAR3_INTERNAL_TEST_API": "1",
                    **({"DXVK_WAR3_VISUAL_API_DIAGNOSTICS": "1"} if suite else {}),
                    "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE": "1",
                    "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE": "1",
                    "DXVK_WAR3_PERF_RECORD_ON_START": "0",
                    "DISABLE_VULKAN_OBS_CAPTURE": "1"}))
        finally:
            os.environ.update(old_env)
        receipt["launch"] = launch
        if not launch.get("ok") or not launch.get("desktop", {}).get("ok"):
            raise RuntimeError("isolated game launch failed")
        if launch.get("dllSha256", "").upper() != args.expected_dll_sha.upper():
            raise RuntimeError("isolated instance DLL hash mismatch")
        pid = int(launch["pid"])
        instance = Path(launch["instanceRoot"])
        ready = war3.wait_for_game_ready(timeout_sec=100, session_id=session_id,
            allow_fallback=False, auto_continue_loading=True,
            require_control_plane_for_continue=True, continue_max_pulses=2)
        receipt["ready"] = ready
        if not ready.get("ok"):
            raise RuntimeError("game readiness gate failed")
        if args.resize_owned_client:
            # Only the HWND of our owned isolated process is changed. The
            # existing helper uses SetWindowPos with NOACTIVATE/NOZORDER; no
            # registry write, foreground switch or simulated global input.
            receipt["ownedClientResize"] = resize_owned_isolated_client(war3, launch)
            if not receipt["ownedClientResize"].get("ok"):
                raise RuntimeError("owned isolated client resize failed")
        if suite:
            # Read-only scene placement evidence. This is not a per-particle
            # terrain query and never changes camera or authored weather.
            receipt["sceneCameraSnapshot"] = war3._invoke_internal_test_request(
                pid, instance, "camera.snapshot", timeout_sec=5)
        try:
            import psutil
            memory_process = psutil.Process(pid)
            receipt["memorySampling"] = {"covered": True, "pid": pid,
                "units": "bytes", "fields": ["privateBytes", "vmsBytes", "rssBytes"],
                "interpretation": "process memory, not total GPU-resident bytes"}
        except Exception as memory_error:
            memory_process = None
            receipt["memorySampling"] = {"covered": False, "reason": str(memory_error),
                "packagesInstalled": False}
        capture_root = out / "captures"
        capture_root.mkdir(exist_ok=False)
        began = time.monotonic()
        captured = set()
        while time.monotonic() - began < (suite.MAX_RUN_SECONDS if suite else 240):
            if not war3._pid_alive(pid):
                raise RuntimeError("owned process exited during weather observation")
            stages = read_stages(instance)
            stage = stages[-1]["stage"] if stages else -1
            status = war3._invoke_internal_test_request(pid, instance, "weather-status", timeout_sec=5)
            if not status.get("ok"):
                raise RuntimeError(f"weather-status unavailable: {status.get('error')}")
            observation = status.get("result", {})
            if not all(name in observation for name in ("runtime", "render", "volumetric", "frozenInput")):
                raise RuntimeError("weather-status schema incomplete")
            elapsed = round(time.monotonic() - began, 3)
            memory_sample = {"covered": False, "reason": "psutil unavailable"}
            if memory_process is not None:
                try:
                    memory_info = memory_process.memory_info()
                    memory_sample = {"covered": hasattr(memory_info, "private"), "pid": pid,
                        "privateBytes": getattr(memory_info, "private", None),
                        "vmsBytes": memory_info.vms, "rssBytes": memory_info.rss}
                    if not memory_sample["covered"]:
                        memory_sample["reason"] = "psutil did not expose Windows private bytes"
                except Exception as memory_error:
                    memory_sample = {"covered": False, "reason": str(memory_error)}
            after_status = read_stages(instance)
            stage_after_status = after_status[-1]["stage"] if after_status else -1
            receipt["snapshots"].append({"elapsedSec": elapsed, "jassStage": stage,
                "stageAtStatusCompletion": stage_after_status,
                "processMemory": memory_sample,
                "observation": observation, "alignment": frame_alignment(observation)})
            stage_map = {row["stage"]: row for row in after_status}
            draw = observation.get("render", {})
            off_capture_ready = (stage in (0, 8, 13)
                and observation.get("runtime", {}).get("enabled") is False
                and draw.get("stage") == "disabled" and draw.get("commandsRecorded") is False
                and observation.get("frozenInput", {}).get("id") == 0
                and observation.get("volumetric", {}).get("compositeSubmitted") is False)
            ready_to_capture = (suite.capture_ready(observation, stage, stage_map) if suite
                else off_capture_ready or active_capture_matches(observation, stage, stage_map))
            if (stage in capture_labels and stage not in captured and stage == stage_after_status
                    and ready_to_capture):
                path = capture_root / (capture_labels[stage] + ".bmp")
                capture = war3._request_internal_frame_capture(pid, path, instance, timeout_sec=12)
                after_capture_status = war3._invoke_internal_test_request(
                    pid, instance, "weather-status", timeout_sec=5)
                if not after_capture_status.get("ok"):
                    raise RuntimeError("weather-status unavailable after screenshot")
                after_capture = read_stages(instance)
                row = {"stage": stage, "label": capture_labels[stage], "elapsedSec": elapsed,
                    "ok": bool(capture.get("ok")), "error": capture.get("error"), "path": str(path),
                    "stageAtRequest": stage_after_status,
                    "stageAtCompletion": after_capture[-1]["stage"] if after_capture else -1,
                    "beforeCaptureObservation": observation,
                    "afterCaptureObservation": after_capture_status.get("result", {}),
                    "weatherFrameBinding": "phase bracket only; capture exposes no weather-frame witness",
                    "captureDetails": capture.get("details", {})}
                if path.is_file():
                    row.update({"sha256": sha256(path), "size": path.stat().st_size,
                                "dimensions": list(image_dimensions(path))})
                receipt["captures"].append(row)
                captured.add(stage)
            receipt["stageReceipts"] = read_stages(instance)
            if receipt["stageReceipts"] and receipt["stageReceipts"][-1]["stage"] == (suite.FINAL_STAGE if suite else 14):
                break
            time.sleep(0.75)
        receipt["classification"] = suite.classify(receipt) if suite else classify(receipt)
        non_acceptance = (suite.NON_ACCEPTANCE if suite else (
            "captureExactWeatherFrameCertified", "gpuCompletionCertified", "pixelOrArtisticAcceptance"))
        required = [key for key in receipt["classification"]
                    if key not in non_acceptance]
        receipt["runtimeAcceptance"] = all(receipt["classification"][key] for key in required)
        print(json.dumps(receipt["classification"], ensure_ascii=False))
        return 0 if receipt["runtimeAcceptance"] else 2
    except Exception as error:
        receipt["error"] = str(error)
        raise
    finally:
        if instance is not None and pid > 0:
            try:
                receipt["healthEvidence"] = collect_health(war3, session_id, instance, pid)
                if receipt["healthEvidence"].get("noRecordedGpuFailureInCollectedEvidence") is False:
                    receipt["runtimeAcceptance"] = False
            except Exception as health_error:
                receipt["healthEvidence"] = {"covered": False, "reason": str(health_error),
                    "gpuCorrectnessCertified": False}
        if launch and (launch.get("ok") or int(launch.get("pid", 0)) > 0):
            try:
                receipt["stop"] = war3.stop_war3(session_id=session_id, graceful_wait_sec=5,
                    force=True, avoid_foreground_switch=True)
            except Exception as cleanup_error:
                receipt["stop"] = {"ok": False, "error": str(cleanup_error)}
        save_final_run_receipt(out, receipt, protected, dimensions, winreg, war3.WAR3_VIDEO_REG_KEY)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare", action="store_true")
    mode.add_argument("--check-jass", action="store_true")
    mode.add_argument("--run", action="store_true")
    parser.add_argument("--name", required=True)
    parser.add_argument("--suite", choices=("functional", "stress"), default="functional")
    parser.add_argument("--expected-map-sha", default=SOURCE_MAP_SHA)
    parser.add_argument("--dll")
    parser.add_argument("--expected-dll-sha")
    parser.add_argument("--resize-owned-client", action="store_true",
        help="Resize only this owned isolated HWND to 2560x1440 without changing video registry")
    args = parser.parse_args()
    if args.suite == "stress":
        import weather_stress_contract_20261010 as suite
    else:
        suite = None
    return prepare(args, suite) if args.prepare else check_jass(args) if args.check_jass else run(args, suite)


if __name__ == "__main__":
    raise SystemExit(main())
