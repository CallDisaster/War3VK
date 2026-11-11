"""Observe a real game's compile-time D3D9 census on an owned non-input Desktop.

This is NOT the x64 renderer, an equivalence/performance gate, or a release test.
Only a fresh sandbox instance is writable. No global input, registry writes,
foreground changes, renderer build, or player deployment. Default observe-only
mode sends no window messages. Explicit --normal-exit-test adds only stock
EndGame and exact owned non-input-Desktop WM_CLOSE after ready/capture proof.

The existing shutdown_session command only acknowledges soft shutdown; it does
not exit the game. Therefore this runner deliberately has no shutdown command.
An observe-only deadline normally requires the existing launcher's Job cleanup:
the observations are retained, but timeout/forced cleanup NEVER passes the run.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes as wt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import threading
import time
import uuid

from autotest_sessions import (DEFAULT_SANDBOX_ROOT, _iter_runtime_files,
    build_instance_layout, deploy_content_addressed_map, materialize_instance_root,
    normalize_identifier, preflight_instance_pool)
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
import analyze_d3d9_migration_census as census_reader
import analyze_mdl_link_trace as mdl_reader
import analyze_startup_witness as startup_reader


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = ROOT / "AutoTest/artifacts"
PLAYER_DLL = Path("E:/Work/Warcraft III/d3d9.dll")
CENSUS_MARKER = "WARVK_D3D9_MIGRATION_CENSUS "
CENSUS_SCHEMA = census_reader.SCHEMA_V2
CENSUS_SCHEMAS = census_reader.SUPPORTED_SCHEMAS
COMMANDS = frozenset(("ping", "get_runtime_status", "wait_until", "capture_final_frame"))
MAX_RESPONSE = 2 * 1024 * 1024
MAX_LOG_LINE = 512 * 1024
MAX_MEMORY_SAMPLES = 64
MEMORY_INTERVAL_SECONDS = 2.0
INSTANCE_ARTIFACT_DIRECTORIES = (
    "WarVK/Log", "WarVK/Temp", "WarVK/Crash", "Logger", "Logs", "Log",
    "Crash", "crashes", "Errors",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def exit_request_allowed(enabled, command, payload):
    # Never widen the general observer command set or allow arbitrary internal
    # test commands. Booleans/floats are not valid timeout substitutes.
    return (enabled is True and command == "invoke_test_command" and type(payload) is dict and
        set(payload) == {"command", "payload", "timeoutMs"} and
        payload["command"] == "game.end_for_exit_test" and
        type(payload["payload"]) is dict and not payload["payload"] and
        type(payload["timeoutMs"]) is int and payload["timeoutMs"] == 3000)


class PipeOpenError(RuntimeError):
    def __init__(self, code, pipe_name, expected):
        # Capture GetLastError at CreateFile failure, before formatting/querying
        # anything else. Only absent/busy startup states may be observed again.
        self.diagnostic = dict(operation="CreateFileW(control-pipe)", win32Error=int(code),
            pipeName=pipe_name, expectedPid=expected["pid"],
            expectedCreation100ns=expected["creation100ns"],
            startupRetryAllowed=code in (2, 231))
        super().__init__(f"control pipe open failed: Win32={code}, pipe={pipe_name}, "
                         f"pid={expected['pid']}, creation100ns={expected['creation100ns']}")


def strict_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate JSON field: " + key)
            result[key] = value
        return result
    def invalid(value):
        raise ValueError("non-finite JSON value: " + value)
    return json.loads(text, object_pairs_hook=pairs, parse_constant=invalid)


class RejectedReply(ValueError):
    """Retain bounded original bytes for a protocol failure, never accept them."""
    def __init__(self, error, data):
        super().__init__(str(error))
        self.raw = bytes(data)


def checked_path(path, *, exists=False):
    """Check lexical components before resolving; reject links and junctions."""
    path = Path(os.path.abspath(os.fspath(path)))
    for component in reversed((path, *path.parents)):
        try:
            info = component.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and
                not (getattr(info, "st_file_attributes", 0) & 0x400),
                "reparse/link path refused: " + str(component))
    require(not exists or path.exists(), "missing path: " + str(path))
    return path.resolve(strict=exists)


def within(path, root):
    path, root = checked_path(path), checked_path(root)
    require(path != root and path.is_relative_to(root), "path outside allowed root")
    return path


def identity(path):
    path = checked_path(path, exists=True)
    require(path.is_file(), "identity requires regular file")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest.hexdigest().upper())


def instance_artifacts(instance):
    # The production fatal handler uses WarVK/Crash, not the root Crash path.
    # An empty index only means none in this explicit directory set; it is not
    # evidence that the process did not crash.
    result = []
    for relative in INSTANCE_ARTIFACT_DIRECTORIES:
        directory = checked_path(instance / relative)
        if directory.is_dir():
            result.extend(identity(path) for path in sorted(directory.rglob("*"))
                          if path.is_file())
    return result


def pe_bits(path):
    with Path(path).open("rb") as stream:
        header = stream.read(64)
        require(len(header) == 64 and header[:2] == b"MZ", "not PE")
        stream.seek(struct.unpack_from("<I", header, 60)[0])
        pe = stream.read(26)
    require(len(pe) == 26 and pe[:4] == b"PE\0\0", "invalid PE")
    pair = struct.unpack_from("<H", pe, 4)[0], struct.unpack_from("<H", pe, 24)[0]
    require(pair in ((0x14c, 0x10b), (0x8664, 0x20b)), "unsupported PE")
    return 32 if pair[0] == 0x14c else 64


def bmp_dimensions(path):
    """Validate the actual 24-bit BMP emitted by capture_final_frame."""
    with Path(path).open("rb") as stream:
        header = stream.read(54)
    require(len(header) == 54 and header[:2] == b"BM", "capture is not a BMP")
    size, offset = struct.unpack_from("<I", header, 2)[0], struct.unpack_from("<I", header, 10)[0]
    dib, width, height, planes, bits, compression = struct.unpack_from("<IiiHHI", header, 14)
    require(dib == 40 and offset == 54 and 0 < width <= 16384 and 0 < height <= 16384 and
            planes == 1 and bits == 24 and compression == 0, "unsupported capture BMP layout")
    require(size == 54 + ((width * 3 + 3) & ~3) * height == Path(path).stat().st_size,
            "incomplete capture BMP")
    return [width, height]


def loadfile_argument(instance, deployed, absolute):
    """Select only the unique deployed, hash-verified map; no free-form argv."""
    require(type(absolute) is bool, "loadfile mode must be boolean")
    target = within(deployed["target"], instance)
    require(target.is_file() and identity(target)["sha256"] == deployed["sha256"].upper(),
            "deployed map identity mismatch")
    relative = str(target.relative_to(checked_path(instance))).replace("/", "\\")
    require(relative == deployed["loadfileArg"], "deployed relative map alias mismatch")
    return str(target) if absolute else relative


def safe_environment(parent, instance, log_dir):
    # Deliberately do not inherit DXVK/WARVK experiments, Vulkan layer/ICD
    # injection, proxy tokens, PYTHONPATH, or arbitrary parent environment.
    allowed = {"systemroot", "windir", "comspec", "path", "pathext", "username",
               "userprofile", "appdata", "localappdata", "programdata", "allusersprofile"}
    environment = {key: value for key, value in parent.items() if key.lower() in allowed}
    environment.update(TEMP=str(instance / "WarVK/Temp"), TMP=str(instance / "WarVK/Temp"),
        DXVK_LOG_PATH=str(log_dir), DXVK_LOG_LEVEL="info",
        DXVK_CONFIG_FILE=str(log_dir / "absent.conf"),
        DXVK_WAR3_INTERNAL_TEST_API="1",
        DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE="1",
        DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE="1",
        DXVK_WAR3_FRAME_EVIDENCE="0", DXVK_WAR3_FRAME_EVIDENCE_INPUTS="0",
        DXVK_WAR3_FRAME_EVIDENCE_DRAWS="0", DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT="0",
        DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED="0")
    return environment


def launch_identity(row, executable):
    require(type(row) is dict, "missing owned launch")
    for field in ("pid", "creation100ns"):
        require(type(row.get(field)) is int and row[field] > 0, "invalid " + field)
    require(row["pid"] < 1 << 32 and row["creation100ns"] < 1 << 64, "identity overflow")
    require(row.get("jobAtomic") is True and row.get("handleListApplied") is True,
            "launch lacks atomic ownership")
    require(os.path.normcase(str(checked_path(row.get("actualExecutable", "")))) ==
            os.path.normcase(str(checked_path(executable))), "actual executable mismatch")
    token = row.get("restrictedToken", {})
    require(token.get("childTokenVerifiedBeforeResume") is True and
            token.get("resumedAfterVerification") is True and
            token.get("resumePreviousCount") == 1, "restricted child not verified/resumed")
    require(row.get("desktop") and row["desktop"] != row.get("inputBefore"), "not isolated")
    return {key: row[key] for key in ("pid", "creation100ns", "actualExecutable", "desktop")}


def validate_reply(reply, request, server_pid):
    require(type(reply) is dict and reply.get("requestId") == request["requestId"] and
            reply.get("command") == request["command"], "IPC response identity mismatch")
    require(type(reply.get("ok")) is bool and type(reply.get("result")) is dict, "IPC response shape")
    require(type(server_pid) is int and server_pid == request["pid"], "wrong pipe server PID")
    if request["command"] == "ping" and reply["ok"]:
        require(type(reply["result"].get("pid")) is int and reply["result"]["pid"] == server_pid,
                "ping process mismatch")
    return reply


def parse_census(path):
    """Preserve line provenance; malformed/truncated census never silently passes."""
    rows, errors = [], []
    with Path(path).open("r", encoding="utf-8", errors="strict") as stream:
        number = 0
        while True:
            line = stream.readline(MAX_LOG_LINE + 1)
            if not line:
                break
            number += 1
            if len(line) > MAX_LOG_LINE:
                errors.append(dict(line=number, error="overlong log line"))
                break
            if CENSUS_MARKER.rstrip() not in line:
                continue
            try:
                require(len(rows) < census_reader.MAX_RECORDS, "too many census records")
                # Use the production strict reader, not a schema-only shortcut.
                # Keep original file/line identities even when a later row fails.
                data = census_reader.parse_log(line)[0]["snapshot"]
                rows.append(dict(path=str(path), line=number, census=data))
            except (ValueError, TypeError) as error:
                errors.append(dict(line=number, error=str(error)))
    if rows:
        try:
            census_reader.validate_sequences([
                dict(line=row["line"], snapshot=row["census"]) for row in rows])
        except (ValueError, TypeError) as error:
            errors.append(dict(line=rows[-1]["line"], error=str(error)))
    return dict(path=str(path), rows=rows, errors=errors)


def memory_recorder_request_allowed(enabled, command, payload):
    if enabled is not True or command != 'frame_evidence' or type(payload) is not dict:
        return False
    if payload == dict(action='arm', capacity=262144) and type(payload['capacity']) is int:
        return True
    if set(payload) != {'action', 'session'} or payload['action'] not in ('freeze', 'discard'):
        return False
    value = payload['session']
    return type(value) is str and value.isascii() and value.isdigit() and 0 < int(value) < 1 << 64


def classify(receipt):
    launch = receipt.get("gameLaunch", {})
    observer = receipt.get("observer", {})
    normal = bool(launch.get("ok") is True and type(launch.get("exitCode")) is int and launch["exitCode"] == 0 and
                  launch.get("timedOut") is False and launch.get("forcedTermination") is False and
                  launch.get("processSettled") is True and launch.get("jobEmpty") is True)
    census = receipt.get("census", [])
    observed = bool(observer.get("ready") is True and observer.get("modulesVerified") is True and
                    observer.get("screenshotVerified") is True and any(x["rows"] for x in census) and
                    not any(x["errors"] for x in census))
    receipt.update(observationAvailable=observed, normalExitProven=normal,
        cleanupPassed=bool(normal and receipt.get("desktopSafety", {}).get("ok") is True and
            receipt.get("observerJoined") is True and not receipt.get("cleanupErrors") and
            not observer.get("cleanupErrors") and receipt.get("protectedUnchanged") is True),
        ok=False, fullGameCaptureProven=False, x64GameRenderingProven=False,
        gameMemorySavingsProven=False, performanceAcceptance=False, visualAcceptance=False)
    receipt["verdict"] = ("OBSERVATIONS_RETAINED_CLEANUP_FAILED" if observed and not normal else
        "OBSERVATIONS_RETAINED_NOT_A_RENDERING_ACCEPTANCE" if observed else "NOT_COVERED")
    uncovered = ["full-game-resource-capture", "x64-game-rendering", "performance", "visual-equivalence"]
    if not normal:
        uncovered.append("normal-game-exit")
    if not observed:
        uncovered.append("usable-game-census")
    if receipt.get("cleanupErrors") or observer.get("cleanupErrors") or receipt.get("observerJoined") is not True:
        uncovered.append("runner-cleanup")
    if receipt.get("protectedUnchanged") is not True:
        uncovered.append("source-and-player-preservation")
    if receipt.get("normalExitTestRequested") is True:
        exit_test = observer.get("normalExitTest", {})
        memory_observed = bool(receipt.get("memoryReviewRequested") is True and
            observer.get("ready") is True and observer.get("modulesVerified") is True and
            observer.get("screenshotVerified") is True and
            observer.get("memoryReview", {}).get("complete") is True)
        # A memory-only build may compile census out. Preserve census coverage
        # as false; accept only this separately named metadata+exit gate.
        passed = bool((memory_observed if receipt.get("memoryReviewRequested") else observed) and receipt["cleanupPassed"] and
            not receipt.get("error") and not observer.get("error") and
            exit_test.get("ok") is True and exit_test.get("normalExitProven") is True and
            exit_test.get("endGameAccepted") is True and exit_test.get("mapTeardownObserved") is True)
        if receipt.get('cpuRecorderReviewRequested'):
            passed = passed and observer.get('cpuRecorderDisposed') is True
        receipt["ok"] = passed
        receipt["verdict"] = ("LOCAL_GAME_MEMORY_METADATA_AND_EXIT_PASSED" if memory_observed else
            "LOCAL_GAME_START_CAPTURE_EXIT_PASSED") if passed else "LOCAL_GAME_EXIT_TEST_FAILED"
        if not passed:
            uncovered.append("local-game-start-capture-exit")
    receipt["notCovered"] = uncovered
    return receipt


class _Overlapped(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]


class _MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + [
        (field, ctypes.c_size_t) for field in ("PeakWorkingSetSize", "WorkingSetSize",
        "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
        "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]


class NativeObserver:
    """Read-only process authority plus bounded IPC; never launches/stops a PID."""
    def __init__(self):
        require(os.name == "nt", "native survey is Windows-only")
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.p = ctypes.WinDLL("psapi", use_last_error=True)
        self.process = 0
        self.expected = None
        self.leases = []
        self.pending = []
        self.normal_exit_test = False
        signatures = (
            (self.k, "OpenProcess", wt.HANDLE, (wt.DWORD, wt.BOOL, wt.DWORD)),
            (self.k, "CloseHandle", wt.BOOL, (wt.HANDLE,)),
            (self.k, "GetProcessTimes", wt.BOOL, (wt.HANDLE,) + (ctypes.POINTER(wt.FILETIME),) * 4),
            (self.k, "QueryFullProcessImageNameW", wt.BOOL, (wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD))),
            (self.k, "WaitForSingleObject", wt.DWORD, (wt.HANDLE, wt.DWORD)),
            (self.k, "CreateFileW", wt.HANDLE, (wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE)),
            (self.k, "CreateEventW", wt.HANDLE, (wt.LPVOID, wt.BOOL, wt.BOOL, wt.LPCWSTR)),
            (self.k, "GetNamedPipeServerProcessId", wt.BOOL, (wt.HANDLE, ctypes.POINTER(wt.DWORD))),
            (self.k, "SetNamedPipeHandleState", wt.BOOL, (wt.HANDLE, ctypes.POINTER(wt.DWORD), wt.LPVOID, wt.LPVOID)),
            (self.k, "ReadFile", wt.BOOL, (wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(_Overlapped))),
            (self.k, "WriteFile", wt.BOOL, (wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(_Overlapped))),
            (self.k, "GetOverlappedResult", wt.BOOL, (wt.HANDLE, ctypes.POINTER(_Overlapped), ctypes.POINTER(wt.DWORD), wt.BOOL)),
            (self.k, "CancelIoEx", wt.BOOL, (wt.HANDLE, ctypes.POINTER(_Overlapped))),
            (self.p, "EnumProcessModulesEx", wt.BOOL, (wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD)),
            (self.p, "GetModuleFileNameExW", wt.DWORD, (wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD)),
            (self.p, "GetProcessMemoryInfo", wt.BOOL, (wt.HANDLE, ctypes.POINTER(_MemoryCounters), wt.DWORD)),
        )
        for lib, name, result, arguments in signatures:
            function = getattr(lib, name)
            function.restype, function.argtypes = result, arguments

    def check(self, value, operation):
        require(value, operation + ": Win32=" + str(ctypes.get_last_error()))
        return value

    def lease(self, path):
        path = checked_path(path, exists=True)
        # Read sharing only denies write/delete through any hardlink alias.
        handle = self.k.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x80, None)
        require(handle not in (0, None, ctypes.c_void_p(-1).value), "deny-write lease failed: " + str(path))
        self.leases.append(handle)

    def bind(self, expected):
        require(not self.process, "process authority already bound")
        self.expected = expected
        self.process = self.check(self.k.OpenProcess(0x100000 | 0x400 | 0x10, False, expected["pid"]), "OpenProcess(read-only)")
        self.verify_process()

    def verify_process(self):
        self.check(self.process, "unbound process")
        require(self.k.WaitForSingleObject(self.process, 0) == 258, "owned game already exited")
        stamps = [wt.FILETIME() for _ in range(4)]
        self.check(self.k.GetProcessTimes(self.process, *(ctypes.byref(x) for x in stamps)), "GetProcessTimes")
        creation = stamps[0].dwLowDateTime | stamps[0].dwHighDateTime << 32
        name = ctypes.create_unicode_buffer(32768)
        length = wt.DWORD(len(name))
        self.check(self.k.QueryFullProcessImageNameW(self.process, 0, name, ctypes.byref(length)), "process image")
        require(creation == self.expected["creation100ns"] and
                os.path.normcase(name.value) == os.path.normcase(self.expected["actualExecutable"]),
                "retained process identity mismatch")

    def modules(self):
        self.verify_process()
        modules = (wt.HMODULE * 2048)()
        needed = wt.DWORD()
        self.check(self.p.EnumProcessModulesEx(self.process, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3), "module snapshot")
        require(0 < needed.value <= ctypes.sizeof(modules), "module snapshot empty/truncated")
        rows = []
        for module in modules[:needed.value // ctypes.sizeof(wt.HMODULE)]:
            path = ctypes.create_unicode_buffer(32768)
            self.check(self.p.GetModuleFileNameExW(self.process, module, path, len(path)), "module path")
            rows.append(dict(name=Path(path.value).name, path=path.value))
        self.verify_process()
        return rows

    def memory(self):
        self.verify_process()
        counters = _MemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        self.check(self.p.GetProcessMemoryInfo(self.process, ctypes.byref(counters), counters.cb), "process memory")
        return dict(privateBytes=counters.PrivateUsage, workingSetBytes=counters.WorkingSetSize,
                    peakWorkingSetBytes=counters.PeakWorkingSetSize)

    def _transfer(self, handle, buffer, size, write, deadline):
        event = self.check(self.k.CreateEventW(None, True, False, None), "IPC event")
        overlap, transferred = _Overlapped(), wt.DWORD()
        overlap.hEvent = event
        retained = False
        try:
            ok = (self.k.WriteFile if write else self.k.ReadFile)(handle, buffer, size, ctypes.byref(transferred), ctypes.byref(overlap))
            error = 0 if ok else ctypes.get_last_error()
            if not ok and error == 997:
                left = max(0, math.ceil((deadline - time.monotonic()) * 1000))
                result = self.k.WaitForSingleObject(event, left)
                if result != 0:
                    self.k.CancelIoEx(handle, ctypes.byref(overlap))
                    if self.k.WaitForSingleObject(event, 1000) != 0:
                        # Never free an OVERLAPPED/buffer still owned by the kernel.
                        self.pending.append((handle, event, overlap, buffer))
                        retained = True
                        raise RuntimeError("IPC cancellation did not settle; authority retained")
                    self.k.GetOverlappedResult(handle, ctypes.byref(overlap), ctypes.byref(transferred), False)
                    raise TimeoutError("IPC deadline; cancellation drained")
                ok = self.k.GetOverlappedResult(handle, ctypes.byref(overlap), ctypes.byref(transferred), False)
                error = 0 if ok else ctypes.get_last_error()
            elif not ok and not write and error == 234:
                # Immediate ERROR_MORE_DATA is a completed partial message.
                # ReadFile may leave lpNumberOfBytesRead at zero on that warning;
                # retrieve the completed OVERLAPPED count just as for pending IO.
                ok = self.k.GetOverlappedResult(handle, ctypes.byref(overlap), ctypes.byref(transferred), False)
                error = 0 if ok else ctypes.get_last_error()
            require(ok or (not write and error == 234), "IPC IO failure: " + str(error))
            require(transferred.value <= size, "IPC completion count exceeds buffer")
            require(error != 234 or transferred.value > 0, "IPC continuation made no progress")
            return transferred.value, error == 234
        finally:
            if not retained:
                self.check(self.k.CloseHandle(event), "close IPC event")

    def request(self, command, payload, timeout=3.0):
        require(command in COMMANDS or exit_request_allowed(
            getattr(self, "normal_exit_test", False), command, payload) or memory_recorder_request_allowed(
            getattr(self, "cpu_recorder_review", False), command, payload), "command outside survey allowlist")
        require(type(timeout) in (int, float) and 0 < timeout <= 5, "IPC timeout bound")
        self.verify_process()
        request = dict(requestId="survey-" + uuid.uuid4().hex, command=command,
                       payload=payload, pid=self.expected["pid"])
        pipe_name = rf"\\.\pipe\War3ControlPlane_{self.expected['pid']}"
        handle = self.k.CreateFileW(pipe_name, 0xC0000000, 0, None, 3, 0x40000000, None)
        if handle in (None, 0, ctypes.c_void_p(-1).value):
            code = ctypes.get_last_error()
            raise PipeOpenError(code, pipe_name, self.expected)
        try:
            server = wt.DWORD()
            self.check(self.k.GetNamedPipeServerProcessId(handle, ctypes.byref(server)), "pipe server identity")
            require(server.value == self.expected["pid"], "wrong pipe server PID")
            mode = wt.DWORD(2)
            self.check(self.k.SetNamedPipeHandleState(handle, ctypes.byref(mode), None, None), "pipe message mode")
            deadline = time.monotonic() + timeout
            raw = json.dumps(request, separators=(",", ":")).encode("utf-8")
            buffer = ctypes.create_string_buffer(raw)
            written, _ = self._transfer(handle, buffer, len(raw), True, deadline)
            require(written == len(raw), "partial IPC write")
            data = bytearray()
            while True:
                require(time.monotonic() < deadline, "IPC response deadline")
                buffer = ctypes.create_string_buffer(65536)
                count, more = self._transfer(handle, buffer, len(buffer), False, deadline)
                data.extend(buffer.raw[:count])
                require(len(data) <= MAX_RESPONSE, "IPC response too large")
                if not more:
                    break
            self.verify_process()
            try:
                reply = validate_reply(strict_json(data.decode("utf-8")), request, server.value)
            except (ValueError, UnicodeError) as error:
                raise RejectedReply(error, data) from error
            return dict(request=request, pipeServerPid=server.value, response=reply)
        finally:
            if not any(row[0] == handle for row in self.pending):
                self.check(self.k.CloseHandle(handle), "close IPC pipe")

    def close(self):
        failures = []
        still_pending = []
        for handle, event, overlap, buffer in self.pending:
            self.k.CancelIoEx(handle, ctypes.byref(overlap))
            if self.k.WaitForSingleObject(event, 1000) != 0:
                still_pending.append((handle, event, overlap, buffer))
                failures.append("IPC cancellation still pending; handles retained")
            else:
                result = wt.DWORD()
                self.k.GetOverlappedResult(handle, ctypes.byref(overlap), ctypes.byref(result), False)
                for owned in (event, handle):
                    if not self.k.CloseHandle(owned):
                        failures.append("IPC CloseHandle failed")
        self.pending = still_pending
        for handle in [self.process, *self.leases]:
            if handle and not self.k.CloseHandle(handle):
                failures.append("read-only authority CloseHandle failed")
        self.process, self.leases = 0, []
        return failures


def verify_modules(rows, expected):
    observed = {}
    for name, path in expected.items():
        matches = [row for row in rows if row["name"].casefold() == name.casefold()]
        require(len(matches) == 1, "missing/duplicate game module: " + name)
        require(checked_path(matches[0]["path"]) == checked_path(path), "module escaped instance: " + name)
        observed[name] = identity(path)
    return observed


def observe(desktop, launch_index, executable, instance, output, stop, receipt, normal_exit_test=False,
            startup_window_diagnostic=False, memory_review=False, steady_seconds=12, cpu_recorder_review=False):
    native = NativeObserver()
    native.normal_exit_test = normal_exit_test
    native.cpu_recorder_review = cpu_recorder_review
    receipt.update(ready=False, modulesVerified=False, screenshotVerified=False,
                   cleanupErrors=[], samples=0, errors=[], ipcDisabled=False,
                   memorySavingsProven=False, memorySampleLimit=MAX_MEMORY_SAMPLES)
    try:
        expected = None
        while not stop.wait(0.05):
            launches = desktop.receipt["launches"]
            if len(launches) <= launch_index:
                continue
            try:
                expected = launch_identity(launches[launch_index], executable)
                break
            except ValueError:
                continue
        require(expected is not None, "game never published verified launch identity")
        receipt["processIdentity"] = expected
        native.bind(expected)
        last_memory_at = None
        with (output / "ipc.jsonl").open("x", encoding="utf-8") as raw:
            def request(command, payload):
                start = time.monotonic()
                try:
                    row = native.request(command, payload)
                    row["seconds"] = time.monotonic() - start
                    raw.write(json.dumps(row, ensure_ascii=False) + "\n")
                    raw.flush()
                    return row["response"]
                except Exception as error:
                    row = dict(command=command, error=f"{type(error).__name__}: {error}",
                               seconds=time.monotonic() - start)
                    if isinstance(error, RejectedReply) and "rejectedReply" not in receipt:
                        rejected = output / "rejected-control-response.bin"
                        with rejected.open("xb") as stream:
                            stream.write(error.raw)
                        receipt["rejectedReply"] = identity(rejected)
                        row["rejectedReply"] = receipt["rejectedReply"]
                    if isinstance(error, PipeOpenError):
                        row.update(error.diagnostic)
                        receipt["lastPipeOpenFailure"] = row
                    raw.write(json.dumps(row, ensure_ascii=False) + "\n")
                    raw.flush()
                    raise
            while not stop.is_set():
                # These independent read-only observations must not depend on a
                # game Bootstrap/control pipe existing. This is current process
                # usage only, never an x86-vs-x64 saving or a performance result.
                now = time.monotonic()
                if receipt["samples"] < MAX_MEMORY_SAMPLES and (last_memory_at is None or
                        now - last_memory_at >= MEMORY_INTERVAL_SECONDS):
                    last_memory_at = now
                    try:
                        sample = dict(monotonic=now, phase="initial" if not receipt["samples"] else "periodic",
                            source="GetProcessMemoryInfo/retained-read-only-process", **native.memory())
                        raw.write(json.dumps(dict(memory=sample)) + "\n")
                        raw.flush()
                        receipt.setdefault("firstMemorySample", sample)
                        receipt["lastMemorySample"] = sample
                        receipt["samples"] += 1
                    except Exception as error:
                        raw.write(json.dumps(dict(memoryError=f"{type(error).__name__}: {error}")) + "\n")
                        raw.flush()
                if (startup_window_diagnostic and receipt["samples"] >= 6 and not receipt["ready"]
                        and "startupWindowDiagnostic" not in receipt):
                    from native_game_startup_snapshot import capture_startup_snapshot
                    receipt["startupWindowDiagnostic"] = capture_startup_snapshot(native,
                        launch=desktop.receipt["launches"][launch_index], instance=instance,
                        desktop=desktop, output=output / "startup-window.bmp")
                    raw.write(json.dumps(dict(startupWindowDiagnostic=receipt["startupWindowDiagnostic"])) + "\n")
                    raw.flush()
                if not receipt["modulesVerified"]:
                    try:
                        rows = native.modules()
                        receipt["moduleSnapshot"] = rows
                        receipt["verifiedModules"] = verify_modules(rows, {
                            "war3.exe": executable, "Game.dll": instance / "Game.dll",
                            "Storm.dll": instance / "Storm.dll",
                            "d3d9.dll": instance / "d3d9.dll"})
                        receipt["modulesVerified"] = True
                        raw.write(json.dumps(dict(moduleIdentity=receipt["verifiedModules"])) + "\n")
                        raw.flush()
                    except Exception as error:
                        receipt["lastModuleError"] = f"{type(error).__name__}: {error}"
                        raw.write(json.dumps(dict(moduleError=receipt["lastModuleError"])) + "\n")
                        raw.flush()
                if not receipt["modulesVerified"]:
                    stop.wait(1.0)
                    continue
                if receipt["samples"] >= 11 and "lowVa" not in receipt:
                    from native_game_va_sample import sample as sample_low_va
                    receipt["lowVa"] = sample_low_va(native)
                    raw.write(json.dumps(dict(lowVa=receipt["lowVa"])) + "\n")
                    raw.flush()
                if receipt["ipcDisabled"]:
                    stop.wait(1.0)
                    continue
                try:
                    request("ping", {})
                    request("get_runtime_status", {})
                    if not receipt["ready"]:
                        reply = request("wait_until", dict(timeoutSec=1.0, pollIntervalMs=50,
                            requireFrameAdvance=True, minFrameAdvance=2,
                            requestSemanticFrameBuild=False, refreshSemanticFrameIfStale=False))
                        receipt["ready"] = reply["ok"] is True and reply["result"].get("ready") is True
                    if receipt["ready"] and not receipt.get("screenshotAttempted"):
                        if memory_review:
                            if cpu_recorder_review and 'cpuRecorderSession' not in receipt:
                                armed = request('frame_evidence', dict(action='arm', capacity=262144))
                                require(armed['ok'] and armed['result'].get('ok') is True,
                                        'CPU recorder arm rejected')
                                session = armed['result']['session']
                                require(memory_recorder_request_allowed(True, 'frame_evidence',
                                    dict(action='freeze', session=session)), 'invalid recorder session')
                                receipt['cpuRecorderSession'] = session
                            receipt.setdefault("readyObservedAt", time.monotonic())
                            if time.monotonic() - receipt["readyObservedAt"] < steady_seconds:
                                stop.wait(0.5)
                                continue
                            fresh = request("wait_until", dict(timeoutSec=1.0, pollIntervalMs=50,
                                requireFrameAdvance=True, minFrameAdvance=2,
                                requestSemanticFrameBuild=False, refreshSemanticFrameIfStale=False))
                            require(fresh["ok"] and fresh["result"].get("ready") is True,
                                    "memory review lost map-ready state")
                            from native_game_memory_review import sample as sample_full_va
                            memory_before = native.memory()
                            full = sample_full_va(native)
                            memory_after = native.memory()
                            path = output / "full-va-map.json"
                            with path.open("x", encoding="utf-8") as stream:
                                json.dump(full, stream, ensure_ascii=False)
                            receipt["memoryReview"] = dict(artifact=identity(path),
                                complete=full["complete"], summary=full["summary"],
                                memoryBefore=memory_before, memoryAfter=memory_after,
                                steadySeconds=steady_seconds, noCameraInputSent=True,
                                cameraEquivalenceProven=False, allocatorOwnershipProven=False,
                                migrationSavingsProven=False)
                            require(full["complete"], "full VA review incomplete: " + full["stopReason"])
                        receipt["screenshotAttempted"] = True
                        picture = within(output / "game-frame.bmp", output)
                        require(not picture.exists(), "screenshot target already exists")
                        reply = request("capture_final_frame", dict(outputPath=str(picture), timeoutMs=2500))
                        result = reply["result"]
                        receipt["capture"] = result
                        if reply["ok"]:
                            require(checked_path(result["outputPath"]) == picture, "capture output mismatch")
                            require(type(result.get("width")) is int and result["width"] > 0 and
                                    type(result.get("height")) is int and result["height"] > 0, "capture dimensions")
                            receipt["screenshot"] = identity(picture)
                            receipt["actualResolution"] = [result["width"], result["height"]]
                            require(bmp_dimensions(picture) == receipt["actualResolution"],
                                    "capture file dimensions differ from IPC")
                            receipt["screenshotVerified"] = True
                    if normal_exit_test and receipt["ready"] and receipt["screenshotVerified"]:
                        if cpu_recorder_review and not receipt.get('cpuRecorderDisposed'):
                            for action in ('freeze', 'discard'):
                                closed = request('frame_evidence', dict(action=action,
                                    session=receipt['cpuRecorderSession']))
                                require(closed['ok'] and closed['result'].get('ok') is True,
                                        'CPU recorder ' + action + ' rejected')
                            receipt['cpuRecorderDisposed'] = True
                        if "lowVa" not in receipt:
                            from native_game_va_sample import sample as sample_low_va
                            receipt["lowVa"] = sample_low_va(native)
                            raw.write(json.dumps(dict(lowVa=receipt["lowVa"])) + "\n")
                            raw.flush()
                        from native_game_exit_acceptance import accept_normal_exit
                        receipt["normalExitTest"] = accept_normal_exit(native,
                            launch=desktop.receipt["launches"][launch_index], observation=receipt,
                            instance=instance, desktop=desktop)
                        return
                except Exception as error:
                    if len(receipt["errors"]) < 16:
                        receipt["errors"].append(f"{type(error).__name__}: {error}")
                    if native.pending:
                        break
                    if not isinstance(error, PipeOpenError) or not error.diagnostic["startupRetryAllowed"]:
                        receipt["ipcDisabled"] = True
                        receipt["ipcDisabledReason"] = f"{type(error).__name__}: {error}"
                stop.wait(1.0)
    except Exception as error:
        receipt["error"] = f"{type(error).__name__}: {error}"
    finally:
        receipt["cleanupErrors"].extend(native.close())
        # On pathological kernel noncompletion retain the backing buffers and
        # exact authority through process exit; never garbage-collect pending IO.
        if native.pending:
            _RETAINED_OBSERVERS.append(native)
        receipt["finished"] = True


_RETAINED_OBSERVERS = []


def prepare(args, receipt):
    output = within(args.output, ARTIFACT_ROOT)
    require(not output.exists(), "output must be fresh")
    require(args.instance_id == normalize_identifier(args.instance_id, "survey"), "explicit instance id required")
    require(1 <= len(args.instance_id) <= 60, "instance id length")
    require(math.isfinite(args.timeout_seconds) and 10 <= args.timeout_seconds <= 120,
            "game timeout must be 10..120 seconds")
    require((args.observe_only is True) != (getattr(args, "normal_exit_test", False) is True),
            "choose exactly one explicit observe-only or normal-exit-test mode")
    sandbox = checked_path(DEFAULT_SANDBOX_ROOT, exists=True)
    source_map, dll = checked_path(args.map, exists=True), checked_path(args.dll, exists=True)
    probes = {bits: checked_path(getattr(args, "probe" + str(bits)), exists=True) for bits in (32, 64)}
    require(pe_bits(dll) == 32, "candidate must be complete x86 DLL")
    for bits, path in probes.items():
        require(pe_bits(path) == bits, "probe architecture mismatch")
    require(source_map.suffix.lower() in (".w3m", ".w3x"), "map suffix")
    for source, _ in _iter_runtime_files(sandbox):
        checked_path(source, exists=True)
    plan = preflight_instance_pool(sandbox, output, source_map, 1, args.instance_id, "survey")
    require(plan["ok"] and not plan["errors"], "instance preflight failed")
    layout = build_instance_layout(sandbox, output, args.instance_id, "survey-01")
    checked_path(layout.instance_root)
    require(not layout.instance_root.exists() and not layout.instance_root.parent.exists(), "instance must be fresh")
    protected = [dll, source_map, sandbox / "war3.exe", sandbox / "Game.dll",
                 sandbox / "Storm.dll", *probes.values()]
    require(PLAYER_DLL.is_file(), "player DLL preservation witness missing")
    protected.append(PLAYER_DLL)
    protected += [source for source, _ in _iter_runtime_files(sandbox) if source.suffix.lower() == ".mpq"]
    receipt.update(preflight=plan, layout=layout.to_dict(), protectedBefore=[identity(p) for p in protected],
                   censusMarker=CENSUS_MARKER, censusSchema=CENSUS_SCHEMA,
                   acceptedCensusSchemas=list(CENSUS_SCHEMAS))
    return output, layout, source_map, dll, probes, protected


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--instance-id", required=True)
    parser.add_argument("--probe32", required=True, type=Path)
    parser.add_argument("--probe64", required=True, type=Path)
    parser.add_argument("--map", type=Path, default=DEFAULT_SANDBOX_ROOT / "Maps/(4)LostTemple.w3m")
    parser.add_argument("--timeout-seconds", type=float, default=120)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--observe-only", action="store_true")
    mode.add_argument("--normal-exit-test", action="store_true",
                      help="owned sandbox only: ready/capture, stock EndGame, owned WM_CLOSE, natural exit")
    parser.add_argument("--mdl-allocation-trace", action="store_true",
                        help="enable exact-version bounded MDLDATA probe in a developer DLL")
    parser.add_argument("--startup-witness", action="store_true")
    parser.add_argument("--startup-window-diagnostic", action="store_true",
                        help="one bounded owned pre-ready PrintWindow; not game/visual acceptance")
    parser.add_argument("--mdl-low-address-off", action="store_true",
                        help="controlled comparison only: disable the MDLDATA native low-address fix")
    parser.add_argument("--absolute-loadfile", action="store_true",
                        help="diagnostic only: use the same deployed map's verified absolute path")
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--memory-review", action="store_true",
                        help="one bounded full VA metadata map after stable map-ready observation")
    parser.add_argument("--steady-seconds", type=int, choices=range(5, 61), default=12)
    parser.add_argument("--cpu-recorder-review", action="store_true",
                        help="memory review only: arm bounded CPU events, no input/image/draw capture")
    args = parser.parse_args(argv)
    require(not args.cpu_recorder_review or (args.memory_review and args.normal_exit_test),
            'CPU recorder review requires full memory review and normal exit')
    receipt = dict(schema="warvk-native-d3d9-survey-run/v1", scope="REAL_GAME_LOCAL_RENDERER_API_CENSUS",
        memoryReviewRequested=args.memory_review,
        cpuRecorderReviewRequested=args.cpu_recorder_review,
        ok=False, observeOnly=args.observe_only, normalExitTestRequested=args.normal_exit_test,
        gameLaunched=False, globalInputUsed=False,
        registryWrittenByRunner=False, foregroundChanged=False, playerDllChanged=False,
        x64RendererUsed=False, resolutionModified=False, cleanupErrors=[])
    output = None
    instance = None
    native = None
    protected = []
    try:
        output, layout, source_map, dll, probes, protected = prepare(args, receipt)
        if args.preflight_only:
            receipt.update(preflightOnly=True, nativeApiCalled=False)
            print(json.dumps(receipt, ensure_ascii=False, indent=2))
            return 0
        require(os.name == "nt", "native survey requires Windows")
        output.mkdir(parents=True, exist_ok=False)
        native = NativeObserver()
        # Preserve source game/MPQ/map/DLL and player assets throughout deployment
        # and execution. These handles never authorize writes or process control.
        for path in protected:
            native.lease(path)
        require([identity(path) for path in protected] == receipt["protectedBefore"],
                "source drift before deny-write leases")
        receipt["materialization"] = materialize_instance_root(layout, reuse_existing=False)
        require(receipt["materialization"]["ok"], "instance materialization failed")
        instance = layout.instance_root
        for path in instance.rglob("*"):
            checked_path(path, exists=True)
        target_dll = within(instance / "d3d9.dll", instance)
        # An existing COPY belongs to this fresh instance, not the player. Verify
        # no hardlink alias before replacing it with the explicit candidate.
        if target_dll.exists():
            require(target_dll.stat().st_nlink == 1, "instance DLL is a shared hardlink")
        shutil.copy2(dll, target_dll)
        require(identity(target_dll)["sha256"] == identity(dll)["sha256"], "candidate copy mismatch")
        receipt["map"] = deploy_content_addressed_map(instance, source_map)
        require(identity(Path(receipt["map"]["target"]))["sha256"] == identity(source_map)["sha256"], "map copy mismatch")
        receipt["loadfileMode"] = "absolute" if args.absolute_loadfile else "relative"
        receipt["loadfileArgument"] = loadfile_argument(instance, receipt["map"], args.absolute_loadfile)
        local_probes = {}
        for bits, source in probes.items():
            target = output / f"desktop-probe-{bits}.exe"
            shutil.copy2(source, target)
            require(identity(source)["sha256"] == identity(target)["sha256"], "probe copy mismatch")
            local_probes[bits] = target
        for path in [instance / "war3.exe", instance / "Game.dll", instance / "Storm.dll", target_dll,
                     Path(receipt["map"]["target"]), *local_probes.values()]:
            native.lease(path)
        receipt["deployed"] = [identity(path) for path in (instance / "war3.exe", instance / "Game.dll",
                                                         instance / "Storm.dll", target_dll)]
        log_dir = output / "logs"
        log_dir.mkdir()
        environment = safe_environment(os.environ, instance, log_dir)
        if args.cpu_recorder_review:
            environment['DXVK_WAR3_FRAME_EVIDENCE'] = '1'
            environment['DXVK_WAR3_FRAME_EVIDENCE_LOCAL'] = '0'
        if args.normal_exit_test:
            environment['DXVK_WAR3_INTERNAL_EXIT_TEST'] = '1'
            environment['DXVK_WAR3_INTERNAL_TEST_API'] = '1'
        if args.mdl_allocation_trace:
            environment['DXVK_WAR3_MDLDATA_TRACE'] = '1'
            environment['DXVK_WAR3_MDLDATA_TRACE_FILE'] = str(log_dir / 'mdl-link-trace.bin')
        receipt['mdlAllocationTraceRequested'] = args.mdl_allocation_trace
        if args.startup_witness:
            environment['DXVK_WAR3_STARTUP_WITNESS'] = '1'
            environment['DXVK_WAR3_STARTUP_WITNESS_FILE'] = str(log_dir / 'startup-witness.bin')
        if args.mdl_low_address_off:
            environment['DXVK_WAR3_MDLDATA_LOW_ADDRESS'] = '0'
        receipt['startupWitnessRequested'] = args.startup_witness
        receipt['mdlLowAddressDisabled'] = args.mdl_low_address_off
        receipt["childEnvironment"] = environment
        stop = threading.Event()
        with IsolatedRenderDesktop(name_prefix="WarVK_NativeSurvey", restricted_medium=True) as desktop:
            receipt["desktopSafety"] = desktop.receipt
            probe = desktop.run([local_probes[32], "--expect-desktop", desktop.desktop_name,
                "--expect-station", "WinSta0", "--input-desktop", desktop.input_before,
                "--child", local_probes[64], "--child-log", output / "probe-child.log"],
                cwd=output, stdout_path=output / "probe.stdout", stderr_path=output / "probe.stderr",
                timeout_s=30, env=environment)
            receipt["probeLaunch"] = probe
            records = []
            for path in (output / "probe.stdout", output / "probe-child.log"):
                records.extend(strict_json(line) for line in path.read_text("utf-8").splitlines() if line.startswith("{"))
            require(len(records) == 2, "expected x86/x64 CPU probe pair")
            validate_probe_receipt(records[0], desktop=desktop.desktop_name,
                input_desktop=desktop.input_before, station="WinSta0", bits=32, child=records[1], launch=probe)
            receipt["probeReports"] = records
            receipt["observer"] = {}
            observer = threading.Thread(target=observe, args=(desktop, len(desktop.receipt["launches"]),
                instance / "war3.exe", instance, output, stop, receipt["observer"], args.normal_exit_test,
                args.startup_window_diagnostic, args.memory_review, args.steady_seconds,
                args.cpu_recorder_review), daemon=True)
            observer.start()
            try:
                receipt["gameLaunchRequested"] = True
                receipt["gameLaunch"] = desktop.run([instance / "war3.exe", "-window", "-loadfile",
                    receipt["loadfileArgument"]], cwd=instance,
                    stdout_path=output / "game.stdout", stderr_path=output / "game.stderr",
                    timeout_s=args.timeout_seconds, env=environment)
            finally:
                if len(desktop.receipt["launches"]) > 1:
                    receipt["gameLaunch"] = desktop.receipt["launches"][1]
                    receipt["gameLaunched"] = type(receipt["gameLaunch"].get("pid")) is int
                stop.set()
                observer.join(timeout=8)
                receipt["observerJoined"] = not observer.is_alive()
                require(not observer.is_alive(), "observer failed bounded shutdown")
    except Exception as error:
        receipt["error"] = f"{type(error).__name__}: {error}"
    finally:
        if native is not None:
            receipt["cleanupErrors"].extend(native.close())
        if output is not None and output.exists() and not args.preflight_only:
            try:
                receipt["protectedAfter"] = [identity(path) for path in protected]
                receipt["protectedUnchanged"] = receipt.get("protectedBefore") == receipt["protectedAfter"]
                receipt["playerDllChanged"] = not all(before == after for before, after in
                    zip(receipt.get("protectedBefore", []), receipt["protectedAfter"])
                    if before["path"] == str(PLAYER_DLL.resolve()))
                require(receipt["protectedUnchanged"], "protected source/player identity drift")
            except Exception as error:
                receipt["cleanupErrors"].append(f"preservation: {error}")
            receipt["census"] = []
            if args.startup_witness:
                try:
                    launch = receipt.get('gameLaunch', {})
                    require(launch.get('jobEmpty') is True and type(launch.get('exitCode')) is int,
                            'startup witness requires exact process/Job exit')
                    receipt['startupWitness'] = startup_reader.analyze_file(
                        output / 'logs/startup-witness.bin', process_exited=True,
                        expected_pid=launch['pid'], expected_creation_time=launch['creation100ns'])
                except Exception as error:
                    receipt['startupWitness'] = dict(verdict='NOT_COVERED', error=str(error))
            if args.mdl_allocation_trace:
                try:
                    launch = receipt.get('gameLaunch', {})
                    require(launch.get('jobEmpty') is True and type(launch.get('exitCode')) is int,
                            'MDLDATA post-exit read requires owned process and Job reaped')
                    trace = output / 'logs/mdl-link-trace.bin'
                    require(trace.is_file(), 'MDLDATA probe file missing; installation not covered')
                    receipt['mdlAllocationTrace'] = mdl_reader.analyze_file(trace,
                        process_exited=True, expected_pid=launch['pid'],
                        expected_creation_time=launch['creation100ns'])
                except Exception as error:
                    receipt['mdlAllocationTrace'] = dict(verdict='NOT_COVERED', certified=False,
                                                        error=str(error))
            for path in sorted((output / "logs").glob("*.log")):
                try:
                    receipt["census"].append(parse_census(path))
                except Exception as error:
                    receipt["census"].append(dict(path=str(path), rows=[], errors=[str(error)]))
            try:
                receipt["artifacts"] = [identity(path) for path in sorted(output.rglob("*")) if path.is_file()]
                receipt["instanceArtifacts"] = []
                if instance is not None:
                    receipt["instanceArtifacts"] = instance_artifacts(instance)
            except Exception as error:
                receipt["cleanupErrors"].append(f"artifact-index: {error}")
            classify(receipt)
            (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: receipt.get(key) for key in ("scope", "ok", "verdict", "observationAvailable",
        "normalExitProven", "gameLaunched", "error")}, ensure_ascii=False))
    # Only the explicit local start/capture/exit mode can sign that narrow gate.
    # Neither mode certifies x64 rendering, visual equivalence or memory savings.
    return 0 if receipt["ok"] and args.normal_exit_test else 2


if __name__ == "__main__":
    raise SystemExit(main())
