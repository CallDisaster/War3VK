#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Control Shadow/Pose full trace for an already-running Warcraft III process."""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import csv
import io
import json
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional


GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
PIPE_READMODE_MESSAGE = 0x00000002
ERROR_BROKEN_PIPE = 109
ERROR_MORE_DATA = 234
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

_KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
_KERNEL32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
_KERNEL32.WaitNamedPipeW.restype = wintypes.BOOL
_KERNEL32.CreateFileW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.LPVOID,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.HANDLE,
]
_KERNEL32.CreateFileW.restype = wintypes.HANDLE
_KERNEL32.SetNamedPipeHandleState.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
    wintypes.LPVOID,
]
_KERNEL32.SetNamedPipeHandleState.restype = wintypes.BOOL
_KERNEL32.WriteFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPCVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
_KERNEL32.WriteFile.restype = wintypes.BOOL
_KERNEL32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
_KERNEL32.FlushFileBuffers.restype = wintypes.BOOL
_KERNEL32.ReadFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
_KERNEL32.ReadFile.restype = wintypes.BOOL
_KERNEL32.CloseHandle.argtypes = [wintypes.HANDLE]
_KERNEL32.CloseHandle.restype = wintypes.BOOL


def _now_request_id(pid: int) -> str:
    return f"tracectl_{pid}_{int(time.time() * 1000)}"


def _pipe_name(pid: int) -> str:
    return rf"\\.\pipe\War3ControlPlane_{int(pid)}"


def _request(pid: int, command: str, payload: Dict[str, Any], timeout_sec: float) -> Dict[str, Any]:
    pipe_name = _pipe_name(pid)
    timeout_ms = max(200, int(timeout_sec * 1000.0))
    request = {
        "requestId": _now_request_id(pid),
        "command": command,
        "payload": dict(payload),
        "issuedAtMs": int(time.time() * 1000),
        "pid": int(pid),
    }

    start = time.time()
    if not bool(_KERNEL32.WaitNamedPipeW(pipe_name, timeout_ms)):
        return {
            "transportOk": False,
            "ok": False,
            "error": f"WaitNamedPipeW failed: {ctypes.get_last_error()}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - start, 3),
        }

    handle = _KERNEL32.CreateFileW(
        pipe_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        None,
        OPEN_EXISTING,
        0,
        None,
    )
    if handle == INVALID_HANDLE_VALUE:
        return {
            "transportOk": False,
            "ok": False,
            "error": f"CreateFileW failed: {ctypes.get_last_error()}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - start, 3),
        }

    try:
        mode = wintypes.DWORD(PIPE_READMODE_MESSAGE)
        _KERNEL32.SetNamedPipeHandleState(handle, ctypes.byref(mode), None, None)

        raw = json.dumps(request, ensure_ascii=False).encode("utf-8")
        write_buf = ctypes.create_string_buffer(raw)
        written = wintypes.DWORD()
        if not bool(_KERNEL32.WriteFile(handle, write_buf, len(raw), ctypes.byref(written), None)):
            return {
                "transportOk": False,
                "ok": False,
                "error": f"WriteFile failed: {ctypes.get_last_error()}",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - start, 3),
            }
        _KERNEL32.FlushFileBuffers(handle)

        chunks: List[bytes] = []
        deadline = time.time() + max(0.2, timeout_sec)
        while time.time() < deadline:
            buf = ctypes.create_string_buffer(65536)
            read = wintypes.DWORD()
            ok = bool(_KERNEL32.ReadFile(handle, buf, len(buf), ctypes.byref(read), None))
            err = ctypes.get_last_error()
            if ok or err == ERROR_MORE_DATA:
                if read.value:
                    chunks.append(bytes(buf.raw[: read.value]))
                if ok:
                    break
                continue
            if err == ERROR_BROKEN_PIPE:
                break
            return {
                "transportOk": False,
                "ok": False,
                "error": f"ReadFile failed: {err}",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - start, 3),
            }

        if not chunks:
            return {
                "transportOk": False,
                "ok": False,
                "error": "empty response",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - start, 3),
            }

        text = b"".join(chunks).decode("utf-8", errors="replace")
        try:
            response = json.loads(text)
        except Exception as exc:
            return {
                "transportOk": True,
                "ok": False,
                "error": f"response json parse failed: {exc}",
                "pipeName": pipe_name,
                "request": request,
                "responseText": text,
                "elapsedSec": round(time.time() - start, 3),
            }

        return {
            "transportOk": True,
            "ok": bool(response.get("ok")),
            "error": str(response.get("error", "") or ""),
            "pipeName": pipe_name,
            "request": request,
            "response": response,
            "result": response.get("result", {}),
            "elapsedSec": round(time.time() - start, 3),
        }
    finally:
        _KERNEL32.CloseHandle(handle)


def _discover_war3_pids() -> List[int]:
    names = {"war3.exe", "warcraft iii.exe", "frozen throne.exe"}
    try:
        proc = subprocess.run(
            ["tasklist", "/fo", "csv", "/nh"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="mbcs",
            errors="replace",
        )
    except Exception:
        return []
    if proc.returncode != 0:
        return []
    result: List[int] = []
    for row in csv.reader(io.StringIO(proc.stdout)):
        if len(row) < 2:
            continue
        if row[0].strip().lower() in names:
            try:
                result.append(int(row[1]))
            except ValueError:
                pass
    return result


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Start/stop/query War3 Shadow/Pose full trace through the control-plane named pipe."
    )
    parser.add_argument("action", choices=["start", "stop", "status"])
    parser.add_argument("--pid", type=int, default=0, help="Warcraft III process id. Auto-detected when omitted.")
    parser.add_argument("--max-seconds", type=int, default=15, help="Trace duration for start.")
    parser.add_argument("--include-matrix-bytes", action="store_true", default=True)
    parser.add_argument("--no-matrix-bytes", dest="include_matrix_bytes", action="store_false")
    parser.add_argument("--max-pose-records", type=int, default=0, help="0 means unlimited.")
    parser.add_argument("--max-shadow-object-records", type=int, default=0, help="0 means unlimited.")
    parser.add_argument("--max-current-draw-records", type=int, default=0, help="0 means unlimited.")
    parser.add_argument("--timeout-sec", type=float, default=6.0)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    pid = int(args.pid or 0)
    if pid <= 0:
        pids = _discover_war3_pids()
        if len(pids) == 1:
            pid = pids[0]
        elif not pids:
            print(json.dumps({"ok": False, "error": "No Warcraft III process found; pass --pid."}, ensure_ascii=False, indent=2))
            return 2
        else:
            print(json.dumps({"ok": False, "error": "Multiple Warcraft III processes found; pass --pid.", "pids": pids}, ensure_ascii=False, indent=2))
            return 2

    if args.action == "start":
        command = "start_shadow_pose_full_trace"
        payload = {
            "maxSeconds": max(1, int(args.max_seconds)),
            "includeMatrixBytes": bool(args.include_matrix_bytes),
            "maxPoseRecords": max(0, int(args.max_pose_records)),
            "maxShadowObjectRecords": max(0, int(args.max_shadow_object_records)),
            "maxCurrentDrawRecords": max(0, int(args.max_current_draw_records)),
        }
    elif args.action == "stop":
        command = "stop_shadow_pose_full_trace"
        payload = {}
    else:
        command = "get_shadow_pose_full_trace_status"
        payload = {}

    response = _request(pid, command, payload, max(0.5, float(args.timeout_sec)))
    print(json.dumps(response, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
