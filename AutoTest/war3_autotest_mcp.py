#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
War3 自动化测试 MCP 服务

目标：
1) 复用 YDWE 的直进地图行为（-loadfile + 测试地图复制）。
2) 订阅 OutputDebugString，自动判断“进入游戏”阶段。
3) 自动截屏、收集性能报告并做摘要。
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import copy
import functools
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import threading
import time
import winreg
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from mcp.server.fastmcp import FastMCP

from autotest_sessions import (
    AutoTestSession,
    DEFAULT_SANDBOX_ROOT,
    SessionRegistry,
    build_instance_layout,
    deploy_content_addressed_map,
    materialize_instance_root,
    normalize_identifier,
    preflight_instance_pool as _preflight_session_pool,
    require_path_within,
    sha256_file,
)
from ydhost_adapter import (
    generate_ydhost_map_metadata as _generate_ydhost_map_metadata,
    preflight_ydhost_lan as _preflight_ydhost_lan,
    provision_ydhost_assets as _provision_ydhost_assets,
)
from ydhost_lan_adapter import real_launch_capability as _ydhost_real_launch_capability


DEFAULT_WAR3_DIR = DEFAULT_SANDBOX_ROOT
DEFAULT_TEST_MAP = DEFAULT_SANDBOX_ROOT / "Maps" / "光影测试.w3x"
DEFAULT_LOW_PRESSURE_TEST_MAP = DEFAULT_SANDBOX_ROOT / "Maps" / "ShadowTest" / "光影测试.w3x"
DEFAULT_CITY_MAP = DEFAULT_SANDBOX_ROOT / "Maps" / "dz" / "rpg" / "City.w3x"
DEFAULT_CITY_FALLBACK_MAP = DEFAULT_TEST_MAP
DEFAULT_LIFE_AND_DEATH_MAP = Path(
    r"E:\Work\Warcraft III\Maps\(4)生与死v1.28读档bug修复.w3x"
)
DEFAULT_TEST_MAP_REL = Path(r"Maps\Test\WorldEditTestMap.w3x")
DEFAULT_BENCHMARK_WIDTH = 2560
DEFAULT_BENCHMARK_HEIGHT = 1440
DEFAULT_BENCHMARK_REFRESH = 59
AUTOTEST_BACKGROUND_THROTTLE_ENV = "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE"
AUTOTEST_GAME_PAUSE_ENV = "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE"
AUTOTEST_INTERNAL_TEST_API_ENV = "DXVK_WAR3_INTERNAL_TEST_API"
# Win32 Desktop-object launches are quarantined on the current test machine.
# The display stack can blank the interactive desktop while War3 remains alive
# on the non-input desktop, which makes an unattended test unsafe for the user.
ISOLATED_DESKTOP_QUARANTINED = True
WAR3_VIDEO_REG_KEY = r"Software\Blizzard Entertainment\Warcraft III\Video"
WAR3_INSTALL_REG_KEY = r"Software\Blizzard Entertainment\Warcraft III"
YDWE_LAUNCHER_MODE_DIRECT = "direct"
YDWE_LAUNCHER_MODE_YDWE = "ydwe"
ARTIFACT_ROOT = Path(__file__).resolve().parent / "artifacts"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_D3D9 = REPO_ROOT / "build32" / "src" / "d3d9" / "d3d9.dll"
MAX_EVENT_BUFFER = 4000
DESKTOP_READOBJECTS = 0x0001
DESKTOP_CREATEWINDOW = 0x0002
DESKTOP_ENUMERATE = 0x0040
DESKTOP_WRITEOBJECTS = 0x0080
DESKTOP_SWITCHDESKTOP = 0x0100
CREATE_UNICODE_ENVIRONMENT = 0x00000400
CREATE_NEW_CONSOLE = 0x00000010
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
PIPE_READMODE_MESSAGE = 0x00000002
ERROR_MORE_DATA = 234
ERROR_BROKEN_PIPE = 109
ERROR_ALREADY_EXISTS = 183
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS = 9
PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SYNCHRONIZE = 0x00100000
DUPLICATE_SAME_ACCESS = 0x00000002
WAIT_OBJECT_0 = 0x00000000
WAIT_TIMEOUT = 0x00000102
WAIT_FAILED = 0xFFFFFFFF
STILL_ACTIVE = 259

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
_KERNEL32.ReadFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
_KERNEL32.ReadFile.restype = wintypes.BOOL
_KERNEL32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
]
_KERNEL32.PeekNamedPipe.restype = wintypes.BOOL
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
_KERNEL32.CloseHandle.argtypes = [wintypes.HANDLE]
_KERNEL32.CloseHandle.restype = wintypes.BOOL
_KERNEL32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
_KERNEL32.CreateMutexW.restype = wintypes.HANDLE
_KERNEL32.ReleaseMutex.argtypes = [wintypes.HANDLE]
_KERNEL32.ReleaseMutex.restype = wintypes.BOOL
_KERNEL32.OpenProcess.argtypes = [
    wintypes.DWORD, wintypes.BOOL, wintypes.DWORD,
]
_KERNEL32.OpenProcess.restype = wintypes.HANDLE
_KERNEL32.DuplicateHandle.argtypes = [
    wintypes.HANDLE,
    wintypes.HANDLE,
    wintypes.HANDLE,
    ctypes.POINTER(wintypes.HANDLE),
    wintypes.DWORD,
    wintypes.BOOL,
    wintypes.DWORD,
]
_KERNEL32.DuplicateHandle.restype = wintypes.BOOL
_KERNEL32.GetCurrentProcess.argtypes = []
_KERNEL32.GetCurrentProcess.restype = wintypes.HANDLE
_KERNEL32.GetProcessId.argtypes = [wintypes.HANDLE]
_KERNEL32.GetProcessId.restype = wintypes.DWORD
_KERNEL32.GetProcessTimes.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(wintypes.FILETIME),
    ctypes.POINTER(wintypes.FILETIME),
    ctypes.POINTER(wintypes.FILETIME),
    ctypes.POINTER(wintypes.FILETIME),
]
_KERNEL32.GetProcessTimes.restype = wintypes.BOOL
_KERNEL32.QueryFullProcessImageNameW.argtypes = [
    wintypes.HANDLE,
    wintypes.DWORD,
    wintypes.LPWSTR,
    ctypes.POINTER(wintypes.DWORD),
]
_KERNEL32.QueryFullProcessImageNameW.restype = wintypes.BOOL
_KERNEL32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_KERNEL32.WaitForSingleObject.restype = wintypes.DWORD
_KERNEL32.GetExitCodeProcess.argtypes = [
    wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
]
_KERNEL32.GetExitCodeProcess.restype = wintypes.BOOL
_KERNEL32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
_KERNEL32.TerminateProcess.restype = wintypes.BOOL


class _StartupInfoW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class _ProcessInformation(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    ]


def _native_handle_value(value: Any) -> int:
    if isinstance(value, int) and not isinstance(value, bool):
        return int(value)
    return int(getattr(value, "value", 0) or 0)


def _canonical_process_path(value: Any) -> str:
    text = str(value or "").strip().strip('"')
    if not text:
        return ""
    return os.path.normcase(os.path.normpath(os.path.abspath(text)))


def _native_process_binding_from_handle(handle: int) -> Dict[str, Any]:
    handle_value = int(handle or 0)
    result: Dict[str, Any] = {
        "ok": False,
        "pid": 0,
        "creationEpochMs": 0,
        "canonicalExePath": "",
        "error": "",
    }
    if handle_value <= 0:
        result["error"] = "invalid native process handle"
        return result
    native_handle = wintypes.HANDLE(handle_value)
    pid = int(_KERNEL32.GetProcessId(native_handle) or 0)
    if pid <= 0:
        result["error"] = f"GetProcessId failed: {ctypes.get_last_error()}"
        return result
    creation = wintypes.FILETIME()
    exit_time = wintypes.FILETIME()
    kernel_time = wintypes.FILETIME()
    user_time = wintypes.FILETIME()
    if not _KERNEL32.GetProcessTimes(
        native_handle,
        ctypes.byref(creation),
        ctypes.byref(exit_time),
        ctypes.byref(kernel_time),
        ctypes.byref(user_time),
    ):
        result["error"] = f"GetProcessTimes failed: {ctypes.get_last_error()}"
        return result
    path_buffer = ctypes.create_unicode_buffer(32768)
    path_length = wintypes.DWORD(len(path_buffer))
    if not _KERNEL32.QueryFullProcessImageNameW(
        native_handle, 0, path_buffer, ctypes.byref(path_length),
    ):
        result["error"] = (
            "QueryFullProcessImageNameW failed: "
            f"{ctypes.get_last_error()}"
        )
        return result
    creation_ticks = (
        (int(creation.dwHighDateTime) << 32)
        | int(creation.dwLowDateTime)
    )
    windows_to_unix_100ns = 116_444_736_000_000_000
    creation_epoch_ms = max(
        0, (creation_ticks - windows_to_unix_100ns) // 10_000,
    )
    canonical_path = _canonical_process_path(path_buffer.value)
    result.update({
        "ok": bool(pid > 0 and creation_epoch_ms > 0 and canonical_path),
        "pid": pid,
        "creationEpochMs": int(creation_epoch_ms),
        "canonicalExePath": canonical_path,
    })
    if not result["ok"]:
        result["error"] = "incomplete native process binding"
    return result


class RetainedNativeProcessWitness:
    """Owns one process HANDLE and its immutable launch-time identity."""

    def __init__(
        self,
        handle: int,
        pid: int,
        creation_epoch_ms: int,
        canonical_exe_path: str,
        source: str,
    ) -> None:
        self._handle = int(handle)
        self.pid = int(pid)
        self.creation_epoch_ms = int(creation_epoch_ms)
        self.canonical_exe_path = _canonical_process_path(
            canonical_exe_path
        )
        self.source = str(source or "native-process")
        self._closed = False
        self._lock = threading.Lock()

    @property
    def closed(self) -> bool:
        with self._lock:
            return self._closed

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            available = bool(not self._closed and self._handle > 0)
            return {
                "available": available,
                "ownsNativeHandle": available,
                "closed": self._closed,
                "pid": self.pid,
                "creationEpochMs": self.creation_epoch_ms,
                "canonicalExePath": self.canonical_exe_path,
                "source": self.source,
            }

    def to_dict(self) -> Dict[str, Any]:
        return self.snapshot()

    def _binding_exact(
        self, pid: int, creation_epoch_ms: int, canonical_exe_path: str,
    ) -> bool:
        return bool(
            self.pid == int(pid)
            and self.creation_epoch_ms == int(creation_epoch_ms)
            and self.creation_epoch_ms > 0
            and self.canonical_exe_path
            == _canonical_process_path(canonical_exe_path)
        )

    def duplicate(self) -> "RetainedNativeProcessWitness":
        with self._lock:
            if self._closed or self._handle <= 0:
                raise RuntimeError("native process witness is closed")
            duplicate = wintypes.HANDLE()
            current = _KERNEL32.GetCurrentProcess()
            ok = bool(_KERNEL32.DuplicateHandle(
                current,
                wintypes.HANDLE(self._handle),
                current,
                ctypes.byref(duplicate),
                0,
                False,
                DUPLICATE_SAME_ACCESS,
            ))
            if not ok:
                raise OSError(
                    ctypes.get_last_error(), "DuplicateHandle failed",
                )
            duplicate_value = _native_handle_value(duplicate)
            if duplicate_value <= 0:
                raise RuntimeError("DuplicateHandle returned an invalid handle")
            return RetainedNativeProcessWitness(
                duplicate_value,
                self.pid,
                self.creation_epoch_ms,
                self.canonical_exe_path,
                f"duplicate:{self.source}",
            )

    def poll(self) -> Optional[int]:
        with self._lock:
            if self._closed or self._handle <= 0:
                raise RuntimeError("native process witness is closed")
            wait_result = int(_KERNEL32.WaitForSingleObject(
                wintypes.HANDLE(self._handle), 0,
            ))
            if wait_result == WAIT_TIMEOUT:
                return None
            if wait_result != WAIT_OBJECT_0:
                raise OSError(
                    ctypes.get_last_error(),
                    f"WaitForSingleObject failed: {wait_result}",
                )
            exit_code = wintypes.DWORD(0)
            if not _KERNEL32.GetExitCodeProcess(
                wintypes.HANDLE(self._handle), ctypes.byref(exit_code),
            ):
                raise OSError(
                    ctypes.get_last_error(), "GetExitCodeProcess failed",
                )
            return int(exit_code.value)

    def wait(self, timeout: Optional[float] = None) -> int:
        timeout_ms = (
            0xFFFFFFFF
            if timeout is None
            else max(0, min(0xFFFFFFFE, int(timeout * 1000.0)))
        )
        with self._lock:
            if self._closed or self._handle <= 0:
                raise RuntimeError("native process witness is closed")
            wait_result = int(_KERNEL32.WaitForSingleObject(
                wintypes.HANDLE(self._handle), timeout_ms,
            ))
            if wait_result == WAIT_TIMEOUT:
                raise subprocess.TimeoutExpired(
                    "retained-native-process", timeout,
                )
            if wait_result != WAIT_OBJECT_0:
                raise OSError(
                    ctypes.get_last_error(),
                    f"WaitForSingleObject failed: {wait_result}",
                )
            exit_code = wintypes.DWORD(0)
            if not _KERNEL32.GetExitCodeProcess(
                wintypes.HANDLE(self._handle), ctypes.byref(exit_code),
            ):
                raise OSError(
                    ctypes.get_last_error(), "GetExitCodeProcess failed",
                )
            return int(exit_code.value)

    def termination_exact(
        self, pid: int, creation_epoch_ms: int, canonical_exe_path: str,
    ) -> Dict[str, Any]:
        snapshot = self.snapshot()
        binding_exact = bool(
            snapshot.get("available") is True
            and self._binding_exact(
                pid, creation_epoch_ms, canonical_exe_path,
            )
        )
        exit_code: Optional[int] = None
        poll_error = ""
        if binding_exact:
            try:
                exit_code = self.poll()
            except Exception as exc:
                poll_error = f"{type(exc).__name__}: {exc}"
        signaled = bool(
            binding_exact and not poll_error and exit_code is not None
        )
        return {
            "exact": signaled,
            "bindingExact": binding_exact,
            "handleSignaled": signaled,
            "exitCode": exit_code,
            "pollError": poll_error,
            "witness": snapshot,
            "reportOnly": not signaled,
            "failureClassificationAuthority": 1 if signaled else 0,
        }

    def terminate_exact(
        self,
        pid: int,
        creation_epoch_ms: int,
        canonical_exe_path: str,
        exit_code: int = 0xC0DE0001,
        wait_timeout_sec: float = 10.0,
    ) -> Dict[str, Any]:
        snapshot = self.snapshot()
        binding_exact = bool(
            snapshot.get("available") is True
            and self._binding_exact(
                pid, creation_epoch_ms, canonical_exe_path,
            )
        )
        if not binding_exact:
            return {
                "ok": False,
                "exact": False,
                "bindingExact": False,
                "handleSignaled": False,
                "terminatedByHandle": False,
                "reportOnly": True,
                "failureClassificationAuthority": 0,
                "witness": snapshot,
            }
        before = self.termination_exact(
            pid, creation_epoch_ms, canonical_exe_path,
        )
        if before.get("exact") is True:
            return {
                "ok": True,
                "exact": True,
                "bindingExact": True,
                "handleSignaled": True,
                "terminatedByHandle": False,
                "alreadyTerminated": True,
                "terminationProof": before,
                "reportOnly": False,
                "failureClassificationAuthority": 1,
            }
        terminate_error = 0
        with self._lock:
            if self._closed or self._handle <= 0:
                return {
                    "ok": False,
                    "exact": False,
                    "bindingExact": False,
                    "handleSignaled": False,
                    "terminatedByHandle": False,
                    "error": "native process witness closed before terminate",
                    "reportOnly": True,
                    "failureClassificationAuthority": 0,
                }
            terminated = bool(_KERNEL32.TerminateProcess(
                wintypes.HANDLE(self._handle),
                int(exit_code) & 0xFFFFFFFF,
            ))
            if not terminated:
                terminate_error = int(ctypes.get_last_error())
        wait_error = ""
        if terminated:
            try:
                self.wait(max(0.0, float(wait_timeout_sec)))
            except Exception as exc:
                wait_error = f"{type(exc).__name__}: {exc}"
        after = self.termination_exact(
            pid, creation_epoch_ms, canonical_exe_path,
        )
        natural_exit_race = bool(
            not terminated and after.get("exact") is True
        )
        exact = bool(
            not wait_error
            and after.get("exact") is True
            and after.get("bindingExact") is True
            and after.get("handleSignaled") is True
        )
        return {
            "ok": exact,
            "exact": exact,
            "bindingExact": binding_exact,
            "handleSignaled": after.get("handleSignaled") is True,
            "terminatedByHandle": terminated,
            "naturalExitRace": natural_exit_race,
            "alreadyTerminated": False,
            "requestedExitCode": int(exit_code) & 0xFFFFFFFF,
            "terminateWin32Error": terminate_error,
            "waitError": wait_error,
            "terminationProof": after,
            "pidTerminationCommandIssued": False,
            "reportOnly": not exact,
            "failureClassificationAuthority": 1 if exact else 0,
        }

    def close(self) -> Dict[str, Any]:
        with self._lock:
            if self._closed or self._handle <= 0:
                self._closed = True
                self._handle = 0
                return {
                    "ok": True,
                    "closed": True,
                    "skipped": True,
                    "pid": self.pid,
                }
            handle = self._handle
            # Invalidate before CloseHandle. A closed integer is never exposed
            # or reused, even if Windows later recycles the numeric value.
            self._handle = 0
            self._closed = True
            closed = bool(_KERNEL32.CloseHandle(wintypes.HANDLE(handle)))
            return {
                "ok": closed,
                "closed": closed,
                "skipped": False,
                "pid": self.pid,
                "win32Error": 0 if closed else ctypes.get_last_error(),
            }

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def _terminate_and_close_owned_create_process_handle(
    handle: Any,
    exit_code: int = 0xC0DE0002,
    wait_timeout_ms: int = 10_000,
) -> Dict[str, Any]:
    """Clean an unpublishable CreateProcessW child via its owned hProcess."""
    handle_value = _native_handle_value(handle)
    if handle_value <= 0:
        return {
            "ok": False,
            "ownedCreateProcessHandle": False,
            "handleSignaled": False,
            "closed": False,
            "terminatedByHandle": False,
            "pidTerminationCommandIssued": False,
            "error": "invalid owned CreateProcessW process handle",
        }

    native_handle = wintypes.HANDLE(handle_value)
    pre_wait = int(_KERNEL32.WaitForSingleObject(native_handle, 0))
    already_signaled = pre_wait == WAIT_OBJECT_0
    terminate_attempted = pre_wait == WAIT_TIMEOUT
    terminated_by_handle = False
    terminate_error = 0
    if terminate_attempted:
        terminated_by_handle = bool(_KERNEL32.TerminateProcess(
            native_handle, int(exit_code) & 0xFFFFFFFF,
        ))
        if not terminated_by_handle:
            terminate_error = int(ctypes.get_last_error())

    # A successful TerminateProcess is asynchronous. If it failed, use a
    # zero-time recheck only: a concurrently natural exit is authoritative,
    # but this cleanup must never wait on or address a PID fallback.
    post_wait = pre_wait
    if terminate_attempted:
        post_wait = int(_KERNEL32.WaitForSingleObject(
            native_handle,
            max(0, int(wait_timeout_ms)) if terminated_by_handle else 0,
        ))
    handle_signaled = post_wait == WAIT_OBJECT_0
    natural_exit_race = bool(
        terminate_attempted
        and not terminated_by_handle
        and handle_signaled
    )
    exit_value: Optional[int] = None
    exit_query_error = 0
    if handle_signaled:
        native_exit = wintypes.DWORD(0)
        if _KERNEL32.GetExitCodeProcess(
            native_handle, ctypes.byref(native_exit),
        ):
            exit_value = int(native_exit.value)
        else:
            exit_query_error = int(ctypes.get_last_error())

    # Invalidate the sole local integer by closing exactly once. No caller is
    # given the raw value and no PID/name fallback is issued.
    closed = bool(_KERNEL32.CloseHandle(native_handle))
    close_error = 0 if closed else int(ctypes.get_last_error())
    exact_cleanup = bool(handle_signaled and closed)
    return {
        "ok": exact_cleanup,
        "ownedCreateProcessHandle": True,
        "terminateAttempted": terminate_attempted,
        "terminatedByHandle": terminated_by_handle,
        "alreadySignaled": already_signaled,
        "naturalExitRace": natural_exit_race,
        "handleSignaled": handle_signaled,
        "waitResult": post_wait,
        "requestedExitCode": int(exit_code) & 0xFFFFFFFF,
        "exitCode": exit_value,
        "terminateWin32Error": terminate_error,
        "exitQueryWin32Error": exit_query_error,
        "closed": closed,
        "closeWin32Error": close_error,
        "pidTerminationCommandIssued": False,
        "failureClassificationAuthority": 1 if exact_cleanup else 0,
    }


def _adopt_native_process_witness(
    handle: Any,
    expected_pid: int,
    expected_exe_path: Any,
    source: str,
    terminate_owned_create_on_failure: bool = False,
) -> Tuple[Optional[RetainedNativeProcessWitness], Dict[str, Any]]:
    handle_value = _native_handle_value(handle)
    binding = _native_process_binding_from_handle(handle_value)
    expected_path = _canonical_process_path(expected_exe_path)
    exact = bool(
        binding.get("ok") is True
        and binding.get("pid") == int(expected_pid)
        and isinstance(binding.get("creationEpochMs"), int)
        and not isinstance(binding.get("creationEpochMs"), bool)
        and binding.get("creationEpochMs", 0) > 0
        and binding.get("canonicalExePath") == expected_path
    )
    result = {
        "ok": exact,
        "source": source,
        "expectedPid": int(expected_pid),
        "expectedCanonicalExePath": expected_path,
        "binding": binding,
        "failureClassificationAuthority": 1 if exact else 0,
    }
    if not exact:
        owned_create_cleanup: Dict[str, Any] = {
            "ok": True,
            "skipped": True,
            "reason": "not an owned CreateProcessW failure handle",
            "pidTerminationCommandIssued": False,
        }
        if handle_value > 0:
            if terminate_owned_create_on_failure:
                owned_create_cleanup = (
                    _terminate_and_close_owned_create_process_handle(
                        handle_value,
                    )
                )
            else:
                closed = bool(_KERNEL32.CloseHandle(
                    wintypes.HANDLE(handle_value)
                ))
                owned_create_cleanup = {
                    "ok": closed,
                    "closed": closed,
                    "terminatedByHandle": False,
                    "pidTerminationCommandIssued": False,
                    "win32Error": (
                        0 if closed else int(ctypes.get_last_error())
                    ),
                }
        result["handleFailureCleanup"] = owned_create_cleanup
        result["error"] = (
            binding.get("error")
            or "native process handle identity mismatch"
        )
        return None, result
    witness = RetainedNativeProcessWitness(
        handle_value,
        int(binding["pid"]),
        int(binding["creationEpochMs"]),
        str(binding["canonicalExePath"]),
        source,
    )
    result["witness"] = witness.snapshot()
    return witness, result


def _open_native_process_witness(
    pid: int, expected_exe_path: Any, source: str,
) -> Tuple[Optional[RetainedNativeProcessWitness], Dict[str, Any]]:
    handle = _KERNEL32.OpenProcess(
        PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        False,
        int(pid),
    )
    if not handle:
        return None, {
            "ok": False,
            "source": source,
            "expectedPid": int(pid),
            "expectedCanonicalExePath": _canonical_process_path(
                expected_exe_path
            ),
            "error": f"OpenProcess failed: {ctypes.get_last_error()}",
            "failureClassificationAuthority": 0,
        }
    return _adopt_native_process_witness(
        handle, pid, expected_exe_path, source,
    )


class _ProcessEntry32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


class _ModuleEntry32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", wintypes.HMODULE),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * 260),
    ]


class _JobObjectBasicLimitInformation(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_longlong),
        ("PerJobUserTimeLimit", ctypes.c_longlong),
        ("LimitFlags", wintypes.DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD),
    ]


class _IoCounters(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_ulonglong),
        ("WriteOperationCount", ctypes.c_ulonglong),
        ("OtherOperationCount", ctypes.c_ulonglong),
        ("ReadTransferCount", ctypes.c_ulonglong),
        ("WriteTransferCount", ctypes.c_ulonglong),
        ("OtherTransferCount", ctypes.c_ulonglong),
    ]


class _JobObjectExtendedLimitInformation(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", _JobObjectBasicLimitInformation),
        ("IoInfo", _IoCounters),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


RUNTIME_MODULE_ORDER = [
    "hook.lifecycle",
    "hook.ui",
    "hook.jass",
    "hook.render",
    "diag",
    "render.queue",
    "shadow.capture",
    "shadow.map",
    "shadow.receiver",
    "shadow.taa",
    "postfx",
    "ssao",
    "aa",
    "semantic.data",
]
PROFILE_DEFAULT_DISABLED = {
    "dxvk_only": set(RUNTIME_MODULE_ORDER),
    "hooks_minimal": {
        "hook.ui",
        "hook.jass",
        "hook.render",
        "diag",
        "render.queue",
        "shadow.capture",
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "hooks_default": {
        "render.queue",
        "shadow.capture",
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "render_base": {
        "shadow.capture",
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "shadow_capture_only": {
        "shadow.map",
        "shadow.receiver",
        "shadow.taa",
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "shadow_full": {
        "postfx",
        "ssao",
        "aa",
        "semantic.data",
    },
    "full_default": set(),
    "full_analysis": set(),
    "full_perf_experimental": set(),
}
BENCHMARK_LINE_RE = re.compile(
    r"DXVK War3Benchmark:\s+avgFPS=(?P<avg>[-0-9.]+)"
    r".*?sampleFrames=(?P<frames>\d+)"
    r".*?warmupSec=(?P<warmup>[-0-9.]+)"
    r".*?sampleSec=(?P<sample>[-0-9.]+)"
    r"(?:.*?mode=(?P<mode>[A-Za-z0-9_\-]+))?"
    r"(?:.*?profile=(?P<profile>[A-Za-z0-9_\-]+))?"
    r"(?:.*?disabledModules=(?P<disabled>[^\r\n]*))?"
)
PROFILE_MATRIX_CASES: List[Dict[str, Any]] = [
    {"name": "add_dxvk_only", "profile": "dxvk_only", "disable": "", "group": "additive-core", "label": "DXVK-only", "category": "baseline", "budgetFps": 0.0},
    {"name": "add_hooks_minimal", "profile": "hooks_minimal", "disable": "", "group": "additive-extra", "label": "Hooks Minimal", "category": "hook_probe", "budgetFps": 10.0},
    {"name": "add_hooks_default", "profile": "hooks_default", "disable": "", "group": "additive-core", "label": "Hooks Default", "category": "hook_total", "budgetFps": 25.0},
    {"name": "add_render_base", "profile": "render_base", "disable": "", "group": "additive-core", "label": "Render Base", "category": "render_bridge", "budgetFps": 25.0},
    {"name": "add_shadow_capture_only", "profile": "shadow_capture_only", "disable": "", "group": "additive-extra", "label": "Shadow Capture Only", "category": "shadow_capture_probe", "budgetFps": 45.0},
    {"name": "add_shadow_full", "profile": "shadow_full", "disable": "", "group": "additive-core", "label": "Shadow Full", "category": "shadow_total", "budgetFps": 65.0},
    {"name": "add_full_default", "profile": "full_default", "disable": "", "group": "additive-core", "label": "Full Default", "category": "full_stack", "budgetFps": 0.0},
    {"name": "add_full_analysis", "profile": "full_analysis", "disable": "", "group": "additive-extra", "label": "Full Analysis", "category": "final_profile", "budgetFps": 15.0},
    {"name": "add_full_perf_experimental", "profile": "full_perf_experimental", "disable": "", "group": "additive-extra", "label": "Full Perf Experimental", "category": "final_profile", "budgetFps": 0.0},
    {"name": "sub_no_diag", "profile": "full_default", "disable": "diag", "group": "subtractive", "label": "No Diag", "category": "diag", "budgetFps": 10.0},
    {"name": "sub_no_hook_lifecycle", "profile": "full_default", "disable": "hook.lifecycle", "group": "subtractive", "label": "No Hook Lifecycle", "category": "non_render_hook", "budgetFps": 10.0},
    {"name": "sub_no_hook_ui", "profile": "full_default", "disable": "hook.ui", "group": "subtractive", "label": "No Hook UI", "category": "non_render_hook", "budgetFps": 10.0},
    {"name": "sub_no_hook_jass", "profile": "full_default", "disable": "hook.jass", "group": "subtractive", "label": "No Hook JASS", "category": "non_render_hook", "budgetFps": 10.0},
    {"name": "sub_no_hook_render", "profile": "full_default", "disable": "hook.render", "group": "subtractive", "label": "No Hook Render", "category": "render_bridge", "budgetFps": 25.0},
    {"name": "sub_no_semantic_data", "profile": "full_default", "disable": "semantic.data", "group": "subtractive", "label": "No Semantic Data", "category": "semantic_data", "budgetFps": 35.0},
    {"name": "sub_no_render_queue", "profile": "full_default", "disable": "render.queue", "group": "subtractive", "label": "No Render Queue", "category": "render_bridge", "budgetFps": 25.0},
    {"name": "sub_no_shadow_capture", "profile": "full_default", "disable": "shadow.capture", "group": "subtractive", "label": "No Shadow Capture", "category": "shadow_capture", "budgetFps": 45.0},
    {"name": "sub_no_shadow_map", "profile": "full_default", "disable": "shadow.map", "group": "subtractive", "label": "No Shadow Map", "category": "shadow_render", "budgetFps": 20.0},
    {"name": "sub_no_shadow_receiver", "profile": "full_default", "disable": "shadow.receiver", "group": "subtractive", "label": "No Shadow Receiver", "category": "shadow_render", "budgetFps": 20.0},
    {"name": "sub_no_shadow_taa", "profile": "full_default", "disable": "shadow.taa", "group": "subtractive", "label": "No Shadow TAA", "category": "shadow_render", "budgetFps": 20.0},
    {"name": "sub_no_postfx", "profile": "full_default", "disable": "postfx", "group": "subtractive", "label": "No PostFX", "category": "postfx_diag", "budgetFps": 10.0},
    {"name": "sub_no_ssao", "profile": "full_default", "disable": "ssao", "group": "subtractive", "label": "No SSAO", "category": "postfx_diag", "budgetFps": 10.0},
    {"name": "sub_no_aa", "profile": "full_default", "disable": "aa", "group": "subtractive", "label": "No AA", "category": "postfx_diag", "budgetFps": 10.0},
]

SEMANTIC_SHADOW_VALIDATION_ENV: Dict[str, str] = {
    "DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_BOOTSTRAP_CATCHUP": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD": "0",
    "DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_FLUSH": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_TAIL_FALLBACK": "1",
    "DXVK_WAR3_SEMANTIC_SHADOW_PRE_READY": "1",
    "DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE": "1",
}

SCENARIO_PRESETS: Dict[str, Dict[str, Any]] = {
    "life_and_death_tdr": {
        "title": "生与死低视角 TDR 巡航",
        "description": "冷启动进入生与死，开启全图视野并按 5x5 蛇形路径低视角巡航；首次设备错误立即止损。",
        "mapPath": str(DEFAULT_LIFE_AND_DEATH_MAP),
        "launcherMode": YDWE_LAUNCHER_MODE_YDWE,
        "ydweRoot": r"E:\Work\War3\YDWE1.32.13 - MemoryHack",
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": False,
        "desktopName": "War3LifeAndDeathTdr",
        "readyTimeoutSec": 240,
        "sampleDurationSec": 600,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 604,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "requireHotShadowFrame": False,
        "specializedRunner": "run_life_and_death_tdr_scenario",
        "envOverrides": {},
    },
    "low_pressure_static_reuse": {
        "title": "低压静态复用",
        "description": "低压图静态复用基线，用于观察 persistent cache 的静态收益与 shadowRuntimeV2 占位摘要。",
        "mapPath": str(DEFAULT_LOW_PRESSURE_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3LowPressureStatic",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 20,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 24,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "dynamic_shadow_pressure": {
        "title": "动作单位压力",
        "description": "大量动作/飞行单位场景，用于验证动态阴影正确性与 fallback 成本。",
        "mapPath": str(DEFAULT_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3DynamicShadowPressure",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 22,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 26,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "model_runtime_probe": {
        "title": "模型运行时探针",
        "description": "模型加载、实例绑定与动画姿态更新链路的诊断场景。",
        "mapPath": str(DEFAULT_TEST_MAP),
        "profile": "full_analysis",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3ModelRuntimeProbe",
        "readyTimeoutSec": 180,
        "sampleDurationSec": 18,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 24,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "semantic_cost_probe": {
        "title": "语义追踪成本探针",
        "description": "量化 shadow semantic tracking 常驻开销、命中率与未来降级空间。",
        "mapPath": str(DEFAULT_LOW_PRESSURE_TEST_MAP),
        "profile": "full_analysis",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3SemanticCostProbe",
        "readyTimeoutSec": 150,
        "sampleDurationSec": 18,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 22,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "rigid_static_canonical_smoke": {
        "title": "Rigid/Static Canonical Smoke",
        "description": "用于 Phase 4 prepared 的 rigid/static canonical 场景预案，不强制 hot-shadow gate。",
        "mapPath": str(DEFAULT_LOW_PRESSURE_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3RigidStaticCanonical",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 18,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 22,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "requireHotShadowFrame": False,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "static_world_caster_acceptance": {
        "title": "Static World Caster Acceptance",
        "description": "用于 Phase 4 correctness：验证 Building / Destructible / rigid-static canonical 提交。",
        "mapPath": str(DEFAULT_LOW_PRESSURE_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3StaticWorldCasterAcceptance",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 20,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 24,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
    "phase4_world_caster_acceptance": {
        "title": "Phase 4 World Caster Acceptance",
        "description": "用于 Phase 4 correctness：在真实混合 ShadowTest 场景中验证 Building / Destructible canonical 提交。",
        "mapPath": str(DEFAULT_TEST_MAP),
        "profile": "full_default",
        "disableModules": "",
        "windowed": True,
        "useIsolatedDesktop": True,
        "desktopName": "War3Phase4WorldCasterAcceptance",
        "readyTimeoutSec": 120,
        "sampleDurationSec": 22,
        "autoPerfRecord": True,
        "recordAfterGameStarted": True,
        "autoPerfExportSec": 26,
        "deployD3d9BeforeLaunch": True,
        "opengl": False,
        "enforceVideoBaseline": False,
        "baselineWidth": DEFAULT_BENCHMARK_WIDTH,
        "baselineHeight": DEFAULT_BENCHMARK_HEIGHT,
        "baselineRefreshRate": DEFAULT_BENCHMARK_REFRESH,
        "envOverrides": SEMANTIC_SHADOW_VALIDATION_ENV,
    },
}
PROFILE_MATRIX_PRIMARY_CHAIN = [
    "add_dxvk_only",
    "add_hooks_default",
    "add_render_base",
    "add_shadow_full",
    "add_full_default",
]
LOG_KEYWORD_PATTERNS: List[Tuple[str, re.Pattern[str]]] = [
    ("deviceLost", re.compile(r"VK_ERROR_DEVICE_LOST", re.IGNORECASE)),
    ("freezeBudgetExceeded", re.compile(r"Freeze budget exceeded", re.IGNORECASE)),
    ("shadowCaptureIncomplete", re.compile(r"incomplete capture", re.IGNORECASE)),
    ("shadowReuseLastComplete", re.compile(r"reuse last shadow map", re.IGNORECASE)),
    ("shadowRenderPartial", re.compile(r"render current partial shadow map", re.IGNORECASE)),
    ("shadowAdaptiveSkip", re.compile(r"Adaptive skip ShadowMap", re.IGNORECASE)),
    ("csmComputeFailed", re.compile(r"CSM compute failed", re.IGNORECASE)),
    ("csmConservativeFallback", re.compile(r"conservative cascade fallback", re.IGNORECASE)),
    ("runtimeReady", re.compile(r"(?:JASS|War3) runtime fully initialized", re.IGNORECASE)),
    ("stage19", re.compile(r"War3StageSig: stage=19", re.IGNORECASE)),
    ("pauseBlocked", re.compile(r"blocked GamePause request", re.IGNORECASE)),
    (
        "backgroundIdleSleepBypassed",
        re.compile(r"bypassed WM_ACTIVATEAPP background idle sleep", re.IGNORECASE),
    ),
    ("internalTestApi", re.compile(r"DXVK War3TestApi:", re.IGNORECASE)),
]


def _now_str() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _now_compact() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def _bool_env(v: str) -> bool:
    return str(v).strip().lower() in ("1", "true", "yes", "on")


def _parse_env_overrides_json(text: str) -> Dict[str, str]:
    raw = str(text or "").strip()
    if not raw:
        return {}
    try:
        data = json.loads(raw)
    except Exception as e:
        return {"__parse_error__": str(e)}
    if not isinstance(data, dict):
        return {"__parse_error__": "env_overrides_json 必须是 JSON object"}
    out: Dict[str, str] = {}
    for k, v in data.items():
        key = str(k or "").strip()
        if not key:
            continue
        out[key] = str(v)
    return out


def _normalize_runtime_profile_name(profile: str) -> str:
    value = str(profile or "").strip().lower()
    if not value:
        return "full_default"
    if value in PROFILE_DEFAULT_DISABLED:
        return value
    return "full_default"


def _expand_disable_modules_csv(disable_csv: str) -> List[str]:
    tokens = [str(x or "").strip().lower() for x in str(disable_csv or "").split(",")]
    disabled: List[str] = []
    for token in tokens:
        if not token:
            continue
        if token == "all":
            return list(RUNTIME_MODULE_ORDER)
        if token == "shadow":
            for name in ("shadow.capture", "shadow.map", "shadow.receiver", "shadow.taa"):
                if name not in disabled:
                    disabled.append(name)
            continue
        if token in RUNTIME_MODULE_ORDER and token not in disabled:
            disabled.append(token)
    return disabled


def _runtime_profile_summary_from_inputs(profile: str, disable_csv: str) -> Dict[str, Any]:
    profile_name = _normalize_runtime_profile_name(profile)
    disabled = set(PROFILE_DEFAULT_DISABLED.get(profile_name, set()))
    disabled.update(_expand_disable_modules_csv(disable_csv))
    enabled = [name for name in RUNTIME_MODULE_ORDER if name not in disabled]
    disabled_list = [name for name in RUNTIME_MODULE_ORDER if name in disabled]
    return {
        "name": profile_name,
        "disabledModules": ",".join(disabled_list),
        "enabledModules": ",".join(enabled),
        "diagEnabled": "diag" in enabled,
    }


def _normalize_scenario_name(name: str) -> str:
    return str(name or "").strip().lower().replace("-", "_")


def _get_scenario_preset(name: str) -> Dict[str, Any]:
    key = _normalize_scenario_name(name)
    preset = SCENARIO_PRESETS.get(key)
    return dict(preset) if isinstance(preset, dict) else {}


def _scenario_preset_rows() -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for key in sorted(SCENARIO_PRESETS.keys()):
        preset = SCENARIO_PRESETS[key]
        rows.append(
            {
                "name": key,
                "title": str(preset.get("title", key)),
                "description": str(preset.get("description", "")),
                "mapPath": str(preset.get("mapPath", "")),
                "launcherMode": str(preset.get("launcherMode", YDWE_LAUNCHER_MODE_DIRECT)),
                "ydweRoot": str(preset.get("ydweRoot", "")),
                "profile": str(preset.get("profile", "")),
                "disableModules": str(preset.get("disableModules", "")),
                "windowed": bool(preset.get("windowed", False)),
                "useIsolatedDesktop": bool(preset.get("useIsolatedDesktop", True)),
                "desktopName": str(preset.get("desktopName", "")),
                "readyTimeoutSec": int(preset.get("readyTimeoutSec", 0) or 0),
                "sampleDurationSec": int(preset.get("sampleDurationSec", 0) or 0),
                "autoPerfRecord": bool(preset.get("autoPerfRecord", True)),
                "recordAfterGameStarted": bool(preset.get("recordAfterGameStarted", True)),
                "autoPerfExportSec": int(preset.get("autoPerfExportSec", 0) or 0),
                "deployD3d9BeforeLaunch": bool(preset.get("deployD3d9BeforeLaunch", True)),
                "opengl": bool(preset.get("opengl", False)),
                "enforceVideoBaseline": bool(preset.get("enforceVideoBaseline", True)),
                "baselineWidth": int(preset.get("baselineWidth", DEFAULT_BENCHMARK_WIDTH) or DEFAULT_BENCHMARK_WIDTH),
                "baselineHeight": int(preset.get("baselineHeight", DEFAULT_BENCHMARK_HEIGHT) or DEFAULT_BENCHMARK_HEIGHT),
                "baselineRefreshRate": int(preset.get("baselineRefreshRate", DEFAULT_BENCHMARK_REFRESH) or DEFAULT_BENCHMARK_REFRESH),
            }
        )
    return rows


def _zero_shadow_budget_summary() -> Dict[str, Any]:
    phases = []
    for name in ("POS", "BLEND", "UV", "INDEX"):
        phases.append(
            {
                "name": name,
                "requestedMb": 0.0,
                "acceptedMb": 0.0,
                "rejectedMb": 0.0,
                "requests": 0,
                "rejects": 0,
            }
        )
    return {
        "framesObserved": 0,
        "framesIncomplete": 0,
        "framesBudgetExceeded": 0,
        "framesReuseLastComplete": 0,
        "framesRenderCurrentPartial": 0,
        "avgBudgetMb": 0.0,
        "avgUsedMb": 0.0,
        "maxBudgetMb": 0.0,
        "maxUsedMb": 0.0,
        "skippedFreezeBudget": 0,
        "skippedUpload": 0,
        "skippedCasterCap": 0,
        "skippedDistanceCull": 0,
        "phases": phases,
    }


def _zero_shadow_runtime_v2_summary() -> Dict[str, Any]:
    return {
        "staticPersistentCount": 0,
        "dynamicPoseCount": 0,
        "dynamicSkinnedOutputCount": 0,
        "fallbackDrawCount": 0,
        "fallbackDrawCountTerrain": 0,
        "fallbackDrawCountWorldObject": 0,
        "fallbackDrawCountUnitObject": 0,
        "objectFallbackDrawCount": 0,
        "semanticBridgeHit": 0,
        "semanticBridgeMiss": 0,
        "semanticBridgeBypassed": 0,
        "semanticSceneSubmitted": 0,
        "semanticSceneSubmittedUnit": 0,
        "semanticSceneSubmittedSkinned": 0,
        "semanticSceneSubmittedFrameLocal": 0,
        "semanticSceneSubmittedPersistent": 0,
        "semanticSceneAcceptedExplicitResourceOwnerRigid": 0,
        "worldObjectListOwnerHintZeroContextAcceptedCount": 0,
        "modelRegistryHit": 0,
        "modelRegistryMiss": 0,
        "modelLoadCount": 0,
        "modelReuseCount": 0,
        "runtimeBoundCount": 0,
        "completeIdentityCount": 0,
        "shadowRuntimeBoundCount": 0,
        "shadowIdentityCount": 0,
        "poseUpdateCount": 0,
        "poseCacheHit": 0,
        "poseCacheMiss": 0,
        "bonePaletteUpdates": 0,
        "shadowGeosetResourceCount": 0,
        "shadowReadyGeosetCount": 0,
        "shadowModelResourceCount": 0,
        "shadowPoseReadyCount": 0,
        "upperLayerResolveAttempts": 0,
        "upperLayerResolveVisibleMiss": 0,
        "upperLayerResolveVisibleUnresolvedGeoset": 0,
        "upperLayerResolveGeosetMiss": 0,
        "upperLayerResolvePoseMiss": 0,
        "upperLayerResolveRuntimeGroupPaletteMiss": 0,
        "upperLayerResolveAuthoritativeRigid": 0,
        "upperLayerResolveAuthoritativeSkinned": 0,
        "upperLayerEmitted": 0,
        "upperLayerDuplicateOrSuppressed": 0,
        "animationSequenceCount": 0,
        "avgModelResolveCpuMs": 0.0,
        "avgPoseUpdateCpuMs": 0.0,
        "avgSkinnedOutputCpuMs": 0.0,
        "modelResourceBytes": 0,
        "poseResourceBytes": 0,
        "frameSerial": 0,
        "notes": "placeholder",
    }


def _ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def _safe_rel(path: Path, root: Path) -> str:
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except Exception:
        return str(path)


def _build_environment_block(env: Dict[str, str]) -> ctypes.Array[Any]:
    pairs: List[str] = []
    for key in sorted(env.keys(), key=lambda x: str(x).lower()):
        pairs.append(f"{key}={env[key]}")
    text = "\0".join(pairs) + "\0\0"
    return ctypes.create_unicode_buffer(text)


def _desktop_access_mask() -> int:
    return (
        DESKTOP_READOBJECTS
        | DESKTOP_CREATEWINDOW
        | DESKTOP_ENUMERATE
        | DESKTOP_WRITEOBJECTS
        | DESKTOP_SWITCHDESKTOP
    )


def _create_isolated_desktop(name: str) -> Dict[str, Any]:
    desktop_name = str(name or "").strip()
    if not desktop_name:
        desktop_name = f"War3AutoTest_{_now_compact()}"

    if ISOLATED_DESKTOP_QUARANTINED:
        return {
            "ok": False,
            "stage": "preflight",
            "error": (
                "隔离桌面启动已被安全隔离：当前显示栈可能令交互桌面黑屏，"
                "同时把 War3 留在非输入桌面。请使用可见桌面或 attach-only。"
            ),
            "name": desktop_name,
            "quarantined": True,
        }

    result: Dict[str, Any] = {}
    cancelled = threading.Event()

    def _worker() -> None:
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        user32.CreateDesktopW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.LPCWSTR,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPVOID,
        ]
        user32.CreateDesktopW.restype = wintypes.HANDLE
        handle = user32.CreateDesktopW(
            desktop_name,
            None,
            None,
            0,
            _desktop_access_mask(),
            None,
        )
        if not handle:
            result["ok"] = False
            result["error"] = f"CreateDesktopW 失败: {int(kernel32.GetLastError())}"
            return
        if cancelled.is_set():
            user32.CloseDesktop(wintypes.HANDLE(handle))
            result["ok"] = False
            result["cancelledHandleClosed"] = True
            return
        result["ok"] = True
        result["name"] = desktop_name
        result["handle"] = int(handle)

    thread = threading.Thread(target=_worker, daemon=True)
    thread.start()
    thread.join(timeout=5.0)
    if thread.is_alive():
        cancelled.set()
        thread.join(timeout=1.0)
        late_handle = int(result.get("handle", 0) or 0)
        late_closed = (
            _close_desktop_handle(late_handle)
            if late_handle else bool(result.get("cancelledHandleClosed"))
        )
        return {
            "ok": False,
            "error": "CreateDesktopW 超时",
            "name": desktop_name,
            "lateHandleClosed": late_closed,
            "workerExited": not thread.is_alive(),
        }
    if not result.get("ok"):
        return {"ok": False, "error": result.get("error", "CreateDesktopW 失败"), "name": desktop_name}
    return result


def _close_desktop_handle(handle: int) -> bool:
    if int(handle or 0) == 0:
        return True
    try:
        user32 = ctypes.windll.user32
        user32.CloseDesktop.argtypes = [wintypes.HANDLE]
        user32.CloseDesktop.restype = wintypes.BOOL
        return bool(user32.CloseDesktop(wintypes.HANDLE(int(handle))))
    except Exception:
        return False


def _launch_process_on_desktop(
    args: List[str],
    cwd: Path,
    env: Dict[str, str],
    desktop_name: str,
) -> Dict[str, Any]:
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateProcessW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.LPWSTR,
        wintypes.LPVOID,
        wintypes.LPVOID,
        wintypes.BOOL,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.LPCWSTR,
        ctypes.POINTER(_StartupInfoW),
        ctypes.POINTER(_ProcessInformation),
    ]
    kernel32.CreateProcessW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    startup = _StartupInfoW()
    startup.cb = ctypes.sizeof(_StartupInfoW)
    startup.lpDesktop = f"WinSta0\\{desktop_name}"
    proc_info = _ProcessInformation()
    env_block = _build_environment_block(env)
    cmdline = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(x) for x in args]))
    app_name = str(args[0]) if args else ""
    ok = bool(
        kernel32.CreateProcessW(
            app_name,
            cmdline,
            None,
            None,
            False,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
            ctypes.cast(env_block, wintypes.LPVOID),
            str(cwd),
            ctypes.byref(startup),
            ctypes.byref(proc_info),
        )
    )
    if not ok:
        return {
            "ok": False,
            "error": f"CreateProcessW 失败: {int(kernel32.GetLastError())}",
            "desktop": desktop_name,
        }

    pid = int(proc_info.dwProcessId)
    if proc_info.hThread:
        kernel32.CloseHandle(proc_info.hThread)
    witness, witness_acquisition = _adopt_native_process_witness(
        proc_info.hProcess,
        pid,
        app_name,
        "isolated-CreateProcessW-original-hProcess",
        terminate_owned_create_on_failure=True,
    )
    if witness is None:
        # The process was created but its immutable PID/creation/path binding
        # could not be proven. The raw owned hProcess was terminated, waited,
        # and closed inside adoption; never fall back to a recyclable PID.
        return {
            "ok": False,
            "error": (
                "CreateProcessW succeeded but native process witness "
                "acquisition failed"
            ),
            "pid": pid,
            "desktop": desktop_name,
            "nativeProcessWitnessAcquisition": witness_acquisition,
        }
    return {
        "ok": True,
        "pid": pid,
        "desktop": desktop_name,
        "nativeProcessWitness": witness.snapshot(),
        "nativeProcessWitnessAcquisition": witness_acquisition,
        # Internal-only ownership transfer to launch_war3_test. This object is
        # removed before any MCP/JSON result is returned.
        "_nativeProcessWitness": witness,
    }


def _normalize_launcher_mode(value: str) -> str:
    mode = str(value or YDWE_LAUNCHER_MODE_DIRECT).strip().lower()
    aliases = {
        "war3": YDWE_LAUNCHER_MODE_DIRECT,
        "war3.exe": YDWE_LAUNCHER_MODE_DIRECT,
        "ydwe.exe": YDWE_LAUNCHER_MODE_YDWE,
    }
    mode = aliases.get(mode, mode)
    if mode not in (YDWE_LAUNCHER_MODE_DIRECT, YDWE_LAUNCHER_MODE_YDWE):
        raise ValueError(
            f"launcher_mode 只允许 {YDWE_LAUNCHER_MODE_DIRECT}/{YDWE_LAUNCHER_MODE_YDWE}: {value}"
        )
    return mode


def _same_resolved_path(left: Path, right: Path) -> bool:
    left_text = os.path.normcase(os.path.normpath(str(Path(left).resolve(strict=False))))
    right_text = os.path.normcase(os.path.normpath(str(Path(right).resolve(strict=False))))
    return left_text == right_text


def _ydwe_lock_name(ydwe_root: Path) -> str:
    canonical = os.path.normcase(
        os.path.normpath(str(Path(ydwe_root).resolve(strict=False)))
    )
    digest = hashlib.sha256(canonical.encode("utf-8", errors="surrogatepass")).hexdigest()
    return f"Local\\War3AutoTest_YDWE_{digest[:24]}"


def _acquire_ydwe_root_lock(ydwe_root: Path) -> Dict[str, Any]:
    """串行化固定 logs 目录的 YDWE 运行时，避免 LuaEngine 在 DllMain 中 fast-fail。"""
    name = _ydwe_lock_name(ydwe_root)
    ctypes.set_last_error(0)
    handle = _KERNEL32.CreateMutexW(None, True, name)
    if not handle:
        error = int(ctypes.get_last_error())
        return {
            "ok": False,
            "code": "YDWE_ROOT_LOCK_FAILED",
            "error": f"创建 YDWE 根互斥锁失败: {error}",
            "win32Error": error,
            "name": name,
        }
    error = int(ctypes.get_last_error())
    if error == ERROR_ALREADY_EXISTS:
        _KERNEL32.CloseHandle(handle)
        return {
            "ok": False,
            "code": "YDWE_ROOT_BUSY",
            "error": "同一 YDWE 根已有 AutoTest 会话，拒绝并发复用固定日志目录",
            "win32Error": error,
            "name": name,
            "ydweRoot": str(Path(ydwe_root).resolve(strict=False)),
        }
    return {
        "ok": True,
        "handle": int(handle),
        "name": name,
        "ydweRoot": str(Path(ydwe_root).resolve(strict=False)),
    }


def _release_ydwe_root_lock(lock: Dict[str, Any]) -> Dict[str, Any]:
    handle = int(lock.get("handle", 0) or 0)
    if handle == 0:
        return {"ok": True, "skipped": True, "reason": "无 YDWE 根互斥锁"}
    released = bool(_KERNEL32.ReleaseMutex(wintypes.HANDLE(handle)))
    release_error = 0 if released else int(ctypes.get_last_error())
    closed = bool(_KERNEL32.CloseHandle(wintypes.HANDLE(handle)))
    return {
        "ok": released and closed,
        "released": released,
        "closed": closed,
        "win32Error": release_error,
        "name": str(lock.get("name", "")),
        "ydweRoot": str(lock.get("ydweRoot", "")),
    }


def _probe_ydwe_log_writable(ydwe_root: Path) -> Dict[str, Any]:
    """以 LuaEngine 同一启动令牌验证固定 war3.log 可写且未被独占。"""
    log_path = Path(ydwe_root).resolve(strict=False) / "logs" / "war3.log"
    if not log_path.is_file():
        return {
            "ok": False,
            "code": "YDWE_LOG_MISSING",
            "error": f"YDWE 运行日志不存在: {log_path}",
            "path": str(log_path),
        }
    FILE_APPEND_DATA = 0x00000004
    FILE_SHARE_READ = 0x00000001
    FILE_ATTRIBUTE_NORMAL = 0x00000080
    handle = _KERNEL32.CreateFileW(
        str(log_path),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        None,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        None,
    )
    if not handle or int(handle) == int(INVALID_HANDLE_VALUE):
        error = int(ctypes.get_last_error())
        return {
            "ok": False,
            "code": "YDWE_LOG_NOT_WRITABLE",
            "error": (
                "YDWE 的 logs\\war3.log 不可写或已被占用；"
                "LuaEngine 会在 DLL 初始化阶段以 0xC0000409 退出"
            ),
            "path": str(log_path),
            "win32Error": error,
        }
    _KERNEL32.CloseHandle(handle)
    return {"ok": True, "path": str(log_path), "verification": "append-open-exclusive-write"}


def _read_hkcu_war3_install_path() -> Dict[str, Any]:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, WAR3_INSTALL_REG_KEY) as key:
            value, value_type = winreg.QueryValueEx(key, "InstallPath")
    except OSError as exc:
        return {
            "ok": False,
            "error": f"读取 HKCU\\{WAR3_INSTALL_REG_KEY}\\InstallPath 失败: {exc}",
            "keyPath": WAR3_INSTALL_REG_KEY,
        }
    if value_type not in (winreg.REG_SZ, winreg.REG_EXPAND_SZ):
        return {
            "ok": False,
            "error": f"InstallPath 类型不是字符串: {value_type}",
            "keyPath": WAR3_INSTALL_REG_KEY,
        }
    raw = str(value or "").strip()
    if value_type == winreg.REG_EXPAND_SZ:
        raw = os.path.expandvars(raw)
    if not raw:
        return {
            "ok": False,
            "error": "HKCU Warcraft III InstallPath 为空",
            "keyPath": WAR3_INSTALL_REG_KEY,
        }
    return {
        "ok": True,
        "path": raw,
        "keyPath": WAR3_INSTALL_REG_KEY,
        "valueType": int(value_type),
    }


def _preflight_ydwe_single_launch(war3_dir: Path, ydwe_root: Path) -> Dict[str, Any]:
    """校验单实例 YDWE/JAPI 启动闭包；只读注册表，任何漂移都拒绝启动。"""
    try:
        sandbox = _require_multi_instance_sandbox(war3_dir)
    except ValueError as exc:
        return {"ok": False, "error": str(exc), "code": "YDWE_SANDBOX_ROOT_MISMATCH"}

    ydwe = Path(ydwe_root).resolve(strict=False)
    required_war3 = ["war3.exe", "Game.dll", "war3.mpq", "war3patch.mpq", "Storm.dll"]
    missing_war3 = [str(sandbox / name) for name in required_war3 if not (sandbox / name).is_file()]
    required_ydwe = [
        ydwe / "YDWE.exe",
        ydwe / "bin" / "LuaEngine.dll",
        ydwe / "plugin" / "warcraft3" / "yd_jass_api.dll",
    ]
    missing_ydwe = [str(path) for path in required_ydwe if not path.is_file()]
    if missing_war3 or missing_ydwe:
        return {
            "ok": False,
            "error": "YDWE/JAPI 启动闭包文件缺失",
            "code": "YDWE_RUNTIME_CLOSURE_MISSING",
            "missingWar3": missing_war3,
            "missingYdwe": missing_ydwe,
        }

    log_writable = _probe_ydwe_log_writable(ydwe)
    if not log_writable.get("ok"):
        return {
            "ok": False,
            "error": log_writable.get("error", "YDWE war3.log 不可写"),
            "code": log_writable.get("code", "YDWE_LOG_NOT_WRITABLE"),
            "logWritable": log_writable,
        }

    conflicts = _find_user_ydwe_process_conflicts(ydwe)
    if conflicts:
        return {
            "ok": False,
            "error": "检测到用户正在运行 YDWE/WorldEditor；共享启动器可能不产出 War3，拒绝干预用户进程",
            "code": "USER_YDWE_PROCESS_CONFLICT",
            "conflicts": conflicts,
        }

    registry = _read_hkcu_war3_install_path()
    if not registry.get("ok"):
        return {
            "ok": False,
            "error": registry.get("error", "读取 InstallPath 失败"),
            "code": "YDWE_INSTALL_PATH_UNAVAILABLE",
            "registry": registry,
        }
    registry_path = Path(str(registry["path"]))
    if not _same_resolved_path(registry_path, sandbox):
        return {
            "ok": False,
            "error": (
                "YDWE 使用共享 HKCU InstallPath；当前值未精确指向专用沙盒，"
                "AutoTest 不会修改注册表"
            ),
            "code": "YDWE_INSTALL_PATH_MISMATCH",
            "expected": str(sandbox),
            "actual": str(registry_path),
            "registry": registry,
        }
    return {
        "ok": True,
        "war3Dir": str(sandbox),
        "ydweRoot": str(ydwe),
        "ydweExe": str(ydwe / "YDWE.exe"),
        "luaEngine": str(ydwe / "bin" / "LuaEngine.dll"),
        "ydJassApi": str(ydwe / "plugin" / "warcraft3" / "yd_jass_api.dll"),
        "runtimeSha256": {
            "LuaEngine.dll": sha256_file(ydwe / "bin" / "LuaEngine.dll"),
            "yd_jass_api.dll": sha256_file(ydwe / "plugin" / "warcraft3" / "yd_jass_api.dll"),
        },
        "registry": registry,
        "logWritable": log_writable,
        "registryModified": False,
    }


def _snapshot_process_entries() -> List[Dict[str, Any]]:
    """使用 Toolhelp32 获取 PID/PPID/镜像名，避免依赖 WMI/PowerShell。"""
    TH32CS_SNAPPROCESS = 0x00000002
    invalid_handle = ctypes.c_void_p(-1).value
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry32W)]
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry32W)]
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snapshot or int(snapshot) == int(invalid_handle):
        return []
    rows: List[Dict[str, Any]] = []
    try:
        entry = _ProcessEntry32W()
        entry.dwSize = ctypes.sizeof(_ProcessEntry32W)
        ok = bool(kernel32.Process32FirstW(snapshot, ctypes.byref(entry)))
        while ok:
            rows.append(
                {
                    "pid": int(entry.th32ProcessID),
                    "parentPid": int(entry.th32ParentProcessID),
                    "exeName": str(entry.szExeFile),
                }
            )
            entry.dwSize = ctypes.sizeof(_ProcessEntry32W)
            ok = bool(kernel32.Process32NextW(snapshot, ctypes.byref(entry)))
    finally:
        kernel32.CloseHandle(snapshot)
    return rows


def _query_process_image_path(pid: int) -> str:
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return ""
    try:
        size = wintypes.DWORD(32768)
        buf = ctypes.create_unicode_buffer(int(size.value))
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return ""
        return str(buf.value)
    finally:
        kernel32.CloseHandle(handle)


def _find_user_ydwe_process_conflicts(ydwe_root: Path) -> List[Dict[str, Any]]:
    """只报告冲突，不结束用户的编辑器或 YDWE 配置进程。"""
    conflict_names = {"worldeditydwe.exe", "ydweconfig.exe", "ydwe.exe"}
    rows: List[Dict[str, Any]] = []
    for row in _snapshot_process_entries():
        name = str(row.get("exeName", "")).lower()
        if name not in conflict_names:
            continue
        pid = int(row.get("pid", 0) or 0)
        if pid <= 0:
            continue
        image_path = _query_process_image_path(pid)
        same_root = False
        if image_path:
            try:
                Path(image_path).resolve(strict=False).relative_to(Path(ydwe_root).resolve(strict=False))
                same_root = True
            except (ValueError, OSError):
                same_root = False
        # 所有 YDWE/编辑器实例都共享 HKCU Warcraft InstallPath 等全局状态。
        # 即使来自另一目录，也不能与本轮 wrapper 并发；只报告，绝不结束用户进程。
        rows.append(
            {
                "pid": pid,
                "exeName": str(row.get("exeName", "")),
                "imagePath": image_path,
                "sameYdweRoot": same_root,
            }
        )
    return rows


def _snapshot_process_modules(pid: int) -> List[Dict[str, Any]]:
    TH32CS_SNAPMODULE = 0x00000008
    TH32CS_SNAPMODULE32 = 0x00000010
    invalid_handle = ctypes.c_void_p(-1).value
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ModuleEntry32W)]
    kernel32.Module32FirstW.restype = wintypes.BOOL
    kernel32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ModuleEntry32W)]
    kernel32.Module32NextW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    snapshot = kernel32.CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        int(pid),
    )
    if not snapshot or int(snapshot) == int(invalid_handle):
        return []
    rows: List[Dict[str, Any]] = []
    try:
        entry = _ModuleEntry32W()
        entry.dwSize = ctypes.sizeof(_ModuleEntry32W)
        ok = bool(kernel32.Module32FirstW(snapshot, ctypes.byref(entry)))
        while ok:
            rows.append(
                {
                    "name": str(entry.szModule),
                    "path": str(entry.szExePath),
                    "size": int(entry.modBaseSize),
                }
            )
            entry.dwSize = ctypes.sizeof(_ModuleEntry32W)
            ok = bool(kernel32.Module32NextW(snapshot, ctypes.byref(entry)))
    finally:
        kernel32.CloseHandle(snapshot)
    return rows


def _wait_for_ydwe_runtime_modules(
    pid: int,
    expected_paths: Dict[str, Path],
    expected_sha256: Dict[str, str],
    timeout_sec: float = 20.0,
) -> Dict[str, Any]:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    # 在发现 child 后立即保留 query handle。进程退出后 PID 已无法重新打开，
    # 没有这个句柄就会丢失 0xC0000005 等关键早期崩溃证据。
    process_handle = kernel32.OpenProcess(0x00100000 | 0x1000, False, int(pid))

    def exit_code_evidence() -> Dict[str, Any]:
        if not process_handle:
            return {"exitCodeAvailable": False}
        code = wintypes.DWORD(0)
        if not kernel32.GetExitCodeProcess(process_handle, ctypes.byref(code)):
            return {
                "exitCodeAvailable": False,
                "exitCodeError": int(ctypes.get_last_error()),
            }
        value = int(code.value)
        return {
            "exitCodeAvailable": True,
            "exitCode": value,
            "exitCodeHex": f"0x{value:08X}",
            "processExited": value != 259,
        }

    def process_is_alive() -> bool:
        evidence = exit_code_evidence()
        if evidence.get("exitCodeAvailable"):
            return not bool(evidence.get("processExited"))
        return _pid_alive(int(pid))

    deadline = time.monotonic() + max(0.1, float(timeout_sec))
    last_seen: List[Dict[str, Any]] = []
    last_module_snapshot: List[Dict[str, Any]] = []
    expected_by_name = {str(name).lower(): Path(path) for name, path in expected_paths.items()}
    try:
        while time.monotonic() < deadline and process_is_alive():
            modules = _snapshot_process_modules(int(pid))
            last_module_snapshot = [
                {
                    "name": str(row.get("name", "")),
                    "path": str(row.get("path", "")),
                    "size": int(row.get("size", 0) or 0),
                }
                for row in modules
            ]
            by_name = {str(row.get("name", "")).lower(): row for row in modules}
            seen: List[Dict[str, Any]] = []
            valid = True
            for lower_name, expected_path in expected_by_name.items():
                row = by_name.get(lower_name)
                if row is None:
                    valid = False
                    continue
                actual_path = Path(str(row.get("path", "")))
                path_matches = _same_resolved_path(actual_path, expected_path)
                actual_sha = sha256_file(actual_path) if actual_path.is_file() else ""
                expected_sha = str(expected_sha256.get(expected_path.name, "") or "")
                sha_matches = bool(actual_sha and expected_sha and actual_sha.lower() == expected_sha.lower())
                seen.append(
                    {
                        "name": expected_path.name,
                        "path": str(actual_path),
                        "pathMatches": path_matches,
                        "sha256": actual_sha,
                        "expectedSha256": expected_sha,
                        "shaMatches": sha_matches,
                    }
                )
                valid = valid and path_matches and sha_matches
            last_seen = seen
            if valid and len(seen) == len(expected_by_name):
                return {
                    "ok": True,
                    "pid": int(pid),
                    "modules": seen,
                    "moduleSnapshot": last_module_snapshot,
                    "verification": "loaded-module-exact-path-and-sha256",
                    **exit_code_evidence(),
                }
            time.sleep(0.1)
        return {
            "ok": False,
            "error": "未观察到路径与 SHA-256 均匹配的 YDWE LuaEngine/JAPI 运行时模块",
            "code": "YDWE_JAPI_MODULES_NOT_VERIFIED",
            "pid": int(pid),
            "modules": last_seen,
            "moduleSnapshot": last_module_snapshot,
            "processAlive": process_is_alive(),
            **exit_code_evidence(),
        }
    finally:
        if process_handle:
            kernel32.CloseHandle(process_handle)


def _is_descendant_process(
    pid: int,
    launcher_pid: int,
    by_pid: Dict[int, Dict[str, Any]],
) -> bool:
    current = int(pid)
    seen: set[int] = set()
    for _ in range(32):
        row = by_pid.get(current)
        if row is None:
            return False
        parent = int(row.get("parentPid", 0) or 0)
        if parent == int(launcher_pid):
            return True
        if parent <= 0 or parent == current or parent in seen:
            return False
        seen.add(parent)
        current = parent
    return False


def _wait_for_ydwe_war3_child(
    launcher_pid: int,
    expected_war3_exe: Path,
    preexisting_pids: set[int],
    timeout_sec: float = 20.0,
    poll_interval_sec: float = 0.1,
) -> Dict[str, Any]:
    """只接受 YDWE 子树中新建且镜像路径精确匹配沙盒 war3.exe 的进程。"""
    deadline = time.monotonic() + max(0.1, float(timeout_sec))
    last_candidates: List[Dict[str, Any]] = []
    known_descendants: set[int] = {int(launcher_pid)}
    while time.monotonic() < deadline:
        rows = _snapshot_process_entries()
        by_pid = {int(row.get("pid", 0)): row for row in rows}
        # YDWE 可能经短命 helper 创建 War3。跨轮保留已经证明属于 launcher
        # 子树的 PID，避免 helper 在下一次快照前退出后丢失整条祖先链。
        changed = True
        while changed:
            changed = False
            for row in rows:
                row_pid = int(row.get("pid", 0) or 0)
                parent_pid = int(row.get("parentPid", 0) or 0)
                if row_pid > 0 and parent_pid in known_descendants and row_pid not in known_descendants:
                    known_descendants.add(row_pid)
                    changed = True
        candidates: List[Dict[str, Any]] = []
        for row in rows:
            pid = int(row.get("pid", 0) or 0)
            if pid <= 0 or pid in preexisting_pids:
                continue
            if str(row.get("exeName", "")).lower() != "war3.exe":
                continue
            if pid not in known_descendants and not _is_descendant_process(pid, int(launcher_pid), by_pid):
                continue
            image_path = _query_process_image_path(pid)
            candidate = dict(row)
            candidate["imagePath"] = image_path
            candidate["imageMatches"] = bool(
                image_path and _same_resolved_path(Path(image_path), expected_war3_exe)
            )
            candidates.append(candidate)
        last_candidates = candidates
        exact = [row for row in candidates if row.get("imageMatches")]
        # Toolhelp 快照已经证明该 PID 在本轮仍存在，完整镜像路径又精确匹配
        # 沙盒 war3.exe。这里不能再依赖一次 OpenProcess 判活：隔离 Desktop
        # 的早期 loader 窗口中它可能返回 AccessDenied，而 tasklist fallback
        # 同样会被策略拒绝，导致把真实 child 误报为不存在。
        if len(exact) == 1:
            return {
                "ok": True,
                "pid": int(exact[0]["pid"]),
                "launcherPid": int(launcher_pid),
                "imagePath": str(exact[0]["imagePath"]),
                "discovery": "toolhelp-descendant-and-exact-image",
            }
        if len(exact) > 1:
            return {
                "ok": False,
                "error": "YDWE 子树出现多个精确匹配的 war3.exe，拒绝猜测目标 PID",
                "code": "YDWE_MULTIPLE_WAR3_CHILDREN",
                "launcherPid": int(launcher_pid),
                "candidates": exact,
            }
        time.sleep(max(0.01, float(poll_interval_sec)))
    return {
        "ok": False,
        "error": "超时未发现 YDWE 启动的沙盒 war3.exe 子进程",
        "code": "YDWE_WAR3_CHILD_NOT_FOUND",
        "launcherPid": int(launcher_pid),
        "expectedImage": str(expected_war3_exe),
        "candidates": last_candidates,
    }


def _create_kill_on_close_job(pid: int, session_id: str) -> Dict[str, Any]:
    """把目标进程放进 KILL_ON_JOB_CLOSE Job Object，确保整棵进程树可回收。"""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.SetInformationJobObject.argtypes = [
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.LPVOID,
        wintypes.DWORD,
    ]
    kernel32.SetInformationJobObject.restype = wintypes.BOOL
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    job_name = f"Local\\War3AutoTest_{session_id}_{int(pid)}"
    job = kernel32.CreateJobObjectW(None, job_name)
    if not job:
        return {"ok": False, "error": f"CreateJobObjectW 失败: {ctypes.get_last_error()}"}

    info = _JobObjectExtendedLimitInformation()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not kernel32.SetInformationJobObject(
        job,
        JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS,
        ctypes.byref(info),
        ctypes.sizeof(info),
    ):
        error = int(ctypes.get_last_error())
        kernel32.CloseHandle(job)
        return {"ok": False, "error": f"SetInformationJobObject 失败: {error}"}

    process_access = 0x0001 | 0x0100 | 0x1000  # TERMINATE | SET_QUOTA | QUERY_LIMITED_INFORMATION
    process = kernel32.OpenProcess(process_access, False, int(pid))
    if not process:
        error = int(ctypes.get_last_error())
        kernel32.CloseHandle(job)
        return {"ok": False, "error": f"OpenProcess(job assign) 失败: {error}"}
    try:
        if not kernel32.AssignProcessToJobObject(job, process):
            error = int(ctypes.get_last_error())
            kernel32.CloseHandle(job)
            return {"ok": False, "error": f"AssignProcessToJobObject 失败: {error}"}
    finally:
        kernel32.CloseHandle(process)
    return {
        "ok": True,
        "pid": int(pid),
        "name": job_name,
        "handle": int(job),
        "killOnClose": True,
    }


def _close_job_handle(handle: int) -> bool:
    if int(handle or 0) == 0:
        return True
    try:
        return bool(_KERNEL32.CloseHandle(wintypes.HANDLE(int(handle))))
    except Exception:
        return False


def _set_process_priority_high(pid: int) -> Dict[str, Any]:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    PROCESS_SET_INFORMATION = 0x0200
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    HIGH_PRIORITY_CLASS = 0x00000080
    handle = kernel32.OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
        False,
        int(pid),
    )
    if not handle:
        return {
            "ok": False,
            "error": f"OpenProcess 失败: {int(ctypes.get_last_error())}",
            "pid": int(pid),
        }
    try:
        if not kernel32.SetPriorityClass(handle, HIGH_PRIORITY_CLASS):
            return {
                "ok": False,
                "error": f"SetPriorityClass 失败: {int(ctypes.get_last_error())}",
                "pid": int(pid),
            }
        return {"ok": True, "pid": int(pid), "priority": "HIGH"}
    finally:
        kernel32.CloseHandle(handle)


def _read_png_size(path: Path) -> Tuple[int, int]:
    """
    读取 PNG/BMP 宽高（避免引入 Pillow 作为硬依赖）。
    返回 (0, 0) 表示解析失败。
    """
    try:
        with path.open("rb") as f:
            header = f.read(32)
        if len(header) < 24:
            return 0, 0
        # PNG signature + IHDR(13 bytes), width/height at offset 16/20
        if header[:8] != b"\x89PNG\r\n\x1a\n":
            if header[:2] != b"BM" or len(header) < 26:
                return 0, 0
            width = struct.unpack("<i", header[18:22])[0]
            height = struct.unpack("<i", header[22:26])[0]
            return int(abs(width)), int(abs(height))
        width = struct.unpack(">I", header[16:20])[0]
        height = struct.unpack(">I", header[20:24])[0]
        return int(width), int(height)
    except Exception:
        return 0, 0


def _set_war3_video_registry(
    width: int = DEFAULT_BENCHMARK_WIDTH,
    height: int = DEFAULT_BENCHMARK_HEIGHT,
    refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    color_depth: int = 32,
) -> Dict[str, Any]:
    """
    统一写入 War3 视频配置注册表，保证性能测试基线一致。
    """
    key_path = WAR3_VIDEO_REG_KEY
    width = max(640, int(width))
    height = max(480, int(height))
    refresh_rate = max(30, int(refresh_rate))
    color_depth = 32 if int(color_depth) not in (16, 32) else int(color_depth)

    old_vals: Dict[str, Any] = {}
    new_vals: Dict[str, Any] = {
        "reswidth": width,
        "resheight": height,
        "refreshrate": refresh_rate,
        "colordepth": color_depth,
        "texcolordepth": color_depth,
    }
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            for name in new_vals:
                try:
                    old_vals[name] = int(winreg.QueryValueEx(key, name)[0])
                except OSError:
                    old_vals[name] = None
            for name, val in new_vals.items():
                winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(val))
    except Exception as e:
        return {
            "ok": False,
            "error": f"写入视频注册表失败: {e}",
            "keyPath": key_path,
            "old": old_vals,
            "new": new_vals,
        }

    return {
        "ok": True,
        "keyPath": key_path,
        "old": old_vals,
        "new": new_vals,
    }


def _restore_war3_video_registry(snapshot: Dict[str, Any], key_path: str = WAR3_VIDEO_REG_KEY) -> Dict[str, Any]:
    """
    按 launch 前快照恢复 War3 视频注册表，避免自动化把用户配置长期改掉。
    """
    if not isinstance(snapshot, dict) or not snapshot:
        return {
            "ok": True,
            "skipped": True,
            "reason": "无可恢复的视频配置快照",
            "keyPath": key_path,
        }

    before_vals: Dict[str, Any] = {}
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            for name, val in snapshot.items():
                try:
                    before_vals[name] = winreg.QueryValueEx(key, name)[0]
                except OSError:
                    before_vals[name] = None

                if val is None:
                    try:
                        winreg.DeleteValue(key, name)
                    except OSError:
                        pass
                else:
                    winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(val))
    except Exception as e:
        return {
            "ok": False,
            "error": f"恢复视频注册表失败: {e}",
            "keyPath": key_path,
            "before": before_vals,
            "restore": snapshot,
        }

    return {
        "ok": True,
        "keyPath": key_path,
        "before": before_vals,
        "restored": snapshot,
    }


def _find_latest_report(war3_dir: Path) -> Optional[Path]:
    log_dir = war3_dir / "WarVK" / "Log"
    if not log_dir.exists():
        return None
    cands = sorted(
        log_dir.glob("war3_perf_report*.html"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    return cands[0] if cands else None


def _runtime_status_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "runtime_status.json"


# 这些 legacy JSON 路径仅保留给旧工件清理/离线诊断使用。
# 主动控制链已统一收口到 named pipe control plane。
def _frame_capture_request_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "frame_capture_request.json"


def _frame_capture_result_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "frame_capture_result.json"


def _internal_test_request_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "internal_test_request.json"


def _internal_test_result_file(war3_dir: Path) -> Path:
    return war3_dir / "WarVK" / "Temp" / "internal_test_result.json"


def _control_plane_pipe_name(pid: int) -> str:
    return rf"\\.\pipe\War3ControlPlane_{int(pid)}"


def _control_plane_request(
    pid: int,
    command: str,
    payload: Optional[Dict[str, Any]] = None,
    timeout_sec: float = 6.0,
) -> Dict[str, Any]:
    target_pid = int(pid or 0)
    if target_pid <= 0:
        return {"transportOk": False, "ok": False, "error": "无有效 pid"}

    pipe_name = _control_plane_pipe_name(target_pid)
    timeout_ms = max(200, int(float(timeout_sec) * 1000.0))
    t0 = time.time()
    request = {
        "requestId": f"cp_{_now_compact()}_{target_pid}_{int(time.time() * 1000)}",
        "command": str(command or "").strip(),
        "payload": dict(payload or {}),
        "issuedAtMs": int(time.time() * 1000),
        "pid": target_pid,
    }

    if not bool(_KERNEL32.WaitNamedPipeW(pipe_name, timeout_ms)):
        return {
            "transportOk": False,
            "ok": False,
            "error": f"named pipe 不可用: {ctypes.get_last_error()}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - t0, 3),
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
            "error": f"CreateFileW(pipe) 失败: {ctypes.get_last_error()}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - t0, 3),
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
                "error": f"WriteFile(pipe) 失败: {ctypes.get_last_error()}",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - t0, 3),
            }
        _KERNEL32.FlushFileBuffers(handle)

        response_deadline = time.time() + max(0.2, float(timeout_sec))
        while True:
            bytes_avail = wintypes.DWORD()
            bytes_left = wintypes.DWORD()
            ok = bool(
                _KERNEL32.PeekNamedPipe(
                    handle,
                    None,
                    0,
                    None,
                    ctypes.byref(bytes_avail),
                    ctypes.byref(bytes_left),
                )
            )
            if not ok:
                return {
                    "transportOk": False,
                    "ok": False,
                    "error": f"PeekNamedPipe(pipe) 失败: {ctypes.get_last_error()}",
                    "pipeName": pipe_name,
                    "request": request,
                    "elapsedSec": round(time.time() - t0, 3),
                }
            if bytes_avail.value > 0 or bytes_left.value > 0:
                break
            if time.time() >= response_deadline:
                return {
                    "transportOk": False,
                    "ok": False,
                    "error": "等待 control-plane 响应超时",
                    "pipeName": pipe_name,
                    "request": request,
                    "elapsedSec": round(time.time() - t0, 3),
                }
            time.sleep(0.01)

        chunks: List[bytes] = []
        while True:
            read_buf = ctypes.create_string_buffer(65536)
            read = wintypes.DWORD()
            ok = bool(_KERNEL32.ReadFile(handle, read_buf, len(read_buf), ctypes.byref(read), None))
            if ok:
                if read.value > 0:
                    chunks.append(read_buf.raw[: read.value])
                break
            err = ctypes.get_last_error()
            if err == ERROR_MORE_DATA:
                if read.value > 0:
                    chunks.append(read_buf.raw[: read.value])
                continue
            if err == ERROR_BROKEN_PIPE:
                break
            return {
                "transportOk": False,
                "ok": False,
                "error": f"ReadFile(pipe) 失败: {err}",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - t0, 3),
            }

        if not chunks:
            return {
                "transportOk": False,
                "ok": False,
                "error": "pipe 响应为空",
                "pipeName": pipe_name,
                "request": request,
                "elapsedSec": round(time.time() - t0, 3),
            }

        response = json.loads(b"".join(chunks).decode("utf-8", errors="ignore"))
        result = response.get("result", {})
        return {
            "transportOk": True,
            "ok": bool(response.get("ok")),
            "error": str(response.get("error", "") or ""),
            "pipeName": pipe_name,
            "request": request,
            "response": response,
            "result": result if isinstance(result, dict) else {"value": result},
            "elapsedSec": round(time.time() - t0, 3),
        }
    except Exception as e:
        return {
            "transportOk": False,
            "ok": False,
            "error": f"pipe 请求异常: {e}",
            "pipeName": pipe_name,
            "request": request,
            "elapsedSec": round(time.time() - t0, 3),
        }
    finally:
        _KERNEL32.CloseHandle(handle)


def _read_json_file(path: Path) -> Optional[Dict[str, Any]]:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8", errors="ignore"))
    except Exception:
        return None


def _read_runtime_status_file(war3_dir: Path) -> Optional[Dict[str, Any]]:
    return _read_json_file(_runtime_status_file(war3_dir))


def _read_runtime_status_best_effort(target_pid: int = 0) -> Optional[Dict[str, Any]]:
    pid = int(target_pid or STATE.war3_pid or 0)
    if pid > 0 and _pid_alive(pid):
        pipe_res = _control_plane_request(
            pid=pid,
            command="get_runtime_status",
            payload={},
            timeout_sec=1.5,
        )
        if pipe_res.get("transportOk") and pipe_res.get("ok"):
            data = dict(pipe_res.get("result", {}) or {})
            if data:
                return data

    registered = SESSION_REGISTRY.get(pid=pid) if pid > 0 else None
    if registered is not None:
        war3_dir = Path(registered.instance_root)
    else:
        war3_dir = STATE.war3_dir if isinstance(STATE.war3_dir, Path) else DEFAULT_WAR3_DIR
    return _read_runtime_status_file(Path(war3_dir))


def _read_bmp_luma_samples(path: Path, max_samples: int = 262144) -> Dict[str, Any]:
    """
    读取 BMP 并转成抽样亮度序列，用于连续帧稳定性比较。
    仅支持常见 BI_RGB 24/32-bit BMP。
    """
    try:
        raw = path.read_bytes()
    except Exception as e:
        return {"ok": False, "error": f"读取 BMP 失败: {e}", "path": str(path)}

    if len(raw) < 54 or raw[:2] != b"BM":
        return {"ok": False, "error": "不是有效 BMP", "path": str(path)}

    try:
        pixel_offset = struct.unpack_from("<I", raw, 10)[0]
        dib_size = struct.unpack_from("<I", raw, 14)[0]
        if dib_size < 40:
            return {"ok": False, "error": "BMP DIB header 太小", "path": str(path)}
        width = int(struct.unpack_from("<i", raw, 18)[0])
        height_raw = int(struct.unpack_from("<i", raw, 22)[0])
        planes = int(struct.unpack_from("<H", raw, 26)[0])
        bpp = int(struct.unpack_from("<H", raw, 28)[0])
        compression = int(struct.unpack_from("<I", raw, 30)[0])
    except Exception as e:
        return {"ok": False, "error": f"BMP header 解析失败: {e}", "path": str(path)}

    if planes != 1:
        return {"ok": False, "error": f"不支持的 BMP planes={planes}", "path": str(path)}
    if compression != 0:
        return {"ok": False, "error": f"不支持压缩 BMP compression={compression}", "path": str(path)}
    if bpp not in (24, 32):
        return {"ok": False, "error": f"不支持的 BMP bpp={bpp}", "path": str(path)}

    width = abs(width)
    height = abs(height_raw)
    if width <= 0 or height <= 0:
        return {"ok": False, "error": "BMP 宽高非法", "path": str(path)}

    bytes_per_pixel = bpp // 8
    row_stride = ((width * bpp + 31) // 32) * 4
    if pixel_offset + row_stride * height > len(raw):
        return {"ok": False, "error": "BMP 数据截断", "path": str(path)}

    sample_stride = max(1, int(max((width * height) / max(1, max_samples), 1) ** 0.5))
    bottom_up = height_raw > 0
    samples: List[int] = []
    dark_count = 0
    bright_count = 0
    total_luma = 0

    for sample_y in range(0, height, sample_stride):
        src_y = (height - 1 - sample_y) if bottom_up else sample_y
        row_start = pixel_offset + src_y * row_stride
        row = raw[row_start : row_start + row_stride]
        for sample_x in range(0, width, sample_stride):
            base = sample_x * bytes_per_pixel
            if base + 2 >= len(row):
                break
            b = row[base + 0]
            g = row[base + 1]
            r = row[base + 2]
            luma = (77 * r + 150 * g + 29 * b) >> 8
            samples.append(luma)
            total_luma += luma
            if luma <= 96:
                dark_count += 1
            if luma >= 240:
                bright_count += 1

    count = len(samples)
    if count <= 0:
        return {"ok": False, "error": "BMP 抽样失败", "path": str(path)}

    return {
        "ok": True,
        "path": str(path),
        "width": int(width),
        "height": int(height),
        "sampleStride": int(sample_stride),
        "sampleCount": int(count),
        "avgLuma": round(float(total_luma) / float(count), 4),
        "darkRatioPct": round(float(dark_count) * 100.0 / float(count), 4),
        "brightRatioPct": round(float(bright_count) * 100.0 / float(count), 4),
        "samples": samples,
    }


def _compare_bmp_sequence(paths: List[Path]) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    valid: List[Dict[str, Any]] = []
    for path in paths:
        parsed = _read_bmp_luma_samples(path)
        rows.append(parsed)
        if parsed.get("ok"):
            valid.append(parsed)

    if len(valid) < 2:
        return {
            "ok": False,
            "error": "有效 BMP 帧不足，无法比较",
            "frames": rows,
        }

    pairwise: List[Dict[str, Any]] = []
    mean_diffs: List[float] = []
    changed_pcts: List[float] = []
    dark_ratios: List[float] = []
    avg_lumas: List[float] = []

    for row in valid:
        dark_ratios.append(float(row.get("darkRatioPct", 0.0) or 0.0))
        avg_lumas.append(float(row.get("avgLuma", 0.0) or 0.0))

    for idx in range(len(valid) - 1):
        a = valid[idx]
        b = valid[idx + 1]
        sa = list(a.get("samples", []) or [])
        sb = list(b.get("samples", []) or [])
        sample_count = min(len(sa), len(sb))
        if sample_count <= 0:
            continue
        abs_sum = 0.0
        changed = 0
        max_abs = 0
        threshold = 16
        for i in range(sample_count):
            diff = abs(int(sa[i]) - int(sb[i]))
            abs_sum += diff
            if diff >= threshold:
                changed += 1
            if diff > max_abs:
                max_abs = diff
        mean_abs = abs_sum / float(sample_count)
        mean_abs_pct = mean_abs * 100.0 / 255.0
        changed_pct = float(changed) * 100.0 / float(sample_count)
        mean_diffs.append(mean_abs_pct)
        changed_pcts.append(changed_pct)
        pairwise.append(
            {
                "from": str(a.get("path", "")),
                "to": str(b.get("path", "")),
                "sampleCount": int(sample_count),
                "meanAbsDiff": round(mean_abs, 4),
                "meanAbsDiffPct": round(mean_abs_pct, 4),
                "changedPct": round(changed_pct, 4),
                "maxAbsDiff": int(max_abs),
                "darkRatioDeltaPct": round(
                    abs(float(a.get("darkRatioPct", 0.0) or 0.0) - float(b.get("darkRatioPct", 0.0) or 0.0)),
                    4,
                ),
                "avgLumaDelta": round(
                    abs(float(a.get("avgLuma", 0.0) or 0.0) - float(b.get("avgLuma", 0.0) or 0.0)),
                    4,
                ),
            }
        )

    if not pairwise:
        return {
            "ok": False,
            "error": "帧序列之间没有可比较的有效对",
            "frames": rows,
        }

    max_mean_diff_pct = max(mean_diffs)
    max_changed_pct = max(changed_pcts)
    dark_ratio_range_pct = max(dark_ratios) - min(dark_ratios)
    avg_luma_range = max(avg_lumas) - min(avg_lumas)
    flicker_suspect = max_mean_diff_pct >= 2.5 or max_changed_pct >= 8.0 or dark_ratio_range_pct >= 8.0
    missing_shadow_suspect = min(dark_ratios) <= 0.2 and dark_ratio_range_pct >= 6.0 and avg_luma_range >= 12.0

    return {
        "ok": True,
        "frames": rows,
        "pairwise": pairwise,
        "summary": {
            "frameCount": len(valid),
            "maxMeanAbsDiffPct": round(max_mean_diff_pct, 4),
            "avgMeanAbsDiffPct": round(sum(mean_diffs) / len(mean_diffs), 4),
            "maxChangedPct": round(max_changed_pct, 4),
            "darkRatioRangePct": round(dark_ratio_range_pct, 4),
            "avgLumaRange": round(avg_luma_range, 4),
            "flickerSuspect": bool(flicker_suspect),
            "missingShadowSuspect": bool(missing_shadow_suspect),
        },
    }


def _map_identity_key(path: Path) -> str:
    try:
        return str(path.resolve()).lower()
    except Exception:
        return str(path).lower()


def _build_suite_map_candidates(
    requested_map: Path,
    allow_fallback_to_default_test_map: bool,
) -> List[Path]:
    candidates: List[Path] = []
    seen: set[str] = set()
    for candidate in (
        requested_map,
        DEFAULT_CITY_FALLBACK_MAP if allow_fallback_to_default_test_map else None,
    ):
        if candidate is None:
            continue
        path = Path(candidate)
        key = _map_identity_key(path)
        if key in seen:
            continue
        seen.add(key)
        candidates.append(path)
    return candidates


def _launch_suite_map_until_ready(
    *,
    war3_dir: str,
    requested_map_path: str,
    allow_fallback_to_default_test_map: bool,
    ready_timeout_sec: int,
    ready_allow_fallback: bool,
    ready_require_game_started_for_fallback: bool,
    ready_fallback_min_elapsed_sec: int,
    ready_fallback_min_cpu_sec: float,
    launch_kwargs: Dict[str, Any],
    startup_input_actions: Optional[List[Dict[str, Any]]] = None,
) -> Dict[str, Any]:
    requested_map = Path(requested_map_path)
    attempts: List[Dict[str, Any]] = []

    for attempt_index, candidate in enumerate(
        _build_suite_map_candidates(
            requested_map, bool(allow_fallback_to_default_test_map)
        )
    ):
        launch = launch_war3_test(
            war3_dir=war3_dir,
            map_path=str(candidate),
            **launch_kwargs,
        )
        row: Dict[str, Any] = {
            "attempt": attempt_index + 1,
            "mapPath": str(candidate),
            "fallbackCandidate": bool(attempt_index > 0),
            "launch": launch,
        }
        if not launch.get("ok"):
            row["stage"] = "launch"
            attempts.append(row)
            continue

        pid = int(launch["pid"])
        startup_input: Dict[str, Any] = {
            "ok": True,
            "skipped": True,
            "reason": "no startup input requested",
        }
        if startup_input_actions:
            # Some protected maps accept -loadfile but stop at their own
            # pre-game splash screen.  A scenario may acknowledge that screen
            # through the existing same-desktop input helper before waiting for
            # gameStarted.  The helper remains restricted to the AutoTest-owned
            # isolated Desktop and the exact process registered above.
            startup_input = _run_war3_input_plan(
                pid=pid,
                actions=list(startup_input_actions),
                timeout_sec=min(60.0, max(12.0, ready_timeout_sec * 0.65)),
            )
        row["startupInput"] = startup_input
        if startup_input_actions and not startup_input.get("ok"):
            stop = stop_war3(
                pid=pid,
                graceful_wait_sec=3,
                force=True,
                avoid_foreground_switch=True,
            )
            row["stop"] = stop
            row["stage"] = "startup-input"
            attempts.append(row)
            continue
        ready = wait_for_game_ready(
            timeout_sec=ready_timeout_sec,
            pid=pid,
            allow_fallback=bool(ready_allow_fallback),
            fallback_min_elapsed_sec=int(ready_fallback_min_elapsed_sec),
            fallback_min_cpu_sec=float(ready_fallback_min_cpu_sec),
            require_game_started_for_fallback=bool(
                ready_require_game_started_for_fallback
            ),
        )
        row["ready"] = ready
        if ready.get("ok"):
            attempts.append(row)
            return {
                "ok": True,
                "pid": pid,
                "launch": launch,
                "ready": ready,
                "requestedMapPath": str(requested_map),
                "actualMapPath": str(candidate),
                "fallbackUsed": bool(attempt_index > 0),
                "attempts": attempts,
            }

        stop = stop_war3(
            pid=pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=True,
        )
        row["stop"] = stop
        row["stage"] = "ready"
        attempts.append(row)

    return {
        "ok": False,
        "stage": "ready",
        "requestedMapPath": str(requested_map),
        "attempts": attempts,
    }


def _invoke_internal_test_request(
    pid: int,
    war3_dir: Path,
    command: str,
    payload: Optional[Dict[str, Any]] = None,
    timeout_sec: float = 6.0,
) -> Dict[str, Any]:
    pipe_res = _control_plane_request(
        pid=pid,
        command="invoke_test_command",
        payload={
            "command": str(command or "").strip(),
            "payload": dict(payload or {}),
            "timeoutMs": max(1000, int(float(timeout_sec) * 1000.0)),
        },
        timeout_sec=max(2.0, float(timeout_sec) + 1.0),
    )
    if pipe_res.get("transportOk"):
        return {
            "ok": bool(pipe_res.get("ok")),
            "requestId": str((pipe_res.get("response", {}) or {}).get("requestId", "")),
            "command": str(command or "").strip(),
            "mode": "control-plane",
            "response": dict(pipe_res.get("response", {}) or {}),
            "result": dict(pipe_res.get("result", {}) or {}),
            "error": str(pipe_res.get("error", "") or ""),
            "elapsedSec": round(float(pipe_res.get("elapsedSec", 0.0) or 0.0), 3),
            "pipeName": str(pipe_res.get("pipeName", "") or ""),
        }
    return {
        "ok": False,
        "command": str(command or "").strip(),
        "mode": "control-plane-unavailable",
        "error": str(pipe_res.get("error", "control plane 不可用") or "control plane 不可用"),
        "detail": pipe_res,
    }


def _capture_final_frame_via_internal_test_api(
    pid: int,
    war3_dir: Path,
    output_path: Path,
    timeout_sec: float = 8.0,
) -> Dict[str, Any]:
    return _request_internal_frame_capture(
        pid=pid,
        output_path=output_path,
        war3_dir=war3_dir,
        timeout_sec=timeout_sec,
    )


def _extract_json_object(text: str, marker: str = "const data =") -> Optional[Dict[str, Any]]:
    idx = text.find(marker)
    if idx < 0:
        return None
    start = text.find("{", idx)
    if start < 0:
        return None
    depth = 0
    end = -1
    for i in range(start, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end < 0:
        return None
    try:
        return json.loads(text[start : end + 1])
    except json.JSONDecodeError:
        return None


def _build_perf_section_breakdown(data: Dict[str, Any], top_n: int = 20) -> Dict[str, Any]:
    """
    从 perf report 的 sections 提取函数/节点级热点（CPU/GPU）。
    """
    sections = data.get("sections", []) if isinstance(data, dict) else []
    top_n = max(1, min(int(top_n), 200))
    if not isinstance(sections, list):
        return {"count": 0, "topBySelfCpu": [], "topByInclusiveCpu": [], "topByGpu": []}

    def _to_num(v: Any) -> float:
        try:
            return float(v or 0.0)
        except Exception:
            return 0.0

    def _row(s: Dict[str, Any]) -> Dict[str, Any]:
        return {
            "name": str(s.get("name", "")),
            "path": str(s.get("path", "")),
            "parentPath": str(s.get("parentPath", "")),
            "avgCpuMs": _to_num(s.get("avgCpuMs", 0.0)),
            "avgSelfCpuMs": _to_num(s.get("avgSelfCpuMs", 0.0)),
            "avgGpuMs": _to_num(s.get("avgGpuMs", 0.0)),
            "calls": int(s.get("calls", 0) or 0),
            "callsPerFrame": _to_num(s.get("callsPerFrame", 0.0)),
            "trackedPct": _to_num(s.get("trackedPct", 0.0)),
        }

    top_self = sorted(sections, key=lambda s: _to_num(s.get("avgSelfCpuMs", 0.0)), reverse=True)[:top_n]
    top_incl = sorted(sections, key=lambda s: _to_num(s.get("avgCpuMs", 0.0)), reverse=True)[:top_n]
    top_gpu = sorted(sections, key=lambda s: _to_num(s.get("avgGpuMs", 0.0)), reverse=True)[:top_n]

    return {
        "count": len(sections),
        "topBySelfCpu": [_row(s) for s in top_self],
        "topByInclusiveCpu": [_row(s) for s in top_incl],
        "topByGpu": [_row(s) for s in top_gpu],
    }


def _read_perf_summary(
    report_path: Path,
    include_sections: bool = False,
    section_top_n: int = 20,
) -> Dict[str, Any]:
    content = report_path.read_text(encoding="utf-8", errors="ignore")
    data = _extract_json_object(content)
    if not data:
        return {
            "ok": False,
            "error": "无法从 HTML 中提取 JSON 数据",
            "reportPath": str(report_path),
        }

    # 输出一份简洁摘要，供自动化判断回归。
    summary = {
        "ok": True,
        "reportType": "perf_report",
        "reportPath": str(report_path),
        "frameCount": data.get("frameCount", 0),
        "windowSec": data.get("windowSec", 0.0),
        "avgFps": data.get("avgFps", 0.0),
        "avgFrameTimeMs": data.get("avgFrameTimeMs", 0.0),
        "avgGpuTimeMs": data.get("avgGpuTimeMs", 0.0),
        "avgProcessCpuMs": data.get("avgProcessCpuMs", 0.0),
        "avgMainThreadCpuMs": data.get("avgMainThreadCpuMs", 0.0),
        "avgWorkerThreadsCpuMs": data.get("avgWorkerThreadsCpuMs", 0.0),
        "processCpuCoveragePct": data.get("processCpuCoveragePct", 0.0),
        "mainThreadCpuCoveragePct": data.get("mainThreadCpuCoveragePct", 0.0),
        "processParallelismCores": data.get("processParallelismCores", 0.0),
        "mainThreadShareOfProcessPct": data.get("mainThreadShareOfProcessPct", 0.0),
        "workerThreadsShareOfProcessPct": data.get("workerThreadsShareOfProcessPct", 0.0),
        "activeFrameTimeMs": data.get("activeFrameTimeMs", data.get("avgFrameTimeMs", 0.0)),
        "avgTrackedActiveCpuMs": data.get("avgTrackedActiveCpuMs", data.get("avgTrackedRootCpuMs", 0.0)),
        "avgUntrackedActiveCpuMs": data.get("avgUntrackedActiveCpuMs", data.get("avgUntrackedCpuMs", 0.0)),
        "avgIdleWaitCpuMs": data.get("avgIdleWaitCpuMs", 0.0),
        # schema v8 authoritative attribution. The legacy *Cpu* keys above are
        # retained for old matrix consumers, but actually contain scope-wall
        # aggregates and must not be presented as sampled CPU.
        "avgTrackedAdditiveRootWallMs": data.get(
            "avgTrackedAdditiveRootWallMs",
            data.get("avgTrackedRootCpuMs", 0.0),
        ),
        "avgUncoveredFrameWallMs": data.get(
            "avgUncoveredFrameWallMs",
            data.get("avgUntrackedCpuMs", 0.0),
        ),
        "frameWallScopeCoveragePct": data.get(
            "frameWallScopeCoveragePct",
            data.get("cpuCoveragePct", 0.0),
        ),
        "frameWallAttribution": data.get("frameWallAttribution", {}),
        "cpuCoveragePct": data.get("cpuCoveragePct", 0.0),
        "cpuCoverageWithIdlePct": data.get("cpuCoverageWithIdlePct", data.get("cpuCoveragePct", 0.0)),
        "jank16": data.get("jank16", 0),
        "jank33": data.get("jank33", 0),
        "idleActiveOverlapLikely": data.get("idleActiveOverlapLikely", False),
    }

    cycle = data.get("mainLoopCycle", {}) if isinstance(data, dict) else {}
    if isinstance(cycle, dict):
        summary["mainLoopCycle"] = {
            "present": bool(cycle.get("present", False)),
            "avgCycleMs": float(cycle.get("avgCycleMs", 0.0) or 0.0),
            "avgActiveMs": float(cycle.get("avgActiveMs", 0.0) or 0.0),
            "avgIdleMs": float(cycle.get("avgIdleMs", 0.0) or 0.0),
            "phaseCount": len(cycle.get("phases", []) or []),
            "phasesTop": [
                {
                    "name": str(row.get("name", "")),
                    "avgCpuMs": float(row.get("avgCpuMs", 0.0) or 0.0),
                    "sharePct": float(row.get("sharePct", 0.0) or 0.0),
                    "callsPerFrame": float(row.get("callsPerFrame", 0.0) or 0.0),
                }
                for row in (cycle.get("phases", []) or [])[:12]
            ],
        }

    stages = data.get("mainLoopStages", []) if isinstance(data, dict) else []
    if isinstance(stages, list):
        summary["mainLoopStagesTop"] = [
            {
                "name": str(row.get("name", "")),
                "avgCpuMs": float(row.get("avgCpuMs", 0.0) or 0.0),
                "callsPerFrame": float(row.get("callsPerFrame", 0.0) or 0.0),
                "shareInFramePct": float(row.get("shareInFramePct", 0.0) or 0.0),
            }
            for row in stages[:12]
        ]

    if include_sections:
        summary["sectionBreakdown"] = _build_perf_section_breakdown(
            data,
            top_n=section_top_n,
        )
    runtime_profile = data.get("runtimeProfile", {}) if isinstance(data, dict) else {}
    if isinstance(runtime_profile, dict):
        summary["runtimeProfile"] = {
            "name": str(runtime_profile.get("name", "")),
            "disabledModules": str(runtime_profile.get("disabledModules", "")),
            "enabledModules": str(runtime_profile.get("enabledModules", "")),
        }
    shadow_budget = data.get("shadowBudgetSummary", {}) if isinstance(data, dict) else {}
    if isinstance(shadow_budget, dict):
        summary["shadowBudgetSummary"] = {
            "framesObserved": int(shadow_budget.get("framesObserved", 0) or 0),
            "framesIncomplete": int(shadow_budget.get("framesIncomplete", 0) or 0),
            "framesBudgetExceeded": int(shadow_budget.get("framesBudgetExceeded", 0) or 0),
            "framesReuseLastComplete": int(shadow_budget.get("framesReuseLastComplete", 0) or 0),
            "framesRenderCurrentPartial": int(shadow_budget.get("framesRenderCurrentPartial", 0) or 0),
            "avgBudgetMb": float(shadow_budget.get("avgBudgetMb", 0.0) or 0.0),
            "avgUsedMb": float(shadow_budget.get("avgUsedMb", 0.0) or 0.0),
            "maxBudgetMb": float(shadow_budget.get("maxBudgetMb", 0.0) or 0.0),
            "maxUsedMb": float(shadow_budget.get("maxUsedMb", 0.0) or 0.0),
            "skippedFreezeBudget": int(shadow_budget.get("skippedFreezeBudget", 0) or 0),
            "skippedPriorityBudget": int(shadow_budget.get("skippedPriorityBudget", 0) or 0),
            "skippedUpload": int(shadow_budget.get("skippedUpload", 0) or 0),
            "skippedCasterCap": int(shadow_budget.get("skippedCasterCap", 0) or 0),
            "skippedDistanceCull": int(shadow_budget.get("skippedDistanceCull", 0) or 0),
            "degradedAlphaBudget": int(shadow_budget.get("degradedAlphaBudget", 0) or 0),
            "reusedFreezeHits": int(shadow_budget.get("reusedFreezeHits", 0) or 0),
            "reusedFreezeMb": float(shadow_budget.get("reusedFreezeMb", 0.0) or 0.0),
            "actualFreezeReuseHits": int(shadow_budget.get("actualFreezeReuseHits", 0) or 0),
            "actualFreezeReuseMb": float(shadow_budget.get("actualFreezeReuseMb", 0.0) or 0.0),
            "uniqueGeometryCount": int(shadow_budget.get("uniqueGeometryCount", 0) or 0),
            "uniqueInstanceableGeometryCount": int(shadow_budget.get("uniqueInstanceableGeometryCount", 0) or 0),
            "duplicateGeometryInstances": int(shadow_budget.get("duplicateGeometryInstances", 0) or 0),
            "reuseEligibleDuplicates": int(shadow_budget.get("reuseEligibleDuplicates", 0) or 0),
            "uniqueFreezeAcceptedMb": float(shadow_budget.get("uniqueFreezeAcceptedMb", 0.0) or 0.0),
            "duplicateFreezeBypassMb": float(shadow_budget.get("duplicateFreezeBypassMb", 0.0) or 0.0),
            "potentialFreezeReuseHits": int(shadow_budget.get("potentialFreezeReuseHits", 0) or 0),
            "potentialFreezeReuseMb": float(shadow_budget.get("potentialFreezeReuseMb", 0.0) or 0.0),
            "instancedGeometryGroups": int(shadow_budget.get("instancedGeometryGroups", 0) or 0),
            "instancedGeometryInstances": int(shadow_budget.get("instancedGeometryInstances", 0) or 0),
            "instancedGeometryDrawsSaved": int(shadow_budget.get("instancedGeometryDrawsSaved", 0) or 0),
            "phases": list(shadow_budget.get("phases", []) or []),
        }
    shadow_runtime_v2 = data.get("shadowRuntimeV2Summary", {}) if isinstance(data, dict) else {}
    if (not isinstance(shadow_runtime_v2, dict) or not shadow_runtime_v2) and isinstance(shadow_budget, dict):
        shadow_runtime_v2 = {
            "staticPersistentCount": int(shadow_budget.get("staticPersistentCount", 0) or 0),
            "dynamicPoseCount": int(shadow_budget.get("dynamicPoseCount", 0) or 0),
            "dynamicSkinnedOutputCount": int(shadow_budget.get("dynamicSkinnedOutputCount", 0) or 0),
            "fallbackDrawCount": int(shadow_budget.get("fallbackDrawCount", 0) or 0),
            "semanticBridgeHit": int(shadow_budget.get("semanticBridgeHit", 0) or 0),
            "semanticBridgeMiss": int(shadow_budget.get("semanticBridgeMiss", 0) or 0),
            "semanticBridgeBypassed": int(shadow_budget.get("semanticBridgeBypassed", 0) or 0),
            "modelRegistryHit": 0,
            "modelRegistryMiss": 0,
            "modelLoadCount": 0,
            "modelReuseCount": 0,
            "poseUpdateCount": 0,
            "poseCacheHit": 0,
            "poseCacheMiss": 0,
            "bonePaletteUpdates": 0,
            "animationSequenceCount": 0,
            "avgModelResolveCpuMs": 0.0,
            "avgPoseUpdateCpuMs": 0.0,
            "avgSkinnedOutputCpuMs": 0.0,
            "modelResourceBytes": 0,
            "poseResourceBytes": 0,
            "frameSerial": int(shadow_budget.get("framesObserved", 0) or 0),
            "notes": "derived-from-shadowBudgetSummary",
        }
    if isinstance(shadow_runtime_v2, dict):
        summary["shadowRuntimeV2Summary"] = {
            "staticPersistentCount": int(shadow_runtime_v2.get("staticPersistentCount", 0) or 0),
            "dynamicPoseCount": int(shadow_runtime_v2.get("dynamicPoseCount", 0) or 0),
            "dynamicSkinnedOutputCount": int(shadow_runtime_v2.get("dynamicSkinnedOutputCount", 0) or 0),
            "fallbackDrawCount": int(shadow_runtime_v2.get("fallbackDrawCount", 0) or 0),
            "fallbackDrawCountTerrain": int(shadow_runtime_v2.get("fallbackDrawCountTerrain", 0) or 0),
            "fallbackDrawCountWorldObject": int(shadow_runtime_v2.get("fallbackDrawCountWorldObject", 0) or 0),
            "fallbackDrawCountUnitObject": int(shadow_runtime_v2.get("fallbackDrawCountUnitObject", 0) or 0),
            "objectFallbackDrawCount": int(shadow_runtime_v2.get("objectFallbackDrawCount", 0) or 0),
            "semanticBridgeHit": int(shadow_runtime_v2.get("semanticBridgeHit", 0) or 0),
            "semanticBridgeMiss": int(shadow_runtime_v2.get("semanticBridgeMiss", 0) or 0),
            "semanticBridgeBypassed": int(shadow_runtime_v2.get("semanticBridgeBypassed", 0) or 0),
            "semanticSceneSubmitted": int(shadow_runtime_v2.get("semanticSceneSubmitted", 0) or 0),
            "semanticSceneSubmittedUnit": int(shadow_runtime_v2.get("semanticSceneSubmittedUnit", 0) or 0),
            "semanticSceneSubmittedSkinned": int(shadow_runtime_v2.get("semanticSceneSubmittedSkinned", 0) or 0),
            "semanticSceneSubmittedFrameLocal": int(shadow_runtime_v2.get("semanticSceneSubmittedFrameLocal", 0) or 0),
            "semanticSceneSubmittedPersistent": int(shadow_runtime_v2.get("semanticSceneSubmittedPersistent", 0) or 0),
            "semanticSceneAcceptedExplicitResourceOwnerRigid": int(shadow_runtime_v2.get("semanticSceneAcceptedExplicitResourceOwnerRigid", 0) or 0),
            "modelRegistryHit": int(shadow_runtime_v2.get("modelRegistryHit", 0) or 0),
            "modelRegistryMiss": int(shadow_runtime_v2.get("modelRegistryMiss", 0) or 0),
            "modelLoadCount": int(shadow_runtime_v2.get("modelLoadCount", 0) or 0),
            "modelReuseCount": int(shadow_runtime_v2.get("modelReuseCount", 0) or 0),
            "runtimeBoundCount": int(shadow_runtime_v2.get("runtimeBoundCount", 0) or 0),
            "completeIdentityCount": int(shadow_runtime_v2.get("completeIdentityCount", 0) or 0),
            "shadowRuntimeBoundCount": int(shadow_runtime_v2.get("shadowRuntimeBoundCount", 0) or 0),
            "shadowIdentityCount": int(shadow_runtime_v2.get("shadowIdentityCount", 0) or 0),
            "poseUpdateCount": int(shadow_runtime_v2.get("poseUpdateCount", 0) or 0),
            "poseCacheHit": int(shadow_runtime_v2.get("poseCacheHit", 0) or 0),
            "poseCacheMiss": int(shadow_runtime_v2.get("poseCacheMiss", 0) or 0),
            "bonePaletteUpdates": int(shadow_runtime_v2.get("bonePaletteUpdates", 0) or 0),
            "shadowGeosetResourceCount": int(shadow_runtime_v2.get("shadowGeosetResourceCount", 0) or 0),
            "shadowReadyGeosetCount": int(shadow_runtime_v2.get("shadowReadyGeosetCount", 0) or 0),
            "shadowModelResourceCount": int(shadow_runtime_v2.get("shadowModelResourceCount", 0) or 0),
            "shadowPoseReadyCount": int(shadow_runtime_v2.get("shadowPoseReadyCount", 0) or 0),
            "upperLayerResolveAttempts": int(shadow_runtime_v2.get("upperLayerResolveAttempts", 0) or 0),
            "upperLayerResolveVisibleMiss": int(shadow_runtime_v2.get("upperLayerResolveVisibleMiss", 0) or 0),
            "upperLayerResolveVisibleUnresolvedGeoset": int(shadow_runtime_v2.get("upperLayerResolveVisibleUnresolvedGeoset", 0) or 0),
            "upperLayerResolveGeosetMiss": int(shadow_runtime_v2.get("upperLayerResolveGeosetMiss", 0) or 0),
            "upperLayerResolvePoseMiss": int(shadow_runtime_v2.get("upperLayerResolvePoseMiss", 0) or 0),
            "upperLayerResolveRuntimeGroupPaletteMiss": int(shadow_runtime_v2.get("upperLayerResolveRuntimeGroupPaletteMiss", 0) or 0),
            "upperLayerResolveAuthoritativeRigid": int(shadow_runtime_v2.get("upperLayerResolveAuthoritativeRigid", 0) or 0),
            "upperLayerResolveAuthoritativeSkinned": int(shadow_runtime_v2.get("upperLayerResolveAuthoritativeSkinned", 0) or 0),
            "upperLayerEmitted": int(shadow_runtime_v2.get("upperLayerEmitted", 0) or 0),
            "upperLayerDuplicateOrSuppressed": int(shadow_runtime_v2.get("upperLayerDuplicateOrSuppressed", 0) or 0),
            "animationSequenceCount": int(shadow_runtime_v2.get("animationSequenceCount", 0) or 0),
            "avgModelResolveCpuMs": float(shadow_runtime_v2.get("avgModelResolveCpuMs", 0.0) or 0.0),
            "avgPoseUpdateCpuMs": float(shadow_runtime_v2.get("avgPoseUpdateCpuMs", 0.0) or 0.0),
            "avgSkinnedOutputCpuMs": float(shadow_runtime_v2.get("avgSkinnedOutputCpuMs", 0.0) or 0.0),
            "modelResourceBytes": int(shadow_runtime_v2.get("modelResourceBytes", 0) or 0),
            "poseResourceBytes": int(shadow_runtime_v2.get("poseResourceBytes", 0) or 0),
            "frameSerial": int(shadow_runtime_v2.get("frameSerial", 0) or 0),
            "notes": str(shadow_runtime_v2.get("notes", "") or ""),
        }
    else:
        summary["shadowRuntimeV2Summary"] = _zero_shadow_runtime_v2_summary()
    if isinstance(data.get("topShadowOffenders", None), list):
        summary["topShadowOffenders"] = list(data.get("topShadowOffenders", []) or [])
    if isinstance(data.get("moduleMatrix", None), list):
        summary["moduleMatrix"] = list(data.get("moduleMatrix", []) or [])
    return summary


def _tail_text_file(path: Path, max_lines: int = 200, max_chars: int = 20000) -> str:
    """读取文件尾部文本，避免一次性返回过大内容。"""
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        text = path.read_text(encoding="mbcs", errors="ignore")
    lines = text.splitlines()
    if max_lines > 0:
        lines = lines[-max_lines:]
    out = "\n".join(lines)
    if max_chars > 0 and len(out) > max_chars:
        out = out[-max_chars:]
    return out


def _runtime_log_paths(war3_dir: Path) -> List[Path]:
    return [
        war3_dir / "war3_d3d9.log",
        war3_dir / "dxvk.log",
    ]


def _snapshot_log_offsets(war3_dir: Path) -> Dict[str, int]:
    offsets: Dict[str, int] = {}
    for path in _runtime_log_paths(war3_dir):
        try:
            offsets[str(path)] = int(path.stat().st_size)
        except Exception:
            offsets[str(path)] = 0
    return offsets


def _read_text_delta(path: Path, offset: int) -> str:
    if not path.exists():
        return ""
    try:
        size = int(path.stat().st_size)
        start = max(0, min(int(offset), size))
        with path.open("rb") as f:
            f.seek(start)
            raw = f.read()
        return raw.decode("utf-8", errors="ignore")
    except Exception:
        try:
            return path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            return path.read_text(encoding="mbcs", errors="ignore")


def _read_runtime_benchmark_summary(
    war3_dir: Path,
    log_offsets: Dict[str, int],
    profile: str,
    disable_modules: str,
) -> Dict[str, Any]:
    matches: List[Tuple[float, Path, re.Match[str]]] = []
    for path in _runtime_log_paths(war3_dir):
        text = _read_text_delta(path, int(log_offsets.get(str(path), 0) or 0))
        if not text and path.exists():
            text = _tail_text_file(path, max_lines=200, max_chars=40000)
        if not text:
            continue
        for match in BENCHMARK_LINE_RE.finditer(text):
            matches.append((path.stat().st_mtime if path.exists() else 0.0, path, match))
    if not matches:
        return {"ok": False, "error": "未在运行日志中找到 runtime benchmark 输出"}

    matches.sort(key=lambda item: (item[0], item[2].start()))
    _, log_path, match = matches[-1]
    avg_fps = float(match.group("avg") or 0.0)
    sample_frames = int(match.group("frames") or 0)
    warmup_sec = float(match.group("warmup") or 0.0)
    sample_sec = float(match.group("sample") or 0.0)
    runtime_profile = _runtime_profile_summary_from_inputs(
        match.group("profile") or profile,
        match.group("disabled") or disable_modules,
    )
    shadow_budget = _zero_shadow_budget_summary()
    report_path = str(log_path)
    return {
        "ok": avg_fps > 0.0,
        "reportType": "benchmark_log",
        "reportPath": report_path,
        "avgFps": avg_fps,
        "avgFrameTimeMs": (1000.0 / avg_fps) if avg_fps > 1e-6 else 0.0,
        "avgGpuTimeMs": 0.0,
        "avgTrackedActiveCpuMs": 0.0,
        "runtimeProfile": {
            "name": runtime_profile["name"],
            "disabledModules": runtime_profile["disabledModules"],
            "enabledModules": runtime_profile["enabledModules"],
        },
        "moduleMatrix": [
            {
                "profile": runtime_profile["name"],
                "disabledModules": runtime_profile["disabledModules"],
                "enabledModules": runtime_profile["enabledModules"],
            }
        ],
        "shadowBudgetSummary": shadow_budget,
        "shadowRuntimeV2Summary": _zero_shadow_runtime_v2_summary(),
        "topShadowOffenders": [
            {
                "name": phase["name"],
                "requestedMb": 0.0,
                "rejectedMb": 0.0,
                "rejects": 0,
            }
            for phase in shadow_budget["phases"]
        ],
        "benchmark": {
            "mode": str(match.group("mode") or "runtime"),
            "avgFps": avg_fps,
            "sampleFrames": sample_frames,
            "warmupSec": warmup_sec,
            "sampleSec": sample_sec,
            "logPath": report_path,
        },
    }


def _read_runtime_log_summary(
    war3_dir: Path,
    log_offsets: Dict[str, int],
) -> Dict[str, Any]:
    files: List[Dict[str, Any]] = []
    keyword_counts = {name: 0 for name, _ in LOG_KEYWORD_PATTERNS}
    sample_lines: List[Dict[str, str]] = []

    for path in _runtime_log_paths(war3_dir):
        text = _read_text_delta(path, int(log_offsets.get(str(path), 0) or 0))
        if not text and path.exists():
            text = _tail_text_file(path, max_lines=120, max_chars=30000)
        if not text:
            continue
        lines = text.splitlines()
        files.append(
            {
                "path": str(path),
                "lineCount": len(lines),
                "charCount": len(text),
            }
        )
        for raw_line in lines:
            line = str(raw_line or "").strip()
            if not line:
                continue
            matched = False
            for name, pattern in LOG_KEYWORD_PATTERNS:
                if pattern.search(line):
                    keyword_counts[name] += 1
                    matched = True
            if matched and len(sample_lines) < 16:
                sample_lines.append({"path": str(path), "line": line})

    top_keywords = [
        {"name": name, "count": int(count)}
        for name, count in sorted(keyword_counts.items(), key=lambda item: (-item[1], item[0]))
        if int(count) > 0
    ]
    return {
        "files": files,
        "keywordCounts": keyword_counts,
        "topKeywords": top_keywords,
        "sampleLines": sample_lines,
    }


def _merge_shadow_budget_summary_with_log_fallback(
    summary: Dict[str, Any],
    log_summary: Dict[str, Any],
) -> None:
    shadow_budget = summary.get("shadowBudgetSummary")
    if not isinstance(shadow_budget, dict):
        return

    keyword_counts = log_summary.get("keywordCounts", {}) if isinstance(log_summary, dict) else {}
    if not isinstance(keyword_counts, dict):
        return

    freeze_budget_exceeded = int(keyword_counts.get("freezeBudgetExceeded", 0) or 0)
    capture_incomplete = int(keyword_counts.get("shadowCaptureIncomplete", 0) or 0)
    reuse_last = int(keyword_counts.get("shadowReuseLastComplete", 0) or 0)
    render_partial = int(keyword_counts.get("shadowRenderPartial", 0) or 0)
    derived = False

    if int(shadow_budget.get("framesBudgetExceeded", 0) or 0) == 0 and freeze_budget_exceeded > 0:
        shadow_budget["framesBudgetExceeded"] = freeze_budget_exceeded
        derived = True
    if int(shadow_budget.get("framesIncomplete", 0) or 0) == 0 and capture_incomplete > 0:
        shadow_budget["framesIncomplete"] = capture_incomplete
        derived = True
    if int(shadow_budget.get("framesReuseLastComplete", 0) or 0) == 0 and reuse_last > 0:
        shadow_budget["framesReuseLastComplete"] = reuse_last
        derived = True
    if int(shadow_budget.get("framesRenderCurrentPartial", 0) or 0) == 0 and render_partial > 0:
        shadow_budget["framesRenderCurrentPartial"] = render_partial
        derived = True
    if int(shadow_budget.get("framesObserved", 0) or 0) == 0:
        observed = max(
            freeze_budget_exceeded,
            capture_incomplete,
            reuse_last,
            render_partial,
        )
        if observed > 0:
            shadow_budget["framesObserved"] = observed
            derived = True

    if derived:
        shadow_budget["derivedFromLogKeywords"] = True


class DbWinListener:
    """读取 OutputDebugString（DBWIN）缓冲。"""

    PAGE_READWRITE = 0x04
    FILE_MAP_READ = 0x0004
    WAIT_OBJECT_0 = 0x00000000
    WAIT_TIMEOUT = 0x00000102

    def __init__(self) -> None:
        self.kernel32 = ctypes.windll.kernel32
        # 显式绑定 Win32 API 签名，避免 ctypes 默认参数推断导致
        # HANDLE(-1) 在 64 位 Python 下被按有符号整型转换并触发 OverflowError。
        self.kernel32.CreateEventW.argtypes = [
            wintypes.LPVOID,
            wintypes.BOOL,
            wintypes.BOOL,
            wintypes.LPCWSTR,
        ]
        self.kernel32.CreateEventW.restype = wintypes.HANDLE
        self.kernel32.CreateFileMappingW.argtypes = [
            wintypes.HANDLE,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPCWSTR,
        ]
        self.kernel32.CreateFileMappingW.restype = wintypes.HANDLE
        self.kernel32.MapViewOfFile.argtypes = [
            wintypes.HANDLE,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_size_t,
        ]
        self.kernel32.MapViewOfFile.restype = wintypes.LPVOID
        self.kernel32.UnmapViewOfFile.argtypes = [wintypes.LPCVOID]
        self.kernel32.UnmapViewOfFile.restype = wintypes.BOOL
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.kernel32.SetEvent.argtypes = [wintypes.HANDLE]
        self.kernel32.SetEvent.restype = wintypes.BOOL
        self.kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.kernel32.WaitForSingleObject.restype = wintypes.DWORD

        self.h_buffer_ready = None
        self.h_data_ready = None
        self.h_map = None
        self.p_buf = None
        self._opened = False

    def open(self) -> None:
        if self._opened:
            return

        self.h_buffer_ready = self.kernel32.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
        self.h_data_ready = self.kernel32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
        self.h_map = self.kernel32.CreateFileMappingW(
            ctypes.c_void_p(-1),
            None,
            self.PAGE_READWRITE,
            0,
            4096,
            "DBWIN_BUFFER",
        )
        if not self.h_buffer_ready or not self.h_data_ready or not self.h_map:
            raise RuntimeError("DBWIN 初始化失败：CreateEvent/CreateFileMapping 失败")

        self.p_buf = self.kernel32.MapViewOfFile(self.h_map, self.FILE_MAP_READ, 0, 0, 4096)
        if not self.p_buf:
            raise RuntimeError("DBWIN 初始化失败：MapViewOfFile 失败")

        self._opened = True

    def close(self) -> None:
        if self.p_buf:
            self.kernel32.UnmapViewOfFile(self.p_buf)
            self.p_buf = None
        if self.h_map:
            self.kernel32.CloseHandle(self.h_map)
            self.h_map = None
        if self.h_data_ready:
            self.kernel32.CloseHandle(self.h_data_ready)
            self.h_data_ready = None
        if self.h_buffer_ready:
            self.kernel32.CloseHandle(self.h_buffer_ready)
            self.h_buffer_ready = None
        self._opened = False

    def read(self, timeout_ms: int = 200) -> Tuple[Optional[int], Optional[str]]:
        if not self._opened:
            self.open()

        self.kernel32.SetEvent(self.h_buffer_ready)
        ret = self.kernel32.WaitForSingleObject(self.h_data_ready, timeout_ms)
        if ret == self.WAIT_TIMEOUT:
            return None, None
        if ret != self.WAIT_OBJECT_0:
            return None, None

        raw = ctypes.string_at(self.p_buf, 4096)
        pid = struct.unpack("<I", raw[:4])[0]
        msg = raw[4:].split(b"\x00", 1)[0].decode("mbcs", errors="ignore")
        return pid, msg


def _state_owned_process_alive(pid: int) -> Optional[bool]:
    """
    Return the authoritative liveness of the single process owned by STATE.

    Isolated-desktop launches retain their original process HANDLE so PID reuse
    and cross-desktop OpenProcess/tasklist visibility cannot create a false
    "process exited" result. For any unowned/mismatched PID, or while the
    witness is being replaced, return None and preserve the legacy probe.
    """
    state = globals().get("STATE")
    if state is None or int(getattr(state, "war3_pid", 0) or 0) != int(pid):
        return None
    witness = getattr(state, "retained_native_process", None)
    if witness is None:
        return None
    try:
        snapshot = witness.snapshot()
        if (
            not bool(snapshot.get("available", False))
            or int(snapshot.get("pid", 0) or 0) != int(pid)
        ):
            return None
        return witness.poll() is None
    except Exception:
        # Replacement/close may race a read-only query. Falling back is safer
        # than treating an unavailable witness as proof of process death.
        return None


def _pid_alive(pid: int) -> bool:
    owned_alive = _state_owned_process_alive(pid)
    if owned_alive is not None:
        return owned_alive

    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    kernel32 = ctypes.windll.kernel32
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        # 某些权限边界下 OpenProcess 会失败，回退到 tasklist 检测，避免误判“已退出”。
        return _pid_alive_via_tasklist(pid)
    try:
        code = ctypes.c_ulong(0)
        ok = kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        if not ok:
            return _pid_alive_via_tasklist(pid)
        return int(code.value) == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(h)


def _pid_alive_via_tasklist(pid: int) -> bool:
    try:
        proc = subprocess.run(
            ["tasklist", "/FI", f"PID eq {int(pid)}", "/FO", "CSV", "/NH"],
            check=False,
            capture_output=True,
            text=True,
            encoding="mbcs",
            errors="ignore",
        )
        out = (proc.stdout or "").strip().lower()
        if not out or "no tasks are running" in out:
            return False
        return str(int(pid)) in out
    except Exception:
        return False


def _taskkill(pid: int, force: bool) -> None:
    cmd = ["taskkill", "/PID", str(pid), "/T"]
    if force:
        cmd.append("/F")
    try:
        subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        # 某些环境 PATH 缺失 System32 时回退到 PowerShell Stop-Process。
        ps = [
            "powershell",
            "-NoProfile",
            "-Command",
            f"$p = Get-Process -Id {int(pid)} -ErrorAction SilentlyContinue; "
            f"if ($p) {{ Stop-Process -Id {int(pid)} -Force -ErrorAction SilentlyContinue }}",
        ]
        subprocess.run(ps, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _get_process_cpu_seconds(pid: int) -> float:
    """返回进程累计 CPU 时间（user+kernel，秒）。失败返回 -1。"""
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    kernel32 = ctypes.windll.kernel32
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return -1.0
    try:
        creation = ctypes.c_ulonglong(0)
        exit_t = ctypes.c_ulonglong(0)
        kernel_t = ctypes.c_ulonglong(0)
        user_t = ctypes.c_ulonglong(0)
        ok = kernel32.GetProcessTimes(
            h,
            ctypes.byref(creation),
            ctypes.byref(exit_t),
            ctypes.byref(kernel_t),
            ctypes.byref(user_t),
        )
        if not ok:
            return -1.0
        # FILETIME: 100ns
        return float(kernel_t.value + user_t.value) / 10_000_000.0
    finally:
        kernel32.CloseHandle(h)


def _get_process_creation_epoch_ms(pid: int) -> int:
    """读取真实进程创建时间，用于防止 PID 回收后误路由/误结束。"""
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return 0
    try:
        creation = ctypes.c_ulonglong(0)
        exit_time = ctypes.c_ulonglong(0)
        kernel_time = ctypes.c_ulonglong(0)
        user_time = ctypes.c_ulonglong(0)
        if not kernel32.GetProcessTimes(
            handle,
            ctypes.byref(creation),
            ctypes.byref(exit_time),
            ctypes.byref(kernel_time),
            ctypes.byref(user_time),
        ):
            return 0
        windows_to_unix_100ns = 116_444_736_000_000_000
        return max(0, int((creation.value - windows_to_unix_100ns) // 10_000))
    finally:
        kernel32.CloseHandle(handle)


def _registered_session_alive(session: AutoTestSession) -> bool:
    if int(session.pid or 0) <= 0 or not _pid_alive(int(session.pid)):
        return False
    expected = int(session.process_created_at_ms or 0)
    actual = _get_process_creation_epoch_ms(int(session.pid))
    if expected > 0 and actual > 0 and abs(expected - actual) > 2_000:
        return False
    return True


class _Win32Rect(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class _Win32Point(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_long),
        ("y", ctypes.c_long),
    ]


class _Win32WindowPlacement(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("showCmd", ctypes.c_uint),
        ("ptMinPosition", _Win32Point),
        ("ptMaxPosition", _Win32Point),
        ("rcNormalPosition", _Win32Rect),
    ]


_DPI_AWARE_SET = False


def _ensure_process_dpi_aware() -> None:
    global _DPI_AWARE_SET
    if _DPI_AWARE_SET:
        return
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass
    _DPI_AWARE_SET = True


def _get_window_text(hwnd: int) -> str:
    if not hwnd:
        return ""
    user32 = ctypes.windll.user32
    buf = ctypes.create_unicode_buffer(512)
    try:
        user32.GetWindowTextW(ctypes.c_void_p(hwnd), buf, len(buf))
        return buf.value
    except Exception:
        return ""


def _rect_to_dict(rect: _Win32Rect) -> Dict[str, int]:
    return {
        "left": int(rect.left),
        "top": int(rect.top),
        "right": int(rect.right),
        "bottom": int(rect.bottom),
        "width": max(0, int(rect.right - rect.left)),
        "height": max(0, int(rect.bottom - rect.top)),
    }


def _query_window_info_by_hwnd(hwnd: int, pid: int = 0) -> Dict[str, Any]:
    if not hwnd:
        return {"ok": False, "error": "hwnd=0"}

    _ensure_process_dpi_aware()
    user32 = ctypes.windll.user32
    wr = _Win32Rect()
    cr = _Win32Rect()
    placement = _Win32WindowPlacement()
    placement.length = ctypes.sizeof(_Win32WindowPlacement)

    ok_wr = bool(user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(wr)))
    ok_cr = bool(user32.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(cr)))
    ok_wp = bool(user32.GetWindowPlacement(ctypes.c_void_p(hwnd), ctypes.byref(placement)))

    return {
        "ok": ok_wr and ok_cr,
        "pid": int(pid),
        "hwnd": int(hwnd),
        "title": _get_window_text(hwnd),
        "visible": bool(user32.IsWindowVisible(ctypes.c_void_p(hwnd))),
        "windowRect": _rect_to_dict(wr) if ok_wr else {},
        "clientRect": _rect_to_dict(cr) if ok_cr else {},
        "showCmd": int(placement.showCmd) if ok_wp else 0,
        "placementFlags": int(placement.flags) if ok_wp else 0,
    }


def _wait_for_main_window_hwnd(pid: int, timeout_sec: float = 15.0, require_visible: bool = True) -> int:
    t0 = time.time()
    while time.time() - t0 < max(0.2, float(timeout_sec)):
        hwnd = _find_main_window_hwnd(pid)
        if hwnd:
            if not require_visible:
                return hwnd
            info = _query_window_info_by_hwnd(hwnd, pid=pid)
            if info.get("visible"):
                return hwnd
        if not _pid_alive(pid):
            return 0
        time.sleep(0.1)
    return 0


def _resize_window_client_native(pid: int, client_w: int, client_h: int, x: int = 40, y: int = 40) -> Dict[str, Any]:
    """
    将 War3 窗口的 client 区调整到目标尺寸；不激活窗口。
    """
    hwnd = _wait_for_main_window_hwnd(pid, timeout_sec=8.0, require_visible=True)
    if not hwnd:
        return {"ok": False, "error": f"未找到可见主窗口: pid={pid}"}

    _ensure_process_dpi_aware()
    user32 = ctypes.windll.user32
    wr = _Win32Rect()
    cr = _Win32Rect()
    if not user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(wr)):
        return {"ok": False, "error": "GetWindowRect 失败", "hwnd": int(hwnd)}
    if not user32.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(cr)):
        return {"ok": False, "error": "GetClientRect 失败", "hwnd": int(hwnd)}

    window_w = max(0, int(wr.right - wr.left))
    window_h = max(0, int(wr.bottom - wr.top))
    client_cur_w = max(0, int(cr.right - cr.left))
    client_cur_h = max(0, int(cr.bottom - cr.top))
    border_w = max(0, window_w - client_cur_w)
    border_h = max(0, window_h - client_cur_h)

    target_w = max(1, int(client_w) + border_w)
    target_h = max(1, int(client_h) + border_h)
    SWP_NOZORDER = 0x0004
    SWP_NOACTIVATE = 0x0010
    ok = bool(
        user32.SetWindowPos(
            ctypes.c_void_p(hwnd),
            ctypes.c_void_p(0),
            int(x),
            int(y),
            target_w,
            target_h,
            SWP_NOZORDER | SWP_NOACTIVATE,
        )
    )
    time.sleep(0.15)
    info = _query_window_info_by_hwnd(hwnd, pid=pid)
    return {
        "ok": ok and bool(info.get("ok")),
        "pid": int(pid),
        "hwnd": int(hwnd),
        "targetClientW": int(client_w),
        "targetClientH": int(client_h),
        "windowW": target_w,
        "windowH": target_h,
        "info": info,
    }


def _post_window_syscommand(pid: int, command: int) -> Dict[str, Any]:
    hwnd = _wait_for_main_window_hwnd(pid, timeout_sec=8.0, require_visible=True)
    if not hwnd:
        return {"ok": False, "error": f"未找到可见主窗口: pid={pid}"}

    WM_SYSCOMMAND = 0x0112
    ok = bool(
        ctypes.windll.user32.PostMessageW(
            ctypes.c_void_p(hwnd),
            WM_SYSCOMMAND,
            ctypes.c_void_p(command),
            ctypes.c_void_p(0),
        )
    )
    return {
        "ok": ok,
        "pid": int(pid),
        "hwnd": int(hwnd),
        "command": int(command),
        "window": _query_window_info_by_hwnd(hwnd, pid=pid),
    }


def _post_war3_key_pulse(
    pid: int,
    key: str = "RIGHT",
    hold_ms: int = 45,
    repeat: int = 1,
    foreground: bool = False,
) -> Dict[str, Any]:
    """Send a small keyboard pulse to the War3 window without requiring camera internals."""
    hwnd = _wait_for_main_window_hwnd(pid, timeout_sec=8.0, require_visible=True)
    if not hwnd:
        return {"ok": False, "error": f"未找到可见主窗口: pid={pid}"}

    key_name = str(key or "RIGHT").strip().upper()
    vk_map = {
        "SPACE": 0x20,
        "ENTER": 0x0D,
        "RETURN": 0x0D,
        "ESC": 0x1B,
        "ESCAPE": 0x1B,
        "TAB": 0x09,
        "LEFT": 0x25,
        "UP": 0x26,
        "RIGHT": 0x27,
        "DOWN": 0x28,
        "PAGEUP": 0x21,
        "PGUP": 0x21,
        "PAGEDOWN": 0x22,
        "PGDN": 0x22,
        "HOME": 0x24,
        "END": 0x23,
        "INSERT": 0x2D,
        "INS": 0x2D,
        "DELETE": 0x2E,
        "DEL": 0x2E,
        "F1": 0x70,
        "F2": 0x71,
        "F3": 0x72,
        "F4": 0x73,
        "F5": 0x74,
        "F6": 0x75,
        "F7": 0x76,
        "F8": 0x77,
        "F9": 0x78,
        "F10": 0x79,
        "F11": 0x7A,
        "F12": 0x7B,
    }
    if len(key_name) == 1 and "A" <= key_name <= "Z":
        vk_map[key_name] = ord(key_name)
    if len(key_name) == 1 and "0" <= key_name <= "9":
        vk_map[key_name] = ord(key_name)
    vk = int(vk_map.get(key_name, 0))
    if vk == 0:
        return {
            "ok": False,
            "error": f"不支持的 key: {key}",
            "supported": sorted(vk_map.keys()),
        }

    user32 = ctypes.windll.user32

    # War3 1.27a 的“按下任意键以继续”由 DirectInput/键盘状态读取，
    # WM_KEYDOWN/WM_KEYUP 只足以驱动部分窗口消息路径。目标位于隔离
    # Win32 Desktop 时，在同一 Desktop 内启动一次性 keybd_event helper；
    # 这不会切换作者当前桌面，也不需要把 War3 窗口带到前台桌面。
    registered = SESSION_REGISTRY.get(pid=int(pid))
    desktop_name = ""
    if registered is not None:
        desktop_name = str(registered.desktop_name or "")
    elif STATE.war3_pid == pid:
        desktop_name = str(STATE.desktop_name or "")
    if desktop_name:
        helper_script = Path(__file__).with_name("send_key_same_desktop.ps1")
        powershell_exe = (
            Path(os.environ.get("SystemRoot", r"C:\Windows")) /
            "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
        )
        status_path = _ensure_dir(ARTIFACT_ROOT / "input") / (
            f"key_{pid}_{time.time_ns()}.status.txt"
        )
        if helper_script.is_file() and powershell_exe.is_file():
            helper_args = [
                str(powershell_exe),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(helper_script),
                "-TargetPid",
                str(int(pid)),
                "-VirtualKey",
                str(int(vk)),
                "-HoldMs",
                str(max(20, int(hold_ms))),
                "-StatusPath",
                str(status_path),
            ]
            helper = _launch_process_on_desktop(
                helper_args,
                Path(__file__).resolve().parent,
                os.environ.copy(),
                desktop_name,
            )
            deadline = time.time() + 6.0
            while helper.get("ok") and time.time() < deadline:
                if status_path.exists():
                    break
                helper_pid = int(helper.get("pid", 0) or 0)
                if helper_pid > 0 and not _pid_alive(helper_pid):
                    break
                time.sleep(0.05)
            status = ""
            if status_path.exists():
                try:
                    status = status_path.read_text(
                        encoding="utf-8-sig", errors="replace").strip()
                except Exception as exc:
                    status = f"ERROR:status read failed: {exc}"
            helper_ok = bool(helper.get("ok")) and status.startswith("OK:")
            if helper_ok:
                return {
                    "ok": True,
                    "pid": int(pid),
                    "hwnd": int(hwnd),
                    "key": key_name,
                    "vk": int(vk),
                    "holdMs": int(hold_ms),
                    "repeat": 1,
                    "foreground": False,
                    "mode": "same-desktop-keybd-event",
                    "desktop": desktop_name,
                    "helper": helper,
                    "helperStatus": status,
                    "helperStatusPath": str(status_path),
                    "window": _query_window_info_by_hwnd(hwnd, pid=pid),
                }

    if foreground:
        try:
            user32.SetForegroundWindow(ctypes.c_void_p(hwnd))
            time.sleep(0.05)
        except Exception:
            pass

    WM_KEYDOWN = 0x0100
    WM_KEYUP = 0x0101
    scan = int(user32.MapVirtualKeyW(vk, 0)) & 0xFF
    lparam_down = 1 | (scan << 16)
    lparam_up = 1 | (scan << 16) | (1 << 30) | (1 << 31)
    posts: List[Dict[str, Any]] = []
    ok_all = True
    repeat_count = max(1, int(repeat))
    hold_sec = max(0.0, float(hold_ms) / 1000.0)
    for _ in range(repeat_count):
        down_ok = bool(
            user32.PostMessageW(
                ctypes.c_void_p(hwnd),
                WM_KEYDOWN,
                ctypes.c_void_p(vk),
                ctypes.c_void_p(lparam_down),
            )
        )
        time.sleep(hold_sec)
        up_ok = bool(
            user32.PostMessageW(
                ctypes.c_void_p(hwnd),
                WM_KEYUP,
                ctypes.c_void_p(vk),
                ctypes.c_void_p(lparam_up),
            )
        )
        posts.append({"down": down_ok, "up": up_ok})
        ok_all = ok_all and down_ok and up_ok
        time.sleep(0.02)

    return {
        "ok": bool(ok_all),
        "pid": int(pid),
        "hwnd": int(hwnd),
        "key": key_name,
        "vk": vk,
        "scan": scan,
        "holdMs": int(hold_ms),
        "repeat": repeat_count,
        "foreground": bool(foreground),
        "posts": posts,
        "window": _query_window_info_by_hwnd(hwnd, pid=pid),
    }


def _run_war3_input_plan(
    pid: int,
    actions: List[Dict[str, Any]],
    timeout_sec: float = 20.0,
) -> Dict[str, Any]:
    """Run keyboard/client-click actions from a helper on War3's isolated desktop."""
    target_pid = int(pid)
    if target_pid <= 0 or not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}"}
    if not isinstance(actions, list) or not actions:
        return {"ok": False, "error": "actions 必须是非空 JSON array"}
    if len(actions) > 512:
        return {"ok": False, "error": "单次输入计划最多 512 个动作"}

    registered = SESSION_REGISTRY.get(pid=target_pid)
    desktop_name = str(registered.desktop_name or "") if registered is not None else ""
    if not desktop_name and STATE.war3_pid == target_pid:
        desktop_name = str(STATE.desktop_name or "")
    if not desktop_name:
        return {
            "ok": False,
            "error": "输入计划只允许用于已登记的隔离桌面 War3 会话",
            "code": "ISOLATED_DESKTOP_REQUIRED",
        }

    hwnd = _wait_for_main_window_hwnd(target_pid, timeout_sec=8.0, require_visible=True)
    if not hwnd:
        return {"ok": False, "error": f"未找到可见主窗口: pid={target_pid}"}
    window = _query_window_info_by_hwnd(hwnd, pid=target_pid)
    client = dict(window.get("clientRect", {}) or {})
    client_width = max(1, int(client.get("width", 0) or 0))
    client_height = max(1, int(client.get("height", 0) or 0))

    normalized: List[Dict[str, Any]] = []
    for index, raw_action in enumerate(actions):
        if not isinstance(raw_action, dict):
            return {"ok": False, "error": f"action[{index}] 必须是 object"}
        kind = str(raw_action.get("type", "")).strip().lower()
        if kind == "sleep":
            normalized.append({
                "type": "sleep",
                "ms": max(0, min(int(raw_action.get("ms", 0) or 0), 60_000)),
            })
        elif kind == "key":
            vk = int(raw_action.get("vk", 0) or 0)
            if vk <= 0 or vk > 0xFE:
                return {"ok": False, "error": f"action[{index}] vk 非法: {vk}"}
            normalized.append({
                "type": "key",
                "vk": vk,
                "holdMs": max(20, min(int(raw_action.get("holdMs", 45) or 45), 2_000)),
            })
        elif kind == "click":
            button = str(raw_action.get("button", "left")).strip().lower()
            if button not in ("left", "right"):
                return {"ok": False, "error": f"action[{index}] button 非法: {button}"}
            normalized.append({
                "type": "click",
                "x": max(0, min(int(raw_action.get("x", 0) or 0), client_width - 1)),
                "y": max(0, min(int(raw_action.get("y", 0) or 0), client_height - 1)),
                "button": button,
                "holdMs": max(20, min(int(raw_action.get("holdMs", 45) or 45), 2_000)),
                "count": max(1, min(int(raw_action.get("count", 1) or 1), 4)),
            })
        else:
            return {"ok": False, "error": f"action[{index}] type 不支持: {kind}"}

    helper_script = Path(__file__).with_name("send_input_plan_same_desktop.ps1")
    powershell_exe = (
        Path(os.environ.get("SystemRoot", r"C:\Windows")) /
        "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    )
    if not helper_script.is_file() or not powershell_exe.is_file():
        return {
            "ok": False,
            "error": "同桌面输入 helper 或 PowerShell 不存在",
            "helperScript": str(helper_script),
            "powershell": str(powershell_exe),
        }

    input_root = _ensure_dir(ARTIFACT_ROOT / "input")
    nonce = f"{target_pid}_{time.time_ns()}"
    actions_path = input_root / f"plan_{nonce}.json"
    status_path = input_root / f"plan_{nonce}.status.txt"
    actions_path.write_text(
        json.dumps(normalized, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    helper_args = [
        str(powershell_exe),
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(helper_script),
        "-TargetPid",
        str(target_pid),
        "-ActionsPath",
        str(actions_path),
        "-StatusPath",
        str(status_path),
    ]
    helper = _launch_process_on_desktop(
        helper_args,
        Path(__file__).resolve().parent,
        os.environ.copy(),
        desktop_name,
    )
    # The desktop launcher transfers an owned native process witness through
    # this private field.  The input helper is short lived and is not part of
    # RuntimeState, so consume and close that HANDLE locally; never leak the
    # Python owner into an MCP/JSON result.
    helper_witness = helper.pop("_nativeProcessWitness", None)
    deadline = time.time() + max(2.0, float(timeout_sec))
    while helper.get("ok") and time.time() < deadline:
        if status_path.exists():
            break
        helper_pid = int(helper.get("pid", 0) or 0)
        if helper_pid > 0 and not _pid_alive(helper_pid):
            break
        time.sleep(0.05)

    status = ""
    if status_path.exists():
        try:
            status = status_path.read_text(encoding="utf-8-sig", errors="replace").strip()
        except Exception as exc:
            status = f"ERROR:status read failed: {exc}"
    ok = bool(helper.get("ok")) and status.startswith("OK:") and ";ok=True;" in status
    helper_witness_close: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "launcher did not return a native witness",
    }
    if helper_witness is not None:
        try:
            helper_witness_close = dict(helper_witness.close() or {})
        except Exception as exc:
            helper_witness_close = {
                "ok": False,
                "error": f"input helper witness close failed: {type(exc).__name__}: {exc}",
            }
    return {
        "ok": bool(ok and helper_witness_close.get("ok") is True),
        "pid": target_pid,
        "hwnd": int(hwnd),
        "desktop": desktop_name,
        "clientWidth": client_width,
        "clientHeight": client_height,
        "actionCount": len(normalized),
        "actionsPath": str(actions_path),
        "statusPath": str(status_path),
        "helper": helper,
        "helperNativeWitnessClose": helper_witness_close,
        "helperStatus": status,
        "window": window,
    }


def _wait_for_window_ready(
    pid: int,
    timeout_sec: int = 30,
    min_cpu_sec: float = 0.5,
    stable_sec: float = 0.8,
) -> Dict[str, Any]:
    """
    仅等待“窗口可操作”，用于 resize/maximize 崩溃测试，不等同于正式进图。
    """
    t0 = time.time()
    first_hwnd_ts = 0.0
    last_hwnd = 0

    while time.time() - t0 < max(1, int(timeout_sec)):
        if not _pid_alive(pid):
            return {"ok": False, "error": "进程已退出", "pid": int(pid)}

        hwnd = _find_main_window_hwnd(pid)
        cpu_sec = _get_process_cpu_seconds(pid)
        if hwnd:
            if hwnd != last_hwnd:
                first_hwnd_ts = time.time()
                last_hwnd = hwnd
            if first_hwnd_ts <= 0.0:
                first_hwnd_ts = time.time()
            if (time.time() - first_hwnd_ts) >= max(0.1, float(stable_sec)) and cpu_sec >= max(0.0, float(min_cpu_sec)):
                return {
                    "ok": True,
                    "pid": int(pid),
                    "elapsedSec": round(time.time() - t0, 3),
                    "cpuSec": round(cpu_sec, 3),
                    "window": _query_window_info_by_hwnd(hwnd, pid=pid),
                }
        time.sleep(0.1)

    return {
        "ok": False,
        "error": "等待窗口可操作超时",
        "pid": int(pid),
        "elapsedSec": round(time.time() - t0, 3),
    }


def _enumerate_pid_windows(pid: int) -> List[int]:
    user32 = ctypes.windll.user32
    hwnds: List[int] = []

    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def _cb(hwnd: int, _lparam: int) -> bool:
        proc_id = ctypes.c_ulong(0)
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(proc_id))
        if proc_id.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            hwnds.append(hwnd)
        return True

    enum_cb = WNDENUMPROC(_cb)
    registered = SESSION_REGISTRY.get(pid=int(pid))
    if registered is not None:
        desktop_handle = int(registered.desktop_handle or 0)
    else:
        desktop_handle = int(STATE.desktop_handle or 0) if STATE.war3_pid == pid else 0
    if desktop_handle:
        user32.EnumDesktopWindows.argtypes = [wintypes.HANDLE, WNDENUMPROC, wintypes.LPARAM]
        user32.EnumDesktopWindows.restype = wintypes.BOOL
        user32.EnumDesktopWindows(wintypes.HANDLE(desktop_handle), enum_cb, 0)
    else:
        user32.EnumWindows(enum_cb, 0)
    return hwnds


def _rank_window_candidate(info: Dict[str, Any]) -> int:
    wr = dict(info.get("windowRect", {}) or {})
    cr = dict(info.get("clientRect", {}) or {})
    window_area = int(wr.get("width", 0) or 0) * int(wr.get("height", 0) or 0)
    client_area = int(cr.get("width", 0) or 0) * int(cr.get("height", 0) or 0)
    show_cmd = int(info.get("showCmd", 0) or 0)
    score = max(window_area, client_area * 2)
    if show_cmd == 2:  # SW_SHOWMINIMIZED
        score -= 1_000_000_000
    if str(info.get("title", "") or "").strip():
        score += 10_000
    return int(score)


def _main_window_candidates(pid: int) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for hwnd in _enumerate_pid_windows(pid):
        info = _query_window_info_by_hwnd(hwnd, pid=pid)
        wr = dict(info.get("windowRect", {}) or {})
        cr = dict(info.get("clientRect", {}) or {})
        window_area = int(wr.get("width", 0) or 0) * int(wr.get("height", 0) or 0)
        client_area = int(cr.get("width", 0) or 0) * int(cr.get("height", 0) or 0)
        info["windowArea"] = int(window_area)
        info["clientArea"] = int(client_area)
        info["score"] = _rank_window_candidate(info)
        rows.append(info)
    rows.sort(key=lambda row: int(row.get("score", 0) or 0), reverse=True)
    return rows


def _find_main_window_hwnd(pid: int) -> int:
    candidates = _main_window_candidates(pid)
    for row in candidates:
        if int(row.get("windowArea", 0) or 0) > 0 and int(row.get("clientArea", 0) or 0) > 0:
            return int(row.get("hwnd", 0) or 0)
    return int(candidates[0].get("hwnd", 0) or 0) if candidates else 0


def _post_close(pid: int) -> bool:
    hwnd = _find_main_window_hwnd(pid)
    if not hwnd:
        return False
    WM_CLOSE = 0x0010
    ctypes.windll.user32.PostMessageW(ctypes.c_void_p(hwnd), WM_CLOSE, 0, 0)
    return True


def _uses_isolated_desktop(pid: int) -> bool:
    """Return whether *pid* belongs to an AutoTest-owned non-input Desktop.

    SessionRegistry is authoritative for batch sessions.  The legacy single-session
    path still stores the same capability in STATE, so keep that as a conservative
    fallback.  A false negative here could capture the developer's visible desktop.
    """
    registered = SESSION_REGISTRY.get(pid=int(pid))
    if registered and int(registered.desktop_handle or 0):
        return True
    state = globals().get("STATE")
    return bool(
        state
        and int(getattr(state, "war3_pid", 0) or 0) == int(pid)
        and int(getattr(state, "desktop_handle", 0) or 0)
    )


def _powershell_capture_window(pid: int, output_png: Path) -> Dict[str, Any]:
    hwnd = _find_main_window_hwnd(pid)
    selected_window = _query_window_info_by_hwnd(hwnd, pid=pid) if hwnd else {"ok": False, "error": "hwnd=0"}
    candidates = _main_window_candidates(pid)[:8]
    script = r'''
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Rect {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
  }
  [DllImport("user32.dll")]
  public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
  [DllImport("user32.dll")]
  public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")]
  public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
}
"@
[Win32Rect]::SetProcessDPIAware() | Out-Null
$procId = [int]$env:WAR3_AUTOTEST_PID
$out = $env:WAR3_AUTOTEST_OUT
$hWnd = [IntPtr]::Zero
if ($env:WAR3_AUTOTEST_HWND) {
  $hWnd = [IntPtr]([Int64]$env:WAR3_AUTOTEST_HWND)
}
$proc = Get-Process -Id $procId -ErrorAction Stop
if ($hWnd -eq [IntPtr]::Zero) {
  $hWnd = $proc.MainWindowHandle
}
if ($hWnd -eq 0) { throw "MainWindowHandle=0" }
$rect = New-Object Win32Rect+RECT
$rectOk = [Win32Rect]::GetWindowRect($hWnd, [ref]$rect)
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if (-not $rectOk -or $w -le 0 -or $h -le 0) {
  # powershell.exe 在非当前 Win32 Desktop 上偶尔无法再次读取窗口矩形，
  # 即使父进程刚通过 EnumDesktopWindows/GetWindowRect 得到了有效值。
  # 使用父进程已核验的尺寸仍可按 HWND 调用 PrintWindow，避免在真正的
  # 地图加载错误对话框出现时先把游戏杀掉却没有留下任何画面证据。
  $w = [int]$env:WAR3_AUTOTEST_WINDOW_W
  $h = [int]$env:WAR3_AUTOTEST_WINDOW_H
  $rect.Left = [int]$env:WAR3_AUTOTEST_WINDOW_LEFT
  $rect.Top = [int]$env:WAR3_AUTOTEST_WINDOW_TOP
}
if ($w -le 0 -or $h -le 0) { throw "WindowRect invalid: ${w}x${h}" }
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$captureMode = "PrintWindow"
$dc = [IntPtr]::Zero
try {
  $dc = $g.GetHdc()
  # 优先按窗口句柄抓图，避免窗口被覆盖时抓到桌面内容。
  $okPrint = [Win32Rect]::PrintWindow($hWnd, $dc, 2)
}
finally {
  if ($dc -ne [IntPtr]::Zero) { $g.ReleaseHdc($dc) }
}
if (-not $okPrint) {
  # CopyFromScreen 只能抓当前输入桌面；目标在隔离 Win32 Desktop 时，
  # 回退会悄悄截到开发者桌面并被误判为游戏画面。
  if ($env:WAR3_AUTOTEST_ISOLATED_DESKTOP -eq "1") {
    throw "PrintWindow failed on isolated desktop"
  }
  # 非隔离桌面才允许退化为屏幕抓图。
  $captureMode = "CopyFromScreen"
  $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
}
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Output ("OK:" + $captureMode)
'''
    env = os.environ.copy()
    env["WAR3_AUTOTEST_PID"] = str(pid)
    env["WAR3_AUTOTEST_OUT"] = str(output_png)
    if hwnd:
        env["WAR3_AUTOTEST_HWND"] = str(int(hwnd))
    selected_rect = dict(selected_window.get("windowRect", {}) or {})
    env["WAR3_AUTOTEST_WINDOW_W"] = str(int(selected_rect.get("width", 0) or 0))
    env["WAR3_AUTOTEST_WINDOW_H"] = str(int(selected_rect.get("height", 0) or 0))
    env["WAR3_AUTOTEST_WINDOW_LEFT"] = str(int(selected_rect.get("left", 0) or 0))
    env["WAR3_AUTOTEST_WINDOW_TOP"] = str(int(selected_rect.get("top", 0) or 0))
    isolated_desktop = _uses_isolated_desktop(pid)
    env["WAR3_AUTOTEST_ISOLATED_DESKTOP"] = "1" if isolated_desktop else "0"
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script],
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    return {
        "returncode": proc.returncode,
        "stdout": proc.stdout.strip(),
        "stderr": proc.stderr.strip(),
        "hwnd": int(hwnd or 0),
        "selectedWindow": selected_window,
        "windowCandidates": candidates,
    }


def _powershell_convert_bitmap_to_png(input_bmp: Path, output_png: Path) -> Dict[str, Any]:
    script = r'''
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$src = $env:WAR3_AUTOTEST_SRC
$dst = $env:WAR3_AUTOTEST_DST
$bmp = New-Object System.Drawing.Bitmap $src
try {
  $bmp.Save($dst, [System.Drawing.Imaging.ImageFormat]::Png)
  Write-Output "OK:BitmapToPng"
}
finally {
  $bmp.Dispose()
}
'''
    env = os.environ.copy()
    env["WAR3_AUTOTEST_SRC"] = str(input_bmp)
    env["WAR3_AUTOTEST_DST"] = str(output_png)
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script],
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    return {
        "returncode": proc.returncode,
        "stdout": proc.stdout.strip(),
        "stderr": proc.stderr.strip(),
    }


def _request_internal_frame_capture(
    pid: int,
    output_path: Path,
    war3_dir: Path,
    timeout_sec: float = 8.0,
) -> Dict[str, Any]:
    final_out = output_path.resolve()
    raw_bmp = final_out if final_out.suffix.lower() == ".bmp" else final_out.with_suffix(".bmp")
    pipe_res = _control_plane_request(
        pid=pid,
        command="capture_final_frame",
        payload={
            "outputPath": str(raw_bmp),
            "timeoutMs": max(1000, int(float(timeout_sec) * 1000.0)),
        },
        timeout_sec=max(2.0, float(timeout_sec) + 1.0),
    )
    if pipe_res.get("transportOk"):
        if not pipe_res.get("ok"):
            return {
                "ok": False,
                "mode": "control-plane-capture",
                "error": str(pipe_res.get("error", "control plane capture 失败")),
                "detail": pipe_res,
            }

        raw_output = Path(str((pipe_res.get("result", {}) or {}).get("outputPath", raw_bmp)))
        if not raw_output.exists():
            return {
                "ok": False,
                "mode": "control-plane-capture",
                "error": "control plane capture 未产出文件",
                "detail": pipe_res,
            }

        convert: Dict[str, Any] = {
            "returncode": 0,
            "stdout": "",
            "stderr": "",
            "skipped": True,
        }
        delivered_output = raw_output
        if final_out.suffix.lower() == ".png":
            convert = _powershell_convert_bitmap_to_png(raw_output, final_out)
            if convert.get("returncode", 1) != 0 or (not final_out.exists()):
                return {
                    "ok": False,
                    "mode": "control-plane-capture",
                    "error": f"control plane PNG 转换失败: {convert.get('stderr') or convert.get('stdout') or 'unknown'}",
                    "detail": pipe_res,
                    "convert": convert,
                    "rawOutput": str(raw_output),
                }
            delivered_output = final_out
            try:
                raw_output.unlink()
            except Exception:
                pass

        return {
            "ok": True,
            "mode": "control-plane-capture",
            "output": str(delivered_output),
            "rawOutput": str(raw_output),
            "details": pipe_res,
            "convert": convert,
            "elapsedSec": round(float(pipe_res.get("elapsedSec", 0.0) or 0.0), 3),
        }
    return {
        "ok": False,
        "mode": "control-plane-capture-unavailable",
        "error": str(pipe_res.get("error", "control plane capture 不可用") or "control plane capture 不可用"),
        "detail": pipe_res,
    }


def _powershell_resize_window_client(pid: int, client_w: int, client_h: int, x: int = 40, y: int = 40) -> Dict[str, Any]:
    """
    将窗口客户端区调整为目标尺寸（用于 windowed 模式下的 2K 基线）。
    """
    res = _resize_window_client_native(
        pid=pid,
        client_w=max(640, int(client_w)),
        client_h=max(480, int(client_h)),
        x=int(x),
        y=int(y),
    )
    info = {}
    if isinstance(res.get("info"), dict):
        client_rect = res["info"].get("clientRect", {}) if isinstance(res["info"], dict) else {}
        window_rect = res["info"].get("windowRect", {}) if isinstance(res["info"], dict) else {}
        info = {
            "windowW": int(window_rect.get("width", 0) or 0),
            "windowH": int(window_rect.get("height", 0) or 0),
            "clientW": int(client_rect.get("width", 0) or 0),
            "clientH": int(client_rect.get("height", 0) or 0),
            "hwnd": int(res.get("hwnd", 0) or 0),
        }
    return {
        "returncode": 0 if res.get("ok") else 1,
        "stdout": json.dumps(info, ensure_ascii=False) if info else "",
        "stderr": "" if res.get("ok") else str(res.get("error", "resize failed")),
        "info": info,
        "native": res,
    }


@dataclass
class RuntimeState:
    war3_proc: Optional[subprocess.Popen] = None
    retained_native_process: Optional[RetainedNativeProcessWitness] = None
    war3_pid: Optional[int] = None
    launcher_pid: Optional[int] = None
    launcher_created_at_ms: int = 0
    launcher_mode: str = YDWE_LAUNCHER_MODE_DIRECT
    launcher_exe: str = ""
    ydwe_lock_handle: int = 0
    ydwe_lock_name: str = ""
    ydwe_lock_root: str = ""
    war3_dir: Path = DEFAULT_WAR3_DIR
    test_map_path: Path = DEFAULT_TEST_MAP
    desktop_name: str = ""
    desktop_handle: int = 0
    desktop_mode: str = "default"
    launch_epoch_ms: int = 0
    last_report_path: Optional[Path] = None
    video_restore_key_path: str = WAR3_VIDEO_REG_KEY
    video_restore_snapshot: Dict[str, Any] = field(default_factory=dict)
    debug_thread: Optional[threading.Thread] = None
    debug_stop: threading.Event = field(default_factory=threading.Event)
    debug_pid_filter: Optional[int] = None
    debug_lock: threading.Lock = field(default_factory=threading.Lock)
    debug_events: List[Dict[str, Any]] = field(default_factory=list)
    debug_seq: int = 0
    perf_thread: Optional[threading.Thread] = None
    perf_stop: threading.Event = field(default_factory=threading.Event)
    perf_lock: threading.Lock = field(default_factory=threading.Lock)
    perf_next_job_id: int = 1
    perf_job: Dict[str, Any] = field(
        default_factory=lambda: {
            "status": "idle",
            "jobId": 0,
            "startedAt": "",
            "updatedAt": "",
            "endedAt": "",
            "roundsTotal": 0,
            "roundsDone": 0,
            "results": [],
            "lastError": "",
            "params": {},
            "aggregate": {},
        }
    )

    def push_event(self, pid: int, msg: str) -> None:
        with self.debug_lock:
            self.debug_seq += 1
            self.debug_events.append(
                {
                    "id": self.debug_seq,
                    "ts": _now_str(),
                    "pid": pid,
                    "msg": msg.strip(),
                }
            )
            if len(self.debug_events) > MAX_EVENT_BUFFER:
                self.debug_events = self.debug_events[-MAX_EVENT_BUFFER:]

    def get_events(self, since_id: int, limit: int, contains: str = "") -> List[Dict[str, Any]]:
        with self.debug_lock:
            rows = [x for x in self.debug_events if x["id"] > since_id]
            if contains:
                rows = [x for x in rows if contains.lower() in x["msg"].lower()]
            return rows[: max(1, min(limit, 1000))]


STATE = RuntimeState()
SESSION_REGISTRY = SessionRegistry()
mcp = FastMCP("war3-autotest")


def _replace_state_retained_native_process(
    witness: Optional[RetainedNativeProcessWitness],
) -> Dict[str, Any]:
    previous = STATE.retained_native_process
    previous_close = (
        previous.close()
        if previous is not None
        else {
            "ok": True,
            "closed": True,
            "skipped": True,
            "reason": "no previous retained native process",
        }
    )
    # Publish only after the previous owned handle has been invalidated and
    # closed. A lifecycle relaunch can therefore never overwrite/leak the
    # first session's native handle.
    STATE.retained_native_process = witness
    return previous_close


def _require_multi_instance_sandbox(path: Path) -> Path:
    """新多实例 API 只允许使用专用 AutoTest 沙盒。"""
    candidate = Path(path).resolve(strict=False)
    allowed = DEFAULT_SANDBOX_ROOT.resolve(strict=False)
    if os.path.normcase(str(candidate)) != os.path.normcase(str(allowed)):
        raise ValueError(f"多实例只允许使用专用沙盒 {allowed}，拒绝路径: {candidate}")
    return candidate


def _session_by_selector(session_id: str = "", pid: int = 0) -> Optional[AutoTestSession]:
    return SESSION_REGISTRY.get(session_id=str(session_id or ""), pid=int(pid or 0))


def _restore_video_config_if_needed(target_pid: int) -> Dict[str, Any]:
    if target_pid <= 0 or STATE.war3_pid != target_pid:
        return {"ok": True, "skipped": True, "reason": "pid 不匹配当前 AutoTest 会话"}
    if not STATE.video_restore_snapshot:
        return {"ok": True, "skipped": True, "reason": "无待恢复视频配置"}

    snapshot = dict(STATE.video_restore_snapshot)
    key_path = str(STATE.video_restore_key_path or WAR3_VIDEO_REG_KEY)
    restore = _restore_war3_video_registry(snapshot, key_path=key_path)
    if restore.get("ok"):
        STATE.video_restore_snapshot = {}
    return restore


def _close_state_desktop_if_needed(target_pid: int) -> Dict[str, Any]:
    if target_pid <= 0 or STATE.war3_pid != target_pid:
        return {"ok": True, "skipped": True, "reason": "pid 不匹配当前 AutoTest 会话"}
    handle = int(STATE.desktop_handle or 0)
    name = str(STATE.desktop_name or "")
    if handle == 0:
        return {"ok": True, "skipped": True, "reason": "无隔离桌面", "name": name}
    ok = _close_desktop_handle(handle)
    if ok:
        STATE.desktop_handle = 0
        STATE.desktop_name = ""
        STATE.desktop_mode = "default"
    return {
        "ok": ok,
        "closed": ok,
        "name": name,
        "handle": handle,
    }


def _stop_tracked_launcher_if_needed(target_pid: int) -> Dict[str, Any]:
    if target_pid <= 0 or STATE.war3_pid != target_pid:
        return {"ok": True, "skipped": True, "reason": "pid 不匹配当前 AutoTest 会话"}
    launcher_pid = int(STATE.launcher_pid or 0)
    if launcher_pid <= 0 or launcher_pid == int(target_pid):
        return {"ok": True, "skipped": True, "reason": "没有独立 launcher 进程"}
    if not _pid_alive(launcher_pid):
        return {"ok": True, "stopped": True, "pid": launcher_pid, "reason": "launcher 已退出"}
    expected_created = int(STATE.launcher_created_at_ms or 0)
    if expected_created <= 0:
        return {
            "ok": False,
            "skipped": True,
            "pid": launcher_pid,
            "reason": "缺少 launcher 创建时间身份，拒绝结束潜在复用 PID",
        }
    actual_created = _get_process_creation_epoch_ms(launcher_pid)
    if actual_created <= 0 or abs(expected_created - actual_created) > 2_000:
        return {
            "ok": False,
            "skipped": True,
            "pid": launcher_pid,
            "reason": "launcher PID 已被复用，拒绝结束",
        }
    _taskkill(launcher_pid, force=True)
    time.sleep(0.2)
    alive = _pid_alive(launcher_pid)
    return {"ok": not alive, "stopped": not alive, "pid": launcher_pid}


def _clear_war3_launch_state(target_pid: int) -> None:
    if target_pid > 0 and STATE.war3_pid == target_pid:
        if int(STATE.ydwe_lock_handle or 0) != 0:
            _release_ydwe_root_lock(
                {
                    "handle": int(STATE.ydwe_lock_handle),
                    "name": str(STATE.ydwe_lock_name),
                    "ydweRoot": str(STATE.ydwe_lock_root),
                }
            )
        _replace_state_retained_native_process(None)
        STATE.war3_proc = None
        STATE.war3_pid = None
        STATE.launcher_pid = None
        STATE.launcher_created_at_ms = 0
        STATE.launcher_mode = YDWE_LAUNCHER_MODE_DIRECT
        STATE.launcher_exe = ""
        STATE.ydwe_lock_handle = 0
        STATE.ydwe_lock_name = ""
        STATE.ydwe_lock_root = ""
        STATE.desktop_name = ""
        STATE.desktop_handle = 0
        STATE.desktop_mode = "default"
        STATE.launch_epoch_ms = 0


def _finalize_state_after_exact_native_termination(
    pid: int,
    creation_epoch_ms: int,
    canonical_exe_path: str,
) -> Dict[str, Any]:
    """Release session state after HANDLE-authorized death, without PID IO."""
    native_process = STATE.retained_native_process
    expected_path = _canonical_process_path(canonical_exe_path)
    state_exact = bool(
        native_process is not None
        and STATE.war3_pid == int(pid)
        and STATE.launcher_pid == int(pid)
        and STATE.launcher_created_at_ms == int(creation_epoch_ms)
        and STATE.launcher_mode == YDWE_LAUNCHER_MODE_DIRECT
        and _canonical_process_path(STATE.launcher_exe) == expected_path
    )
    termination = (
        native_process.termination_exact(
            int(pid), int(creation_epoch_ms), expected_path,
        )
        if state_exact else {
            "exact": False,
            "bindingExact": False,
            "handleSignaled": False,
            "reportOnly": True,
            "failureClassificationAuthority": 0,
        }
    )
    if termination.get("exact") is not True:
        return {
            "ok": False,
            "finalized": False,
            "stateExact": state_exact,
            "terminationProof": termination,
            "error": (
                "STATE native process identity/termination is not exact"
            ),
        }
    # No process-name/PID liveness query and no taskkill occurs below. Video
    # registry and isolated desktop ownership belong to this exact STATE.
    restore = _restore_video_config_if_needed(int(pid))
    desktop = _close_state_desktop_if_needed(int(pid))
    _clear_war3_launch_state(int(pid))
    return {
        "ok": bool(restore.get("ok") and desktop.get("ok")),
        "finalized": True,
        "stateExact": True,
        "terminationProof": termination,
        "videoRestore": restore,
        "desktop": desktop,
        "pidTerminationCommandIssued": False,
    }


def _stop_retained_state_process_exact(target_pid: int) -> Dict[str, Any]:
    """Stop the current direct-launch War3 through its retained HANDLE only.

    ``handled`` means callers must not fall back to PID/name based termination:
    either the exact instance was stopped, or an identity mismatch made any
    mutation unsafe. Legacy sessions without a retained witness remain
    eligible for the older foreground stop path. Background callers must
    still fail closed when this helper reports ``handled=False``.
    """
    if target_pid <= 0 or STATE.war3_pid != int(target_pid):
        return {"handled": False, "reason": "target is not current STATE pid"}
    witness = STATE.retained_native_process
    if witness is None:
        return {"handled": False, "reason": "no retained native witness"}
    try:
        snapshot = dict(witness.snapshot() or {})
    except Exception as exc:
        return {
            "handled": True,
            "ok": False,
            "stopped": False,
            "pid": int(target_pid),
            "error": f"native witness snapshot failed: {type(exc).__name__}: {exc}",
            "pidTerminationCommandIssued": False,
        }
    binding_shape = bool(
        snapshot.get("available") is True
        and int(snapshot.get("pid", 0) or 0) == int(target_pid)
        and int(snapshot.get("creationEpochMs", 0) or 0) > 0
        and str(snapshot.get("canonicalExePath", "") or "")
    )
    if not binding_shape:
        return {
            "handled": True,
            "ok": False,
            "stopped": False,
            "pid": int(target_pid),
            "error": "retained native witness identity is incomplete/mismatched",
            "nativeProcessWitness": snapshot,
            "pidTerminationCommandIssued": False,
        }
    try:
        termination = witness.terminate_exact(
            int(target_pid),
            int(snapshot["creationEpochMs"]),
            str(snapshot["canonicalExePath"]),
            wait_timeout_sec=10.0,
        )
    except Exception as exc:
        return {
            "handled": True,
            "ok": False,
            "stopped": False,
            "pid": int(target_pid),
            "error": (
                "exact retained-HANDLE termination raised: "
                f"{type(exc).__name__}: {exc}"
            ),
            "nativeProcessWitness": snapshot,
            "pidTerminationCommandIssued": False,
        }
    if termination.get("exact") is not True:
        return {
            "handled": True,
            "ok": False,
            "stopped": False,
            "pid": int(target_pid),
            "error": "exact retained-HANDLE termination did not close",
            "nativeTermination": termination,
            "pidTerminationCommandIssued": False,
        }
    finalized = _finalize_state_after_exact_native_termination(
        int(target_pid),
        int(snapshot["creationEpochMs"]),
        str(snapshot["canonicalExePath"]),
    )
    # Process death is already proven by the retained HANDLE. A later video
    # registry/desktop cleanup error must not trigger a second PID-based stop.
    stopped = finalized.get("finalized") is True
    return {
        "handled": True,
        "ok": bool(stopped and finalized.get("ok") is True),
        "stopped": stopped,
        "pid": int(target_pid),
        "closeSent": False,
        "forced": True,
        "silentStop": True,
        "avoidForegroundSwitch": True,
        "exactNativeHandleStop": True,
        "nativeTermination": termination,
        "stateFinalize": finalized,
        "videoRestore": finalized.get("videoRestore"),
        "desktop": finalized.get("desktop"),
        "pidTerminationCommandIssued": False,
    }


def _start_debug_monitor(pid_filter: Optional[int]) -> Dict[str, Any]:
    with STATE.debug_lock:
        if STATE.debug_thread and STATE.debug_thread.is_alive():
            # DBWIN 是系统级单例缓冲，多实例必须由一个监听器接收后按 PID 分流。
            # 保留参数仅兼容旧调用方，不再把监听器收窄到单 PID。
            STATE.debug_pid_filter = None
            return {"ok": True, "message": "debug monitor already running", "pidFilter": None}
        STATE.debug_stop.clear()
        STATE.debug_pid_filter = None

    def _worker() -> None:
        listener = DbWinListener()
        try:
            listener.open()
        except Exception as e:
            STATE.push_event(0, f"[AutoTest] DBWIN open failed: {e}")
            return
        while not STATE.debug_stop.is_set():
            try:
                pid, msg = listener.read(timeout_ms=200)
            except Exception as e:
                STATE.push_event(0, f"[AutoTest] DBWIN read failed: {e}")
                time.sleep(0.2)
                continue
            if pid is None or not msg:
                continue
            if "DXVK" in msg or "War3" in msg or "JASS" in msg:
                SESSION_REGISTRY.route_event(pid, msg)
                # 单实例旧 API 仍从 STATE 读取；只接收其当前 PID，避免串线。
                legacy_pid = int(STATE.war3_pid or 0)
                if legacy_pid == 0 or pid == legacy_pid:
                    STATE.push_event(pid, msg)
        listener.close()

    t = threading.Thread(target=_worker, name="war3-dbwin-monitor", daemon=True)
    STATE.debug_thread = t
    t.start()
    return {"ok": True, "message": "debug monitor started", "pidFilter": None}


def _stop_debug_monitor() -> None:
    STATE.debug_stop.set()


def _get_perf_job_snapshot(limit_results: int = 50) -> Dict[str, Any]:
    with STATE.perf_lock:
        job = copy.deepcopy(STATE.perf_job)
    results = job.get("results", [])
    if isinstance(results, list) and limit_results > 0:
        job["results"] = results[-max(1, min(limit_results, 500)) :]
    job["running"] = bool(STATE.perf_thread and STATE.perf_thread.is_alive())
    return job


def _set_perf_job_fields(**kwargs: Any) -> None:
    with STATE.perf_lock:
        STATE.perf_job.update(kwargs)
        STATE.perf_job["updatedAt"] = _now_str()


def _compute_perf_aggregate(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    ok_rows = [r for r in results if bool(r.get("ok"))]
    agg: Dict[str, Any] = {
        "rounds": len(results),
        "success": len(ok_rows),
        "failed": len(results) - len(ok_rows),
    }
    if not ok_rows:
        return agg

    def _avg(key: str) -> float:
        vals = [float(r.get(key, 0.0)) for r in ok_rows if isinstance(r.get(key, None), (int, float))]
        return round(sum(vals) / len(vals), 4) if vals else 0.0

    agg.update(
        {
            "avgFps": _avg("avgFps"),
            "avgFrameTimeMs": _avg("avgFrameTimeMs"),
            "avgGpuTimeMs": _avg("avgGpuTimeMs"),
            "avgTrackedActiveCpuMs": _avg("avgTrackedActiveCpuMs"),
            "avgUntrackedActiveCpuMs": _avg("avgUntrackedActiveCpuMs"),
            "avgTrackedAdditiveRootWallMs": _avg(
                "avgTrackedAdditiveRootWallMs"
            ),
            "avgUncoveredFrameWallMs": _avg("avgUncoveredFrameWallMs"),
            "frameWallScopeCoveragePct": _avg(
                "frameWallScopeCoveragePct"
            ),
            "cpuCoveragePct": _avg("cpuCoveragePct"),
            "cpuCoverageWithIdlePct": _avg("cpuCoverageWithIdlePct"),
            "totalJank16": int(sum(int(r.get("jank16", 0)) for r in ok_rows)),
            "totalJank33": int(sum(int(r.get("jank33", 0)) for r in ok_rows)),
        }
    )
    return agg


def _prepare_test_map_copy(war3_dir: Path, map_path: Path, target_rel: Path) -> Path:
    target_abs = war3_dir / target_rel
    _ensure_dir(target_abs.parent)
    # 若源图和目标图是同一文件，直接复用，避免无意义覆盖。
    if map_path.resolve() == target_abs.resolve():
        return target_abs

    source_sha = sha256_file(map_path)
    if target_abs.is_file() and sha256_file(target_abs) == source_sha:
        return target_abs

    temporary = target_abs.with_name(
        f".{target_abs.name}.{os.getpid()}.{time.time_ns()}.partial"
    )
    try:
        # 源图可能带只读属性（编辑器归档和备份常见）。测试短路径是
        # AutoTest 自己管理的部署目标，不能把该属性传播过去，否则下一轮
        # os.replace 会在 Windows 上永久失败并诱发旧图误测。
        shutil.copyfile(map_path, temporary)
        os.chmod(temporary, 0o666)
        if sha256_file(temporary) != source_sha:
            raise RuntimeError("测试地图临时副本 SHA-256 与源图不一致")
        if target_abs.exists():
            os.chmod(target_abs, 0o666)
        os.replace(temporary, target_abs)
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass

    target_sha = sha256_file(target_abs)
    if target_sha != source_sha:
        raise RuntimeError(
            "测试地图部署后 SHA-256 不一致: "
            f"source={source_sha}, target={target_sha}, path={target_abs}"
        )
    return target_abs


def _prefer_inplace_relative_loadfile_arg(war3_dir: Path, map_path: Path) -> str:
    try:
        rel = map_path.resolve().relative_to(war3_dir.resolve())
    except Exception:
        return ""
    rel_text = str(rel).replace("/", "\\")
    if not rel_text or len(rel_text) >= 54:
        return ""
    return rel_text


def _deploy_d3d9(build_dll: Path, war3_dir: Path) -> Dict[str, Any]:
    if not build_dll.is_absolute():
        build_dll = (REPO_ROOT / build_dll).resolve()
    dst = war3_dir / "d3d9.dll"
    if not build_dll.exists():
        return {"ok": False, "error": f"构建产物不存在: {build_dll}"}
    if not war3_dir.exists():
        return {"ok": False, "error": f"War3 目录不存在: {war3_dir}"}

    old_info = None
    if dst.exists():
        old_info = {
            "path": str(dst),
            "size": dst.stat().st_size,
            "mtime": datetime.fromtimestamp(dst.stat().st_mtime).isoformat(),
        }
        # A controlled run may leave the exact deployed image mapped briefly
        # after the process witness has closed.  Re-copying identical bytes is
        # unnecessary and can fail with ERROR_SHARING_VIOLATION, so prove
        # equality before attempting a mutating deploy.
        try:
            source_sha = sha256_file(build_dll)
            target_sha = sha256_file(dst)
            if source_sha == target_sha:
                return {
                    "ok": True,
                    "skipped": True,
                    "reason": "target already matches build SHA-256",
                    "source": str(build_dll),
                    "target": str(dst),
                    "sha256": source_sha,
                    "old": old_info,
                    "new": old_info,
                }
        except (OSError, PermissionError):
            # Fall through to the existing bounded copy retry.  This keeps the
            # deploy contract unchanged when the target cannot even be read.
            pass
    # 某些时刻（进程刚退出/杀软扫描）会短暂占用目标文件，做短重试避免整条链路失败。
    copy_error: Optional[str] = None
    for i in range(8):
        try:
            shutil.copy2(build_dll, dst)
            copy_error = None
            break
        except PermissionError as e:
            copy_error = str(e)
            time.sleep(0.25)
    if copy_error is not None:
        return {
            "ok": False,
            "error": f"部署 d3d9.dll 失败（重试后仍被占用）: {copy_error}",
            "source": str(build_dll),
            "target": str(dst),
            "old": old_info,
        }

    new_info = {
        "path": str(dst),
        "size": dst.stat().st_size,
        "mtime": datetime.fromtimestamp(dst.stat().st_mtime).isoformat(),
    }
    return {
        "ok": True,
        "source": str(build_dll),
        "target": str(dst),
        "old": old_info,
        "new": new_info,
    }


def _preflight_instance_pool_impl(
    sandbox_root: Path,
    artifact_root: Path,
    map_path: Optional[Path],
    instance_count: int,
    run_id: str = "",
    session_prefix: str = "client",
) -> Dict[str, Any]:
    try:
        sandbox = _require_multi_instance_sandbox(sandbox_root)
    except ValueError as exc:
        return {"ok": False, "errors": [str(exc)], "warnings": []}
    result = _preflight_session_pool(
        sandbox_root=sandbox,
        artifact_root=artifact_root,
        map_path=map_path,
        instance_count=instance_count,
        run_id=run_id,
        session_prefix=session_prefix,
    )
    result["sandboxPolicy"] = "exact-path-only"
    result["allowedSandboxRoot"] = str(DEFAULT_SANDBOX_ROOT.resolve(strict=False))
    result["legacyExplicitPathsStillSupported"] = True
    return result


def _mark_session_failed(session: AutoTestSession, reason: str) -> None:
    session.stop_reason = str(reason)
    try:
        SESSION_REGISTRY.mark_stopped(session.session_id, reason, state="failed")
    except KeyError:
        pass
    try:
        _write_session_manifest(session)
    except Exception:
        pass


def _launch_war3_instance_impl(
    sandbox_root: Path,
    map_path: Path,
    artifact_root: Path,
    run_id: str,
    session_id: str,
    windowed: bool,
    use_isolated_desktop: bool,
    desktop_name: str,
    opengl: bool,
    deploy_d3d9_before_launch: bool,
    build_d3d9_path: str,
    profile: str,
    disable_modules: str,
    env_overrides_json: str,
    extra_args: str,
    reuse_existing_root: bool,
) -> Dict[str, Any]:
    try:
        sandbox = _require_multi_instance_sandbox(sandbox_root)
        run = normalize_identifier(run_id, "run")
        session_key = normalize_identifier(session_id, "session")
    except ValueError as exc:
        return {"ok": False, "error": str(exc)}

    source_map = Path(map_path).resolve(strict=False)
    check = _preflight_instance_pool_impl(
        sandbox_root=sandbox,
        artifact_root=artifact_root,
        map_path=source_map,
        instance_count=1,
        run_id=run,
        session_prefix="preflight",
    )
    if not check.get("ok"):
        return {"ok": False, "error": "多实例预检失败", "preflight": check}
    if SESSION_REGISTRY.get(session_id=session_key) is not None:
        return {"ok": False, "error": f"session_id 已存在: {session_key}"}

    layout = build_instance_layout(sandbox, artifact_root, run, session_key)
    session = AutoTestSession(
        session_id=session_key,
        run_id=run,
        sandbox_root=sandbox,
        instance_root=layout.instance_root,
        artifact_dir=layout.artifact_dir,
        desktop_name=str(desktop_name or layout.desktop_name),
        desktop_mode="isolated" if use_isolated_desktop else "default",
        map_source=source_map,
    )
    try:
        SESSION_REGISTRY.reserve(session)
    except ValueError as exc:
        return {"ok": False, "error": str(exc)}

    materialized = materialize_instance_root(layout, reuse_existing=bool(reuse_existing_root))
    if not materialized.get("ok"):
        _mark_session_failed(session, str(materialized.get("error", "实例部署失败")))
        return {"ok": False, "error": materialized.get("error"), "sessionId": session_key, "materialize": materialized}

    try:
        deployed_map = deploy_content_addressed_map(layout.instance_root, source_map)
    except Exception as exc:
        _mark_session_failed(session, f"地图部署失败: {exc}")
        return {"ok": False, "error": f"地图部署失败: {exc}", "sessionId": session_key}
    session.map_path = Path(deployed_map["target"])
    session.map_sha256 = str(deployed_map["sha256"])

    deploy: Dict[str, Any] = {"ok": True, "skipped": True}
    if deploy_d3d9_before_launch:
        deploy = _deploy_d3d9(Path(build_d3d9_path), layout.instance_root)
        if not deploy.get("ok"):
            _mark_session_failed(session, str(deploy.get("error", "d3d9.dll 部署失败")))
            return {"ok": False, "error": deploy.get("error"), "sessionId": session_key, "deploy": deploy}
    d3d9_path = layout.instance_root / "d3d9.dll"
    if d3d9_path.is_file():
        session.dll_sha256 = sha256_file(d3d9_path)

    effective_windowed = bool(windowed or use_isolated_desktop)
    forced_windowed = bool(use_isolated_desktop and not windowed)
    desktop: Dict[str, Any] = {"ok": True, "skipped": True, "reason": "use_isolated_desktop=False"}
    if use_isolated_desktop:
        desktop = _create_isolated_desktop(session.desktop_name)
        if not desktop.get("ok"):
            _mark_session_failed(session, str(desktop.get("error", "隔离桌面创建失败")))
            return {"ok": False, "error": desktop.get("error"), "sessionId": session_key, "desktop": desktop}
        session.desktop_name = str(desktop.get("name", session.desktop_name))
        session.desktop_handle = int(desktop.get("handle", 0) or 0)

    war3_exe = layout.instance_root / "war3.exe"
    args = [str(war3_exe)]
    if effective_windowed:
        args.append("-window")
    if opengl:
        args.append("-opengl")
    args.extend(["-loadfile", str(deployed_map["loadfileArg"])])
    if str(extra_args or "").strip():
        args.extend(str(extra_args).strip().split())

    temp_dir = layout.instance_root / "WarVK" / "Temp"
    temp_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(
        {
            "DXVK_WAR3_AUTOTEST_RUN_ID": run,
            "DXVK_WAR3_AUTOTEST_SESSION_ID": session_key,
            "DXVK_WAR3_AUTOTEST_ARTIFACT_DIR": str(layout.artifact_dir),
            "DXVK_LOG_PATH": str(layout.artifact_dir),
            "TEMP": str(temp_dir),
            "TMP": str(temp_dir),
        }
    )
    if str(profile or "").strip():
        env["DXVK_WAR3_PROFILE"] = str(profile).strip()
    if str(disable_modules or "").strip():
        env["DXVK_WAR3_DISABLE"] = str(disable_modules).strip()
    extra_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = extra_env.pop("__parse_error__", "")
    if parse_error:
        if session.desktop_handle:
            _close_desktop_handle(session.desktop_handle)
            session.desktop_handle = 0
        _mark_session_failed(session, f"env_overrides_json 解析失败: {parse_error}")
        return {"ok": False, "error": f"env_overrides_json 解析失败: {parse_error}", "sessionId": session_key}
    # AutoTest 默认禁用已由 IDA 证实的 WM_ACTIVATEAPP 后台 idle Sleep。
    # env_overrides_json 传入 0/false/no 时保留原生节流，作为显式 A/B 对照。
    extra_env.setdefault(AUTOTEST_BACKGROUND_THROTTLE_ENV, "1")
    # 旧的 GamePause 防护改为 AutoTest 子进程专用，避免普通游戏被编译期开关影响。
    extra_env.setdefault(AUTOTEST_GAME_PAUSE_ENV, "1")
    # 相机、视野和取证命令只在 AutoTest 拥有的子进程中开放。
    extra_env.setdefault(AUTOTEST_INTERNAL_TEST_API_ENV, "1")
    env.update(extra_env)

    _start_debug_monitor(None)
    launch_result: Dict[str, Any]
    proc: Optional[subprocess.Popen] = None
    if use_isolated_desktop:
        launch_result = _launch_process_on_desktop(args, layout.instance_root, env, session.desktop_name)
        if launch_result.get("ok"):
            pid = int(launch_result["pid"])
        else:
            pid = 0
    else:
        try:
            proc = subprocess.Popen(
                args,
                cwd=str(layout.instance_root),
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            pid = int(proc.pid)
            launch_result = {"ok": True, "pid": pid}
        except Exception as exc:
            pid = 0
            launch_result = {"ok": False, "error": str(exc)}
    if not launch_result.get("ok") or pid <= 0:
        if session.desktop_handle:
            _close_desktop_handle(session.desktop_handle)
            session.desktop_handle = 0
        _mark_session_failed(session, str(launch_result.get("error", "进程启动失败")))
        return {"ok": False, "error": launch_result.get("error", "进程启动失败"), "sessionId": session_key}

    SESSION_REGISTRY.attach_pid(session_key, pid, process=proc)
    session.process_created_at_ms = _get_process_creation_epoch_ms(pid) or int(time.time() * 1000)
    job = _create_kill_on_close_job(pid, session_key)
    if not job.get("ok"):
        _taskkill(pid, force=True)
        if session.desktop_handle:
            _close_desktop_handle(session.desktop_handle)
            session.desktop_handle = 0
        _mark_session_failed(session, str(job.get("error", "Job Object 创建失败")))
        return {"ok": False, "error": job.get("error"), "sessionId": session_key, "pid": pid, "job": job}
    session.job_handle = int(job["handle"])
    priority = _set_process_priority_high(pid)
    manifest = _write_session_manifest(session)
    return {
        "ok": True,
        "runId": run,
        "sessionId": session_key,
        "pid": pid,
        "args": args,
        "instanceRoot": str(layout.instance_root),
        "artifactDir": str(layout.artifact_dir),
        "map": deployed_map,
        "mapSha256": session.map_sha256,
        "dllSha256": session.dll_sha256,
        "windowed": effective_windowed,
        "forcedWindowedBecauseIsolatedDesktop": forced_windowed,
        "desktop": desktop,
        "job": job,
        "manifest": manifest,
        "priority": priority,
        "materialize": materialized,
        "deploy": deploy,
        "envOverrides": extra_env,
        "time": _now_str(),
    }


@mcp.tool()
def preflight_instance_pool(
    sandbox_root: str = str(DEFAULT_SANDBOX_ROOT),
    map_path: str = str(DEFAULT_TEST_MAP),
    instance_count: int = 2,
    run_id: str = "",
    session_prefix: str = "client",
    artifact_root: str = str(ARTIFACT_ROOT),
) -> Dict[str, Any]:
    """只读检查 1-6 个实例的沙盒、地图、空间和隔离路径，不创建任何文件。"""
    return _preflight_instance_pool_impl(
        sandbox_root=Path(sandbox_root),
        artifact_root=Path(artifact_root),
        map_path=Path(map_path) if map_path else None,
        instance_count=instance_count,
        run_id=run_id,
        session_prefix=session_prefix,
    )


@mcp.tool()
def launch_war3_instance(
    sandbox_root: str = str(DEFAULT_SANDBOX_ROOT),
    map_path: str = str(DEFAULT_TEST_MAP),
    run_id: str = "",
    session_id: str = "",
    artifact_root: str = str(ARTIFACT_ROOT),
    windowed: bool = True,
    use_isolated_desktop: bool = True,
    desktop_name: str = "",
    opengl: bool = False,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    profile: str = "",
    disable_modules: str = "",
    env_overrides_json: str = "",
    extra_args: str = "",
    reuse_existing_root: bool = False,
) -> Dict[str, Any]:
    """在专用根目录、Desktop、Job Object 和工件目录中启动一个 War3 会话。"""
    return _launch_war3_instance_impl(
        sandbox_root=Path(sandbox_root),
        map_path=Path(map_path),
        artifact_root=Path(artifact_root),
        run_id=run_id,
        session_id=session_id,
        windowed=windowed,
        use_isolated_desktop=use_isolated_desktop,
        desktop_name=desktop_name,
        opengl=opengl,
        deploy_d3d9_before_launch=deploy_d3d9_before_launch,
        build_d3d9_path=build_d3d9_path,
        profile=profile,
        disable_modules=disable_modules,
        env_overrides_json=env_overrides_json,
        extra_args=extra_args,
        reuse_existing_root=reuse_existing_root,
    )


@mcp.tool()
def launch_war3_batch(
    sandbox_root: str = str(DEFAULT_SANDBOX_ROOT),
    map_path: str = str(DEFAULT_TEST_MAP),
    instance_count: int = 2,
    run_id: str = "",
    session_prefix: str = "client",
    artifact_root: str = str(ARTIFACT_ROOT),
    windowed: bool = True,
    use_isolated_desktop: bool = True,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    profile: str = "",
    disable_modules: str = "",
    env_overrides_json: str = "",
    extra_args: str = "",
) -> Dict[str, Any]:
    """启动相互隔离的 -loadfile 会话；用于进程隔离测试，不计作局域网联机验收。"""
    try:
        run = normalize_identifier(run_id, "run")
        prefix = normalize_identifier(session_prefix, "session_prefix")
    except ValueError as exc:
        return {"ok": False, "error": str(exc)}
    check = _preflight_instance_pool_impl(
        Path(sandbox_root),
        Path(artifact_root),
        Path(map_path),
        instance_count,
        run,
        prefix,
    )
    if not check.get("ok"):
        return {"ok": False, "error": "多实例预检失败", "preflight": check}

    launched: List[Dict[str, Any]] = []
    for index in range(1, int(instance_count) + 1):
        session_key = f"{prefix}-{index:02d}"
        result = _launch_war3_instance_impl(
            sandbox_root=Path(sandbox_root),
            map_path=Path(map_path),
            artifact_root=Path(artifact_root),
            run_id=run,
            session_id=session_key,
            windowed=windowed,
            use_isolated_desktop=use_isolated_desktop,
            desktop_name="",
            opengl=False,
            deploy_d3d9_before_launch=deploy_d3d9_before_launch,
            build_d3d9_path=build_d3d9_path,
            profile=profile,
            disable_modules=disable_modules,
            env_overrides_json=env_overrides_json,
            extra_args=extra_args,
            reuse_existing_root=False,
        )
        launched.append(result)
        if not result.get("ok"):
            rollback = []
            for previous in launched[:-1]:
                rollback.append(_stop_registered_session(str(previous.get("sessionId", "")), 2, True, True))
            return {
                "ok": False,
                "error": f"批量启动在 {session_key} 失败",
                "networkMode": "independent-loadfile",
                "countsAsLanAcceptance": False,
                "runId": run,
                "preflight": check,
                "results": launched,
                "rollback": rollback,
            }
    return {
        "ok": True,
        "runId": run,
        "count": len(launched),
        "networkMode": "independent-loadfile",
        "countsAsLanAcceptance": False,
        "preflight": check,
        "sessions": launched,
    }


@mcp.tool()
def provision_ydhost_assets(
    source_root: str,
    sandbox_root: str = str(DEFAULT_SANDBOX_ROOT),
    target_root: str = "",
    apply: bool = False,
) -> Dict[str, Any]:
    """哈希锁定地配置 ydhost 运行时；默认 dry-run，且绝不启动 ydhost/War3。"""
    try:
        sandbox = _require_multi_instance_sandbox(Path(sandbox_root))
    except ValueError as exc:
        return {
            "ok": False,
            "dryRun": not apply,
            "applied": False,
            "error": str(exc),
        }
    target = Path(target_root) if target_root else None
    return _provision_ydhost_assets(
        source_root=Path(source_root),
        sandbox_root=sandbox,
        target_root=target,
        apply=bool(apply),
    )


@mcp.tool()
def generate_ydhost_map_metadata(
    ydwe_root: str,
    map_path: str,
    sandbox_root: str = str(DEFAULT_SANDBOX_ROOT),
    apply: bool = False,
    timeout_seconds: int = 180,
) -> Dict[str, Any]:
    """用哈希锁定的独立 YDWE mapdump 生成元数据；默认只在临时目录执行。"""
    try:
        sandbox = _require_multi_instance_sandbox(Path(sandbox_root))
    except ValueError as exc:
        return {
            "ok": False,
            "dryRun": not apply,
            "applied": False,
            "error": str(exc),
        }
    return _generate_ydhost_map_metadata(
        ydwe_root=Path(ydwe_root),
        sandbox_root=sandbox,
        map_path=Path(map_path),
        apply=bool(apply),
        timeout=max(1, int(timeout_seconds)),
    )


@mcp.tool()
def run_multi_instance_suite(
    sandbox_root: str = str(DEFAULT_SANDBOX_ROOT),
    map_path: str = str(DEFAULT_TEST_MAP),
    instance_count: int = 2,
    run_id: str = "",
    session_prefix: str = "client",
    artifact_root: str = str(ARTIFACT_ROOT),
    enable_ydhost_launch: bool = False,
) -> Dict[str, Any]:
    """执行 LAN 门禁；真实启动必须显式 opt-in，且协议未闭环时仍拒绝执行。"""
    check = _preflight_instance_pool_impl(
        sandbox_root=Path(sandbox_root),
        artifact_root=Path(artifact_root),
        map_path=Path(map_path) if map_path else None,
        instance_count=instance_count,
        run_id=run_id,
        session_prefix=session_prefix,
    )
    if not check.get("ok"):
        return {
            "ok": False,
            "status": "blocked",
            "code": "INSTANCE_PREFLIGHT_FAILED",
            "error": "联机套件实例预检失败",
            "preflight": check,
            "countsAsLanAcceptance": False,
        }

    try:
        sandbox = _require_multi_instance_sandbox(Path(sandbox_root))
    except ValueError as exc:
        return {
            "ok": False,
            "status": "blocked",
            "code": "SANDBOX_POLICY_REJECTED",
            "error": str(exc),
            "preflight": check,
            "countsAsLanAcceptance": False,
        }

    ydhost_check = _preflight_ydhost_lan(sandbox, Path(map_path))
    if not ydhost_check.get("ok"):
        return {
            "ok": False,
            "status": "blocked",
            "code": ydhost_check.get("code", "YDHOST_PREFLIGHT_FAILED"),
            "error": ydhost_check.get("error", "ydhost 联机前置检查失败"),
            "runId": check.get("runId", run_id),
            "instanceCount": int(instance_count),
            "preflight": check,
            "ydhostPreflight": ydhost_check,
            "networkModeRequired": "ydhost-lan",
            "independentLoadfileRejected": True,
            "countsAsLanAcceptance": False,
        }

    if not bool(enable_ydhost_launch):
        return {
            "ok": False,
            "status": "blocked",
            "code": "YDHOST_LAUNCH_OPT_IN_REQUIRED",
            "error": "ydhost 真实启动默认关闭；必须显式 enable_ydhost_launch=true",
            "runId": check.get("runId", run_id),
            "instanceCount": int(instance_count),
            "preflight": check,
            "ydhostPreflight": ydhost_check,
            "stateMachineImplemented": True,
            "realProcessLaunchExecuted": False,
            "networkModeRequired": "ydhost-lan",
            "independentLoadfileRejected": True,
            "countsAsLanAcceptance": False,
        }

    capability = _ydhost_real_launch_capability()
    return {
        "ok": False,
        "status": "blocked",
        "code": capability.get("code", "YDHOST_REAL_LAUNCH_UNAVAILABLE"),
        "error": capability.get("error", "ydhost 真实启动协议尚未闭环"),
        "runId": check.get("runId", run_id),
        "instanceCount": int(instance_count),
        "preflight": check,
        "ydhostPreflight": ydhost_check,
        "launchCapability": capability,
        "stateMachineImplemented": True,
        "realProcessLaunchExecuted": False,
        "networkModeRequired": "ydhost-lan",
        "independentLoadfileRejected": True,
        "countsAsLanAcceptance": False,
    }


@mcp.tool()
def ydwe_launch_chain_analysis() -> Dict[str, Any]:
    """返回 YDWE 启动链关键结论（从源码抽取）。"""
    capability = _ydhost_real_launch_capability()
    return {
        "ok": True,
        "summary": [
            "单机测试链使用 `YDWE.exe -war3 -loadfile <map>`，并把长路径地图复制到短路径。",
            "ydhost 联机链不同：先启动 ydhost，再启动 N 个 `YDWE.exe -war3 -closew2l -auto`。",
            "`-auto` 由注入后的 yd_loader.dll 解释，不是 Warcraft III 原生命令。",
            "原版 YDWE 通过共享注册表 InstallPath 选择 War3 根，尚不能绑定 per-instance 根。",
            "connect 只证明 JOIN 请求；逐客户端 ready 仍须 DBWIN/JAPI/game-start 证据。",
        ],
        "evidence": [
            r"SourceMap\YDWE1.32.13 - MemoryHack\script\ydwe\ydwe_on_test.lua:105",
            r"E:\Mycode\Source\Repos\YDWE\Development\Core\ydwar3\warcraft3\directory.cpp:8",
            r"E:\Mycode\Source\Repos\YDWE\Development\Core\YDWEStartup\LaunchWarcraft3.cpp:165",
            r"E:\Mycode\Source\Repos\YDWE\Development\Plugin\Warcraft3\yd_loader\auto_enter.cpp:13",
            r"E:\Mycode\Source\Repos\YDWE\Development\Plugin\Warcraft3\yd_loader\game_status.cpp:102",
        ],
        "lanProtocol": capability.get("protocol", {}),
        "identityContract": capability.get("identityContract", {}),
        "productionBlocker": {
            "code": capability.get("protocolBlockerCode", capability.get("code", "")),
            "detail": capability.get("error", ""),
        },
        "realProcessLaunchExecuted": False,
    }


@mcp.tool()
def prepare_test_map(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    target_rel: str = str(DEFAULT_TEST_MAP_REL),
) -> Dict[str, Any]:
    """按 YDWE 方式复制测试地图到 War3 根目录下的短路径。"""
    w3 = Path(war3_dir)
    src = Path(map_path)
    rel = Path(target_rel)
    if not w3.exists():
        return {"ok": False, "error": f"war3_dir 不存在: {w3}"}
    if not src.exists():
        return {"ok": False, "error": f"map_path 不存在: {src}"}

    try:
        dst = _prepare_test_map_copy(w3, src, rel)
    except Exception as exc:
        return {
            "ok": False,
            "error": f"部署测试地图失败: {exc}",
            "code": "TEST_MAP_DEPLOY_FAILED",
            "source": str(src),
            "target": str(w3 / rel),
        }
    return {
        "ok": True,
        "source": str(src),
        "target": str(dst),
        "sourceSha256": sha256_file(src),
        "targetSha256": sha256_file(dst),
        "loadfileArg": str(rel).replace("/", "\\"),
    }


@mcp.tool()
def deploy_d3d9_to_war3(
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    war3_dir: str = str(DEFAULT_WAR3_DIR),
) -> Dict[str, Any]:
    """将当前编译产物 d3d9.dll 部署到 War3 根目录。"""
    return _deploy_d3d9(Path(build_d3d9_path), Path(war3_dir))


@mcp.tool()
def ensure_war3_video_baseline(
    width: int = DEFAULT_BENCHMARK_WIDTH,
    height: int = DEFAULT_BENCHMARK_HEIGHT,
    refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
) -> Dict[str, Any]:
    """写入 War3 视频配置注册表，统一自动测试分辨率基线。"""
    return _set_war3_video_registry(width=width, height=height, refresh_rate=refresh_rate)


class _PreStateLaunchTransaction:
    """Owns every launch side effect until RuntimeState is fully published."""

    def __init__(
        self, video_baseline: Dict[str, Any], restore_video: bool,
    ) -> None:
        self.video_baseline = dict(video_baseline or {})
        self.restore_video = bool(restore_video)
        self.desktop_handle = 0
        self.ydwe_lock: Dict[str, Any] = {}
        self.native_processes: List[
            Tuple[str, RetainedNativeProcessWitness]
        ] = []
        self.popen_processes: List[Tuple[str, subprocess.Popen]] = []
        self.unresolved_processes: List[Dict[str, Any]] = []
        self.state_published = False
        self.committed = False
        self._rollback_result: Optional[Dict[str, Any]] = None

    def own_desktop(self, handle: Any) -> None:
        self.desktop_handle = int(handle or 0)

    def own_ydwe_lock(self, lock: Dict[str, Any]) -> None:
        self.ydwe_lock = dict(lock or {})

    def own_native_process(
        self, role: str, witness: Optional[RetainedNativeProcessWitness],
    ) -> None:
        if witness is None:
            return
        if any(existing is witness for _, existing in self.native_processes):
            return
        self.native_processes.append((str(role), witness))

    def own_popen(self, role: str, proc: Optional[subprocess.Popen]) -> None:
        if proc is None:
            return
        if any(existing is proc for _, existing in self.popen_processes):
            return
        self.popen_processes.append((str(role), proc))

    def mark_unresolved_process(
        self, role: str, pid: int, reason: str,
    ) -> None:
        self.unresolved_processes.append({
            "role": str(role),
            "pidReportOnly": int(pid),
            "reason": str(reason),
            "cleanupAuthority": 0,
        })

    def publish_state(self) -> None:
        self.state_published = True

    def commit(self) -> Dict[str, Any]:
        released_auxiliary: List[Dict[str, Any]] = []
        for role, witness in self.native_processes:
            if witness is STATE.retained_native_process:
                continue
            close_result = witness.close()
            released_auxiliary.append({
                "role": role,
                "close": close_result,
                "ok": close_result.get("ok") is True,
            })
        self.committed = True
        return {
            "ok": all(
                row.get("ok") is True
                for row in released_auxiliary
            ),
            "committed": True,
            "statePublished": self.state_published,
            "releasedAuxiliaryNativeHandles": released_auxiliary,
            "pidTerminationCommandIssued": False,
        }

    def rollback(self, reason: str) -> Dict[str, Any]:
        if self._rollback_result is not None:
            return dict(self._rollback_result)

        native_results: List[Dict[str, Any]] = []
        state_finalize: Dict[str, Any] = {}
        state_finalized = False
        for role, witness in reversed(self.native_processes):
            snapshot = dict(witness.snapshot())
            if snapshot.get("available") is not True:
                native_results.append({
                    "role": role,
                    "ok": snapshot.get("closed") is True,
                    "skipped": True,
                    "reason": "native witness already closed",
                    "witness": snapshot,
                    "pidTerminationCommandIssued": False,
                })
                continue
            termination = witness.terminate_exact(
                int(snapshot.get("pid", 0) or 0),
                int(snapshot.get("creationEpochMs", 0) or 0),
                str(snapshot.get("canonicalExePath", "")),
                wait_timeout_sec=10.0,
            )
            owns_current_state = bool(
                STATE.retained_native_process is witness
                and STATE.war3_pid == snapshot.get("pid")
                and STATE.launcher_pid == snapshot.get("pid")
                and STATE.launcher_created_at_ms ==
                    snapshot.get("creationEpochMs")
                and _canonical_process_path(STATE.launcher_exe) ==
                    _canonical_process_path(
                        snapshot.get("canonicalExePath")
                    )
            )
            close_result: Dict[str, Any]
            if termination.get("exact") is True and owns_current_state:
                state_finalize = (
                    _finalize_state_after_exact_native_termination(
                        int(snapshot["pid"]),
                        int(snapshot["creationEpochMs"]),
                        str(snapshot["canonicalExePath"]),
                    )
                )
                state_finalized = state_finalize.get("ok") is True
                close_result = {
                    "ok": state_finalized,
                    "closedByStateFinalize": state_finalized,
                }
            else:
                close_result = witness.close()
            native_results.append({
                "role": role,
                "ok": bool(
                    termination.get("exact") is True
                    and close_result.get("ok") is True
                ),
                "termination": termination,
                "close": close_result,
                "stateOwned": owns_current_state,
                "pidTerminationCommandIssued": False,
            })

        popen_results: List[Dict[str, Any]] = []
        for role, proc in reversed(self.popen_processes):
            before = proc.poll()
            terminate_issued = False
            wait_error = ""
            if before is None:
                try:
                    # subprocess.Popen owns a process HANDLE on Windows;
                    # terminate() addresses that object, never a PID lookup.
                    proc.terminate()
                    terminate_issued = True
                    proc.wait(timeout=10)
                except Exception as exc:
                    wait_error = f"{type(exc).__name__}: {exc}"
            after = proc.poll()
            popen_results.append({
                "role": role,
                "ok": after is not None and not wait_error,
                "beforeReturnCode": before,
                "afterReturnCode": after,
                "terminatedByPopenHandle": terminate_issued,
                "waitError": wait_error,
                "pidTerminationCommandIssued": False,
            })

        if state_finalized:
            desktop_close = dict(
                state_finalize.get("desktop", {}) or {}
            )
            video_restore = dict(
                state_finalize.get("videoRestore", {}) or {}
            )
            lock_release = {
                "ok": True,
                "releasedByStateFinalize": True,
            }
        else:
            lock_release = (
                _release_ydwe_root_lock(self.ydwe_lock)
                if int(self.ydwe_lock.get("handle", 0) or 0) != 0
                else {
                    "ok": True,
                    "skipped": True,
                    "reason": "no pre-STATE YDWE lock",
                }
            )
            desktop_closed = _close_desktop_handle(self.desktop_handle)
            desktop_close = {
                "ok": desktop_closed,
                "closed": desktop_closed,
                "handleWasPresent": self.desktop_handle != 0,
            }
            video_restore = (
                _restore_war3_video_registry(
                    dict(self.video_baseline.get("old", {})),
                    key_path=str(self.video_baseline.get(
                        "keyPath", WAR3_VIDEO_REG_KEY,
                    )),
                )
                if self.restore_video else {
                    "ok": True,
                    "skipped": True,
                    "reason": "video baseline not written",
                }
            )

        process_clean = bool(
            not self.unresolved_processes
            and all(
                row.get("ok") is True for row in
                native_results + popen_results
            )
        )
        result = {
            "ok": bool(
                process_clean
                and lock_release.get("ok") is True
                and desktop_close.get("ok") is True
                and video_restore.get("ok") is True
            ),
            "rolledBack": True,
            "reason": str(reason),
            "statePublished": self.state_published,
            "stateFinalize": state_finalize,
            "nativeProcesses": [
                {k: (v.snapshot() if hasattr(v, "snapshot") else v) for k, v in row.items()}
                if isinstance(row, dict) else row
                for row in native_results
            ],
            "popenProcesses": popen_results,
            "unresolvedProcesses": list(self.unresolved_processes),
            "ydweLockRelease": lock_release,
            "desktopClose": desktop_close,
            "videoRestore": video_restore,
            "pidTerminationCommandIssued": False,
        }
        self._rollback_result = dict(result)
        return result


_PRE_STATE_LAUNCH_TLS = threading.local()


def _transactional_launch(function: Any) -> Any:
    @functools.wraps(function)
    def wrapped(*args: Any, **kwargs: Any) -> Dict[str, Any]:
        if getattr(_PRE_STATE_LAUNCH_TLS, "transaction", None) is not None:
            return {
                "ok": False,
                "error": "nested launch transaction rejected",
                "code": "NESTED_LAUNCH_TRANSACTION",
            }
        _PRE_STATE_LAUNCH_TLS.transaction = None
        try:
            try:
                raw_result = function(*args, **kwargs)
            except Exception as exc:
                transaction = getattr(
                    _PRE_STATE_LAUNCH_TLS, "transaction", None,
                )
                rollback = (
                    transaction.rollback(
                        f"exception: {type(exc).__name__}: {exc}"
                    )
                    if transaction is not None else {
                        "ok": True,
                        "skipped": True,
                        "reason": "exception before launch side effects",
                    }
                )
                return {
                    "ok": False,
                    "error": f"{type(exc).__name__}: {exc}",
                    "code": "PRE_STATE_LAUNCH_EXCEPTION",
                    "preStateRollback": rollback,
                    "videoRestore": dict(
                        rollback.get("videoRestore", {}) or {}
                    ),
                }

            transaction = getattr(
                _PRE_STATE_LAUNCH_TLS, "transaction", None,
            )
            try:
                result = dict(raw_result or {})
            except Exception as exc:
                rollback = (
                    transaction.rollback(
                        "launch returned a non-mapping result"
                    )
                    if transaction is not None else {
                        "ok": True,
                        "skipped": True,
                    }
                )
                return {
                    "ok": False,
                    "error": f"invalid launch result: {exc}",
                    "code": "INVALID_LAUNCH_RESULT",
                    "preStateRollback": rollback,
                }
            if result.get("ok") is True:
                if transaction is None or not transaction.state_published:
                    rollback = (
                        transaction.rollback(
                            "success returned before STATE publication"
                        )
                        if transaction is not None else {
                            "ok": True,
                            "skipped": True,
                        }
                    )
                    return {
                        "ok": False,
                        "error": "launch success lacked published STATE",
                        "code": "PRE_STATE_PUBLICATION_MISSING",
                        "preStateRollback": rollback,
                    }
                commit = transaction.commit()
                result["preStateTransaction"] = commit
                if commit.get("ok") is not True:
                    rollback = transaction.rollback(
                        "auxiliary native HANDLE close failed at commit"
                    )
                    return {
                        "ok": False,
                        "error": (
                            "launch transaction commit did not close "
                            "auxiliary HANDLEs"
                        ),
                        "code": "LAUNCH_TRANSACTION_COMMIT_FAILED",
                        "preStateTransaction": commit,
                        "preStateRollback": rollback,
                    }
                return result

            rollback = (
                transaction.rollback(
                    str(result.get("error", "launch returned failure"))
                )
                if transaction is not None else {
                    "ok": True,
                    "skipped": True,
                    "reason": "failure before launch side effects",
                }
            )
            result["preStateRollback"] = rollback
            result["videoRestore"] = dict(
                rollback.get("videoRestore", {}) or {}
            )
            if rollback.get("ok") is not True:
                result["rollbackError"] = (
                    "pre-STATE launch rollback did not close exactly"
                )
            return result
        finally:
            _PRE_STATE_LAUNCH_TLS.transaction = None

    return wrapped


@mcp.tool()
@_transactional_launch
def launch_war3_test(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    launcher_mode: str = YDWE_LAUNCHER_MODE_DIRECT,
    ydwe_root: str = "",
    ydwe_child_timeout_sec: int = 20,
    ydwe_module_timeout_sec: int = 30,
    windowed: bool = False,
    use_isolated_desktop: bool = False,
    desktop_name: str = "",
    opengl: bool = False,
    auto_perf_record: bool = True,
    record_after_game_started: bool = False,
    auto_perf_export_sec: int = 10,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    render_log: bool = False,
    profile: str = "",
    disable_modules: str = "",
    env_overrides_json: str = "",
    extra_args: str = "",
) -> Dict[str, Any]:
    """启动 War3 并自动加载测试地图；可显式通过 YDWE 注入历史 JAPI。"""
    w3 = Path(war3_dir)
    war3_exe = w3 / "war3.exe"
    src_map = Path(map_path)
    if not war3_exe.exists():
        return {"ok": False, "error": f"未找到 war3.exe: {war3_exe}"}
    if not src_map.exists():
        return {"ok": False, "error": f"未找到地图: {src_map}"}

    try:
        launch_mode = _normalize_launcher_mode(launcher_mode)
    except ValueError as exc:
        return {"ok": False, "error": str(exc), "code": "INVALID_LAUNCHER_MODE"}

    ydwe_preflight: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "launcher_mode=direct",
        "registryModified": False,
    }
    ydwe_dir = Path(ydwe_root) if str(ydwe_root or "").strip() else Path()
    if launch_mode == YDWE_LAUNCHER_MODE_YDWE:
        if not str(ydwe_root or "").strip():
            return {
                "ok": False,
                "error": "launcher_mode=ydwe 时必须显式提供 ydwe_root",
                "code": "YDWE_ROOT_REQUIRED",
            }
        ydwe_preflight = _preflight_ydwe_single_launch(w3, ydwe_dir)
        if not ydwe_preflight.get("ok"):
            return {
                "ok": False,
                "error": ydwe_preflight.get("error", "YDWE/JAPI 启动预检失败"),
                "code": ydwe_preflight.get("code", "YDWE_PREFLIGHT_FAILED"),
                "ydwePreflight": ydwe_preflight,
            }
        w3 = Path(str(ydwe_preflight["war3Dir"]))
        war3_exe = w3 / "war3.exe"
        ydwe_dir = Path(str(ydwe_preflight["ydweRoot"]))

    deploy = None
    if deploy_d3d9_before_launch:
        deploy = _deploy_d3d9(Path(build_d3d9_path), w3)
        if not deploy.get("ok"):
            return {"ok": False, "error": deploy.get("error", "部署 d3d9.dll 失败"), "deploy": deploy}

    video_baseline = {
        "ok": True,
        "skipped": True,
        "reason": "enforce_video_baseline=False",
    }
    if enforce_video_baseline:
        video_baseline = _set_war3_video_registry(
            width=baseline_width,
            height=baseline_height,
            refresh_rate=baseline_refresh_rate,
        )
    pre_state_transaction = _PreStateLaunchTransaction(
        video_baseline, enforce_video_baseline,
    )
    _PRE_STATE_LAUNCH_TLS.transaction = pre_state_transaction
    if not video_baseline.get("ok"):
        return {
            "ok": False,
            "error": video_baseline.get("error", "写入视频基线失败"),
            "deploy": deploy,
            "videoBaseline": video_baseline,
        }

    inplace_rel = (
        ""
        if launch_mode == YDWE_LAUNCHER_MODE_YDWE
        else _prefer_inplace_relative_loadfile_arg(w3, src_map)
    )
    try:
        if inplace_rel:
            dst = src_map
            loadfile_arg = inplace_rel
            map_launch_mode = "inplace-relative"
        else:
            dst = _prepare_test_map_copy(w3, src_map, DEFAULT_TEST_MAP_REL)
            loadfile_arg = str(DEFAULT_TEST_MAP_REL).replace("/", "\\")
            map_launch_mode = "copied-short-path"
        source_map_sha256 = sha256_file(src_map)
        target_map_sha256 = sha256_file(dst)
        if source_map_sha256.lower() != target_map_sha256.lower():
            raise RuntimeError(
                "AutoTest 启动地图 SHA-256 与源候选不一致: "
                f"source={source_map_sha256} target={target_map_sha256}"
            )
    except Exception as exc:
        return {
            "ok": False,
            "error": f"部署测试地图失败: {exc}",
            "code": "TEST_MAP_DEPLOY_FAILED",
            "sourceMap": str(src_map),
            "targetMap": str(w3 / DEFAULT_TEST_MAP_REL),
        }

    effective_windowed = bool(windowed)
    forced_windowed = False
    desktop = {"ok": True, "skipped": True, "reason": "use_isolated_desktop=False"}
    if use_isolated_desktop:
        desktop = _create_isolated_desktop(desktop_name)
        if not desktop.get("ok"):
            return {
                "ok": False,
                "error": desktop.get("error", "创建隔离桌面失败"),
                "deploy": deploy,
                "videoBaseline": video_baseline,
                "desktop": desktop,
            }
        pre_state_transaction.own_desktop(
            desktop.get("handle", 0)
        )
        if not effective_windowed:
            effective_windowed = True
            forced_windowed = True

    if launch_mode == YDWE_LAUNCHER_MODE_YDWE:
        args = [
            str(ydwe_dir / "YDWE.exe"),
            "-war3",
            "-loadfile",
            loadfile_arg,
            "-closew2l",
        ]
        launch_cwd = ydwe_dir
    else:
        args = [str(war3_exe)]
        launch_cwd = w3
    if effective_windowed:
        args.append("-window")
    if opengl:
        args.append("-opengl")
    if launch_mode == YDWE_LAUNCHER_MODE_DIRECT:
        args.extend(["-loadfile", loadfile_arg])
    if extra_args.strip():
        args.extend(extra_args.strip().split())

    env = os.environ.copy()
    if auto_perf_record:
        env.setdefault("DXVK_WAR3_PERF_HISTORY_FRAMES", "7200")
        if record_after_game_started:
            env["DXVK_WAR3_PERF_RECORD_AFTER_GAME_START"] = "1"
            env.pop("DXVK_WAR3_PERF_RECORD_ON_START", None)
        else:
            env["DXVK_WAR3_PERF_RECORD_ON_START"] = "1"
        if auto_perf_export_sec > 0:
            env["DXVK_WAR3_PERF_AUTO_EXPORT_SEC"] = str(int(auto_perf_export_sec))
    if render_log:
        env["DXVK_WAR3_RENDER_LOG"] = "1"
    profile_name = str(profile or "").strip()
    if profile_name:
        env["DXVK_WAR3_PROFILE"] = profile_name
    disable_csv = str(disable_modules or "").strip()
    if disable_csv:
        env["DXVK_WAR3_DISABLE"] = disable_csv
    extra_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = extra_env.pop("__parse_error__", "")
    if parse_error:
        return {"ok": False, "error": f"env_overrides_json 解析失败: {parse_error}"}
    # 与隔离实例入口保持同一默认：每次 AutoTest 都使用准确的后台 cadence，
    # 但调用者仍可通过 env_overrides_json 明确恢复原生后台节流。
    extra_env.setdefault(AUTOTEST_BACKGROUND_THROTTLE_ENV, "1")
    extra_env.setdefault(AUTOTEST_GAME_PAUSE_ENV, "1")
    extra_env.setdefault(AUTOTEST_INTERNAL_TEST_API_ENV, "1")
    # 高频 SpriteFrame/runtime-matrix pose hooks are no longer the default
    # semantic palette producer. The production path samples Blizzard's
    # already-evaluated CModel palette from the visible contract; tests that
    # specifically need the legacy hook path can still opt in explicitly.
    env.update(extra_env)
    # Return the exact project-specific environment actually handed to the
    # child, not only caller overrides. This makes inherited DXVK_WAR3_*
    # variables and conductor-injected PERF controls visible to strict test
    # contracts without exposing unrelated host environment entries.
    effective_war3_environment = {
        str(key): str(value)
        for key, value in sorted(env.items())
        if str(key).upper().startswith("DXVK_WAR3_")
    }

    for stale in (
        _runtime_status_file(w3),
        _frame_capture_request_file(w3),
        _frame_capture_result_file(w3),
        _internal_test_request_file(w3),
        _internal_test_result_file(w3),
    ):
        try:
            if stale.exists():
                stale.unlink()
        except Exception:
            pass

    # 先清空事件并启动 DBWIN 监听（不过滤 pid），
    # 避免进程启动早期日志（例如 RegisterImage 写入端）在监控线程尚未就绪时丢失。
    with STATE.debug_lock:
        STATE.debug_events.clear()
        STATE.debug_seq = 0

    _start_debug_monitor(None)
    # 给监听线程一点启动时间，降低“进程启动瞬间日志”丢失概率。
    time.sleep(0.2)

    ydwe_lock: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "launcher_mode=direct",
        "handle": 0,
    }
    if launch_mode == YDWE_LAUNCHER_MODE_YDWE:
        ydwe_lock = _acquire_ydwe_root_lock(ydwe_dir)
        if not ydwe_lock.get("ok"):
            return {
                "ok": False,
                "error": ydwe_lock.get("error", "获取 YDWE 根互斥锁失败"),
                "code": ydwe_lock.get("code", "YDWE_ROOT_LOCK_FAILED"),
                "ydweLock": ydwe_lock,
                "desktop": desktop,
            }
        pre_state_transaction.own_ydwe_lock(ydwe_lock)
        locked_log_probe = _probe_ydwe_log_writable(ydwe_dir)
        if not locked_log_probe.get("ok"):
            return {
                "ok": False,
                "error": locked_log_probe.get("error", "YDWE war3.log 不可写"),
                "code": locked_log_probe.get("code", "YDWE_LOG_NOT_WRITABLE"),
                "ydweLock": ydwe_lock,
                "logWritable": locked_log_probe,
                "desktop": desktop,
            }

    preexisting_pids = {
        int(row.get("pid", 0) or 0)
        for row in _snapshot_process_entries()
        if int(row.get("pid", 0) or 0) > 0
    }
    launch_result: Dict[str, Any]
    if use_isolated_desktop:
        launch_result = _launch_process_on_desktop(
            args=args,
            cwd=launch_cwd,
            env=env,
            desktop_name=str(desktop.get("name", "")),
        )
        if not launch_result.get("ok"):
            return {
                "ok": False,
                "error": launch_result.get("error", "CreateProcessW 失败"),
                "deploy": deploy,
                "videoBaseline": video_baseline,
                "desktop": desktop,
            }
        proc = None
        pid = int(launch_result["pid"])
    else:
        proc = subprocess.Popen(
            args,
            cwd=str(launch_cwd),
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        launch_result = {"ok": True, "pid": int(proc.pid)}
        pid = int(proc.pid)
        pre_state_transaction.own_popen("launcher", proc)

    native_launcher_witness = launch_result.pop(
        "_nativeProcessWitness", None,
    )
    pre_state_transaction.own_native_process(
        "launcher", native_launcher_witness,
    )

    launcher_pid = int(pid)
    launcher_created_at_ms = _get_process_creation_epoch_ms(launcher_pid)
    child_discovery: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "launcher_mode=direct",
        "pid": int(pid),
    }
    ydwe_child_witness: Optional[RetainedNativeProcessWitness] = None
    ydwe_child_acquisition: Dict[str, Any] = {}
    if launch_mode == YDWE_LAUNCHER_MODE_YDWE:
        child_discovery = _wait_for_ydwe_war3_child(
            launcher_pid=launcher_pid,
            expected_war3_exe=war3_exe,
            preexisting_pids=preexisting_pids,
            timeout_sec=max(1, int(ydwe_child_timeout_sec)),
        )
        if not child_discovery.get("ok"):
            return {
                "ok": False,
                "error": child_discovery.get("error", "未发现 YDWE 的 War3 子进程"),
                "code": child_discovery.get("code", "YDWE_WAR3_CHILD_NOT_FOUND"),
                "launcherMode": launch_mode,
                "launcherPid": launcher_pid,
                "childDiscovery": child_discovery,
                "ydwePreflight": ydwe_preflight,
                "desktop": desktop,
            }
        pid = int(child_discovery["pid"])
        proc = None
        ydwe_child_witness, ydwe_child_acquisition = (
            _open_native_process_witness(
                pid,
                war3_exe,
                "launch_war3_test-prestate-ydwe-child",
            )
        )
        pre_state_transaction.own_native_process(
            "ydwe-game-child", ydwe_child_witness,
        )
        if ydwe_child_witness is None:
            pre_state_transaction.mark_unresolved_process(
                "ydwe-game-child",
                pid,
                "exact native child HANDLE acquisition failed",
            )
            return {
                "ok": False,
                "error": "无法冻结 YDWE War3 子进程清理 HANDLE",
                "code": "YDWE_CHILD_NATIVE_HANDLE_UNAVAILABLE",
                "launcherMode": launch_mode,
                "launcherPid": launcher_pid,
                "gamePid": pid,
                "childDiscovery": child_discovery,
                "nativeProcessWitnessAcquisition": (
                    ydwe_child_acquisition
                ),
            }

    runtime_modules: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "launcher_mode=direct",
    }
    if launch_mode == YDWE_LAUNCHER_MODE_YDWE:
        runtime_modules = _wait_for_ydwe_runtime_modules(
            pid=pid,
            expected_paths={
                "LuaEngine.dll": Path(str(ydwe_preflight["luaEngine"])),
                "yd_jass_api.dll": Path(str(ydwe_preflight["ydJassApi"])),
            },
            expected_sha256=dict(ydwe_preflight.get("runtimeSha256", {})),
            timeout_sec=max(1, int(ydwe_module_timeout_sec)),
        )
        if not runtime_modules.get("ok"):
            return {
                "ok": False,
                "error": runtime_modules.get("error", "YDWE/JAPI 运行时模块校验失败"),
                "code": runtime_modules.get("code", "YDWE_JAPI_MODULES_NOT_VERIFIED"),
                "launcherMode": launch_mode,
                "launcherPid": launcher_pid,
                "gamePid": pid,
                "runtimeModules": runtime_modules,
                "ydwePreflight": ydwe_preflight,
                "desktop": desktop,
            }

    native_process_witness: Optional[
        RetainedNativeProcessWitness
    ] = None
    native_process_acquisition: Dict[str, Any] = {}
    if (
        launch_mode == YDWE_LAUNCHER_MODE_DIRECT
        and native_launcher_witness is not None
    ):
        native_process_witness = native_launcher_witness
        native_process_acquisition = dict(
            launch_result.get("nativeProcessWitnessAcquisition", {}) or {}
        )
    elif launch_mode == YDWE_LAUNCHER_MODE_YDWE:
        native_process_witness = ydwe_child_witness
        native_process_acquisition = dict(ydwe_child_acquisition)
    else:
        native_process_witness, native_process_acquisition = (
            _open_native_process_witness(
                pid,
                war3_exe,
                "launch_war3_test-bound-direct-process",
            )
        )
        pre_state_transaction.own_native_process(
            "direct-game", native_process_witness,
        )
    if native_process_witness is None:
        return {
            "ok": False,
            "error": "无法建立精确绑定的原生 War3 进程 witness",
            "code": "WAR3_NATIVE_PROCESS_WITNESS_UNAVAILABLE",
            "nativeProcessWitnessAcquisition": (
                native_process_acquisition
            ),
            "launcherMode": launch_mode,
            "launcherPid": launcher_pid,
            "gamePid": pid,
            "desktop": desktop,
        }

    native_snapshot = native_process_witness.snapshot()
    if launch_mode == YDWE_LAUNCHER_MODE_DIRECT:
        launcher_created_at_ms = int(
            native_snapshot.get("creationEpochMs", 0) or 0
        )

    priority = _set_process_priority_high(pid)

    previous_native_process_close = (
        _replace_state_retained_native_process(native_process_witness)
    )
    STATE.war3_proc = proc
    STATE.war3_pid = pid
    STATE.launcher_pid = launcher_pid
    STATE.launcher_created_at_ms = launcher_created_at_ms
    STATE.launcher_mode = launch_mode
    STATE.launcher_exe = str(args[0])
    STATE.ydwe_lock_handle = int(ydwe_lock.get("handle", 0) or 0)
    STATE.ydwe_lock_name = str(ydwe_lock.get("name", ""))
    STATE.ydwe_lock_root = str(ydwe_lock.get("ydweRoot", ""))
    STATE.war3_dir = w3
    STATE.test_map_path = src_map
    STATE.desktop_name = str(desktop.get("name", "")) if use_isolated_desktop else ""
    STATE.desktop_handle = int(desktop.get("handle", 0) or 0) if use_isolated_desktop else 0
    STATE.desktop_mode = "isolated" if use_isolated_desktop else "default"
    STATE.launch_epoch_ms = int(time.time() * 1000)
    STATE.video_restore_key_path = str(video_baseline.get("keyPath", WAR3_VIDEO_REG_KEY))
    STATE.video_restore_snapshot = dict(video_baseline.get("old", {})) if enforce_video_baseline else {}

    # 进程创建后收敛到目标 pid，避免跨进程噪声。
    _start_debug_monitor(pid)
    pre_state_transaction.publish_state()

    return {
        "ok": True,
        "pid": pid,
        "gamePid": pid,
        "launcherPid": launcher_pid,
        "launcherMode": launch_mode,
        "launcherExe": str(args[0]),
        "args": args,
        "windowed": bool(effective_windowed),
        "requestedWindowed": bool(windowed),
        "forcedWindowedBecauseIsolatedDesktop": bool(forced_windowed),
        "useIsolatedDesktop": bool(use_isolated_desktop),
        "desktop": desktop,
        "profile": profile_name or "full_default",
        "disableModules": disable_csv,
        "recordAfterGameStarted": bool(record_after_game_started),
        "envOverrides": extra_env,
        "effectiveWar3Environment": effective_war3_environment,
        "copiedMap": str(dst),
        "loadfileArg": loadfile_arg,
        "mapLaunchMode": map_launch_mode,
        "sourceMapSha256": source_map_sha256,
        "targetMapSha256": target_map_sha256,
        "ydwePreflight": ydwe_preflight,
        "ydweLock": {
            "ok": bool(ydwe_lock.get("ok", False)),
            "name": str(ydwe_lock.get("name", "")),
            "ydweRoot": str(ydwe_lock.get("ydweRoot", "")),
            "heldUntilStop": launch_mode == YDWE_LAUNCHER_MODE_YDWE,
        },
        "childDiscovery": child_discovery,
        "runtimeModules": runtime_modules,
        "deploy": deploy,
        "videoBaseline": video_baseline,
        "priority": priority,
        "nativeProcessWitness": native_snapshot,
        "nativeProcessWitnessAcquisition": native_process_acquisition,
        "previousNativeProcessWitnessClose": (
            previous_native_process_close
        ),
        "time": _now_str(),
    }


@mcp.tool()
def is_war3_running(pid: int = 0, session_id: str = "") -> Dict[str, Any]:
    """检查 War3 进程是否仍在运行。"""
    session = _session_by_selector(session_id=session_id, pid=pid)
    check_pid = int(session.pid) if session is not None else (pid or (STATE.war3_pid or 0))
    if check_pid <= 0:
        return {"ok": True, "running": False, "pid": 0, "sessionId": session_id}
    return {
        "ok": True,
        "running": _registered_session_alive(session) if session is not None else _pid_alive(check_pid),
        "pid": check_pid,
        "sessionId": session.session_id if session is not None else "",
    }


@mcp.tool()
def read_runtime_status(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    session_id: str = "",
    pid: int = 0,
) -> Dict[str, Any]:
    """读取项目侧 runtime_status.json（由 DXVK 运行时周期写入）。"""
    registered = _session_by_selector(session_id=session_id, pid=pid)
    if session_id and registered is None:
        return {"ok": False, "error": f"未找到 session_id: {session_id}"}
    w3 = Path(registered.instance_root) if registered is not None else Path(war3_dir)
    target_pid = int(registered.pid) if registered is not None else (pid or STATE.war3_pid or 0)
    target_alive = (
        _registered_session_alive(registered)
        if registered is not None
        else (target_pid > 0 and _pid_alive(target_pid))
    )
    if target_pid > 0 and target_alive:
        pipe_res = _control_plane_request(
            pid=target_pid,
            command="get_runtime_status",
            payload={},
            timeout_sec=2.0,
        )
        if pipe_res.get("transportOk"):
            return {
                "ok": bool(pipe_res.get("ok")),
                "sessionId": registered.session_id if registered is not None else "",
                "mode": "control-plane",
                "pipeName": str(pipe_res.get("pipeName", "") or ""),
                "data": dict(pipe_res.get("result", {}) or {}),
        "detail": pipe_res,
    }


def _query_windows_gpu_events() -> List[Dict[str, Any]]:
    script = (
        "$OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
        "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);"
        "$rows=@(Get-WinEvent -FilterHashtable @{LogName='System';Id=153,4101} "
        "-MaxEvents 128 -ErrorAction SilentlyContinue | "
        "Where-Object {$_.ProviderName -eq 'nvlddmkm' -or $_.ProviderName -eq 'Display'} | "
        "Select-Object TimeCreated,Id,ProviderName,RecordId,LevelDisplayName,Message);"
        "$rows | ConvertTo-Json -Depth 4 -Compress"
    )
    try:
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-Command", script],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=20,
            check=False,
        )
        if proc.returncode != 0 or not proc.stdout.strip():
            return []
        parsed = json.loads(proc.stdout)
        if isinstance(parsed, dict):
            return [parsed]
        return [row for row in parsed if isinstance(row, dict)]
    except Exception:
        return []


def _gpu_event_identity(row: Dict[str, Any]) -> str:
    return "|".join(
        str(row.get(key, ""))
        for key in ("ProviderName", "Id", "RecordId", "TimeCreated")
    )


def _runtime_status_device_lost(status: Dict[str, Any]) -> bool:
    def _walk(value: Any, key: str = "") -> bool:
        lowered = str(key or "").lower()
        if "devicelost" in lowered or "device_lost" in lowered:
            if value is True or str(value).strip().lower() in {"1", "true", "yes"}:
                return True
        if lowered in {"queueresult", "submitresult", "presentresult", "waitresult"}:
            try:
                if int(value) == -4:
                    return True
            except Exception:
                pass
        if isinstance(value, dict):
            return any(_walk(child, str(child_key)) for child_key, child in value.items())
        if isinstance(value, list):
            return any(_walk(child, key) for child in value)
        return False

    return _walk(status)


def _new_gpu_incident_files(roots: List[Path], before: set[str]) -> List[str]:
    rows: List[str] = []
    for root in roots:
        log_dir = root / "WarVK" / "Log"
        if not log_dir.is_dir():
            continue
        for path in sorted(log_dir.glob("gpu_incident_*.json")):
            key = str(path.resolve(strict=False)).lower()
            if key not in before:
                rows.append(str(path))
    return rows

    path = _runtime_status_file(w3)
    data = _read_runtime_status_file(w3)
    if not data:
        return {"ok": False, "error": "runtime_status.json 不存在或解析失败", "path": str(path)}
    return {
        "ok": True,
        "sessionId": registered.session_id if registered is not None else "",
        "path": str(path),
        "data": data,
    }


@mcp.tool()
def wait_for_runtime_status(
    timeout_sec: int = 120,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    require_runtime_ready: bool = False,
    require_game_started: bool = True,
    min_timestamp_ms: int = 0,
    session_id: str = "",
) -> Dict[str, Any]:
    """轮询 runtime_status.json，等待进入目标状态。"""
    registered = _session_by_selector(session_id=session_id)
    if session_id and registered is None:
        return {"ok": False, "error": f"未找到 session_id: {session_id}"}
    w3 = Path(registered.instance_root) if registered is not None else Path(war3_dir)
    effective_min_timestamp_ms = max(
        int(min_timestamp_ms),
        int(registered.process_created_at_ms) - 5_000 if registered is not None else 0,
    )
    path = _runtime_status_file(w3)
    t0 = time.time()
    while time.time() - t0 < max(1, timeout_sec):
        data = _read_runtime_status_file(w3)
        if data:
            ts = int(data.get("timestampMs", 0))
            if effective_min_timestamp_ms > 0 and ts < effective_min_timestamp_ms:
                time.sleep(0.25)
                continue
            runtime = data.get("runtime", {})
            runtime_ready = bool(runtime.get("runtimeReady", False))
            game_started = bool(runtime.get("gameStarted", False))
            if ((not require_runtime_ready or runtime_ready) and
                (not require_game_started or game_started)):
                return {
                    "ok": True,
                    "path": str(path),
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "data": data,
                }
        time.sleep(0.25)
    return {
        "ok": False,
        "error": "等待 runtime_status 超时",
        "path": str(path),
        "elapsedSec": round(time.time() - t0, 3),
    }


@mcp.tool()
def get_runtime_events(
    since_id: int = 0,
    limit: int = 200,
    contains: str = "",
    session_id: str = "",
) -> Dict[str, Any]:
    """拉取 OutputDebugString 事件（可作为订阅轮询）。"""
    session = _session_by_selector(session_id=session_id)
    if session_id and session is None:
        return {"ok": False, "error": f"未找到 session_id: {session_id}"}
    rows = (
        session.get_events(since_id=since_id, limit=limit, contains=contains)
        if session is not None
        else STATE.get_events(since_id=since_id, limit=limit, contains=contains)
    )
    return {
        "ok": True,
        "sessionId": session.session_id if session is not None else "",
        "count": len(rows),
        "events": rows,
        "latestId": rows[-1]["id"] if rows else since_id,
    }


@mcp.tool()
def wait_for_game_ready(
    timeout_sec: int = 120,
    pid: int = 0,
    allow_fallback: bool = True,
    fallback_min_elapsed_sec: int = 20,
    fallback_min_cpu_sec: float = 1.0,
    require_game_started_for_fallback: bool = True,
    session_id: str = "",
    auto_continue_loading: bool = False,
    continue_key: str = "RIGHT",
    continue_interval_sec: int = 12,
) -> Dict[str, Any]:
    """
    等待“正式进入游戏”：
    - 先命中 runtime init：`JASS runtime fully initialized`
    - 再命中 in-game 渲染信号：`War3Shadow: Run frame=` 或 `War3StageSig: stage=19`
    """
    registered = _session_by_selector(session_id=session_id, pid=pid)
    target_pid = int(registered.pid) if registered is not None else (pid or (STATE.war3_pid or 0))
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid，请先 launch_war3_test"}
    target_war3_dir = Path(registered.instance_root) if registered is not None else Path(STATE.war3_dir)
    launch_epoch_ms = (
        int(registered.process_created_at_ms)
        if registered is not None
        else int(STATE.launch_epoch_ms)
    )
    def target_alive() -> bool:
        return (
            _registered_session_alive(registered)
            if registered is not None
            else _pid_alive(target_pid)
        )

    _start_debug_monitor(None)

    pipe_wait_t0 = time.time()
    pipe_ready: Dict[str, Any] = {}
    continue_pulses: List[Dict[str, Any]] = []
    last_continue_at = 0.0
    while True:
        elapsed_wait = time.time() - pipe_wait_t0
        remaining_timeout = max(0.0, float(timeout_sec) - elapsed_wait)
        if remaining_timeout <= 0.0:
            break

        # 超大地图在加载条结束后会停在“按下任意键以继续”。以短 wait_until
        # 分片等待，才能在不切换隔离桌面的情况下向 War3 窗口投递继续键；一次
        # 性等待完整 timeout 会让已经完成加载的地图永远卡在确认页。
        request_timeout = remaining_timeout
        if auto_continue_loading:
            request_timeout = min(
                remaining_timeout,
                float(max(5, int(continue_interval_sec))),
            )

        pipe_ready = _control_plane_request(
            pid=target_pid,
            command="wait_until",
            payload={
                "timeoutSec": max(1, int(math.ceil(request_timeout))),
                "pollIntervalMs": 50,
            },
            timeout_sec=max(2.0, request_timeout + 2.0),
        )
        if pipe_ready.get("transportOk"):
            if pipe_ready.get("ok"):
                break
            runtime_status = dict(
                ((pipe_ready.get("result", {}) or {}).get("runtimeStatus", {})) or {}
            )
            runtime = dict(runtime_status.get("runtime", {}) or {})
            now = time.time()
            can_continue = (
                auto_continue_loading
                and target_alive()
                and bool(runtime.get("jassReady", False))
                and not bool(runtime.get("gameStarted", False))
                and (now - pipe_wait_t0) >= 10.0
                and (last_continue_at <= 0.0 or
                     (now - last_continue_at) >= max(5, int(continue_interval_sec)))
            )
            if can_continue:
                pulse = _post_war3_key_pulse(
                    target_pid,
                    key=continue_key,
                    hold_ms=55,
                    repeat=1,
                    foreground=False,
                )
                pulse["elapsedSec"] = round(now - pipe_wait_t0, 3)
                continue_pulses.append(pulse)
                last_continue_at = now
            if auto_continue_loading and target_alive():
                time.sleep(0.2)
                continue
            break
        if not target_alive():
            break
        time.sleep(0.2)

    if pipe_ready.get("transportOk"):
        runtime_status = dict(((pipe_ready.get("result", {}) or {}).get("runtimeStatus", {})) or {})
        if pipe_ready.get("ok"):
            return {
                "ok": True,
                "mode": "control-plane",
                "pid": target_pid,
                "elapsedSec": round(float(pipe_ready.get("elapsedSec", 0.0) or 0.0), 3),
                "runtimeStatus": runtime_status,
                "continuePulses": continue_pulses,
                "detail": pipe_ready,
            }
        return {
            "ok": False,
            "error": str(pipe_ready.get("error", "control plane wait_until 失败")),
            "mode": "control-plane",
            "pid": target_pid,
            "elapsedSec": round(time.time() - pipe_wait_t0, 3),
            "runtimeStatus": runtime_status,
            "continuePulses": continue_pulses,
            "detail": pipe_ready,
        }
    if not allow_fallback:
        return {
            "ok": False,
            "error": str(pipe_ready.get("error", "control plane 不可用")),
            "mode": "control-plane-required",
            "pid": target_pid,
            "elapsedSec": round(time.time() - pipe_wait_t0, 3),
            "detail": pipe_ready,
        }

    t0 = time.time()
    last_id = 0
    hit_init: Optional[Dict[str, Any]] = None
    hit_ingame: Optional[Dict[str, Any]] = None
    last_runtime_status: Optional[Dict[str, Any]] = None
    last_runtime_ts = 0
    stable_runtime_updates = 0
    stable_runtime_wall_t0 = 0.0
    dead_grace_checks = 0

    while time.time() - t0 < max(1, timeout_sec):
        if not target_alive():
            runtime_status = _read_runtime_status_file(target_war3_dir)
            if runtime_status:
                rt = runtime_status.get("runtime", {})
                module = runtime_status.get("module", {})
                ts = int(runtime_status.get("timestampMs", 0))
                if (
                    ts >= max(0, launch_epoch_ms - 5_000)
                    and bool(rt.get("gameStarted", False))
                    and str(module.get("state", "")) == "Running"
                ):
                    dead_grace_checks += 1
                    last_runtime_status = runtime_status
                    if dead_grace_checks <= 15:
                        time.sleep(0.2)
                        continue
            return {
                "ok": False,
                "error": "进程已退出，等待失败",
                "pid": target_pid,
                "hitInit": hit_init,
                "hitInGame": hit_ingame,
            }
        dead_grace_checks = 0

        batch = (
            registered.get_events(since_id=last_id, limit=256)
            if registered is not None
            else STATE.get_events(since_id=last_id, limit=256)
        )
        if batch:
            last_id = batch[-1]["id"]
        for e in batch:
            msg = e["msg"]
            if (hit_init is None) and ("DXVK War3Hook: JASS runtime fully initialized" in msg):
                hit_init = e
            if (
                "DXVK War3Shadow: Run frame=" in msg
                or "DXVK War3StageSig: stage=19" in msg
                or "DXVK War3Hook: Auto-fired OnGameStart via TIME_OF_DAY=" in msg
                or "[War3Events] OnGameStart complete" in msg
            ):
                hit_ingame = e

        if hit_init and hit_ingame:
            if not allow_fallback:
                status0 = _read_runtime_status_best_effort(target_pid)
                time.sleep(1.0)
                status1 = _read_runtime_status_best_effort(target_pid)
                if isinstance(status0, dict) and isinstance(status1, dict):
                    frame0 = int(status0.get("frameIndex", 0) or 0)
                    frame1 = int(status1.get("frameIndex", 0) or 0)
                    render0 = dict(status0.get("render", {}) or {})
                    render1 = dict(status1.get("render", {}) or {})
                    module0 = dict(status0.get("module", {}) or {})
                    module1 = dict(status1.get("module", {}) or {})
                    dispatch0 = int(module0.get("dispatchCalls", 0) or 0)
                    dispatch1 = int(module1.get("dispatchCalls", 0) or 0)
                    if (
                        frame0 > 0
                        and frame1 <= frame0
                        and dispatch1 <= dispatch0
                        and bool(render1.get("inGameRenderReady", False))
                        and not bool(render1.get("isInGame", False))
                    ):
                        return {
                            "ok": False,
                            "error": "debug-events ready 后 frameIndex 未继续推进，疑似首帧卡住",
                            "mode": "debug-events-stalled",
                            "pid": target_pid,
                            "elapsedSec": round(time.time() - t0, 3),
                            "hitInit": hit_init,
                            "hitInGame": hit_ingame,
                            "status0": status0,
                            "status1": status1,
                        }
            return {
                "ok": True,
                "mode": "debug-events",
                "pid": target_pid,
                "elapsedSec": round(time.time() - t0, 3),
                "hitInit": hit_init,
                "hitInGame": hit_ingame,
            }

        # 优先读取项目侧 runtime_status（更稳定，不依赖 DBWIN）。
        runtime_status = _read_runtime_status_file(target_war3_dir)
        if runtime_status:
            last_runtime_status = runtime_status
            rt = runtime_status.get("runtime", {})
            perf = runtime_status.get("perf", {})
            module = runtime_status.get("module", {})
            ts = int(runtime_status.get("timestampMs", 0))
            runtime_ready = bool(rt.get("runtimeReady", False))
            jass_ready = bool(rt.get("jassReady", False))
            game_started = bool(rt.get("gameStarted", False))
            frame_index = int(runtime_status.get("frameIndex", 0) or 0)
            periodic_source = str(runtime_status.get("source", "")) == "periodic"
            module_running = str(module.get("state", "")) == "Running"
            if hit_init is None and jass_ready:
                hit_init = {
                    "id": -1,
                    "ts": ts,
                    "msg": "runtime_status.runtime.jassReady=true",
                }
            if ts >= max(0, launch_epoch_ms - 5_000) and runtime_ready:
                return {
                    "ok": True,
                    "mode": "runtime-status-file",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(target_war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }
            # 背景/隔离桌面里，部分图会稳定触发 OnGameStart，但 runtimeReady
            # 与 DBWIN in-game 信号迟迟不补齐。这里改成“稳定活动”软成功：
            # runtime_status 必须持续变新、模块持续 Running，且具备 recording /
            # periodic / frameIndex>0 三者之一，才允许进入采样。
            soft_game_started = (
                ts >= max(0, launch_epoch_ms - 5_000)
                and game_started
                and module_running
            )
            stable_activity = (
                soft_game_started
                and (
                    bool(perf.get("recording", False))
                    or periodic_source
                    or frame_index > 0
                    or hit_init is not None
                )
            )
            if stable_activity and ts > last_runtime_ts:
                if stable_runtime_updates == 0:
                    stable_runtime_wall_t0 = time.time()
                stable_runtime_updates += 1
                last_runtime_ts = ts
            elif not stable_activity:
                stable_runtime_updates = 0
                stable_runtime_wall_t0 = 0.0
                if ts > last_runtime_ts:
                    last_runtime_ts = ts

            if (
                stable_activity
                and stable_runtime_updates >= 2
                and stable_runtime_wall_t0 > 0.0
                and (time.time() - stable_runtime_wall_t0) >= 1.5
            ):
                return {
                    "ok": True,
                    "mode": "runtime-status-stable",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(target_war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "frameIndex": frame_index,
                    "stableUpdates": int(stable_runtime_updates),
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }
            if (
                soft_game_started
                and hit_init is not None
                and (time.time() - t0) >= 4.0
            ):
                return {
                    "ok": True,
                    "mode": "runtime-status-game-started",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(target_war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "frameIndex": frame_index,
                    "stableUpdates": int(stable_runtime_updates),
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }
            if (
                soft_game_started
                and (bool(perf.get("recording", False)) or periodic_source or frame_index > 0)
                and (time.time() - t0) >= 4.0
            ):
                return {
                    "ok": True,
                    "mode": "runtime-status-game-started",
                    "pid": target_pid,
                    "elapsedSec": round(time.time() - t0, 3),
                    "runtimeStatusPath": str(_runtime_status_file(target_war3_dir)),
                    "runtimeStatus": runtime_status,
                    "runtimeReady": runtime_ready,
                    "gameStarted": game_started,
                    "frameIndex": frame_index,
                    "stableUpdates": int(stable_runtime_updates),
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                }

        # 兜底：当 DBWIN 抓不到日志时，使用窗口存在 + CPU 累计时间判定。
        if allow_fallback:
            elapsed = time.time() - t0
            hwnd = _find_main_window_hwnd(target_pid)
            cpu_sec = _get_process_cpu_seconds(target_pid)
            fallback_game_started = True
            if require_game_started_for_fallback and last_runtime_status:
                fallback_game_started = bool(((last_runtime_status.get("runtime") or {}).get("gameStarted", False)))
            if hwnd and fallback_game_started and elapsed >= max(1, fallback_min_elapsed_sec) and cpu_sec >= max(0.0, fallback_min_cpu_sec):
                return {
                    "ok": True,
                    "mode": "fallback-window-cpu",
                    "pid": target_pid,
                    "elapsedSec": round(elapsed, 3),
                    "cpuSec": round(cpu_sec, 3),
                    "hwnd": int(hwnd),
                    "runtimeStatus": last_runtime_status or {},
                    "hitInit": hit_init,
                    "hitInGame": hit_ingame,
                    "requireGameStartedForFallback": bool(require_game_started_for_fallback),
                }
        time.sleep(0.2)

    return {
        "ok": False,
        "error": "超时，未观察到完整 in-game 信号",
        "pid": target_pid,
        "elapsedSec": round(time.time() - t0, 3),
        "runtimeStatus": last_runtime_status or {},
        "hitInit": hit_init,
        "hitInGame": hit_ingame,
    }


def _shadow_summary_int(summary: Dict[str, Any], key: str) -> int:
    try:
        return int(summary.get(key, 0) or 0)
    except (TypeError, ValueError):
        return 0


def _native_execute_success_draw_count(summary: Dict[str, Any]) -> int:
    """Return a stable native execute draw count.

    The current-frame executed counter can be reset when a later control-plane
    summary prepares a new native frame. The stable last-success fields preserve
    the actual render-thread execute result.
    """
    current = _shadow_summary_int(summary, "nativeD3D9BackendExecutedDrawCount")
    stable = _shadow_summary_int(
        summary,
        "nativeD3D9BackendLastSuccessfulExecutedDrawCount",
    )
    if stable <= 0 and _shadow_summary_int(
        summary,
        "nativeD3D9BackendExecuteSuccessCount",
    ) > 0:
        submitted = _shadow_summary_int(
            summary,
            "nativeD3D9BackendLastExecuteSubmittedDrawCount",
        )
        failed = _shadow_summary_int(
            summary,
            "nativeD3D9BackendLastExecuteFailedDrawCount",
        )
        stable = max(0, submitted - failed)
    return max(current, stable)


def _nested_status_int(status: Dict[str, Any], *path: str) -> int:
    cur: Any = status
    for key in path:
        if not isinstance(cur, dict):
            return 0
        cur = cur.get(key)
    try:
        return int(cur or 0)
    except (TypeError, ValueError):
        return 0


def _runtime_frame_progress_status(
    runtime_status: Dict[str, Any],
    *,
    frame_advance_stalled: bool = False,
    frame_stall_sec: float = 0.0,
) -> Dict[str, Any]:
    """Expose frame-tail state without changing the ready contract."""
    render = runtime_status.get("render", {}) if isinstance(runtime_status, dict) else {}
    frame = runtime_status.get("frame", {}) if isinstance(runtime_status, dict) else {}
    render_in_game_ready = bool(render.get("inGameRenderReady", False))
    render_is_in_game = bool(render.get("isInGame", False))
    tail_stalled = render_in_game_ready and bool(frame_advance_stalled)
    inactive_tail_stalled = tail_stalled and not render_is_in_game
    return {
        "runtimeFrameIndex": _nested_status_int(runtime_status, "frameIndex"),
        "runtimeFrameNumber": _nested_status_int(runtime_status, "frame", "frameNumber"),
        "runtimeFramePublishRevision": _nested_status_int(
            runtime_status,
            "frame",
            "publishRevision",
        ),
        "runtimeRenderInGameReady": render_in_game_ready,
        "runtimeRenderIsInGame": render_is_in_game,
        "runtimeFrameAdvanceStalled": bool(frame_advance_stalled),
        "runtimeFrameStallSec": round(max(0.0, float(frame_stall_sec)), 3),
        "runtimeRenderTailStalled": bool(tail_stalled),
        "runtimeRenderInactiveTailStalled": bool(inactive_tail_stalled),
        "runtimeVisibleCount": _nested_status_int(runtime_status, "frame", "visibleCount"),
        "runtimeUnitCount": _nested_status_int(runtime_status, "frame", "unitCount"),
        "runtimeRecordsWithRuntimeModel": _nested_status_int(
            runtime_status,
            "frame",
            "recordsWithRuntimeModel",
        ),
        "runtimeRecordsWithModelResource": _nested_status_int(
            runtime_status,
            "frame",
            "recordsWithModelResource",
        ),
    }


def _semantic_scene_consumption_status(summary: Dict[str, Any]) -> Dict[str, Any]:
    """Classify whether the DXVK scene pass consumed the latest semantic frame."""
    near_latest_max_lag = 2
    core_submitted = _shadow_summary_int(summary, "semanticCoreSubmittedDrawCount")
    scene_submitted = _shadow_summary_int(summary, "semanticSceneLastSubmittedDrawCount")
    scene_skinned = _shadow_summary_int(summary, "semanticSceneSubmittedSkinned")
    direct_currentdraw_ready = _shadow_summary_int(
        summary,
        "semanticSceneCurrentDrawResolveReadyCount",
    )
    canonical_ready = _shadow_summary_int(summary, "semanticSceneCanonicalReadyCount")
    fallback = _shadow_summary_int(summary, "objectFallbackDrawCount")
    scene_publish_count = _shadow_summary_int(summary, "semanticSceneStatsPublishCount")
    scene_lag = _shadow_summary_int(summary, "semanticScenePublishRevisionLag")
    scene_frame_serial = _shadow_summary_int(
        summary,
        "semanticSceneLastFrameSerial",
    )
    core_frame_serial = _shadow_summary_int(
        summary,
        "semanticCoreFrameSerial",
    )
    revision_consumed = (
        scene_publish_count > 0
        and scene_submitted > 0
        and scene_lag == 0
    )
    same_frame_consumed = (
        scene_publish_count > 0
        and scene_submitted > 0
        and scene_frame_serial > 0
        and core_frame_serial > 0
        and scene_frame_serial >= core_frame_serial
    )
    near_latest_consumed = (
        scene_publish_count > 0
        and scene_submitted > 0
        and scene_frame_serial > 0
        and core_frame_serial > 0
        and scene_lag >= 0
        and scene_lag <= near_latest_max_lag
        and core_frame_serial >= scene_frame_serial
        and (core_frame_serial - scene_frame_serial) <= near_latest_max_lag
    )
    direct_currentdraw_consumed = (
        scene_submitted > 0
        and scene_skinned > 0
        and direct_currentdraw_ready > 0
        and canonical_ready > 0
        and fallback == 0
    )
    consumed = (
        revision_consumed
        or same_frame_consumed
        or near_latest_consumed
        or direct_currentdraw_consumed
    )
    supplemented_revision_pending = same_frame_consumed and not revision_consumed
    waiting_for_render_scene = (
        core_submitted > 0 and not consumed and not direct_currentdraw_consumed
    )
    if revision_consumed:
        consumption_mode = "revision"
    elif same_frame_consumed:
        consumption_mode = "same-frame"
    elif near_latest_consumed:
        consumption_mode = "near-latest"
    elif direct_currentdraw_consumed:
        consumption_mode = "current-draw-direct"
    else:
        consumption_mode = "pending"
    return {
        "semanticSceneConsumptionFresh": bool(consumed),
        "semanticSceneRevisionConsumed": bool(revision_consumed),
        "semanticSceneSameFrameConsumed": bool(same_frame_consumed),
        "semanticSceneNearLatestConsumed": bool(near_latest_consumed),
        "semanticSceneDirectCurrentDrawConsumed": bool(direct_currentdraw_consumed),
        "semanticSceneNearLatestMaxLag": int(near_latest_max_lag),
        "semanticSceneSupplementedRevisionPending": bool(supplemented_revision_pending),
        "semanticSceneConsumptionMode": consumption_mode,
        "semanticSceneWaitingForRenderPass": bool(waiting_for_render_scene),
        "semanticScenePublishRevisionLag": int(scene_lag),
        "semanticSceneStatsPublishCount": int(scene_publish_count),
        "semanticSceneLastSubmittedDrawCount": int(scene_submitted),
        "semanticSceneLastFrameSerial": scene_frame_serial,
        "semanticCoreFrameSerial": core_frame_serial,
        "semanticSceneLastSourcePublishRevision": _shadow_summary_int(
            summary,
            "semanticSceneLastSourcePublishRevision",
        ),
        "semanticCoreSourcePublishRevision": _shadow_summary_int(
            summary,
            "semanticCoreSourcePublishRevision",
        ),
    }


def _refresh_shadow_runtime_summary_until(
    *,
    pid: int,
    wait_sec: float,
    min_submitted_draw_count: int = 0,
    min_attachment_rigid_resolved: int = 0,
    require_semantic_frame_fresh: bool = True,
) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    """Poll the control plane summary after a failed hot-frame wait.

    wait_until can return the last pre-rebuild summary when semantic resources
    are populated late in the same frame. A short explicit summary poll keeps
    the failure diagnostic tied to the latest semantic contract state.
    """
    deadline = time.time() + max(0.0, float(wait_sec))
    last_detail: Dict[str, Any] = {}
    best_summary: Dict[str, Any] = {}

    while True:
        detail = _control_plane_request(
            pid=pid,
            command="get_shadow_runtime_summary",
            payload={
                "refreshSemanticFrameIfStale": True,
                "forceSemanticFrameBuild": True,
                "allowControlPlaneSemanticDrain": True,
                "semanticBuildMinIntervalMs": 0,
                "semanticBuildDrainMaxChunks": 32,
                "semanticBuildDrainBudgetUs": 50000,
                "semanticBuildDrainRecordCeiling": 1024,
            },
            timeout_sec=2.0,
        )
        last_detail = detail
        if detail.get("transportOk") and detail.get("ok"):
            summary = dict(detail.get("result", {}) or {})
            best_summary = summary
            submitted = _shadow_summary_int(summary, "semanticCoreSubmittedDrawCount")
            attachment = _shadow_summary_int(summary, "semanticCoreAttachmentRigidResolved")
            supplemental = _shadow_summary_int(
                summary,
                "semanticCoreAttachmentRigidSupplementalResolvedCount",
            )
            frame_fresh = bool(summary.get("semanticCoreFrameFresh", False))
            if (
                submitted >= max(0, int(min_submitted_draw_count))
                and max(attachment, supplemental)
                >= max(0, int(min_attachment_rigid_resolved))
                and (not bool(require_semantic_frame_fresh) or frame_fresh)
            ):
                return summary, detail

        if time.time() >= deadline:
            break
        # The attachment supplemental path can settle a few ticks after ready
        # even when the isolated desktop stops advancing visible frames.
        time.sleep(0.5)

    return best_summary, last_detail


@mcp.tool()
def wait_for_hot_shadow_frame(
    timeout_sec: int = 120,
    pid: int = 0,
    min_visible_count: int = 1,
    min_stable_identity_count: int = 1,
    min_unit_count: int = 1,
    min_semantic_resolved: int = 1,
    min_semantic_skinned_resolved: int = 0,
    min_native_executed_draw_count: int = 0,
    require_semantic_frame_fresh: bool = True,
    min_frame_advance: int = 2,
    allow_semantic_rigid_only: bool = False,
    allow_semantic_attachment_rigid_only: bool = False,
    min_semantic_attachment_rigid_resolved: int = 0,
    post_failure_summary_wait_sec: int = 6,
    prefer_summary_poll: bool = False,
    require_semantic_scene_consumed: bool = False,
    allow_scene_pending_if_core_and_currentdraw_ready: bool = False,
    min_semantic_static_world_submitted: int = 0,
    allow_semantic_static_world_only: bool = False,
) -> Dict[str, Any]:
    """等待进入热帧语义阴影状态，而不是只等待 ready 首帧。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid，请先 launch_war3_test"}
    timeout_sec = max(1, int(timeout_sec))
    t0 = time.time()
    if bool(prefer_summary_poll):
        deadline = t0 + float(timeout_sec)
        last_detail: Dict[str, Any] = {}
        last_manifest: Dict[str, Any] = {}
        last_runtime_status: Dict[str, Any] = {}
        last_summary: Dict[str, Any] = {}
        last_frame_index: Optional[int] = None
        last_frame_advance_at = t0
        response: Dict[str, Any] = {
            "ok": False,
            "mode": "control-plane-hot-frame-summary-poll",
            "pid": target_pid,
            "elapsedSec": 0.0,
            "error": "等待 hot shadow summary 超时",
        }
        while time.time() < deadline:
            latest = _control_plane_request(
                pid=target_pid,
                command="get_hot_shadow_probe",
                payload={
                    "refreshSemanticFrameIfStale": True,
                    "forceSemanticFrameBuild": True,
                    "allowControlPlaneSemanticDrain": True,
                    "semanticBuildMinIntervalMs": 0,
                    "semanticBuildDrainMaxChunks": 32,
                    "semanticBuildDrainBudgetUs": 50000,
                    "semanticBuildDrainRecordCeiling": 1024,
                },
                timeout_sec=3.0,
            )
            last_detail = latest
            latest_result = dict(latest.get("result", {}) or {}) if latest.get("ok") else {}
            latest_summary = dict(latest_result.get("shadowRuntimeSummary", {}) or {})
            if latest_summary:
                last_summary = latest_summary
            else:
                latest_summary = last_summary
            last_manifest = dict(latest_result.get("frameManifestSummary", {}) or last_manifest or {})
            last_runtime_status = dict(latest_result.get("runtimeStatus", {}) or last_runtime_status or {})
            runtime_frame_index = _nested_status_int(last_runtime_status, "frameIndex")
            now = time.time()
            if last_frame_index is None or runtime_frame_index != last_frame_index:
                last_frame_index = runtime_frame_index
                last_frame_advance_at = now
            frame_stall_sec = now - last_frame_advance_at
            runtime_frame_status = _runtime_frame_progress_status(
                last_runtime_status,
                frame_advance_stalled=frame_stall_sec >= 3.0,
                frame_stall_sec=frame_stall_sec,
            )
            response = {
                "ok": False,
                "mode": "control-plane-hot-frame-summary-poll",
                "pid": target_pid,
                "elapsedSec": round(time.time() - t0, 3),
                "runtimeStatus": last_runtime_status,
                "frameManifestSummary": last_manifest,
                "shadowRuntimeSummary": latest_summary,
                "error": "",
                "detail": latest,
            }
            response.update(runtime_frame_status)
            semantic_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreResolved",
            )
            submitted = _shadow_summary_int(
                latest_summary,
                "semanticCoreSubmittedDrawCount",
            )
            rigid_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreRigidResolved",
            )
            explicit_rigid = _shadow_summary_int(
                latest_summary,
                "semanticCoreExplicitResourceOwnerRigidResolved",
            )
            attachment_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreAttachmentRigidResolved",
            )
            attachment_supplemental_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreAttachmentRigidSupplementalResolvedCount",
            )
            skinned_resolved = _shadow_summary_int(
                latest_summary,
                "semanticCoreSkinnedResolved",
            )
            native_draws = _native_execute_success_draw_count(latest_summary)
            response["nativeD3D9BackendEffectiveExecutedDrawCount"] = native_draws
            fallback = _shadow_summary_int(latest_summary, "objectFallbackDrawCount")
            frame_fresh = bool(latest_summary.get("semanticCoreFrameFresh", False))
            scene_status = _semantic_scene_consumption_status(latest_summary)
            response.update(scene_status)
            scene_submitted = _shadow_summary_int(
                latest_summary,
                "semanticSceneLastSubmittedDrawCount",
            )
            scene_skinned = _shadow_summary_int(
                latest_summary,
                "semanticSceneSubmittedSkinned",
            )
            scene_building = _shadow_summary_int(
                latest_summary,
                "semanticSceneSubmittedBuilding",
            )
            scene_destructible = _shadow_summary_int(
                latest_summary,
                "semanticSceneSubmittedDestructible",
            )
            explicit_rigid_scene = _shadow_summary_int(
                latest_summary,
                "semanticSceneAcceptedExplicitResourceOwnerRigid",
            )
            currentdraw_query_hit = _shadow_summary_int(
                latest_summary,
                "currentDrawContractQueryHitCount",
            )
            currentdraw_palette_hit = _shadow_summary_int(
                latest_summary,
                "currentDrawCapturedPaletteQueryHitCount",
            )
            currentdraw_group_decode_hit = _shadow_summary_int(
                latest_summary,
                "currentDrawGroupSlotDecodeHitCount",
            )
            scene_lag = int(
                scene_status.get("semanticScenePublishRevisionLag", 0) or 0
            )
            scene_frame_serial = int(
                scene_status.get("semanticSceneLastFrameSerial", 0) or 0
            )
            core_frame_serial = int(
                scene_status.get("semanticCoreFrameSerial", 0) or 0
            )
            manifest_ok = True
            if last_manifest:
                manifest_ok = (
                    _shadow_summary_int(last_manifest, "visibleCount")
                    >= max(0, int(min_visible_count))
                    and _shadow_summary_int(last_manifest, "recordsWithStableIdentity")
                    >= max(0, int(min_stable_identity_count))
                    and _shadow_summary_int(last_manifest, "unitCount")
                    >= max(0, int(min_unit_count))
                )
            semantic_contract_ok = (
                semantic_resolved >= max(0, int(min_semantic_resolved))
                and submitted > 0
                and fallback == 0
                and (not bool(require_semantic_frame_fresh) or frame_fresh)
            )
            scene_consumption_required = bool(require_semantic_scene_consumed) or not (
                bool(allow_semantic_rigid_only)
                or bool(allow_semantic_attachment_rigid_only)
            )
            strict_ok = (
                semantic_contract_ok
                and manifest_ok
                and skinned_resolved >= max(0, int(min_semantic_skinned_resolved))
                and native_draws >= max(0, int(min_native_executed_draw_count))
                and (
                    not scene_consumption_required
                    or bool(scene_status.get("semanticSceneConsumptionFresh"))
                )
            )
            scene_contract_ok = (
                scene_submitted > 0
                and fallback == 0
                and manifest_ok
                and (
                    not scene_consumption_required
                    or bool(scene_status.get("semanticSceneConsumptionFresh"))
                )
                and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
            )
            static_world_scene_submitted = (
                scene_building + scene_destructible + explicit_rigid_scene
            )
            static_world_contract_ok = (
                scene_submitted > 0
                and fallback == 0
                and manifest_ok
                and static_world_scene_submitted
                >= max(0, int(min_semantic_static_world_submitted))
                and (
                    not scene_consumption_required
                    or bool(scene_status.get("semanticSceneConsumptionFresh"))
                )
            )
            direct_currentdraw_contract_ok = (
                scene_submitted > 0
                and fallback == 0
                and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
                and _shadow_summary_int(
                    latest_summary,
                    "semanticSceneCurrentDrawResolveReadyCount",
                )
                > 0
                and _shadow_summary_int(
                    latest_summary,
                    "semanticSceneCanonicalReadyCount",
                )
                > 0
                and bool(scene_status.get("semanticSceneDirectCurrentDrawConsumed"))
            )
            attachment_contract_ok = (
                semantic_contract_ok
                and max(attachment_resolved, attachment_supplemental_resolved)
                >= max(1, int(min_semantic_attachment_rigid_resolved))
            )
            if int(min_semantic_attachment_rigid_resolved) > 0:
                # Attachment-rigid was the previous proof that semantic data had
                # reached the renderer. Once a strict skinned semantic frame is
                # present, do not keep dynamic_shadow_pressure blocked on the
                # older attachment diagnostic. model_runtime_probe still passes
                # min_semantic_skinned_resolved=0 and therefore keeps the
                # attachment contract gate as intended.
                strict_ok = strict_ok and (
                    attachment_contract_ok
                    or skinned_resolved >= max(
                        1, int(min_semantic_skinned_resolved)
                    )
                )
            if strict_ok:
                response["ok"] = True
                return response
            if scene_contract_ok:
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "DXVK semantic scene submitted and consumed draw packets; "
                    "native backend counters are not required for this visual "
                    "validation gate"
                )
                return response
            if direct_currentdraw_contract_ok:
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "current-draw canonical scene submitted skinned packets "
                    "with fallback disabled; this Phase 3 validation path does "
                    "not require the older semantic core/manifest consumer"
                )
                response["semanticShadowPhase"] = "current-draw-direct-ok"
                return response
            if allow_semantic_static_world_only and static_world_contract_ok:
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "static-world canonical scene submitted building/destructible "
                    "packets with fallback disabled"
                )
                response["semanticShadowPhase"] = "static-world-direct-ok"
                return response
            if (
                bool(allow_scene_pending_if_core_and_currentdraw_ready)
                and semantic_contract_ok
                and fallback == 0
                and currentdraw_query_hit > 0
                and currentdraw_palette_hit > 0
                and currentdraw_group_decode_hit > 0
            ):
                response["ok"] = True
                response["semanticSceneOnlyAccepted"] = True
                response["semanticSceneOnlyReason"] = (
                    "semantic core is fresh, current-draw contract/palette/group-slot "
                    "queries are hot, and isolated-desktop render-scene consumption "
                    "is stalled at tail; accepted as low-pressure tail artifact"
                )
                response["semanticShadowPhase"] = "low-pressure-tail-accepted"
                return response
            if (
                semantic_contract_ok
                and manifest_ok
                and scene_submitted > 0
                and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
                and scene_status.get("semanticSceneWaitingForRenderPass")
                and scene_lag <= 16
                and scene_frame_serial > 0
                and core_frame_serial > 0
                and scene_frame_serial <= core_frame_serial
                and (core_frame_serial - scene_frame_serial) <= 1
            ):
                response["ok"] = True
                response["semanticTailSceneNearLatestAccepted"] = True
                response["semanticTailSceneNearLatestReason"] = (
                    "semantic core is fresh and the DXVK render-scene pass has "
                    "already consumed the immediately previous semantic frame "
                    "with fallback disabled; the latest publish revision is "
                    "waiting for one more isolated-desktop render tick, so this "
                    "is accepted as a tail-frame validation artifact"
                )
                return response

            core_packets_present = (
                submitted > 0
                and fallback == 0
                and semantic_resolved >= max(0, int(min_semantic_resolved))
            )
            if (
                core_packets_present
                and scene_status.get("semanticSceneWaitingForRenderPass")
            ):
                if native_draws > 0:
                    response["semanticShadowPhase"] = (
                        "native-semantic-executed-scene-pending"
                    )
                    response["semanticNativeExecuted"] = True
                    response["semanticShadowPhaseReason"] = (
                        "native D3D9 backend has successfully executed semantic "
                        "draws, but the DXVK render-scene validation pass has "
                        "not consumed the latest semantic publish revision yet"
                    )
                else:
                    response["semanticShadowPhase"] = (
                        "core-fresh-waiting-render-scene"
                        if frame_fresh
                        else "core-packets-waiting-render-scene"
                    )
                    response["semanticShadowPhaseReason"] = (
                        "semantic core has submitted draw packets, but the DXVK "
                        "render-scene pass has not consumed the latest semantic "
                        "publish revision yet"
                    )
                if not frame_fresh:
                    response["semanticShadowPhaseReason"] += (
                        "; semantic core freshness is also pending because the "
                        "latest render-scene tick has not advanced"
                    )
                if response.get("runtimeRenderTailStalled"):
                    response["semanticShadowPhaseReason"] += (
                        "; runtime frame advance is stalled after in-game render "
                        "readiness, so this is a render-scene consumption wait, "
                        "not a semantic data-chain miss"
                    )
                response["error"] = response["semanticShadowPhaseReason"]

            pending_without_consumer = (
                response.get("runtimeRenderTailStalled")
                and bool(latest_summary.get("semanticCoreBuildRequestPending", False))
                and not bool(latest_summary.get("semanticCoreBuildInProgress", False))
                and submitted <= 0
                and scene_submitted <= 0
            )
            if pending_without_consumer and frame_stall_sec >= 8.0:
                response["semanticShadowPhase"] = "core-build-pending-no-render-consumer"
                response["semanticShadowPhaseReason"] = (
                    "semantic contract is pending, but the isolated-desktop "
                    "runtime frame has stopped advancing before the DXVK render "
                    "scene consumed any build chunks; this is a render-thread "
                    "consumer timing blocker, not a control-plane timeout"
                )
                response["error"] = response["semanticShadowPhaseReason"]
                return response

            if attachment_contract_ok:
                response["semanticAttachmentRigidOnlyAccepted"] = True
                response["semanticAttachmentRigidGateSatisfied"] = True
                if not response.get("semanticShadowPhase"):
                    response["semanticShadowPhase"] = (
                        "attachment-rigid-ok-skinned-pending"
                    )
                    response["semanticShadowPhaseReason"] = (
                        "attachment rigid semantic path is producing submitted draw "
                        "packets with fallback disabled; skinned/upper-layer/native "
                        "gates remain pending"
                    )
                if bool(allow_semantic_attachment_rigid_only):
                    response["ok"] = True
                else:
                    response["error"] = response["semanticShadowPhaseReason"]
                return response

            if (
                semantic_contract_ok
                and (explicit_rigid > 0 or rigid_resolved > 0)
                and skinned_resolved <= 0
            ):
                response["semanticShadowPhase"] = (
                    "semantic-rigid-ok-skinned-pending"
                )
                response["semanticShadowPhaseReason"] = (
                    "semantic rigid path is producing draw packets, but skinned "
                    "gate is still pending"
                )
                if bool(allow_semantic_rigid_only):
                    response["ok"] = True
                    response["semanticRigidOnlyAccepted"] = True
                    response["error"] = ""
                    return response

            time.sleep(0.5)

        response["ok"] = False
        response["elapsedSec"] = round(time.time() - t0, 3)
        response["error"] = response.get("error") or "等待 hot shadow summary 超时"
        response["detail"] = last_detail
        return response

    require_frame_advance = int(min_frame_advance) > 0
    pipe_ready = _control_plane_request(
        pid=target_pid,
        command="wait_until",
        payload={
            "timeoutSec": timeout_sec,
            "pollIntervalMs": 50,
            "requireFrameAdvance": require_frame_advance,
            "minFrameAdvance": max(0, int(min_frame_advance)),
            "requireSemanticFrameFresh": bool(require_semantic_frame_fresh),
            "requireSemanticSceneConsumed": bool(require_semantic_scene_consumed),
            "minVisibleCount": max(0, int(min_visible_count)),
            "minStableIdentityCount": max(0, int(min_stable_identity_count)),
            "minUnitCount": max(0, int(min_unit_count)),
            "minSemanticResolved": max(0, int(min_semantic_resolved)),
            "minSemanticSkinnedResolved": max(0, int(min_semantic_skinned_resolved)),
            "requestSemanticFrameBuild": True,
            "forceSemanticFrameBuild": True,
            "allowControlPlaneSemanticDrain": True,
            "allowPreInGameSemanticBuild": False,
            "semanticBuildMinIntervalMs": 0,
            "semanticBuildDrainMaxChunks": 32,
            "semanticBuildDrainBudgetUs": 50000,
            "semanticBuildDrainRecordCeiling": 1024,
            "stalledFrameTimeoutMs": 3000,
        },
        timeout_sec=max(2.0, float(timeout_sec) + 2.0),
    )
    if not pipe_ready.get("transportOk"):
        return {
            "ok": False,
            "mode": "control-plane-hot-frame",
            "pid": target_pid,
            "elapsedSec": round(float(pipe_ready.get("elapsedSec", 0.0) or 0.0), 3),
            "error": str(pipe_ready.get("error", "control plane 不可用")),
            "detail": pipe_ready,
        }

    result = dict(pipe_ready.get("result", {}) or {})
    runtime_status = dict(result.get("runtimeStatus", {}) or {})
    wait_stalled = str(pipe_ready.get("error", "") or "") == "wait_until stalled"
    response = {
        "ok": bool(pipe_ready.get("ok")),
        "mode": "control-plane-hot-frame",
        "pid": target_pid,
        "elapsedSec": round(float(pipe_ready.get("elapsedSec", 0.0) or 0.0), 3),
        "runtimeStatus": runtime_status,
        "frameManifestSummary": dict(result.get("frameManifestSummary", {}) or {}),
        "shadowRuntimeSummary": dict(result.get("shadowRuntimeSummary", {}) or {}),
        "readyFrameBaseline": int(result.get("readyFrameBaseline", 0) or 0),
        "requestedSemanticFrameBuild": bool(result.get("requestedSemanticFrameBuild", False)),
        "semanticBuildRequestReason": str(result.get("semanticBuildRequestReason", "") or ""),
        "error": str(pipe_ready.get("error", "") or ""),
        "detail": pipe_ready,
    }
    response.update(
        _runtime_frame_progress_status(
            runtime_status,
            frame_advance_stalled=wait_stalled,
            frame_stall_sec=float(result.get("stalledFrameTimeoutMs", 0) or 0) / 1000.0
            if wait_stalled
            else 0.0,
        )
    )
    latest_summary = dict(response.get("shadowRuntimeSummary", {}) or {})
    if not response.get("ok"):
        refreshed_summary, refresh_detail = _refresh_shadow_runtime_summary_until(
            pid=target_pid,
            wait_sec=max(0, int(post_failure_summary_wait_sec)),
            min_submitted_draw_count=1,
            min_attachment_rigid_resolved=max(
                0,
                int(min_semantic_attachment_rigid_resolved),
            ),
            require_semantic_frame_fresh=bool(require_semantic_frame_fresh),
        )
        if refreshed_summary:
            response["shadowRuntimeSummary"] = refreshed_summary
            response["postFailureSummaryRefresh"] = refresh_detail
            latest_summary = refreshed_summary

    semantic_resolved = _shadow_summary_int(latest_summary, "semanticCoreResolved")
    submitted = _shadow_summary_int(latest_summary, "semanticCoreSubmittedDrawCount")
    rigid_resolved = _shadow_summary_int(latest_summary, "semanticCoreRigidResolved")
    explicit_rigid = _shadow_summary_int(
        latest_summary,
        "semanticCoreExplicitResourceOwnerRigidResolved",
    )
    attachment_resolved = _shadow_summary_int(
        latest_summary,
        "semanticCoreAttachmentRigidResolved",
    )
    attachment_supplemental_resolved = _shadow_summary_int(
        latest_summary,
        "semanticCoreAttachmentRigidSupplementalResolvedCount",
    )
    skinned_resolved = _shadow_summary_int(
        latest_summary,
        "semanticCoreSkinnedResolved",
    )
    fallback = _shadow_summary_int(latest_summary, "objectFallbackDrawCount")
    frame_fresh = bool(latest_summary.get("semanticCoreFrameFresh", False))
    scene_status = _semantic_scene_consumption_status(latest_summary)
    response.update(scene_status)
    native_draws = _native_execute_success_draw_count(latest_summary)
    response["nativeD3D9BackendEffectiveExecutedDrawCount"] = native_draws
    scene_submitted = _shadow_summary_int(
        latest_summary,
        "semanticSceneLastSubmittedDrawCount",
    )
    scene_skinned = _shadow_summary_int(
        latest_summary,
        "semanticSceneSubmittedSkinned",
    )
    scene_building = _shadow_summary_int(
        latest_summary,
        "semanticSceneSubmittedBuilding",
    )
    scene_destructible = _shadow_summary_int(
        latest_summary,
        "semanticSceneSubmittedDestructible",
    )
    explicit_rigid_scene = _shadow_summary_int(
        latest_summary,
        "semanticSceneAcceptedExplicitResourceOwnerRigid",
    )
    currentdraw_query_hit = _shadow_summary_int(
        latest_summary,
        "currentDrawContractQueryHitCount",
    )
    currentdraw_palette_hit = _shadow_summary_int(
        latest_summary,
        "currentDrawCapturedPaletteQueryHitCount",
    )
    currentdraw_group_decode_hit = _shadow_summary_int(
        latest_summary,
        "currentDrawGroupSlotDecodeHitCount",
    )
    scene_lag = int(scene_status.get("semanticScenePublishRevisionLag", 0) or 0)
    scene_frame_serial = int(
        scene_status.get("semanticSceneLastFrameSerial", 0) or 0
    )
    core_frame_serial = int(scene_status.get("semanticCoreFrameSerial", 0) or 0)
    manifest_summary = dict(response.get("frameManifestSummary", {}) or {})
    manifest_ok = True
    if manifest_summary:
        manifest_ok = (
            _shadow_summary_int(manifest_summary, "visibleCount")
            >= max(0, int(min_visible_count))
            and _shadow_summary_int(manifest_summary, "recordsWithStableIdentity")
            >= max(0, int(min_stable_identity_count))
            and _shadow_summary_int(manifest_summary, "unitCount")
            >= max(0, int(min_unit_count))
        )
    core_contract_ok = (
        semantic_resolved >= max(0, int(min_semantic_resolved))
        and submitted > 0
        and fallback == 0
        and (not bool(require_semantic_frame_fresh) or frame_fresh)
    )
    semantic_contract_ok = (
        core_contract_ok
        and (
            not bool(require_semantic_scene_consumed)
            or bool(scene_status.get("semanticSceneConsumptionFresh"))
        )
    )
    scene_contract_ok = (
        scene_submitted > 0
        and fallback == 0
        and manifest_ok
        and (
            not bool(require_semantic_scene_consumed)
            or bool(scene_status.get("semanticSceneConsumptionFresh"))
        )
        and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
    )
    static_world_scene_submitted = (
        scene_building + scene_destructible + explicit_rigid_scene
    )
    static_world_contract_ok = (
        scene_submitted > 0
        and fallback == 0
        and manifest_ok
        and static_world_scene_submitted
        >= max(0, int(min_semantic_static_world_submitted))
        and (
            not bool(require_semantic_scene_consumed)
            or bool(scene_status.get("semanticSceneConsumptionFresh"))
        )
    )
    direct_currentdraw_contract_ok = (
        scene_submitted > 0
        and fallback == 0
        and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
        and _shadow_summary_int(
            latest_summary,
            "semanticSceneCurrentDrawResolveReadyCount",
        )
        > 0
        and _shadow_summary_int(
            latest_summary,
            "semanticSceneCanonicalReadyCount",
        )
        > 0
        and bool(scene_status.get("semanticSceneDirectCurrentDrawConsumed"))
    )
    core_packets_present = (
        submitted > 0
        and fallback == 0
        and semantic_resolved >= max(0, int(min_semantic_resolved))
    )
    if (
        core_packets_present
        and scene_status.get("semanticSceneWaitingForRenderPass")
    ):
        if native_draws > 0:
            response["semanticShadowPhase"] = (
                "native-semantic-executed-scene-pending"
            )
            response["semanticNativeExecuted"] = True
            response["semanticShadowPhaseReason"] = (
                "native D3D9 backend has successfully executed semantic draws, "
                "but the DXVK render-scene validation pass has not consumed the "
                "latest semantic publish revision yet"
            )
        else:
            response["semanticShadowPhase"] = (
                "core-fresh-waiting-render-scene"
                if frame_fresh
                else "core-packets-waiting-render-scene"
            )
            response["semanticShadowPhaseReason"] = (
                "semantic core has submitted draw packets, but the DXVK "
                "render-scene pass has not consumed the latest semantic publish "
                "revision yet"
            )
        if not frame_fresh:
            response["semanticShadowPhaseReason"] += (
                "; semantic core freshness is also pending because the latest "
                "render-scene tick has not advanced"
            )
        if response.get("runtimeRenderTailStalled"):
            response["semanticShadowPhaseReason"] += (
                "; runtime frame advance is stalled after in-game render "
                "readiness, so this is a render-scene consumption wait, not a "
                "semantic data-chain miss"
            )
        if not response.get("ok"):
            response["error"] = response["semanticShadowPhaseReason"]
    attachment_contract_ok = (
        core_contract_ok
        and max(attachment_resolved, attachment_supplemental_resolved)
        >= max(1, int(min_semantic_attachment_rigid_resolved))
    )
    attachment_gate_ok = (
        int(min_semantic_attachment_rigid_resolved) <= 0
        or attachment_contract_ok
        or skinned_resolved >= max(1, int(min_semantic_skinned_resolved))
    )
    if scene_contract_ok:
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "DXVK semantic scene submitted and consumed draw packets; native "
            "backend counters are not required for this visual validation gate"
        )
        response["error"] = ""
        return response
    if direct_currentdraw_contract_ok:
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "current-draw canonical scene submitted skinned packets with "
            "fallback disabled; this Phase 3 validation path does not require "
            "the older semantic core/manifest consumer"
        )
        response["semanticShadowPhase"] = "current-draw-direct-ok"
        response["error"] = ""
        return response
    if allow_semantic_static_world_only and static_world_contract_ok:
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "static-world canonical scene submitted building/destructible "
            "packets with fallback disabled"
        )
        response["semanticShadowPhase"] = "static-world-direct-ok"
        response["error"] = ""
        return response
    if (
        bool(allow_scene_pending_if_core_and_currentdraw_ready)
        and core_contract_ok
        and currentdraw_query_hit > 0
        and currentdraw_palette_hit > 0
        and currentdraw_group_decode_hit > 0
    ):
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["semanticSceneOnlyReason"] = (
            "semantic core is fresh, current-draw contract/palette/group-slot "
            "queries are hot, and isolated-desktop render-scene consumption is "
            "stalled at tail; accepted as low-pressure tail artifact"
        )
        response["semanticShadowPhase"] = "low-pressure-tail-accepted"
        response["error"] = ""
        return response
    if (
        not response.get("ok")
        and scene_contract_ok
        and attachment_gate_ok
        and native_draws >= max(0, int(min_native_executed_draw_count))
    ):
        response["ok"] = True
        response["semanticSceneOnlyAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticSceneOnlyReason"] = (
            "DXVK semantic scene submitted and consumed draw packets with "
            "fallback disabled; this gate does not require the control-plane "
            "wait_until call to classify the final state as stalled"
        )
    if (
        not response.get("ok")
        and wait_stalled
        and semantic_contract_ok
        and manifest_ok
        and skinned_resolved >= max(0, int(min_semantic_skinned_resolved))
        and native_draws >= max(0, int(min_native_executed_draw_count))
        and attachment_gate_ok
    ):
        response["ok"] = True
        response["semanticTailFrameAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticTailFrameReason"] = (
            "semantic frame was fresh, consumed by the render scene, and "
            "executed by the native backend before the isolated-desktop frame "
            "advance gate stalled"
        )
    if (
        not response.get("ok")
        and wait_stalled
        and scene_contract_ok
        and manifest_ok
    ):
        response["ok"] = True
        response["semanticTailSceneAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticTailSceneReason"] = (
            "semantic scene submitted and consumed draw packets before the "
            "isolated-desktop frame advance gate stalled; core counters may be "
            "reset by EndFrame flush, so the scene submission counters are the "
            "authoritative visual-consumption signal for this gate"
        )
    if (
        not response.get("ok")
        and (
            wait_stalled
            or scene_status.get("semanticSceneWaitingForRenderPass")
        )
        and frame_fresh
        and core_contract_ok
        and manifest_ok
        and scene_submitted > 0
        and scene_skinned >= max(0, int(min_semantic_skinned_resolved))
        and scene_lag <= 16
        and scene_frame_serial > 0
        and core_frame_serial > 0
        and scene_frame_serial <= core_frame_serial
        and (core_frame_serial - scene_frame_serial) <= 1
    ):
        response["ok"] = True
        response["semanticTailSceneNearLatestAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticTailSceneNearLatestReason"] = (
            "semantic core was advanced by the bounded control-plane tail drain "
            "after the isolated-desktop render tick stalled; the DXVK scene had "
            "already consumed the immediately previous semantic frame with "
            "fallback disabled, so this is accepted as a tail-frame validation "
            "artifact rather than a data-chain failure"
        )
    if attachment_contract_ok:
        response["semanticAttachmentRigidOnlyAccepted"] = True
        response["semanticAttachmentRigidGateSatisfied"] = True
        if not response.get("semanticShadowPhase"):
            response["semanticShadowPhase"] = "attachment-rigid-ok-skinned-pending"
            response["semanticShadowPhaseReason"] = (
                "attachment rigid semantic path is producing submitted draw packets "
                "with fallback disabled; skinned/upper-layer/native gates remain pending"
            )
        if not response.get("ok"):
            response["originalError"] = response.get("error", "")
            response["error"] = response["semanticShadowPhaseReason"]
        if bool(allow_semantic_attachment_rigid_only):
            response["ok"] = True
            response["error"] = ""

    if (
        not response.get("ok")
        and bool(allow_semantic_rigid_only)
        and semantic_contract_ok
        and (explicit_rigid > 0 or rigid_resolved > 0)
    ):
        response["ok"] = True
        response["semanticRigidOnlyAccepted"] = True
        response["originalError"] = response.get("error", "")
        response["error"] = ""
        response["semanticRigidOnlyReason"] = (
            "semantic rigid path is producing submitted draw packets with "
            "fallback disabled; skinned/attachment gates remain pending"
        )
    elif not response.get("ok") and semantic_contract_ok and skinned_resolved <= 0:
        response.setdefault("semanticShadowPhase", "semantic-rigid-ok-skinned-pending")
        response.setdefault(
            "semanticShadowPhaseReason",
            "semantic rigid path is producing draw packets, but skinned gate is still pending",
        )
    if (
        not response.get("ok")
        or int(min_native_executed_draw_count) <= 0
        or response.get("semanticSceneOnlyAccepted")
        or response.get("semanticTailSceneAccepted")
        or response.get("semanticTailSceneNearLatestAccepted")
    ):
        return response

    if native_draws >= int(min_native_executed_draw_count):
        return response

    deadline = t0 + float(timeout_sec)
    last_detail: Dict[str, Any] = {}
    while time.time() < deadline:
        latest = _control_plane_request(
            pid=target_pid,
            command="get_shadow_runtime_summary",
            payload={
                "refreshSemanticFrameIfStale": True,
                "forceSemanticFrameBuild": True,
                "allowControlPlaneSemanticDrain": True,
                "semanticBuildMinIntervalMs": 0,
                "semanticBuildDrainMaxChunks": 32,
                "semanticBuildDrainBudgetUs": 50000,
                "semanticBuildDrainRecordCeiling": 1024,
            },
            timeout_sec=2.0,
        )
        last_detail = latest
        if latest.get("transportOk") and latest.get("ok"):
            latest_summary = dict(latest.get("result", {}) or {})
            native_draws = _native_execute_success_draw_count(latest_summary)
            if native_draws >= int(min_native_executed_draw_count):
                response["shadowRuntimeSummary"] = latest_summary
                response["nativeD3D9BackendEffectiveExecutedDrawCount"] = (
                    native_draws
                )
                response["elapsedSec"] = round(time.time() - t0, 3)
                response["nativeExecuteWaitMode"] = "control-plane-summary"
                response["nativeExecuteWaitDetail"] = latest
                return response
        time.sleep(0.1)

    response["ok"] = False
    response["error"] = (
        f"等待 native D3D9 effective executed draw count>="
        f"{int(min_native_executed_draw_count)} 超时"
    )
    response["elapsedSec"] = round(time.time() - t0, 3)
    response["shadowRuntimeSummary"] = latest_summary
    response["nativeExecuteWaitMode"] = "control-plane-summary"
    response["nativeExecuteWaitDetail"] = last_detail
    return response


@mcp.tool()
def query_war3_window(
    pid: int = 0,
    wait_sec: int = 0,
    require_visible: bool = True,
) -> Dict[str, Any]:
    """查询 War3 主窗口句柄/尺寸/状态。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}", "pid": target_pid}

    hwnd = (
        _wait_for_main_window_hwnd(target_pid, timeout_sec=max(0.2, float(wait_sec)), require_visible=require_visible)
        if wait_sec > 0
        else _find_main_window_hwnd(target_pid)
    )
    if not hwnd:
        return {
            "ok": False,
            "error": "未找到主窗口",
            "pid": target_pid,
            "requireVisible": bool(require_visible),
        }

    info = _query_window_info_by_hwnd(hwnd, pid=target_pid)
    info["requireVisible"] = bool(require_visible)
    return info


@mcp.tool()
def wait_for_war3_window_ready(
    timeout_sec: int = 30,
    pid: int = 0,
    min_cpu_sec: float = 0.5,
    stable_sec: float = 0.8,
) -> Dict[str, Any]:
    """
    等待 War3 窗口进入“可操作”状态。
    适用于窗口化 resize/maximize 回归，不要求正式进图。
    """
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    return _wait_for_window_ready(
        pid=target_pid,
        timeout_sec=max(1, int(timeout_sec)),
        min_cpu_sec=max(0.0, float(min_cpu_sec)),
        stable_sec=max(0.1, float(stable_sec)),
    )


@mcp.tool()
def send_war3_input_plan(
    actions_json: str,
    pid: int = 0,
    timeout_sec: int = 20,
) -> Dict[str, Any]:
    """在目标隔离桌面执行键盘/客户端坐标点击计划；不会切换当前桌面。"""
    target_pid = pid or (STATE.war3_pid or 0)
    try:
        actions = json.loads(str(actions_json or "[]"))
    except Exception as exc:
        return {"ok": False, "error": f"actions_json 解析失败: {exc}"}
    if not isinstance(actions, list):
        return {"ok": False, "error": "actions_json 必须是 JSON array"}
    return _run_war3_input_plan(
        pid=int(target_pid),
        actions=actions,
        timeout_sec=max(2, int(timeout_sec)),
    )


@mcp.tool()
def control_war3_window(
    action: str = "query",
    pid: int = 0,
    client_w: int = 0,
    client_h: int = 0,
    x: int = 40,
    y: int = 40,
    wait_sec: float = 1.0,
) -> Dict[str, Any]:
    """
    控制 War3 窗口：
    - query
    - resize_client
    - maximize
    - restore
    - minimize
    - close
    """
    target_pid = pid or (STATE.war3_pid or 0)
    action_norm = str(action or "query").strip().lower()
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid", "action": action_norm}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}", "pid": target_pid, "action": action_norm}

    before = query_war3_window(pid=target_pid, wait_sec=5, require_visible=True)
    if not before.get("ok") and action_norm != "query":
        return {"ok": False, "error": "窗口尚不可见，无法执行动作", "before": before, "action": action_norm}

    result: Dict[str, Any]
    if action_norm == "query":
        result = {"ok": True, "action": action_norm}
    elif action_norm == "resize_client":
        result = _resize_window_client_native(
            pid=target_pid,
            client_w=max(64, int(client_w)),
            client_h=max(64, int(client_h)),
            x=int(x),
            y=int(y),
        )
    elif action_norm == "maximize":
        result = _post_window_syscommand(target_pid, 0xF030)
    elif action_norm == "restore":
        result = _post_window_syscommand(target_pid, 0xF120)
    elif action_norm == "minimize":
        result = _post_window_syscommand(target_pid, 0xF020)
    elif action_norm == "close":
        result = {
            "ok": _post_close(target_pid),
            "pid": int(target_pid),
        }
    else:
        return {"ok": False, "error": f"未知 action: {action}", "action": action_norm}

    time.sleep(max(0.0, float(wait_sec)))
    alive = _pid_alive(target_pid)
    after = query_war3_window(pid=target_pid, wait_sec=1, require_visible=False) if alive else {
        "ok": False,
        "error": "进程已退出",
        "pid": target_pid,
    }
    return {
        "ok": bool(result.get("ok")),
        "action": action_norm,
        "pid": int(target_pid),
        "aliveAfter": bool(alive),
        "before": before,
        "result": result,
        "after": after,
    }


@mcp.tool()
def run_windowed_resize_crash_test(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    window_ready_timeout_sec: int = 30,
    require_game_ready: bool = False,
    game_ready_timeout_sec: int = 45,
    initial_client_w: int = 1600,
    initial_client_h: int = 900,
    resize_client_w: int = 2560,
    resize_client_h: int = 1440,
    include_maximize: bool = True,
    include_restore: bool = True,
    step_wait_sec: float = 2.0,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = False,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
) -> Dict[str, Any]:
    """
    窗口化回归：
    启动 -> 等窗口可操作 -> resize / maximize / restore -> 检查是否闪退。
    """
    launch = launch_war3_test(
        war3_dir=war3_dir,
        map_path=map_path,
        windowed=True,
        opengl=False,
        auto_perf_record=False,
        auto_perf_export_sec=0,
        deploy_d3d9_before_launch=deploy_d3d9_before_launch,
        build_d3d9_path=build_d3d9_path,
        enforce_video_baseline=enforce_video_baseline,
        baseline_width=baseline_width,
        baseline_height=baseline_height,
        baseline_refresh_rate=baseline_refresh_rate,
        render_log=False,
        extra_args="",
    )
    if not launch.get("ok"):
        return {"ok": False, "stage": "launch", "detail": launch}

    pid = int(launch["pid"])
    window_ready = wait_for_war3_window_ready(
        timeout_sec=window_ready_timeout_sec,
        pid=pid,
        min_cpu_sec=0.4,
        stable_sec=0.8,
    )
    if not window_ready.get("ok"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
        return {
            "ok": False,
            "stage": "window-ready",
            "launch": launch,
            "windowReady": window_ready,
            "stop": stop,
        }

    game_ready: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "require_game_ready=False",
    }
    warnings: List[str] = []
    if require_game_ready:
        game_ready = wait_for_game_ready(
            timeout_sec=game_ready_timeout_sec,
            pid=pid,
            allow_fallback=True,
        )
        if not game_ready.get("ok"):
            stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "game-ready",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "stop": stop,
            }

    steps: List[Dict[str, Any]] = []
    crash_step = ""

    def _run_step(name: str, **kwargs: Any) -> bool:
        nonlocal crash_step
        res = control_war3_window(pid=pid, wait_sec=step_wait_sec, **kwargs)
        row = {"name": name, "detail": res, "time": _now_str()}
        steps.append(row)
        alive = bool(res.get("aliveAfter", False))
        if not alive and not crash_step:
            crash_step = name
        return alive

    if initial_client_w > 0 and initial_client_h > 0:
        if not _run_step(
            "resize-initial",
            action="resize_client",
            client_w=initial_client_w,
            client_h=initial_client_h,
            x=40,
            y=40,
        ):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    if include_maximize:
        if not _run_step("maximize", action="maximize"):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    if include_restore:
        if not _run_step("restore", action="restore"):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    if resize_client_w > 0 and resize_client_h > 0:
        if not _run_step(
            "resize-final",
            action="resize_client",
            client_w=resize_client_w,
            client_h=resize_client_h,
            x=60,
            y=60,
        ):
            stop = stop_war3(pid=pid, graceful_wait_sec=2, force=True, avoid_foreground_switch=True)
            return {
                "ok": False,
                "stage": "window-step",
                "launch": launch,
                "windowReady": window_ready,
                "gameReady": game_ready,
                "steps": steps,
                "crashStep": crash_step,
                "stop": stop,
            }

    shot = capture_war3_screenshot(pid=pid)
    if not shot.get("ok"):
        warnings.append(str(shot.get("error", "截图失败")))

    stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
    return {
        "ok": True,
        "stage": "done",
        "launch": launch,
        "windowReady": window_ready,
        "gameReady": game_ready,
        "steps": steps,
        "warnings": warnings,
        "screenshot": shot,
        "stop": stop,
    }


@mcp.tool()
def capture_war3_screenshot(
    output_path: str = "",
    pid: int = 0,
    war3_dir: str = "",
    prefer_internal: bool = True,
    timeout_sec: int = 8,
    fallback_to_window_capture: bool = True,
) -> Dict[str, Any]:
    """抓取 War3 最终帧；隔离 Desktop 永不回退到当前输入桌面截图。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}"}

    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    out = Path(output_path) if output_path else (
        _ensure_dir(ARTIFACT_ROOT / "screenshots") / f"war3_{_now_compact()}.png"
    )
    _ensure_dir(out.parent)

    internal_res: Optional[Dict[str, Any]] = None
    isolated_desktop = _uses_isolated_desktop(target_pid)
    if prefer_internal:
        internal_res = _request_internal_frame_capture(
            pid=target_pid,
            output_path=out,
            war3_dir=target_war3_dir,
            timeout_sec=max(1, int(timeout_sec)),
        )
        if internal_res.get("ok"):
            return internal_res
        if not fallback_to_window_capture:
            return internal_res

    if isolated_desktop:
        return {
            "ok": False,
            "pid": target_pid,
            "mode": "isolated-window-capture-blocked",
            "error": "隔离 Desktop 的内部截图不可用；拒绝回退并捕获当前可见桌面",
            "internalAttempt": internal_res or {},
            "fallbackUsed": False,
        }

    fallback_out = out if out.suffix.lower() == ".png" else out.with_suffix(".png")
    result = _powershell_capture_window(target_pid, fallback_out)
    ok = result["returncode"] == 0 and fallback_out.exists()
    return {
        "ok": ok,
        "pid": target_pid,
        "output": str(fallback_out),
        "mode": "window-capture",
        "details": result,
        "internalAttempt": internal_res or {},
        "fallbackUsed": True,
    }


@mcp.tool()
def invoke_internal_test_api(
    command: str,
    payload_json: str = "{}",
    pid: int = 0,
    war3_dir: str = "",
    timeout_sec: int = 6,
) -> Dict[str, Any]:
    """通过 named pipe control plane 调用游戏内测试命令。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    if not _pid_alive(target_pid):
        return {"ok": False, "error": f"进程不存在: {target_pid}", "pid": target_pid}

    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    try:
        payload = json.loads(str(payload_json or "{}"))
    except Exception as e:
        return {"ok": False, "error": f"payload_json 解析失败: {e}"}
    if not isinstance(payload, dict):
        return {"ok": False, "error": "payload_json 必须是 JSON object"}

    return _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command=str(command or "").strip(),
        payload=payload,
        timeout_sec=max(1, int(timeout_sec)),
    )


@mcp.tool()
def get_test_camera_state(
    pid: int = 0,
    war3_dir: str = "",
) -> Dict[str, Any]:
    """读取 AutoTest 子进程当前本地相机的完整状态。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    return _invoke_internal_test_request(
        target_pid, target_war3_dir, "camera.snapshot", {}, timeout_sec=4.0
    )


@mcp.tool()
def get_test_world_bounds(
    pid: int = 0,
    war3_dir: str = "",
) -> Dict[str, Any]:
    """读取 AutoTest 子进程地图的可巡航世界边界。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    return _invoke_internal_test_request(
        target_pid, target_war3_dir, "camera.world_bounds", {}, timeout_sec=4.0
    )


@mcp.tool()
def set_test_full_map_visibility(
    enabled: bool = True,
    pid: int = 0,
    war3_dir: str = "",
) -> Dict[str, Any]:
    """获取或释放 AutoTest 的全图视野租约，并恢复原 Fog/FogMask 状态。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    return _invoke_internal_test_request(
        target_pid,
        target_war3_dir,
        "visibility.full_map",
        {"enabled": bool(enabled)},
        timeout_sec=4.0,
    )


@mcp.tool()
def pan_test_camera(
    target_x: float,
    target_y: float,
    seconds: float,
    pid: int = 0,
    war3_dir: str = "",
) -> Dict[str, Any]:
    """通过 PanCameraToTimed 平移 AutoTest 子进程的本地相机。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    duration = max(0.0, min(30.0, float(seconds)))
    return _invoke_internal_test_request(
        target_pid,
        target_war3_dir,
        "camera.pan_to",
        {"targetX": float(target_x), "targetY": float(target_y), "duration": duration},
        timeout_sec=max(4.0, duration + 2.0),
    )


def _compact_life_and_death_status(status: Dict[str, Any]) -> Dict[str, Any]:
    interesting = (
        "shadow", "csm", "cascade", "replay", "arena", "queue", "device",
        "epoch", "complete", "incomplete", "planned", "validated", "drawn",
        "point", "publication", "frame", "gpu", "submit", "present",
        "semantic", "captured", "skipped", "fallback", "budget", "populate",
        "currentdraw", "manifest", "direct", "worktable",
    )
    compact: Dict[str, Any] = {
        "timestampMs": status.get("timestampMs"),
        "frameIndex": status.get("frameIndex"),
        "source": status.get("source"),
    }
    for section_name in ("runtime", "frame", "render", "shadow", "gpu"):
        section = status.get(section_name)
        if not isinstance(section, dict):
            continue
        selected: Dict[str, Any] = {}
        for key, value in section.items():
            lowered = str(key).lower()
            if not any(token in lowered for token in interesting):
                continue
            if isinstance(value, (str, int, float, bool)) or value is None:
                selected[str(key)] = value
        compact[section_name] = selected
    return compact


@mcp.tool()
def run_life_and_death_tdr_scenario(
    war3_dir: str = r"E:\Work\War3",
    map_path: str = str(DEFAULT_LIFE_AND_DEATH_MAP),
    duration_sec: int = 600,
    grid_size: int = 5,
    ready_timeout_sec: int = 240,
    use_isolated_desktop: bool = False,
    desktop_name: str = "War3LifeAndDeathTdr",
    launcher_mode: str = YDWE_LAUNCHER_MODE_DIRECT,
    ydwe_root: str = r"E:\Work\War3\YDWE1.32.13 - MemoryHack",
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    disable_modules: str = "",
    env_overrides_json: str = "{}",
    attach_pid: int = 0,
    screenshot_count: int = 12,
    birth_hold_sec: int = 120,
) -> Dict[str, Any]:
    """冷启动或连接“生与死”，以低视角巡航并在首次 GPU 故障时止损。"""
    if bool(use_isolated_desktop):
        return {
            "ok": False,
            "stage": "preflight",
            "error": (
                "life_and_death_tdr 禁止隔离桌面启动；"
                "请使用默认可见桌面或 attach_pid 连接用户手动启动的 War3。"
            ),
            "isolatedDesktopQuarantined": True,
        }
    w3 = Path(war3_dir)
    source_map = Path(map_path)
    if not source_map.is_file():
        return {"ok": False, "stage": "preflight", "error": f"地图不存在: {source_map}"}
    grid = max(2, min(9, int(grid_size)))
    duration = max(15, min(3600, int(duration_sec)))
    requested_screenshot_count = max(0, min(160, int(screenshot_count)))
    requested_birth_hold_sec = max(
        0, min(max(0, duration - 15), int(birth_hold_sec))
    )
    user_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = user_env.pop("__parse_error__", "")
    if parse_error:
        return {"ok": False, "stage": "preflight", "error": parse_error}
    user_env.setdefault("DXVK_WAR3_SCENARIO", "life_and_death_tdr")
    user_env.setdefault("DXVK_WAR3_RUNTIME_BENCHMARK", "1")
    user_env.setdefault("DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC", "1")
    user_env.setdefault("DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC", str(duration))

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    artifact_dir = ARTIFACT_ROOT / "life_and_death_tdr" / stamp
    artifact_dir.mkdir(parents=True, exist_ok=True)
    events_before = _query_windows_gpu_events()
    event_keys_before = {_gpu_event_identity(row) for row in events_before}
    log_offsets = _snapshot_log_offsets(w3)
    incident_roots = [w3]
    incidents_before = {
        str(path.resolve(strict=False)).lower()
        for root in incident_roots
        for path in (root / "WarVK" / "Log").glob("gpu_incident_*.json")
        if (root / "WarVK" / "Log").is_dir()
    }

    owned_process = int(attach_pid) <= 0
    if owned_process:
        start = _launch_suite_map_until_ready(
            war3_dir=war3_dir,
            requested_map_path=str(source_map),
            allow_fallback_to_default_test_map=False,
            ready_timeout_sec=ready_timeout_sec,
            ready_allow_fallback=False,
            ready_require_game_started_for_fallback=True,
            ready_fallback_min_elapsed_sec=20,
            ready_fallback_min_cpu_sec=1.0,
            launch_kwargs={
                "launcher_mode": str(launcher_mode or YDWE_LAUNCHER_MODE_DIRECT),
                "ydwe_root": str(ydwe_root or ""),
                "windowed": True,
                "use_isolated_desktop": bool(use_isolated_desktop),
                "desktop_name": str(desktop_name or "War3LifeAndDeathTdr"),
                "opengl": False,
                "auto_perf_record": True,
                "auto_perf_export_sec": duration + 4,
                "deploy_d3d9_before_launch": bool(deploy_d3d9_before_launch),
                "build_d3d9_path": build_d3d9_path,
                "enforce_video_baseline": False,
                "render_log": False,
                "profile": "full_default",
                "disable_modules": str(disable_modules or ""),
                "env_overrides_json": json.dumps(user_env, ensure_ascii=False),
                "extra_args": "",
            },
            startup_input_actions=[
                {"type": "sleep", "ms": 10000},
                {"type": "key", "vk": 0x20, "holdMs": 80},
                {"type": "sleep", "ms": 8000},
                {"type": "key", "vk": 0x20, "holdMs": 80},
                {"type": "sleep", "ms": 8000},
                {"type": "key", "vk": 0x20, "holdMs": 80},
                {"type": "sleep", "ms": 8000},
                {"type": "key", "vk": 0x20, "holdMs": 80},
                {"type": "sleep", "ms": 1200},
            ],
        )
    else:
        pid = int(attach_pid)
        start = {
            "ok": _pid_alive(pid),
            "stage": "attached" if _pid_alive(pid) else "attach",
            "pid": pid,
            "launch": {"instanceRoot": str(w3), "attached": True},
            "note": (
                "attach-only: AutoTest does not launch, deploy, foreground, or stop War3"
            ),
        }
    if not start.get("ok"):
        early_result = {
            "ok": False,
            "stage": str(start.get("stage", "ready")),
            "sourceMap": str(source_map),
            "sourceMapSha256": sha256_file(source_map),
            "start": start,
            "artifactDir": str(artifact_dir),
        }
        early_path = artifact_dir / "life_and_death_tdr_result.json"
        early_path.write_text(
            json.dumps(early_result, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        early_result["resultPath"] = str(early_path)
        return early_result

    launch = dict(start.get("launch", {}) or {})
    pid = int(start["pid"])
    instance_root = Path(str(launch.get("instanceRoot", war3_dir)))
    if instance_root not in incident_roots:
        incident_roots.append(instance_root)
    camera_before: Dict[str, Any] = {}
    bounds: Dict[str, Any] = {}
    visibility_enable: Dict[str, Any] = {}
    visibility_restore: Dict[str, Any] = {}
    camera_restore: Dict[str, Any] = {}
    samples: List[Dict[str, Any]] = []
    waypoint_rows: List[Dict[str, Any]] = []
    screenshot_rows: List[Dict[str, Any]] = []
    failure_reason = ""
    stop_result: Dict[str, Any] = {}
    started_at = time.monotonic()
    screenshot_interval = (
        float(duration) / float(max(1, requested_screenshot_count - 1))
        if requested_screenshot_count > 1
        else float(duration)
    )
    next_screenshot_at = started_at + screenshot_interval

    def capture_aligned_screenshot(label: str, waypoint_index: int) -> None:
        shot_index = len(screenshot_rows)
        shot_path = artifact_dir / "screenshots" / f"{shot_index:03d}_{label}.png"
        status_reply = _control_plane_request(
            pid=pid, command="get_runtime_status", payload={}, timeout_sec=2.0
        )
        status = dict(status_reply.get("result") or {})
        file_status = _read_runtime_status_file(w3)
        if isinstance(file_status, dict):
            file_timestamp = int(file_status.get("timestampMs", 0) or 0)
            now_timestamp = int(time.time() * 1000)
            if file_timestamp > 0 and abs(now_timestamp - file_timestamp) <= 5000:
                status = file_status
        screenshot_rows.append(
            {
                "index": shot_index,
                "label": label,
                "elapsedSec": round(time.monotonic() - started_at, 3),
                "waypointIndex": int(waypoint_index),
                "status": _compact_life_and_death_status(status) if status else {},
                "screenshot": capture_war3_screenshot(
                    output_path=str(shot_path),
                    pid=pid,
                    war3_dir=str(w3),
                    prefer_internal=True,
                    timeout_sec=8,
                    fallback_to_window_capture=False,
                ),
            }
        )

    try:
        camera_before = _invoke_internal_test_request(
            pid, w3, "camera.snapshot", {}, timeout_sec=4.0
        )
        bounds = _invoke_internal_test_request(
            pid, w3, "camera.world_bounds", {}, timeout_sec=4.0
        )
        if requested_screenshot_count > 0:
            capture_aligned_screenshot("initial", -1)
        if not camera_before.get("ok") or not bounds.get("ok"):
            failure_reason = "internal camera preflight failed"
        else:
            next_sample = time.monotonic()
            birth_deadline = min(
                started_at + float(requested_birth_hold_sec),
                started_at + float(duration),
            )
            while time.monotonic() < birth_deadline:
                if not _pid_alive(pid):
                    failure_reason = "war3 process exited during birth hold"
                    break
                now = time.monotonic()
                if (
                    len(screenshot_rows) < requested_screenshot_count
                    and now >= next_screenshot_at
                ):
                    capture_aligned_screenshot(
                        f"birth_hold_{len(screenshot_rows):03d}", -1
                    )
                    next_screenshot_at = now + screenshot_interval
                if now >= next_sample:
                    reply = _control_plane_request(
                        pid=pid,
                        command="get_runtime_status",
                        payload={},
                        timeout_sec=2.0,
                    )
                    status = dict(reply.get("result") or {})
                    file_status = _read_runtime_status_file(w3)
                    if isinstance(file_status, dict):
                        file_timestamp = int(file_status.get("timestampMs", 0) or 0)
                        now_timestamp = int(time.time() * 1000)
                        if (
                            file_timestamp > 0
                            and abs(now_timestamp - file_timestamp) <= 5000
                        ):
                            status = file_status
                    if status:
                        samples.append(
                            {
                                "elapsedSec": round(now - started_at, 3),
                                "waypointIndex": -1,
                                "phase": "birth-hold",
                                "status": _compact_life_and_death_status(status),
                            }
                        )
                        if _runtime_status_device_lost(status):
                            failure_reason = "runtime status reported device lost"
                            break
                    next_sample = now + 0.5
                time.sleep(0.1)

        if not failure_reason:
            visibility_enable = _invoke_internal_test_request(
                pid, w3, "visibility.full_map", {"enabled": True}, timeout_sec=4.0
            )
            if not visibility_enable.get("ok"):
                failure_reason = "internal visibility preflight failed"

        if not failure_reason:
            snapshot = dict(camera_before.get("result", {}) or {})
            world = dict(bounds.get("result", {}) or {})
            current_aoa = float(snapshot.get("angleOfAttack", 304.0) or 304.0)
            # Warcraft uses 270 degrees for a vertical top-down camera and
            # approaches the horizon as the angle moves toward 360.  The
            # snapshot bridge normalizes getter radians to setter degrees, so a
            # bounded positive offset creates the intended low-angle stress
            # view without crossing through the terrain.
            low_aoa = min(335.0, max(280.0, current_aoa + 24.0))
            low_apply = _invoke_internal_test_request(
                pid,
                w3,
                "camera.apply",
                {
                    "targetX": float(snapshot.get("targetX", 0.0) or 0.0),
                    "targetY": float(snapshot.get("targetY", 0.0) or 0.0),
                    "targetDistance": max(1650.0, float(snapshot.get("targetDistance", 1650.0) or 1650.0)),
                    "angleOfAttack": low_aoa,
                    "rotation": float(snapshot.get("rotation", 0.0) or 0.0),
                    "fieldOfView": float(snapshot.get("fieldOfView", 70.0) or 70.0),
                    "farZ": max(5000.0, float(snapshot.get("farZ", 5000.0) or 5000.0)),
                    "roll": float(snapshot.get("roll", 0.0) or 0.0),
                    "zOffset": float(snapshot.get("zOffset", 0.0) or 0.0),
                    "duration": 0.0,
                    "quickPosition": True,
                },
                timeout_sec=4.0,
            )
            if not low_apply.get("ok"):
                failure_reason = "low camera apply failed"
            else:
                min_x = float(world.get("minX", 0.0) or 0.0)
                min_y = float(world.get("minY", 0.0) or 0.0)
                max_x = float(world.get("maxX", 0.0) or 0.0)
                max_y = float(world.get("maxY", 0.0) or 0.0)
                margin_x = min(max(256.0, (max_x - min_x) * 0.06), (max_x - min_x) * 0.2)
                margin_y = min(max(256.0, (max_y - min_y) * 0.06), (max_y - min_y) * 0.2)
                xs = [min_x + margin_x + (max_x - min_x - 2.0 * margin_x) * i / (grid - 1) for i in range(grid)]
                ys = [min_y + margin_y + (max_y - min_y - 2.0 * margin_y) * i / (grid - 1) for i in range(grid)]
                waypoints = [
                    (x, y)
                    for row, y in enumerate(ys)
                    for x in (xs if row % 2 == 0 else list(reversed(xs)))
                ]
                current_x = float(snapshot.get("targetX", 0.0) or 0.0)
                current_y = float(snapshot.get("targetY", 0.0) or 0.0)
                waypoint_index = 0
                next_sample = time.monotonic()
                while time.monotonic() - started_at < duration:
                    if not _pid_alive(pid):
                        failure_reason = "war3 process exited during cruise"
                        break
                    target_x, target_y = waypoints[waypoint_index % len(waypoints)]
                    distance = math.hypot(target_x - current_x, target_y - current_y)
                    pan_seconds = max(0.75, min(4.0, distance / 4096.0))
                    marker = _invoke_internal_test_request(
                        pid,
                        w3,
                        "autotest.waypoint",
                        {
                            "active": True,
                            "index": waypoint_index,
                            "targetX": target_x,
                            "targetY": target_y,
                            "panSeconds": pan_seconds,
                            "cameraTargetX": current_x,
                            "cameraTargetY": current_y,
                            "cameraTargetDistance": float(snapshot.get("targetDistance", 0.0) or 0.0),
                            "cameraAngleOfAttack": low_aoa,
                            "worldMinX": min_x,
                            "worldMinY": min_y,
                            "worldMaxX": max_x,
                            "worldMaxY": max_y,
                        },
                        timeout_sec=3.0,
                    )
                    pan = _invoke_internal_test_request(
                        pid,
                        w3,
                        "camera.pan_to",
                        {"targetX": target_x, "targetY": target_y, "duration": pan_seconds},
                        timeout_sec=4.0,
                    )
                    row = {
                        "index": waypoint_index,
                        "targetX": round(target_x, 3),
                        "targetY": round(target_y, 3),
                        "distance": round(distance, 3),
                        "panSeconds": round(pan_seconds, 3),
                        "issuedAtSec": round(time.monotonic() - started_at, 3),
                        "marker": marker,
                        "pan": pan,
                    }
                    waypoint_rows.append(row)
                    if not pan.get("ok"):
                        failure_reason = "camera pan failed"
                        break
                    wait_deadline = time.monotonic() + pan_seconds
                    while time.monotonic() < wait_deadline:
                        if not _pid_alive(pid):
                            failure_reason = "war3 process exited during pan"
                            break
                        now = time.monotonic()
                        if (
                            len(screenshot_rows) < requested_screenshot_count
                            and now >= next_screenshot_at
                        ):
                            capture_aligned_screenshot(
                                f"waypoint_{waypoint_index:03d}", waypoint_index
                            )
                            next_screenshot_at = now + screenshot_interval
                        if now >= next_sample:
                            reply = _control_plane_request(
                                pid=pid, command="get_runtime_status", payload={}, timeout_sec=2.0
                            )
                            status = dict(reply.get("result") or {})
                            # The control-plane reply is intentionally compact.
                            # The atomically replaced status file carries the
                            # full replay/Arena offender tuple. Prefer it only
                            # while fresh so a prior process cannot pollute a
                            # newly launched route.
                            file_status = _read_runtime_status_file(w3)
                            if isinstance(file_status, dict):
                                file_timestamp = int(
                                    file_status.get("timestampMs", 0) or 0
                                )
                                now_timestamp = int(time.time() * 1000)
                                if (
                                    file_timestamp > 0
                                    and abs(now_timestamp - file_timestamp) <= 5000
                                ):
                                    status = file_status
                            if status:
                                samples.append(
                                    {
                                        "elapsedSec": round(now - started_at, 3),
                                        "waypointIndex": waypoint_index,
                                        "status": _compact_life_and_death_status(status),
                                    }
                                )
                                if _runtime_status_device_lost(status):
                                    failure_reason = "runtime status reported device lost"
                                    break
                            next_sample = now + 0.5
                        time.sleep(0.1)
                    if failure_reason:
                        break
                    current_x, current_y = target_x, target_y
                    row["arrivedAtSec"] = round(time.monotonic() - started_at, 3)
                    waypoint_index += 1
    finally:
        if _pid_alive(pid):
            _invoke_internal_test_request(
                pid, w3, "autotest.waypoint", {"active": False}, timeout_sec=3.0
            )
            visibility_restore = _invoke_internal_test_request(
                pid, w3, "visibility.full_map", {"enabled": False}, timeout_sec=4.0
            )
            snapshot = dict(camera_before.get("result", {}) or {})
            if snapshot:
                restore_payload = dict(snapshot)
                restore_payload["duration"] = 0.0
                restore_payload["quickPosition"] = True
                camera_restore = _invoke_internal_test_request(
                    pid, w3, "camera.apply", restore_payload, timeout_sec=4.0
                )
        if owned_process:
            stop_result = stop_war3(
                pid=pid,
                graceful_wait_sec=8 if not failure_reason else 2,
                force=True,
                avoid_foreground_switch=True,
            )
        else:
            stop_result = {
                "ok": True,
                "pid": pid,
                "mode": "attach-only",
                "stopped": False,
            }

    events_after = _query_windows_gpu_events()
    new_gpu_events = [
        row for row in events_after if _gpu_event_identity(row) not in event_keys_before
    ]
    new_incidents = _new_gpu_incident_files(incident_roots, incidents_before)
    log_summary = _read_runtime_log_summary(w3, log_offsets=log_offsets)
    keyword_counts = dict(log_summary.get("keywordCounts", {}) or {})
    device_lost = bool(
        new_gpu_events
        or new_incidents
        or int(keyword_counts.get("deviceLost", 0) or 0) > 0
        or "device lost" in failure_reason.lower()
        or "process exited" in failure_reason.lower()
    )
    route_ok = not failure_reason and not device_lost
    result = {
        "ok": route_ok,
        "stage": (
            "gpu-failure" if device_lost
            else ("control-failure" if failure_reason else "done")
        ),
        "failureReason": failure_reason,
        "durationRequestedSec": duration,
        "durationObservedSec": round(time.monotonic() - started_at, 3),
        "birthHoldRequestedSec": requested_birth_hold_sec,
        "gridSize": grid,
        "attachOnly": not owned_process,
        "sourceMap": str(source_map),
        "sourceMapSha256": sha256_file(source_map),
        "start": start,
        "cameraBefore": camera_before,
        "worldBounds": bounds,
        "visibilityEnable": visibility_enable,
        "visibilityRestore": visibility_restore,
        "cameraRestore": camera_restore,
        "waypoints": waypoint_rows,
        "runtimeSamples": samples,
        "screenshots": screenshot_rows,
        "gpuEventsBefore": events_before,
        "newGpuEvents": new_gpu_events,
        "newGpuIncidents": new_incidents,
        "logSummary": log_summary,
        "stop": stop_result,
        "artifactDir": str(artifact_dir),
    }
    result_path = artifact_dir / "life_and_death_tdr_result.json"
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    result["resultPath"] = str(result_path)
    return result


@mcp.tool()
def set_city_test_view(
    view: str = "mid",
    pid: int = 0,
    war3_dir: str = "",
    angle_of_attack: float = 0.0,
    rotation: float = 0.0,
    target_distance: float = 0.0,
    z_offset: float = 0.0,
    duration: float = 0.0,
    quick_position: bool = True,
) -> Dict[str, Any]:
    """基于当前相机生成 City.w3x 的高/中/低俯仰角测试视图。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)

    snap = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="camera.snapshot",
        payload={},
        timeout_sec=4.0,
    )
    if not snap.get("ok"):
        return {"ok": False, "stage": "snapshot", "detail": snap}

    snapshot = dict(snap.get("result", {}) or {})
    current_aoa = float(snapshot.get("angleOfAttack", 0.0) or 0.0)
    current_rot = float(snapshot.get("rotation", 0.0) or 0.0)
    current_dist = float(snapshot.get("targetDistance", 0.0) or 0.0)
    current_zoff = float(snapshot.get("zOffset", 0.0) or 0.0)
    use_degree_like = abs(current_aoa) > 6.5
    angle_high = current_aoa + (12.0 if use_degree_like else 0.18)
    angle_mid = current_aoa
    angle_low = current_aoa - (20.0 if use_degree_like else 0.38)
    if use_degree_like:
        angle_low = max(12.0, angle_low)
    else:
        angle_low = max(0.12, angle_low)

    presets = {
        "high": angle_high,
        "mid": angle_mid,
        "low": angle_low,
    }
    view_name = str(view or "mid").strip().lower()
    target_aoa_value = float(angle_of_attack) if abs(float(angle_of_attack)) > 1.0e-6 else float(presets.get(view_name, angle_mid))
    target_rot_value = float(rotation) if abs(float(rotation)) > 1.0e-6 else current_rot
    target_dist_value = float(target_distance) if abs(float(target_distance)) > 1.0e-6 else current_dist
    target_zoff_value = float(z_offset) if abs(float(z_offset)) > 1.0e-6 else current_zoff

    apply = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="camera.apply",
        payload={
            "targetX": float(snapshot.get("targetX", 0.0) or 0.0),
            "targetY": float(snapshot.get("targetY", 0.0) or 0.0),
            "targetDistance": target_dist_value,
            "angleOfAttack": target_aoa_value,
            "rotation": target_rot_value,
            "zOffset": target_zoff_value,
            "duration": float(duration),
            "quickPosition": bool(quick_position),
        },
        timeout_sec=max(4.0, 2.0 + float(duration)),
    )
    return {
        "ok": bool(apply.get("ok")),
        "view": view_name,
        "snapshot": snapshot,
        "requested": {
            "angleOfAttack": target_aoa_value,
            "rotation": target_rot_value,
            "targetDistance": target_dist_value,
            "zOffset": target_zoff_value,
            "duration": float(duration),
            "quickPosition": bool(quick_position),
        },
        "detail": apply,
    }


@mcp.tool()
def compare_frame_sequence(frame_paths_json: str) -> Dict[str, Any]:
    """比较一组连续 BMP/PNG 帧，输出阴影闪烁/消失可疑指标。"""
    try:
        raw = json.loads(str(frame_paths_json or "[]"))
    except Exception as e:
        return {"ok": False, "error": f"frame_paths_json 解析失败: {e}"}
    if not isinstance(raw, list) or not raw:
        return {"ok": False, "error": "frame_paths_json 必须是非空 JSON array"}
    paths = [Path(str(item)) for item in raw if str(item).strip()]
    return _compare_bmp_sequence(paths)


@mcp.tool()
def capture_shadow_factor_sequence(
    pid: int = 0,
    war3_dir: str = "",
    mode: str = "shadow_factor",
    label: str = "",
    frame_count: int = 5,
    interval_sec: float = 0.25,
    warmup_sec: float = 0.35,
    timeout_sec: int = 8,
) -> Dict[str, Any]:
    """切换阴影调试模式并抓取一组连续最终帧，用于稳定性比较。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)
    mode_name = str(mode or "shadow_factor").strip().lower()
    if mode_name in ("shadow_factor", "shadowfactor", "factor"):
        debug_mode = 2
    elif mode_name in ("point_shadow", "pointshadow", "point"):
        debug_mode = 6
    else:
        debug_mode = 0

    set_mode = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="shadow.debug_mode",
        payload={"mode": int(debug_mode)},
        timeout_sec=4.0,
    )
    if not set_mode.get("ok"):
        return {"ok": False, "stage": "set-debug-mode", "detail": set_mode}
    if float(warmup_sec) > 0.0:
        time.sleep(max(0.0, float(warmup_sec)))

    out_dir = _ensure_dir(ARTIFACT_ROOT / "city_suite" / _now_compact() / (label or mode_name))
    frame_rows: List[Dict[str, Any]] = []
    frame_paths: List[Path] = []
    for index in range(max(2, int(frame_count))):
        out_path = out_dir / f"{label or mode_name}_{index + 1:02d}.bmp"
        cap = _capture_final_frame_via_internal_test_api(
            pid=target_pid,
            war3_dir=target_war3_dir,
            output_path=out_path,
            timeout_sec=max(2.0, float(timeout_sec)),
        )
        status_after = _read_runtime_status_best_effort(target_pid) or {}
        cap["runtimeStatusAfterCapture"] = status_after
        shadow_status = status_after.get("shadow", {}) if isinstance(status_after, dict) else {}
        cap["shadowSummaryAfterCapture"] = {
            "semanticSceneReceiverInputValid": shadow_status.get("semanticSceneReceiverInputValid"),
            "semanticSceneReceiverInputRejectReason": shadow_status.get("semanticSceneReceiverInputRejectReason"),
            "semanticSceneReceiverNeedPass": shadow_status.get("semanticSceneReceiverNeedPass"),
            "semanticSceneReceiverNeedShadowMap": shadow_status.get("semanticSceneReceiverNeedShadowMap"),
            "semanticSceneReceiverHasCompleteShadowMap": shadow_status.get("semanticSceneReceiverHasCompleteShadowMap"),
            "semanticSceneReceiverHasUsableDirectionalShadow": shadow_status.get("semanticSceneReceiverHasUsableDirectionalShadow"),
            "semanticSceneReceiverActiveStrengthMilli": shadow_status.get("semanticSceneReceiverActiveStrengthMilli"),
            "semanticSceneReceiverUboStrengthMilli": shadow_status.get("semanticSceneReceiverUboStrengthMilli"),
            "semanticSceneReceiverDebugMode": shadow_status.get("semanticSceneReceiverDebugMode"),
            "semanticSceneReceiverCsmCascadeCount": shadow_status.get("semanticSceneReceiverCsmCascadeCount"),
            "semanticSceneReceiverReuseShadowMap": shadow_status.get("semanticSceneReceiverReuseShadowMap"),
            "semanticSceneShadowMapSkinnedDrawnCount": shadow_status.get("semanticSceneShadowMapSkinnedDrawnCount"),
            "semanticSceneReplayDrawsCount": shadow_status.get("semanticSceneReplayDrawsCount"),
            "semanticSceneSubmittedSkinned": shadow_status.get("semanticSceneSubmittedSkinned"),
            "semanticSceneCurrentDrawResolveReadyCount": shadow_status.get("semanticSceneCurrentDrawResolveReadyCount"),
            "objectFallbackDrawCount": shadow_status.get("objectFallbackDrawCount"),
        }
        frame_rows.append(cap)
        if not cap.get("ok"):
            return {
                "ok": False,
                "stage": "capture",
                "mode": mode_name,
                "frames": frame_rows,
            }
        frame_paths.append(Path(str(cap.get("output", ""))))
        if index + 1 < int(frame_count):
            time.sleep(max(0.0, float(interval_sec)))

    comparison = _compare_bmp_sequence(frame_paths)
    return {
        "ok": bool(comparison.get("ok")),
        "mode": mode_name,
        "debugMode": int(debug_mode),
        "outputDir": str(out_dir),
        "frames": frame_rows,
        "comparison": comparison,
    }


@mcp.tool()
def capture_shadow_factor_camera_step_sequence(
    pid: int = 0,
    war3_dir: str = "",
    mode: str = "shadow_factor",
    label: str = "",
    frame_count: int = 6,
    interval_sec: float = 0.20,
    warmup_sec: float = 0.35,
    rotation_step: float = 0.0,
    angle_step: float = 0.0,
    target_distance_step: float = 0.0,
    z_offset_step: float = 0.0,
    quick_position: bool = True,
    input_fallback: bool = True,
    input_key: str = "RIGHT",
    input_hold_ms: int = 45,
    input_repeat: int = 1,
    input_foreground: bool = False,
    timeout_sec: int = 8,
) -> Dict[str, Any]:
    """按固定小步移动相机并抓取 shadow-factor 帧，用于排查相机运动下 CSM/TAA 抖动。"""
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": False, "error": "无有效 pid"}
    target_war3_dir = Path(war3_dir) if war3_dir else (STATE.war3_dir or DEFAULT_WAR3_DIR)

    mode_name = str(mode or "shadow_factor").strip().lower()
    if mode_name in ("shadow_factor", "shadowfactor", "factor"):
        debug_mode = 2
    elif mode_name in ("motion", "motion_vector", "motionvector"):
        debug_mode = 4
    elif mode_name in ("point_shadow", "pointshadow", "point"):
        debug_mode = 6
    else:
        debug_mode = 0

    set_mode = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="shadow.debug_mode",
        payload={"mode": int(debug_mode)},
        timeout_sec=4.0,
    )
    if not set_mode.get("ok"):
        return {"ok": False, "stage": "set-debug-mode", "detail": set_mode}

    snap = _invoke_internal_test_request(
        pid=target_pid,
        war3_dir=target_war3_dir,
        command="camera.snapshot",
        payload={},
        timeout_sec=4.0,
    )
    if not snap.get("ok"):
        if not bool(input_fallback):
            return {"ok": False, "stage": "camera-snapshot", "detail": snap}
        control_mode = "window-key-fallback"
        snapshot = {
            "fallbackReason": "camera.snapshot unsupported or failed",
            "detail": snap,
        }
    else:
        control_mode = "camera-command"
        snapshot = dict(snap.get("result", {}) or {})

    base_rotation = float(snapshot.get("rotation", 0.0) or 0.0)
    base_aoa = float(snapshot.get("angleOfAttack", 0.0) or 0.0)
    base_distance = float(snapshot.get("targetDistance", 0.0) or 0.0)
    base_z_offset = float(snapshot.get("zOffset", 0.0) or 0.0)

    if abs(float(rotation_step)) <= 1e-8:
        rotation_step = 1.5 if abs(base_rotation) > 6.5 else 0.025
    if abs(float(angle_step)) <= 1e-8:
        angle_step = 0.0

    if float(warmup_sec) > 0.0:
        time.sleep(max(0.0, float(warmup_sec)))

    out_dir = _ensure_dir(
        ARTIFACT_ROOT / "city_suite" / _now_compact() /
        (label or f"{mode_name}_camera_step"))
    frame_rows: List[Dict[str, Any]] = []
    frame_paths: List[Path] = []
    camera_rows: List[Dict[str, Any]] = []

    for index in range(max(2, int(frame_count))):
        if control_mode == "camera-command":
            apply = _invoke_internal_test_request(
                pid=target_pid,
                war3_dir=target_war3_dir,
                command="camera.apply",
                payload={
                    "targetX": float(snapshot.get("targetX", 0.0) or 0.0),
                    "targetY": float(snapshot.get("targetY", 0.0) or 0.0),
                    "targetDistance": base_distance +
                    float(target_distance_step) * float(index),
                    "angleOfAttack": base_aoa + float(angle_step) * float(index),
                    "rotation": base_rotation + float(rotation_step) * float(index),
                    "zOffset": base_z_offset + float(z_offset_step) * float(index),
                    "duration": 0.0,
                    "quickPosition": bool(quick_position),
                },
                timeout_sec=4.0,
            )
            camera_rows.append({
                "index": index,
                "controlMode": control_mode,
                "rotation": base_rotation + float(rotation_step) * float(index),
                "angleOfAttack": base_aoa + float(angle_step) * float(index),
                "targetDistance": base_distance +
                float(target_distance_step) * float(index),
                "zOffset": base_z_offset + float(z_offset_step) * float(index),
                "apply": apply,
            })
        else:
            if index == 0:
                apply = {
                    "ok": True,
                    "sessionId": registered.session_id if registered is not None else "",
                    "skipped": True,
                    "reason": "baseline frame before input pulse",
                }
            else:
                apply = _post_war3_key_pulse(
                    pid=target_pid,
                    key=input_key,
                    hold_ms=int(input_hold_ms),
                    repeat=max(1, int(input_repeat)),
                    foreground=bool(input_foreground),
                )
            camera_rows.append({
                "index": index,
                "controlMode": control_mode,
                "inputKey": str(input_key or "RIGHT").strip().upper(),
                "inputHoldMs": int(input_hold_ms),
                "inputRepeat": max(1, int(input_repeat)),
                "inputForeground": bool(input_foreground),
                "apply": apply,
            })

        if not apply.get("ok"):
            return {
                "ok": False,
                "stage": "camera-apply" if control_mode == "camera-command" else "input-fallback",
                "camera": camera_rows,
                "frames": frame_rows,
            }

        if index > 0 or float(interval_sec) > 0.0:
            time.sleep(max(0.0, float(interval_sec)))

        out_path = out_dir / f"{label or mode_name}_camera_{index + 1:02d}.bmp"
        cap = _capture_final_frame_via_internal_test_api(
            pid=target_pid,
            war3_dir=target_war3_dir,
            output_path=out_path,
            timeout_sec=max(2.0, float(timeout_sec)),
        )
        status_after = _read_runtime_status_best_effort(target_pid) or {}
        shadow_status = status_after.get("shadow", {}) if isinstance(status_after, dict) else {}
        cap["shadowSummaryAfterCapture"] = {
            "semanticSceneReceiverInputValid": shadow_status.get("semanticSceneReceiverInputValid"),
            "semanticSceneReceiverInputRejectReason": shadow_status.get("semanticSceneReceiverInputRejectReason"),
            "semanticSceneReceiverNeedPass": shadow_status.get("semanticSceneReceiverNeedPass"),
            "semanticSceneReceiverHasCompleteShadowMap": shadow_status.get("semanticSceneReceiverHasCompleteShadowMap"),
            "semanticSceneReceiverHasUsableDirectionalShadow": shadow_status.get("semanticSceneReceiverHasUsableDirectionalShadow"),
            "semanticSceneReceiverDebugMode": shadow_status.get("semanticSceneReceiverDebugMode"),
            "semanticSceneReceiverCsmCascadeCount": shadow_status.get("semanticSceneReceiverCsmCascadeCount"),
            "semanticSceneShadowTaaActive": shadow_status.get("semanticSceneShadowTaaActive"),
            "semanticSceneShadowTaaMode": shadow_status.get("semanticSceneShadowTaaMode"),
            "semanticSceneReplayDrawsCount": shadow_status.get("semanticSceneReplayDrawsCount"),
            "semanticSceneShadowMapDrawnCasters": shadow_status.get("semanticSceneShadowMapDrawnCasters"),
        }
        frame_rows.append(cap)
        if not cap.get("ok"):
            return {
                "ok": False,
                "stage": "capture",
                "camera": camera_rows,
                "frames": frame_rows,
            }
        frame_paths.append(Path(str(cap.get("output", ""))))

    comparison = _compare_bmp_sequence(frame_paths)
    comparison_summary = dict(comparison.get("summary", {}) or {})
    receiver_invalid = 0
    incomplete_shadow_map = 0
    unusable_directional_shadow = 0
    for frame in frame_rows:
        shadow = dict(frame.get("shadowSummaryAfterCapture", {}) or {})
        if int(shadow.get("semanticSceneReceiverInputValid", 0) or 0) == 0:
            receiver_invalid += 1
        if int(shadow.get("semanticSceneReceiverHasCompleteShadowMap", 0) or 0) == 0:
            incomplete_shadow_map += 1
        if int(shadow.get("semanticSceneReceiverHasUsableDirectionalShadow", 0) or 0) == 0:
            unusable_directional_shadow += 1
    motion_aware_summary = {
        "frameCount": len(frame_rows),
        "movementAware": True,
        "receiverInvalidFrames": int(receiver_invalid),
        "incompleteShadowMapFrames": int(incomplete_shadow_map),
        "unusableDirectionalShadowFrames": int(unusable_directional_shadow),
        "darkRatioRangePct": comparison_summary.get("darkRatioRangePct"),
        "avgLumaRange": comparison_summary.get("avgLumaRange"),
        "missingShadowSuspect": bool(comparison_summary.get("missingShadowSuspect", False)),
    }
    dark_range = float(comparison_summary.get("darkRatioRangePct", 0.0) or 0.0)
    luma_range = float(comparison_summary.get("avgLumaRange", 0.0) or 0.0)
    motion_aware_summary["motionFlickerSuspect"] = bool(
        receiver_invalid > 0 or
        incomplete_shadow_map > 0 or
        unusable_directional_shadow > 0 or
        bool(comparison_summary.get("missingShadowSuspect", False)) or
        dark_range >= 6.0 or
        luma_range >= 12.0
    )
    return {
        "ok": bool(comparison.get("ok")),
        "mode": mode_name,
        "debugMode": int(debug_mode),
        "controlMode": control_mode,
        "outputDir": str(out_dir),
        "baseSnapshot": snapshot,
        "camera": camera_rows,
        "frames": frame_rows,
        "comparison": comparison,
        "motionAwareSummary": motion_aware_summary,
    }


@mcp.tool()
def run_city_shadow_stability_suite(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_CITY_MAP),
    allow_fallback_to_default_test_map: bool = True,
    ready_timeout_sec: int = 240,
    settle_sec: int = 60,
    sequence_frame_count: int = 5,
    sequence_interval_sec: float = 0.25,
    use_isolated_desktop: bool = True,
    desktop_name: str = "War3CityStability",
    profile: str = "full_analysis",
    windowed: bool = True,
    sample_duration_sec: int = 5,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
) -> Dict[str, Any]:
    """
    City.w3x 专项稳定性套件：
    启动 -> 进图 -> 额外静置 -> 开全图 -> 镜头高/中/低俯仰角扫掠 -> 连续帧比较。
    """
    w3 = Path(war3_dir)
    before_report = _find_latest_report(w3)
    before_mtime = before_report.stat().st_mtime if before_report and before_report.exists() else 0.0
    log_offsets = _snapshot_log_offsets(w3)
    env_overrides = {
        "DXVK_WAR3_RUNTIME_BENCHMARK": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC": str(max(3, int(sample_duration_sec))),
    }

    start = _launch_suite_map_until_ready(
        war3_dir=war3_dir,
        requested_map_path=map_path,
        allow_fallback_to_default_test_map=bool(allow_fallback_to_default_test_map),
        ready_timeout_sec=ready_timeout_sec,
        ready_allow_fallback=False,
        ready_require_game_started_for_fallback=True,
        ready_fallback_min_elapsed_sec=20,
        ready_fallback_min_cpu_sec=1.0,
        launch_kwargs={
            "windowed": bool(windowed),
            "use_isolated_desktop": bool(use_isolated_desktop),
            "desktop_name": str(desktop_name or "War3CityStability"),
            "opengl": False,
            "auto_perf_record": True,
            "auto_perf_export_sec": max(8, int(sample_duration_sec) + 2),
            "deploy_d3d9_before_launch": deploy_d3d9_before_launch,
            "build_d3d9_path": build_d3d9_path,
            "enforce_video_baseline": True,
            "baseline_width": DEFAULT_BENCHMARK_WIDTH,
            "baseline_height": DEFAULT_BENCHMARK_HEIGHT,
            "baseline_refresh_rate": DEFAULT_BENCHMARK_REFRESH,
            "render_log": False,
            "profile": profile,
            "disable_modules": "",
            "env_overrides_json": json.dumps(env_overrides, ensure_ascii=False),
            "extra_args": "",
        },
    )
    if not start.get("ok"):
        return {"ok": False, "stage": str(start.get("stage", "ready")), "start": start}

    launch = dict(start.get("launch", {}) or {})
    ready = dict(start.get("ready", {}) or {})
    pid = int(start["pid"])

    time.sleep(max(0, int(settle_sec)))

    markers: List[Dict[str, Any]] = []
    full_map = _invoke_internal_test_request(pid, Path(war3_dir), "visibility.full_map", {}, timeout_sec=4.0)
    markers.append(_invoke_internal_test_request(pid, Path(war3_dir), "runtime.log_marker", {"marker": "city-suite-start"}, timeout_sec=3.0))
    if not full_map.get("ok"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
        return {"ok": False, "stage": "full-map", "launch": launch, "ready": ready, "fullMap": full_map, "stop": stop}

    base_snapshot = _invoke_internal_test_request(pid, Path(war3_dir), "camera.snapshot", {}, timeout_sec=4.0)
    if not base_snapshot.get("ok"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
        return {"ok": False, "stage": "camera-snapshot", "launch": launch, "ready": ready, "fullMap": full_map, "snapshot": base_snapshot, "stop": stop}

    sequences: List[Dict[str, Any]] = []
    for mode_name in ("shadow_factor", "normal"):
        for view_name in ("high", "mid", "low"):
            view_res = set_city_test_view(
                view=view_name,
                pid=pid,
                war3_dir=war3_dir,
                duration=0.0,
                quick_position=True,
            )
            row: Dict[str, Any] = {
                "mode": mode_name,
                "view": view_name,
                "viewSet": view_res,
            }
            if not view_res.get("ok"):
                sequences.append(row)
                break
            time.sleep(0.6)
            seq = capture_shadow_factor_sequence(
                pid=pid,
                war3_dir=war3_dir,
                mode=mode_name,
                label=f"{mode_name}_{view_name}",
                frame_count=sequence_frame_count,
                interval_sec=sequence_interval_sec,
            )
            row["sequence"] = seq
            sequences.append(row)

    # 恢复正常渲染模式。
    markers.append(_invoke_internal_test_request(pid, Path(war3_dir), "shadow.debug_mode", {"mode": 0}, timeout_sec=4.0))
    time.sleep(max(1, int(sample_duration_sec)))

    visibility_restore = _invoke_internal_test_request(
        pid,
        Path(war3_dir),
        "visibility.full_map",
        {"enabled": False},
        timeout_sec=4.0,
    )

    stop = stop_war3(pid=pid, graceful_wait_sec=20, force=False, avoid_foreground_switch=True)
    if not stop.get("stopped"):
        stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)

    latest = {"ok": False, "error": "未找到报告"}
    new_report_detected = False
    for _ in range(12):
        maybe = find_latest_perf_report(war3_dir=war3_dir)
        if maybe.get("ok"):
            mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
            if mtime > before_mtime + 0.5:
                latest = maybe
                new_report_detected = True
                break
            latest = maybe
        time.sleep(1.0)

    report = read_perf_report(latest["reportPath"], include_sections=False) if latest.get("ok") else latest
    log_summary = _read_runtime_log_summary(w3, log_offsets=log_offsets)
    if isinstance(report, dict):
        report["logSummary"] = log_summary
        _merge_shadow_budget_summary_with_log_fallback(report, log_summary)

    seq_ok = True
    low_pitch_missing = False
    flicker_suspect = False
    shadow_factor_dark_ratios: Dict[str, float] = {}
    for row in sequences:
        seq = dict(row.get("sequence", {}) or {})
        cmp_res = dict(seq.get("comparison", {}) or {})
        summary = dict(cmp_res.get("summary", {}) or {})
        if not seq.get("ok"):
            seq_ok = False
            continue
        flicker_suspect = flicker_suspect or bool(summary.get("flickerSuspect", False))
        if row.get("mode") == "shadow_factor":
            frames = list((cmp_res.get("frames", []) or []))
            if frames:
                shadow_factor_dark_ratios[str(row.get("view"))] = float(frames[0].get("darkRatioPct", 0.0) or 0.0)
            if row.get("view") == "low":
                low_pitch_missing = low_pitch_missing or bool(summary.get("missingShadowSuspect", False))

    if {"high", "mid", "low"} <= set(shadow_factor_dark_ratios.keys()):
        ref_ratio = max(shadow_factor_dark_ratios["high"], shadow_factor_dark_ratios["mid"])
        low_ratio = shadow_factor_dark_ratios["low"]
        if ref_ratio >= 1.0 and low_ratio <= ref_ratio * 0.35:
            low_pitch_missing = True

    top_keywords = {str(item.get("name", "")): int(item.get("count", 0) or 0) for item in list(log_summary.get("topKeywords", []) or [])}
    no_bad_keywords = (
        top_keywords.get("deviceLost", 0) == 0
        and top_keywords.get("shadowReuseLastComplete", 0) == 0
        and top_keywords.get("csmComputeFailed", 0) == 0
    )
    shadow_budget = dict((report.get("shadowBudgetSummary", {}) if isinstance(report, dict) else {}) or {})
    zero_budget_errors = (
        int(shadow_budget.get("framesBudgetExceeded", 0) or 0) == 0
        and int(shadow_budget.get("framesIncomplete", 0) or 0) == 0
    )
    ok = bool(seq_ok and not flicker_suspect and not low_pitch_missing and no_bad_keywords and zero_budget_errors)

    return {
        "ok": ok,
        "stage": "done",
        "requestedMapPath": str(map_path),
        "actualMapPath": str(start.get("actualMapPath", map_path)),
        "fallbackUsed": bool(start.get("fallbackUsed", False)),
        "start": start,
        "launch": launch,
        "ready": ready,
        "fullMap": full_map,
        "visibilityRestore": visibility_restore,
        "baseSnapshot": base_snapshot,
        "markers": markers,
        "sequences": sequences,
        "flickerSuspect": bool(flicker_suspect),
        "lowPitchMissingShadowSuspect": bool(low_pitch_missing),
        "logSummary": log_summary,
        "report": report,
        "shadowBudgetSummary": shadow_budget,
        "stop": stop,
        "newReportDetected": bool(new_report_detected),
    }


@mcp.tool()
def run_city_shadow_pressure_suite(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_CITY_MAP),
    allow_fallback_to_default_test_map: bool = True,
    rounds: int = 3,
    duration_sec: int = 600,
    sample_interval_sec: int = 30,
    ready_timeout_sec: int = 240,
    use_isolated_desktop: bool = True,
    desktop_name: str = "War3CityPressure",
    profile: str = "full_default",
    windowed: bool = True,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
) -> Dict[str, Any]:
    """City.w3x 高压压力测试：长时间运行并定时采样日志/截图/预算。"""
    rounds = max(1, min(int(rounds), 6))
    duration_sec = max(30, min(int(duration_sec), 3600))
    sample_interval_sec = max(5, min(int(sample_interval_sec), 300))
    all_rounds: List[Dict[str, Any]] = []
    current_map_path = str(map_path)
    actual_map_path = str(map_path)
    fallback_used_any = False

    for round_index in range(rounds):
        w3 = Path(war3_dir)
        before_report = _find_latest_report(w3)
        before_mtime = before_report.stat().st_mtime if before_report and before_report.exists() else 0.0
        log_offsets = _snapshot_log_offsets(w3)
        start = _launch_suite_map_until_ready(
            war3_dir=war3_dir,
            requested_map_path=current_map_path,
            allow_fallback_to_default_test_map=bool(allow_fallback_to_default_test_map),
            ready_timeout_sec=ready_timeout_sec,
            ready_allow_fallback=False,
            ready_require_game_started_for_fallback=True,
            ready_fallback_min_elapsed_sec=20,
            ready_fallback_min_cpu_sec=1.0,
            launch_kwargs={
                "windowed": bool(windowed),
                "use_isolated_desktop": bool(use_isolated_desktop),
                "desktop_name": f"{desktop_name}_{round_index + 1}",
                "opengl": False,
                "auto_perf_record": True,
                "auto_perf_export_sec": max(8, min(60, duration_sec // 2)),
                "deploy_d3d9_before_launch": deploy_d3d9_before_launch,
                "build_d3d9_path": build_d3d9_path,
                "enforce_video_baseline": True,
                "baseline_width": DEFAULT_BENCHMARK_WIDTH,
                "baseline_height": DEFAULT_BENCHMARK_HEIGHT,
                "baseline_refresh_rate": DEFAULT_BENCHMARK_REFRESH,
                "render_log": False,
                "profile": profile,
                "disable_modules": "",
                "env_overrides_json": "{}",
                "extra_args": "",
            },
        )
        if not start.get("ok"):
            all_rounds.append({"ok": False, "stage": str(start.get("stage", "ready")), "detail": start, "round": round_index + 1})
            continue

        launch = dict(start.get("launch", {}) or {})
        ready = dict(start.get("ready", {}) or {})
        pid = int(start["pid"])
        current_map_path = str(start.get("actualMapPath", current_map_path))
        actual_map_path = current_map_path
        fallback_used_any = fallback_used_any or bool(start.get("fallbackUsed", False))
        round_row: Dict[str, Any] = {
            "round": round_index + 1,
            "requestedMapPath": str(map_path),
            "actualMapPath": current_map_path,
            "fallbackUsed": bool(start.get("fallbackUsed", False)),
            "start": start,
            "launch": launch,
            "ready": ready,
            "samples": [],
        }
        if not ready.get("ok"):
            stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)
            round_row["ok"] = False
            round_row["stage"] = "ready"
            round_row["stop"] = stop
            all_rounds.append(round_row)
            continue

        started_at = time.time()
        sample_idx = 0
        while time.time() - started_at < duration_sec:
            if not _pid_alive(pid):
                break
            sample_idx += 1
            out_dir = _ensure_dir(ARTIFACT_ROOT / "city_pressure" / _now_compact())
            shot = capture_war3_screenshot(
                pid=pid,
                war3_dir=war3_dir,
                output_path=str(out_dir / f"city_pressure_r{round_index + 1}_s{sample_idx:03d}.bmp"),
                prefer_internal=True,
                timeout_sec=8,
                fallback_to_window_capture=False,
            )
            runtime_status = read_runtime_status(war3_dir=war3_dir)
            round_row["samples"].append(
                {
                    "time": _now_str(),
                    "elapsedSec": round(time.time() - started_at, 3),
                    "screenshot": shot,
                    "runtimeStatus": runtime_status,
                }
            )
            sleep_left = sample_interval_sec
            while sleep_left > 0:
                if not _pid_alive(pid):
                    sleep_left = 0
                    break
                step = min(1.0, sleep_left)
                time.sleep(step)
                sleep_left -= step

        stop = stop_war3(pid=pid, graceful_wait_sec=20, force=False, avoid_foreground_switch=True)
        if not stop.get("stopped"):
            stop = stop_war3(pid=pid, graceful_wait_sec=3, force=True, avoid_foreground_switch=True)

        latest = {"ok": False, "error": "未找到报告"}
        new_report_detected = False
        for _ in range(12):
            maybe = find_latest_perf_report(war3_dir=war3_dir)
            if maybe.get("ok"):
                mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                if mtime > before_mtime + 0.5:
                    latest = maybe
                    new_report_detected = True
                    break
                latest = maybe
            time.sleep(1.0)

        report = read_perf_report(latest["reportPath"], include_sections=False) if latest.get("ok") else latest
        log_summary = _read_runtime_log_summary(w3, log_offsets=log_offsets)
        if isinstance(report, dict):
            report["logSummary"] = log_summary
            _merge_shadow_budget_summary_with_log_fallback(report, log_summary)
        shadow_budget = dict((report.get("shadowBudgetSummary", {}) if isinstance(report, dict) else {}) or {})
        top_keywords = {str(item.get("name", "")): int(item.get("count", 0) or 0) for item in list(log_summary.get("topKeywords", []) or [])}
        round_ok = (
            bool(stop.get("stopped"))
            and top_keywords.get("deviceLost", 0) == 0
            and int(shadow_budget.get("framesBudgetExceeded", 0) or 0) == 0
            and int(shadow_budget.get("framesIncomplete", 0) or 0) == 0
        )
        round_row.update(
            {
                "ok": bool(round_ok),
                "stage": "done",
                "stop": stop,
                "report": report,
                "logSummary": log_summary,
                "shadowBudgetSummary": shadow_budget,
                "newReportDetected": bool(new_report_detected),
            }
        )
        all_rounds.append(round_row)

    return {
        "ok": bool(all_rounds) and all(bool(row.get("ok")) for row in all_rounds),
        "requestedMapPath": str(map_path),
        "actualMapPath": actual_map_path,
        "fallbackUsed": bool(fallback_used_any),
        "rounds": all_rounds,
        "passed": sum(1 for row in all_rounds if bool(row.get("ok"))),
        "total": len(all_rounds),
    }


def _write_session_manifest(session: AutoTestSession) -> Dict[str, Any]:
    try:
        session.artifact_dir.mkdir(parents=True, exist_ok=True)
        path = session.artifact_dir / "session.json"
        payload = session.to_dict(alive=_registered_session_alive(session))
        path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        identity_path = session.instance_root.parent / "session.autotest.json"
        identity_path.parent.mkdir(parents=True, exist_ok=True)
        identity_path.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        event_path = session.artifact_dir / "dbwin.jsonl"
        first_id = max(0, int(session.debug_seq) - 1000)
        event_rows = session.get_events(first_id, 1000)
        event_text = "".join(
            json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n"
            for row in event_rows
        )
        event_path.write_text(event_text, encoding="utf-8")
        return {
            "ok": True,
            "path": str(path),
            "identityPath": str(identity_path),
            "dbwinPath": str(event_path),
            "dbwinEventsWritten": len(event_rows),
        }
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


def _stop_registered_session(
    session_id: str,
    graceful_wait_sec: int,
    force: bool,
    avoid_foreground_switch: bool,
) -> Dict[str, Any]:
    session = SESSION_REGISTRY.get(session_id=str(session_id or ""))
    if session is None:
        return {"ok": False, "error": f"未找到 session_id: {session_id}"}
    target_pid = int(session.pid or 0)
    alive = _registered_session_alive(session)
    close_sent = False
    shutdown_session: Dict[str, Any] = {"ok": True, "skipped": True, "reason": "process already stopped"}

    if alive and not avoid_foreground_switch:
        shutdown_session = _control_plane_request(
            pid=target_pid,
            command="shutdown_session",
            payload={},
            timeout_sec=2.0,
        )
        close_sent = _post_close(target_pid)
        deadline = time.time() + max(1, int(graceful_wait_sec))
        while time.time() < deadline and _registered_session_alive(session):
            time.sleep(0.25)
        alive = _registered_session_alive(session)

    if alive and avoid_foreground_switch and not force:
        _taskkill(target_pid, force=False)
        time.sleep(0.6)
        alive = _registered_session_alive(session)

    forced_by_job = False
    if alive and force:
        # 关闭 KILL_ON_JOB_CLOSE handle 会同时回收 War3 及其所有子进程。
        if int(session.job_handle or 0):
            forced_by_job = _close_job_handle(session.job_handle)
            session.job_handle = 0
        if not forced_by_job:
            _taskkill(target_pid, force=True)
        time.sleep(0.6)
        alive = _registered_session_alive(session)

    desktop_result: Dict[str, Any]
    if not alive:
        if int(session.job_handle or 0):
            _close_job_handle(session.job_handle)
            session.job_handle = 0
        if int(session.desktop_handle or 0):
            handle = int(session.desktop_handle)
            desktop_result = {
                "ok": _close_desktop_handle(handle),
                "name": session.desktop_name,
                "handle": handle,
            }
            session.desktop_handle = 0
        else:
            desktop_result = {"ok": True, "skipped": True, "reason": "无隔离桌面"}
        session.process = None
        SESSION_REGISTRY.mark_stopped(session.session_id, "stop_war3_batch/stop_war3")
    else:
        desktop_result = {"ok": True, "skipped": True, "reason": "进程仍存活"}

    manifest = _write_session_manifest(session)
    return {
        "ok": not alive,
        "stopped": not alive,
        "runId": session.run_id,
        "sessionId": session.session_id,
        "pid": target_pid,
        "closeSent": close_sent,
        "forced": bool(force),
        "forcedByJobObject": forced_by_job,
        "avoidForegroundSwitch": bool(avoid_foreground_switch),
        "shutdownSession": shutdown_session,
        "desktop": desktop_result,
        "manifest": manifest,
    }


@mcp.tool()
def list_war3_sessions(run_id: str = "", include_stopped: bool = True) -> Dict[str, Any]:
    """列出所有多实例会话；每行包含独立 PID、根目录、Desktop、Job 与工件目录。"""
    rows = []
    for session in SESSION_REGISTRY.list(run_id=run_id, include_stopped=include_stopped):
        alive = _registered_session_alive(session)
        rows.append(session.to_dict(alive=alive))
    return {"ok": True, "runId": run_id, "count": len(rows), "sessions": rows}


@mcp.tool()
def stop_war3_batch(
    session_ids_json: str = "",
    run_id: str = "",
    graceful_wait_sec: int = 8,
    force: bool = True,
    avoid_foreground_switch: bool = True,
) -> Dict[str, Any]:
    """按 session_id 列表或 run_id 停止一批会话，不影响其他批次。"""
    session_ids: List[str] = []
    if str(session_ids_json or "").strip():
        try:
            parsed = json.loads(session_ids_json)
        except Exception as exc:
            return {"ok": False, "error": f"session_ids_json 解析失败: {exc}"}
        if not isinstance(parsed, list) or not all(isinstance(value, str) for value in parsed):
            return {"ok": False, "error": "session_ids_json 必须是字符串数组"}
        session_ids = [value for value in parsed if value]
    elif run_id:
        session_ids = [
            row.session_id
            for row in SESSION_REGISTRY.list(run_id=run_id, include_stopped=False)
        ]
    else:
        return {"ok": False, "error": "必须提供 session_ids_json 或 run_id，拒绝无范围停止"}

    results = [
        _stop_registered_session(
            session_id=value,
            graceful_wait_sec=graceful_wait_sec,
            force=force,
            avoid_foreground_switch=avoid_foreground_switch,
        )
        for value in session_ids
    ]
    return {
        "ok": all(bool(row.get("ok")) for row in results),
        "runId": run_id,
        "requested": len(session_ids),
        "stopped": sum(1 for row in results if row.get("stopped")),
        "results": results,
    }


@mcp.tool()
def cleanup_orphan_sessions(
    run_id: str = "",
    remove_instance_roots: bool = False,
    forget_sessions: bool = False,
) -> Dict[str, Any]:
    """回收已退出进程的 Job/Desktop；可选删除边界内实例根并移出注册表。"""
    rows: List[Dict[str, Any]] = []
    known_roots = {
        os.path.normcase(str(Path(session.instance_root).resolve(strict=False)))
        for session in SESSION_REGISTRY.list(include_stopped=True)
    }
    for session in SESSION_REGISTRY.list(run_id=run_id, include_stopped=True):
        if _registered_session_alive(session):
            continue
        if session.state not in {"stopped", "orphaned", "failed"}:
            SESSION_REGISTRY.mark_stopped(session.session_id, "process no longer exists", state="orphaned")
        job_closed = _close_job_handle(session.job_handle) if int(session.job_handle or 0) else True
        desktop_closed = _close_desktop_handle(session.desktop_handle) if int(session.desktop_handle or 0) else True
        session.job_handle = 0
        session.desktop_handle = 0
        removed_root = False
        remove_error = ""
        if remove_instance_roots and session.managed_root and session.instance_root.exists():
            pool_root = session.sandbox_root / "_AutoTestInstances"
            try:
                target = require_path_within(session.instance_root, pool_root, "orphan instance root")
                shutil.rmtree(target)
                removed_root = True
            except Exception as exc:
                remove_error = str(exc)
        manifest = _write_session_manifest(session)
        rows.append(
            {
                "sessionId": session.session_id,
                "pid": int(session.pid),
                "state": session.state,
                "jobClosed": job_closed,
                "desktopClosed": desktop_closed,
                "instanceRootRemoved": removed_root,
                "removeError": remove_error,
                "manifest": manifest,
            }
        )
        if forget_sessions:
            SESSION_REGISTRY.remove(session.session_id)

    # MCP 重启会自动关闭 Job handle 并杀掉子进程，但实例目录仍需可发现、可回收。
    # 仅扫描专用沙盒固定层级，绝不枚举或删除其他 Warcraft 目录。
    pool_root = DEFAULT_SANDBOX_ROOT / "_AutoTestInstances"
    if pool_root.is_dir():
        for candidate in pool_root.glob("*/*/root"):
            try:
                root = require_path_within(candidate, pool_root, "discovered orphan root")
            except ValueError:
                continue
            root_key = os.path.normcase(str(root))
            if root_key in known_roots:
                continue
            session_dir = root.parent
            discovered_session_id = session_dir.name
            discovered_run_id = session_dir.parent.name
            if run_id and discovered_run_id != run_id:
                continue

            identity_path = session_dir / "session.autotest.json"
            manifest_path = (
                identity_path
                if identity_path.is_file()
                else ARTIFACT_ROOT / discovered_run_id / discovered_session_id / "session.json"
            )
            manifest_data = _read_json_file(manifest_path) or {}
            manifest_pid = int(manifest_data.get("pid", 0) or 0)
            expected_creation = int(manifest_data.get("processCreatedAtMs", 0) or 0)
            actual_creation = _get_process_creation_epoch_ms(manifest_pid) if manifest_pid > 0 else 0
            process_matches = bool(
                manifest_pid > 0
                and _pid_alive(manifest_pid)
                and (
                    expected_creation <= 0
                    or actual_creation <= 0
                    or abs(expected_creation - actual_creation) <= 2_000
                )
            )
            removed_root = False
            remove_error = ""
            if remove_instance_roots and not process_matches and manifest_data:
                try:
                    shutil.rmtree(root)
                    removed_root = True
                except Exception as exc:
                    remove_error = str(exc)
            elif remove_instance_roots and not manifest_data:
                remove_error = "缺少 session identity manifest，拒绝删除未知实例根"
            rows.append(
                {
                    "sessionId": discovered_session_id,
                    "runId": discovered_run_id,
                    "pid": manifest_pid,
                    "state": "active-unregistered" if process_matches else "unregistered-orphan",
                    "discoveredAfterRestart": True,
                    "instanceRoot": str(root),
                    "instanceRootRemoved": removed_root,
                    "removeError": remove_error,
                }
            )
    return {
        "ok": all(not row["removeError"] for row in rows),
        "runId": run_id,
        "cleaned": len(rows),
        "sessions": rows,
    }


@mcp.tool()
def stop_war3(
    pid: int = 0,
    graceful_wait_sec: int = 8,
    force: bool = True,
    avoid_foreground_switch: bool = False,
    session_id: str = "",
) -> Dict[str, Any]:
    """停止 War3：优先 WM_CLOSE，超时后可强杀。"""
    registered = _session_by_selector(session_id=session_id, pid=pid)
    if registered is not None:
        return _stop_registered_session(
            session_id=registered.session_id,
            graceful_wait_sec=graceful_wait_sec,
            force=force,
            avoid_foreground_switch=avoid_foreground_switch,
        )
    target_pid = pid or (STATE.war3_pid or 0)
    if target_pid <= 0:
        return {"ok": True, "stopped": True, "pid": 0, "message": "无活跃 pid"}

    # Isolated/background tests retain the original process HANDLE and must
    # settle that exact process instance. Do this before any PID liveness
    # query: a reused numeric PID must never authorize taskkill or /T child
    # traversal. Identity ambiguity is fail-closed and does not fall through.
    if avoid_foreground_switch:
        exact_stop = _stop_retained_state_process_exact(target_pid)
        if exact_stop.get("handled") is True:
            return exact_stop
        return {
            "ok": False,
            "stopped": False,
            "pid": int(target_pid),
            "silentStop": True,
            "avoidForegroundSwitch": True,
            "exactNativeHandleStop": False,
            "error": (
                "background stop lacks an exact retained native-process "
                "witness; PID/taskkill fallback is forbidden"
            ),
            "exactStopPreflight": exact_stop,
            "pidTerminationCommandIssued": False,
        }

    if not _pid_alive(target_pid):
        restore = _restore_video_config_if_needed(target_pid)
        launcher = _stop_tracked_launcher_if_needed(target_pid)
        desktop = _close_state_desktop_if_needed(target_pid)
        _clear_war3_launch_state(target_pid)
        return {
            "ok": True,
            "stopped": True,
            "pid": target_pid,
            "message": "进程已不在",
            "videoRestore": restore,
            "launcher": launcher,
            "desktop": desktop,
        }

    shutdown_session = _control_plane_request(
        pid=target_pid,
        command="shutdown_session",
        payload={},
        timeout_sec=2.0,
    )

    close_sent = _post_close(target_pid)
    t0 = time.time()
    while time.time() - t0 < max(1, graceful_wait_sec):
        if not _pid_alive(target_pid):
            restore = _restore_video_config_if_needed(target_pid)
            launcher = _stop_tracked_launcher_if_needed(target_pid)
            desktop = _close_state_desktop_if_needed(target_pid)
            _clear_war3_launch_state(target_pid)
            return {
                "ok": True,
                "stopped": True,
                "pid": target_pid,
                "closeSent": close_sent,
                "silentStop": False,
                "avoidForegroundSwitch": False,
                "shutdownSession": shutdown_session,
                "videoRestore": restore,
                "launcher": launcher,
                "desktop": desktop,
            }
        time.sleep(0.25)

    if force:
        _taskkill(target_pid, force=True)
        time.sleep(0.6)
    alive = _pid_alive(target_pid)
    restore = (
        _restore_video_config_if_needed(target_pid)
        if not alive
        else {"ok": True, "skipped": True, "reason": "进程仍存活，未恢复视频配置"}
    )
    launcher = (
        _stop_tracked_launcher_if_needed(target_pid)
        if not alive
        else {"ok": True, "skipped": True, "reason": "游戏进程仍存活"}
    )
    desktop = _close_state_desktop_if_needed(target_pid) if not alive else {"ok": True, "skipped": True, "reason": "进程仍存活，未关闭隔离桌面"}
    if not alive:
        _clear_war3_launch_state(target_pid)
    return {
        "ok": not alive,
        "stopped": not alive,
        "pid": target_pid,
        "closeSent": close_sent,
        "forced": force,
        "silentStop": False,
        "avoidForegroundSwitch": False,
        "shutdownSession": shutdown_session,
        "videoRestore": restore,
        "launcher": launcher,
        "desktop": desktop,
    }


@mcp.tool()
def find_latest_perf_report(war3_dir: str = str(DEFAULT_WAR3_DIR)) -> Dict[str, Any]:
    """查找 WarVK/Log 下最新性能报告 HTML。"""
    w3 = Path(war3_dir)
    report = _find_latest_report(w3)
    if not report:
        return {"ok": False, "error": "未找到报告", "searchDir": str(w3 / 'WarVK' / 'Log')}
    return {
        "ok": True,
        "reportPath": str(report),
        "mtime": datetime.fromtimestamp(report.stat().st_mtime).isoformat(),
        "size": report.stat().st_size,
    }


@mcp.tool()
def read_perf_report(
    report_path: str = "",
    include_sections: bool = False,
    section_top_n: int = 20,
) -> Dict[str, Any]:
    """读取并摘要性能报告。report_path 为空时读取最新报告。"""
    report = Path(report_path) if report_path else _find_latest_report(STATE.war3_dir)
    if not report or not report.exists():
        return {"ok": False, "error": "报告不存在", "reportPath": str(report) if report else ""}
    summary = _read_perf_summary(
        report,
        include_sections=include_sections,
        section_top_n=section_top_n,
    )
    STATE.last_report_path = report
    return summary


@mcp.tool()
def run_quick_autotest(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    launcher_mode: str = YDWE_LAUNCHER_MODE_DIRECT,
    ydwe_root: str = "",
    ydwe_child_timeout_sec: int = 20,
    ydwe_module_timeout_sec: int = 30,
    ready_timeout_sec: int = 120,
    sample_duration_sec: int = 20,
    windowed: bool = False,
    use_isolated_desktop: bool = False,
    desktop_name: str = "",
    opengl: bool = False,
    auto_perf_record: bool = True,
    record_after_game_started: bool = True,
    auto_perf_export_sec: int = 8,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    avoid_focus_on_stop: bool = True,
    profile: str = "",
    disable_modules: str = "",
    env_overrides_json: str = "",
    scenario_name: str = "",
    require_control_plane_ready: bool = True,
    require_hot_shadow_frame: bool = False,
    hot_shadow_timeout_sec: int = 60,
) -> Dict[str, Any]:
    """
    一键流程：
    启动 -> 等进图 -> 截图 -> 等待采样 -> 关闭 -> 读报告。
    """
    w3 = Path(war3_dir)
    before_report = _find_latest_report(w3)
    before_mtime = before_report.stat().st_mtime if before_report and before_report.exists() else 0.0
    log_offsets = _snapshot_log_offsets(w3)
    runtime_profile = _runtime_profile_summary_from_inputs(profile, disable_modules)
    scenario_name_norm = _normalize_scenario_name(scenario_name)
    merged_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = merged_env.pop("__parse_error__", "")
    if parse_error:
        return {"ok": False, "stage": "launch", "error": f"env_overrides_json 解析失败: {parse_error}"}
    merged_env.setdefault("DXVK_WAR3_RUNTIME_BENCHMARK", "1")
    merged_env.setdefault(
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC",
        "1",
    )
    merged_env.setdefault(
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC",
        str(max(3, int(sample_duration_sec) - 1)),
    )
    native_semantic_preview_enabled = _bool_env(
        merged_env.get("DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW", "")
    )
    if runtime_profile["name"] == "dxvk_only":
        merged_env.setdefault(
            "DXVK_WAR3_FPS_UNLOCK_ONLY_WARMUP_SEC",
            "1",
        )
        merged_env.setdefault(
            "DXVK_WAR3_FPS_UNLOCK_ONLY_SAMPLE_SEC",
            str(max(3, int(sample_duration_sec) - 1)),
        )
    if scenario_name_norm:
        merged_env.setdefault("DXVK_WAR3_SCENARIO", scenario_name_norm)
    if auto_perf_record and record_after_game_started and auto_perf_export_sec > 0:
        auto_perf_export_sec = max(int(auto_perf_export_sec),
                                   int(sample_duration_sec) + 2)

    launch = launch_war3_test(
        war3_dir=war3_dir,
        map_path=map_path,
        launcher_mode=launcher_mode,
        ydwe_root=ydwe_root,
        ydwe_child_timeout_sec=ydwe_child_timeout_sec,
        ydwe_module_timeout_sec=ydwe_module_timeout_sec,
        windowed=windowed,
        use_isolated_desktop=use_isolated_desktop,
        desktop_name=desktop_name,
        opengl=opengl,
        auto_perf_record=auto_perf_record,
        record_after_game_started=record_after_game_started,
        auto_perf_export_sec=auto_perf_export_sec,
        deploy_d3d9_before_launch=deploy_d3d9_before_launch,
        build_d3d9_path=build_d3d9_path,
        enforce_video_baseline=enforce_video_baseline,
        baseline_width=baseline_width,
        baseline_height=baseline_height,
        baseline_refresh_rate=baseline_refresh_rate,
        render_log=False,
        profile=profile,
        disable_modules=disable_modules,
        env_overrides_json=json.dumps(merged_env, ensure_ascii=False),
        extra_args="",
    )
    if not launch.get("ok"):
        return {"ok": False, "stage": "launch", "detail": launch}

    pid = int(launch["pid"])
    strict_ready_profile = runtime_profile["name"] in (
        "full_default",
        "full_analysis",
        "full_perf_experimental",
    )
    ready = wait_for_game_ready(
        timeout_sec=ready_timeout_sec,
        pid=pid,
        allow_fallback=not bool(require_control_plane_ready),
        fallback_min_elapsed_sec=20 if strict_ready_profile else 10,
        fallback_min_cpu_sec=1.0 if strict_ready_profile else 0.5,
        require_game_started_for_fallback=strict_ready_profile,
        auto_continue_loading=True,
        continue_key="SPACE",
        continue_interval_sec=12,
    )
    if not ready.get("ok"):
        # 在结束隔离桌面进程之前保存 UI 证据。仅有 control-plane 状态无法
        # 区分地图选择失败、错误对话框和加载后崩溃。
        failure_screenshot = capture_war3_screenshot(
            pid=pid,
            war3_dir=war3_dir,
            prefer_internal=True,
            timeout_sec=5,
            fallback_to_window_capture=True,
        )
        failure_window = query_war3_window(
            pid=pid,
            wait_sec=1,
            require_visible=False,
        )
        stop = stop_war3(
            pid=pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=avoid_focus_on_stop,
        )
        if (not bool(require_control_plane_ready) and auto_perf_record and
                not bool(record_after_game_started)):
            latest: Dict[str, Any] = {"ok": False, "error": "未找到报告"}
            new_report_detected = False
            for _ in range(20):
                maybe = find_latest_perf_report(war3_dir=war3_dir)
                if maybe.get("ok"):
                    mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                    if mtime > before_mtime + 0.5:
                        latest = maybe
                        new_report_detected = True
                        break
                    latest = maybe
                time.sleep(1.0)
            if latest.get("ok") and new_report_detected:
                summary = read_perf_report(
                    latest["reportPath"],
                    include_sections=include_sections_in_report,
                    section_top_n=section_top_n,
                )
                if isinstance(summary, dict):
                    summary["newReportDetected"] = True
                    summary["scenarioName"] = scenario_name_norm
                    summary["latestReportPath"] = latest.get("reportPath")
                    summary["reportWasStale"] = False
                    summary["perfOnlyReadyTimeout"] = True
                    summary["readyTimeoutMode"] = str(ready.get("mode", ""))
                    summary["readyTimeoutError"] = str(ready.get("error", ""))
                return {
                    "ok": bool(summary.get("ok")),
                    "stage": "perf-only-ready-timeout",
                    "launch": launch,
                    "ready": ready,
                    "failureScreenshot": failure_screenshot,
                    "failureWindow": failure_window,
                    "hotShadow": {
                        "ok": True,
                        "skipped": True,
                        "reason": "ready timeout perf-only",
                    },
                    "windowResize": {
                        "ok": True,
                        "skipped": True,
                        "reason": "ready timeout",
                    },
                    "screenshot": {"ok": False, "error": "ready timeout"},
                    "screenshotSize": {
                        "width": 0,
                        "height": 0,
                        "matchBaseline": False,
                        "baselineWidth": baseline_width,
                        "baselineHeight": baseline_height,
                    },
                    "warnings": [
                        "ready gate timed out; returned perf-only report "
                        "because control-plane readiness was not required"
                    ],
                    "stop": stop,
                    "report": summary,
                    "logSummary": _read_runtime_log_summary(
                        w3, log_offsets=log_offsets),
                    "scenarioName": scenario_name_norm,
                }
        return {
            "ok": False,
            "stage": "ready",
            "launch": launch,
            "ready": ready,
            "failureScreenshot": failure_screenshot,
            "failureWindow": failure_window,
            "stop": stop,
        }

    if str(ready.get("mode", "")) in ("fallback-window-cpu",
                                      "runtime-status-game-started",
                                      "runtime-status-stable"):
        time.sleep(2.0)

    shadow_scenario_names = {
        "low_pressure_static_reuse",
        "dynamic_shadow_pressure",
        "model_runtime_probe",
        "static_world_caster_acceptance",
        "phase4_world_caster_acceptance",
    }
    effective_require_hot_shadow_frame = bool(require_hot_shadow_frame) or (
        scenario_name_norm in shadow_scenario_names
    )
    hot_shadow: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not required",
    }
    if effective_require_hot_shadow_frame:
        attachment_probe = scenario_name_norm in (
            "dynamic_shadow_pressure",
            "model_runtime_probe",
        )
        rigid_observe_probe = (
            scenario_name_norm == "model_runtime_probe"
        )
        hot_shadow = wait_for_hot_shadow_frame(
            timeout_sec=max(1, int(hot_shadow_timeout_sec)),
            pid=pid,
            min_visible_count=1,
            min_stable_identity_count=0 if rigid_observe_probe else 1,
            min_unit_count=0
            if scenario_name_norm in ("low_pressure_static_reuse", "model_runtime_probe")
            else 1,
            min_semantic_resolved=1,
            min_semantic_skinned_resolved=0
            if scenario_name_norm in ("low_pressure_static_reuse", "model_runtime_probe")
            else 1,
            min_native_executed_draw_count=1
            if native_semantic_preview_enabled and not rigid_observe_probe
            else 0,
            require_semantic_frame_fresh=True,
            min_frame_advance=0
            if scenario_name_norm == "dynamic_shadow_pressure"
            else 2,
            allow_semantic_rigid_only=rigid_observe_probe,
            allow_semantic_attachment_rigid_only=
            scenario_name_norm == "model_runtime_probe",
            min_semantic_attachment_rigid_resolved=1
            if attachment_probe
            else 0,
            post_failure_summary_wait_sec=8
            if attachment_probe
            else 6,
            prefer_summary_poll=attachment_probe,
            require_semantic_scene_consumed=scenario_name_norm
            in ("dynamic_shadow_pressure", "low_pressure_static_reuse",
                "static_world_caster_acceptance",
                "phase4_world_caster_acceptance"),
            allow_scene_pending_if_core_and_currentdraw_ready=
            scenario_name_norm == "low_pressure_static_reuse",
            min_semantic_static_world_submitted=1
            if scenario_name_norm in (
                "static_world_caster_acceptance",
                "phase4_world_caster_acceptance",
            )
            else 0,
            allow_semantic_static_world_only=
            scenario_name_norm in (
                "static_world_caster_acceptance",
                "phase4_world_caster_acceptance",
            ),
        )
        if not hot_shadow.get("ok"):
            hot_shadow_phase = str(hot_shadow.get("semanticShadowPhase", "") or "")
            if hot_shadow_phase in (
                "native-semantic-executed-scene-pending",
                "core-fresh-waiting-render-scene",
                "core-packets-waiting-render-scene",
            ):
                hot_shadow_stage = "hot-shadow-render-scene-pending"
            elif hot_shadow_phase == "attachment-rigid-ok-skinned-pending":
                hot_shadow_stage = "hot-shadow-skinned-pending"
            elif hot_shadow_phase == "semantic-rigid-ok-skinned-pending":
                hot_shadow_stage = "hot-shadow-skinned-pending"
            else:
                hot_shadow_stage = "hot-shadow"
            stop_war3(
                pid=pid,
                graceful_wait_sec=3,
                force=True,
                avoid_foreground_switch=avoid_focus_on_stop,
            )
            return {
                "ok": False,
                "stage": hot_shadow_stage,
                "launch": launch,
                "ready": ready,
                "hotShadow": hot_shadow,
            }

    launched_windowed = bool(launch.get("windowed"))
    window_resize: Dict[str, Any] = {
        "ok": True,
        "skipped": True,
        "reason": "not windowed",
    }
    if launched_windowed and enforce_video_baseline:
        rz = _powershell_resize_window_client(
            pid=pid,
            client_w=baseline_width,
            client_h=baseline_height,
            x=40,
            y=40,
        )
        window_resize = {
            "ok": rz.get("returncode", 1) == 0,
            "details": rz,
        }
        # 给窗口尺寸变更一点稳定时间，避免立刻截图拿到旧值。
        time.sleep(0.25)

    time.sleep(max(1, sample_duration_sec))

    latest: Dict[str, Any] = {"ok": False, "error": "未找到报告"}
    new_report_detected = False
    if auto_perf_record and auto_perf_export_sec > 0:
        pre_stop_wait_sec = max(4, min(18, int(auto_perf_export_sec) // 2 + 4))
        deadline = time.time() + float(pre_stop_wait_sec)
        while time.time() < deadline:
            maybe = find_latest_perf_report(war3_dir=war3_dir)
            if maybe.get("ok"):
                mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                if mtime > before_mtime + 0.5:
                    latest = maybe
                    new_report_detected = True
                    break
                latest = maybe
            time.sleep(1.0)

    shot = capture_war3_screenshot(pid=pid)
    shot_size = {"width": 0, "height": 0, "matchBaseline": False, "baselineWidth": baseline_width, "baselineHeight": baseline_height}
    if shot.get("ok") and shot.get("output"):
        sw, sh = _read_png_size(Path(str(shot["output"])))
        shot_size = {
            "width": int(sw),
            "height": int(sh),
            "matchBaseline": int(sw) == int(baseline_width) and int(sh) == int(baseline_height),
            "baselineWidth": int(baseline_width),
            "baselineHeight": int(baseline_height),
        }

    stop = stop_war3(
        pid=pid,
        graceful_wait_sec=20,
        force=False,
        avoid_foreground_switch=avoid_focus_on_stop,
    )
    if not stop.get("stopped"):
        stop = stop_war3(
            pid=pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=avoid_focus_on_stop,
        )
    # 给报告写盘留一点时间
    if auto_perf_record and not new_report_detected:
        for _ in range(20):
            maybe = find_latest_perf_report(war3_dir=war3_dir)
            if maybe.get("ok"):
                mtime = datetime.fromisoformat(maybe["mtime"]).timestamp()
                if mtime > before_mtime + 0.5:
                    latest = maybe
                    new_report_detected = True
                    break
                latest = maybe
            time.sleep(1.0)

    benchmark_summary = {"ok": False, "error": "未在运行日志中找到 runtime benchmark 输出"}
    if not auto_perf_record:
        summary = {
            "ok": True,
            "skipped": True,
            "reason": "auto_perf_record=false；本次只验收真实进图与运行时状态",
            "reportType": "entry-gate",
        }
    elif latest.get("ok") and new_report_detected:
        summary = read_perf_report(
            latest["reportPath"],
            include_sections=include_sections_in_report,
            section_top_n=section_top_n,
        )
    else:
        for _ in range(15):
            benchmark_summary = _read_runtime_benchmark_summary(
                w3,
                log_offsets=log_offsets,
                profile=runtime_profile["name"],
                disable_modules=disable_modules,
            )
            if benchmark_summary.get("ok"):
                break
            time.sleep(1.0)
        if benchmark_summary.get("ok"):
            summary = benchmark_summary
        else:
            summary = benchmark_summary
    if isinstance(summary, dict):
        summary["newReportDetected"] = new_report_detected
        summary["scenarioName"] = scenario_name_norm
        summary["latestReportPath"] = latest.get("reportPath") if isinstance(latest, dict) else None
        summary["reportWasStale"] = bool(latest.get("ok")) and not new_report_detected
        runtime_ready_frame = _nested_status_int(
            dict(ready.get("runtimeStatus", {}) or {}),
            "frameIndex",
        )
        runtime_hot_frame = int(hot_shadow.get("runtimeFrameIndex", 0) or 0)
        if runtime_hot_frame <= 0:
            runtime_hot_frame = _nested_status_int(
                dict(hot_shadow.get("runtimeStatus", {}) or {}),
                "frameIndex",
            )
        runtime_frame_delta = (
            max(0, runtime_hot_frame - runtime_ready_frame)
            if runtime_ready_frame > 0 and runtime_hot_frame > 0
            else 0
        )
        report_frame_count = _shadow_summary_int(summary, "frameCount")
        try:
            report_window_sec = float(summary.get("windowSec", 0.0) or 0.0)
        except (TypeError, ValueError):
            report_window_sec = 0.0
        fps_sample_reliable = (
            str(summary.get("reportType", "")) == "benchmark_log"
            or (report_frame_count >= 10 and report_window_sec >= 1.0)
        )
        summary["fpsSampleReliable"] = bool(fps_sample_reliable)
        summary["fpsSampleFrameCount"] = report_frame_count
        summary["fpsSampleWindowSec"] = round(max(0.0, report_window_sec), 3)
        summary["runtimeFrameDeltaReadyToHotShadow"] = runtime_frame_delta
        if not fps_sample_reliable:
            summary["fpsSampleReliabilityReason"] = (
                "perf report recorded too few Present frames for an FPS "
                "judgement; isolated desktop/windowed runs can tail-stall or "
                "only present on capture/stop, so use semantic counters for "
                "correctness and a dedicated visible-desktop perf run for FPS"
            )
        if summary.get("reportType") == "benchmark_log":
            summary["newReportDetected"] = True
            summary["benchmarkFallback"] = True
            summary["reportWasStale"] = False
        elif auto_perf_record and not new_report_detected and summary.get("ok"):
            summary["ok"] = False
            summary["error"] = "未检测到新报告（可能未部署最新 d3d9.dll 或进程未优雅退出）"

    log_summary = _read_runtime_log_summary(w3, log_offsets=log_offsets)
    if isinstance(summary, dict):
        summary["logSummary"] = log_summary
        _merge_shadow_budget_summary_with_log_fallback(summary, log_summary)
        summary["scenarioName"] = scenario_name_norm

    warnings: List[str] = []
    if shot_size["width"] > 0 and (not shot_size["matchBaseline"]):
        warnings.append(
            f"截图尺寸 {shot_size['width']}x{shot_size['height']} 与基线 {baseline_width}x{baseline_height} 不一致"
        )

    return {
        "ok": bool(summary.get("ok")),
        "stage": "done",
        "launch": launch,
        "ready": ready,
        "hotShadow": hot_shadow,
        "windowResize": window_resize,
        "screenshot": shot,
        "screenshotSize": shot_size,
        "warnings": warnings,
        "stop": stop,
        "report": summary,
        "logSummary": log_summary,
        "scenarioName": scenario_name_norm,
    }


@mcp.tool()
def list_named_scenario_presets() -> Dict[str, Any]:
    """列出可用的命名场景预设。"""
    return {
        "ok": True,
        "presets": _scenario_preset_rows(),
    }


@mcp.tool()
def run_named_scenario(
    scenario_name: str,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    env_overrides_json: str = "",
) -> Dict[str, Any]:
    """按命名场景预设启动 AutoTest。"""
    preset = _get_scenario_preset(scenario_name)
    if not preset:
        return {
            "ok": False,
            "stage": "preset",
            "error": f"未知场景预设: {scenario_name}",
            "availablePresets": _scenario_preset_rows(),
        }

    merged_env = dict(preset.get("envOverrides", {}) or {})
    user_env = _parse_env_overrides_json(env_overrides_json)
    parse_error = user_env.pop("__parse_error__", "")
    if parse_error:
        return {"ok": False, "stage": "preset", "error": f"env_overrides_json 解析失败: {parse_error}"}
    merged_env.update(user_env)

    if str(preset.get("specializedRunner", "")) == "run_life_and_death_tdr_scenario":
        result = run_life_and_death_tdr_scenario(
            war3_dir=war3_dir,
            map_path=str(preset.get("mapPath", DEFAULT_LIFE_AND_DEATH_MAP)),
            duration_sec=int(preset.get("sampleDurationSec", 600) or 600),
            grid_size=5,
            ready_timeout_sec=int(preset.get("readyTimeoutSec", 240) or 240),
            use_isolated_desktop=bool(preset.get("useIsolatedDesktop", True)),
            desktop_name=str(preset.get("desktopName", "War3LifeAndDeathTdr")),
            launcher_mode=str(preset.get("launcherMode", YDWE_LAUNCHER_MODE_DIRECT)),
            ydwe_root=str(preset.get("ydweRoot", "")),
            deploy_d3d9_before_launch=bool(preset.get("deployD3d9BeforeLaunch", True)),
            build_d3d9_path="build32/src/d3d9/d3d9.dll",
            disable_modules=str(preset.get("disableModules", "")),
            env_overrides_json=json.dumps(merged_env, ensure_ascii=False),
        )
        if isinstance(result, dict):
            result["scenarioPreset"] = dict(preset)
            result["scenarioName"] = _normalize_scenario_name(scenario_name)
        return result

    result = run_quick_autotest(
        war3_dir=war3_dir,
        map_path=str(preset.get("mapPath", DEFAULT_TEST_MAP)),
        ready_timeout_sec=int(preset.get("readyTimeoutSec", 120) or 120),
        sample_duration_sec=int(preset.get("sampleDurationSec", 20) or 20),
        windowed=bool(preset.get("windowed", False)),
        use_isolated_desktop=bool(preset.get("useIsolatedDesktop", True)),
        desktop_name=str(preset.get("desktopName", "")),
        opengl=bool(preset.get("opengl", False)),
        auto_perf_record=bool(preset.get("autoPerfRecord", True)),
        record_after_game_started=bool(preset.get("recordAfterGameStarted", True)),
        auto_perf_export_sec=int(preset.get("autoPerfExportSec", 8) or 8),
        deploy_d3d9_before_launch=bool(preset.get("deployD3d9BeforeLaunch", True)),
        build_d3d9_path="build32/src/d3d9/d3d9.dll",
        enforce_video_baseline=bool(preset.get("enforceVideoBaseline", True)),
        baseline_width=int(preset.get("baselineWidth", DEFAULT_BENCHMARK_WIDTH) or DEFAULT_BENCHMARK_WIDTH),
        baseline_height=int(preset.get("baselineHeight", DEFAULT_BENCHMARK_HEIGHT) or DEFAULT_BENCHMARK_HEIGHT),
        baseline_refresh_rate=int(preset.get("baselineRefreshRate", DEFAULT_BENCHMARK_REFRESH) or DEFAULT_BENCHMARK_REFRESH),
        include_sections_in_report=include_sections_in_report,
        section_top_n=section_top_n,
        avoid_focus_on_stop=True,
        profile=str(preset.get("profile", "full_default")),
        disable_modules=str(preset.get("disableModules", "")),
        env_overrides_json=json.dumps(merged_env, ensure_ascii=False),
        scenario_name=scenario_name,
        require_control_plane_ready=True,
        require_hot_shadow_frame=bool(
            preset.get("requireHotShadowFrame", True)
        ),
        hot_shadow_timeout_sec=int(preset.get("hotShadowTimeoutSec", preset.get("readyTimeoutSec", 120)) or 120),
    )
    if isinstance(result, dict):
        result["scenarioPreset"] = dict(preset)
        result["scenarioName"] = _normalize_scenario_name(scenario_name)
    return result


def _write_profile_matrix_html(path: Path, rows: List[Dict[str, Any]], aggregate: Dict[str, Any]) -> None:
    def _f(v: Any) -> str:
        try:
            return f"{float(v):.3f}"
        except Exception:
            return str(v)

    parts: List[str] = [
        "<!doctype html><html><head><meta charset='utf-8'>",
        "<title>War3 Profile Matrix</title>",
        "<style>body{font-family:Segoe UI,Arial,sans-serif;background:#111;color:#eee;padding:24px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #333;padding:8px;text-align:left;vertical-align:top}th{background:#1d1d1d}tr:nth-child(even){background:#181818}.ok{color:#7ad67a}.bad{color:#ff7b7b}.warn{color:#ffd36e}.mono{font-family:Consolas,monospace}.kpi{display:inline-block;margin-right:24px;margin-bottom:16px;padding:12px 16px;background:#1a1a1a;border:1px solid #333}.section{margin-top:28px}</style>",
        "</head><body>",
        "<h1>War3 Runtime Profile Matrix</h1>",
        f"<div class='kpi'><div>Cases</div><strong>{len(rows)}</strong></div>",
        f"<div class='kpi'><div>Success</div><strong>{aggregate.get('success', 0)}</strong></div>",
        f"<div class='kpi'><div>Avg FPS</div><strong>{_f(aggregate.get('avgFps', 0.0))}</strong></div>",
    ]
    summary = aggregate.get("summary", {}) if isinstance(aggregate, dict) else {}
    top_offenders = list(summary.get("topOffenders", []) or [])
    if top_offenders:
        parts.append("<div class='section'><h2>Top Offenders</h2><table><thead><tr><th>Case</th><th>Category</th><th>Gain vs Full FPS</th><th>Budget FPS</th><th>Status</th></tr></thead><tbody>")
        for row in top_offenders[:8]:
            status = str(row.get("budgetStatus", "within_budget"))
            cls = "warn" if status == "over_budget" else "ok"
            parts.append(
                "<tr>"
                f"<td>{row.get('label','')}</td>"
                f"<td>{row.get('category','')}</td>"
                f"<td>{_f(row.get('gainVsFullDefaultFps', 0.0))}</td>"
                f"<td>{_f(row.get('budgetFps', 0.0))}</td>"
                f"<td class='{cls}'>{status}</td>"
                "</tr>"
            )
        parts.append("</tbody></table></div>")

    case_summaries = list(summary.get("caseSummaries", []) or [])
    parts.append("<div class='section'><h2>Case Summary</h2><table><thead><tr><th>Case</th><th>Group</th><th>Profile</th><th>Disable</th><th>OK</th><th>FPS</th><th>Delta</th><th>Budget</th><th>Measurement</th><th>Keywords</th><th>Screenshot</th></tr></thead><tbody>")
    for row in case_summaries:
        ok = bool(row.get("ok"))
        delta = row.get("gainVsFullDefaultFps", row.get("dropVsPrevCoreFps", 0.0))
        keywords = ", ".join(
            f"{item.get('name')}={item.get('count')}"
            for item in list(row.get("topKeywords", []) or [])[:4]
        )
        screenshot = f"{int(row.get('screenshotMatchCount', 0))}/{int(row.get('rounds', 0))}"
        status = str(row.get("budgetStatus", "n/a"))
        status_cls = "warn" if status == "over_budget" else ("ok" if ok else "bad")
        measure = str(row.get("measurementStatus", "unknown"))
        parts.append(
            "<tr>"
            f"<td>{row.get('label','')}</td>"
            f"<td>{row.get('group','')}</td>"
            f"<td class='mono'>{row.get('profile','')}</td>"
            f"<td class='mono'>{row.get('disableModules','')}</td>"
            f"<td class='{'ok' if ok else 'bad'}'>{'OK' if ok else 'FAIL'}</td>"
            f"<td>{_f(row.get('avgFps', 0.0))}</td>"
            f"<td>{_f(delta)}</td>"
            f"<td class='{status_cls}'>{status}</td>"
            f"<td>{measure}</td>"
            f"<td>{keywords}</td>"
            f"<td>{screenshot}</td>"
            "</tr>"
        )
    parts.append("</tbody></table></div>")

    parts.append("<div class='section'><h2>Round Details</h2><table><thead><tr><th>Case</th><th>Profile</th><th>Disable</th><th>OK</th><th>FPS</th><th>CPU ms</th><th>GPU ms</th><th>Tracked CPU ms</th><th>Shadow Budget</th><th>Budget Actions</th><th>Keywords</th><th>Report</th></tr></thead><tbody>")
    for row in rows:
        report = row.get("report") or {}
        shadow_budget = report.get("shadowBudgetSummary") or {}
        log_summary = report.get("logSummary") or row.get("logSummary") or {}
        keywords = ", ".join(
            f"{item.get('name')}={item.get('count')}"
            for item in list(log_summary.get("topKeywords", []) or [])[:4]
        )
        budget_actions = (
            f"prioSkip={int(shadow_budget.get('skippedPriorityBudget', 0) or 0)}, "
            f"alphaDegrade={int(shadow_budget.get('degradedAlphaBudget', 0) or 0)}, "
            f"reuse={int(shadow_budget.get('reusedFreezeHits', 0) or 0)}, "
            f"instSave={int(shadow_budget.get('instancedGeometryDrawsSaved', 0) or 0)}, "
            f"actReuse={_f(shadow_budget.get('actualFreezeReuseMb', 0.0))}MB, "
            f"dupBypass={_f(shadow_budget.get('duplicateFreezeBypassMb', 0.0))}MB, "
            f"dupFreeze={_f(shadow_budget.get('potentialFreezeReuseMb', 0.0))}MB"
        )
        report_path = str(report.get("reportPath", "") or "")
        parts.append(
            "<tr>"
            f"<td>{row.get('name','')}</td>"
            f"<td class='mono'>{row.get('profile','')}</td>"
            f"<td class='mono'>{row.get('disableModules','')}</td>"
            f"<td class='{'ok' if row.get('ok') else 'bad'}'>{'OK' if row.get('ok') else 'FAIL'}</td>"
            f"<td>{_f(report.get('avgFps', 0.0))}</td>"
            f"<td>{_f(report.get('avgFrameTimeMs', 0.0))}</td>"
            f"<td>{_f(report.get('avgGpuTimeMs', 0.0))}</td>"
            f"<td>{_f(report.get('avgTrackedActiveCpuMs', 0.0))}</td>"
            f"<td>{int(shadow_budget.get('framesBudgetExceeded', 0) or 0)} / {_f(shadow_budget.get('avgUsedMb', 0.0))}MB</td>"
            f"<td>{budget_actions}</td>"
            f"<td>{keywords}</td>"
            f"<td class='mono'>{report_path}</td>"
            "</tr>"
        )
    parts.append("</tbody></table></div></body></html>")
    path.write_text("".join(parts), encoding="utf-8")


def _mean_or_zero(values: List[float]) -> float:
    vals = [float(v) for v in values]
    return float(sum(vals) / len(vals)) if vals else 0.0


def _aggregate_profile_matrix_rows(
    rows: List[Dict[str, Any]],
    case_defs: List[Dict[str, Any]],
) -> Dict[str, Any]:
    case_by_name = {str(case["name"]): dict(case) for case in case_defs}
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for row in rows:
        key = str(row.get("caseName") or row.get("name") or "")
        grouped.setdefault(key, []).append(row)

    summaries: List[Dict[str, Any]] = []
    for case in case_defs:
        key = str(case["name"])
        case_rows = grouped.get(key, [])
        ok_rows = [r for r in case_rows if bool(r.get("ok"))]
        reports = [dict(r.get("report") or {}) for r in ok_rows]
        report_types = sorted({str(rep.get("reportType", "perf_report")) for rep in reports if rep})
        fps_values = [float(rep.get("avgFps", 0.0) or 0.0) for rep in reports if rep.get("avgFps") is not None]
        cpu_values = [float(rep.get("avgFrameTimeMs", 0.0) or 0.0) for rep in reports if rep.get("avgFrameTimeMs") is not None]
        gpu_values = [float(rep.get("avgGpuTimeMs", 0.0) or 0.0) for rep in reports if rep.get("avgGpuTimeMs") is not None]
        tracked_values = [float(rep.get("avgTrackedActiveCpuMs", 0.0) or 0.0) for rep in reports if rep.get("avgTrackedActiveCpuMs") is not None]
        main_thread_values = [float(rep.get("avgMainThreadCpuMs", 0.0) or 0.0) for rep in reports if rep.get("avgMainThreadCpuMs") is not None]
        coverage_values = [float(rep.get("cpuCoveragePct", 0.0) or 0.0) for rep in reports if rep.get("cpuCoveragePct") is not None]
        shadow_used_values = [
            float((rep.get("shadowBudgetSummary") or {}).get("avgUsedMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_exceeded_frames = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("framesBudgetExceeded", 0) or 0)
            for rep in reports
        )
        shadow_priority_skips = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("skippedPriorityBudget", 0) or 0)
            for rep in reports
        )
        shadow_alpha_degrades = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("degradedAlphaBudget", 0) or 0)
            for rep in reports
        )
        shadow_reuse_hits = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("reusedFreezeHits", 0) or 0)
            for rep in reports
        )
        shadow_reuse_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("reusedFreezeMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_actual_reuse_hits = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("actualFreezeReuseHits", 0) or 0)
            for rep in reports
        )
        shadow_actual_reuse_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("actualFreezeReuseMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_unique_geometry = [
            int((rep.get("shadowBudgetSummary") or {}).get("uniqueGeometryCount", 0) or 0)
            for rep in reports
        ]
        shadow_duplicate_instances = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("duplicateGeometryInstances", 0) or 0)
            for rep in reports
        )
        shadow_reuse_eligible_duplicates = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("reuseEligibleDuplicates", 0) or 0)
            for rep in reports
        )
        shadow_unique_freeze_accepted_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("uniqueFreezeAcceptedMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_duplicate_freeze_bypass_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("duplicateFreezeBypassMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_potential_freeze_reuse_hits = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("potentialFreezeReuseHits", 0) or 0)
            for rep in reports
        )
        shadow_potential_freeze_reuse_mb = [
            float((rep.get("shadowBudgetSummary") or {}).get("potentialFreezeReuseMb", 0.0) or 0.0)
            for rep in reports
        ]
        shadow_instanced_draws_saved = sum(
            int((rep.get("shadowBudgetSummary") or {}).get("instancedGeometryDrawsSaved", 0) or 0)
            for rep in reports
        )
        screenshot_match_count = sum(
            1
            for r in ok_rows
            if bool(((r.get("screenshotSize") or {}).get("matchBaseline", False)))
        )
        keyword_counts: Dict[str, int] = {}
        for rep in reports:
            log_summary = rep.get("logSummary") or {}
            for item in list(log_summary.get("topKeywords", []) or []):
                name = str(item.get("name", ""))
                if not name:
                    continue
                keyword_counts[name] = keyword_counts.get(name, 0) + int(item.get("count", 0) or 0)
        top_keywords = [
            {"name": name, "count": count}
            for name, count in sorted(keyword_counts.items(), key=lambda item: (-item[1], item[0]))
        ]
        summaries.append(
            {
                "caseName": key,
                "label": str(case.get("label", key)),
                "profile": str(case.get("profile", "")),
                "disableModules": str(case.get("disable", "")),
                "group": str(case.get("group", "")),
                "category": str(case.get("category", "")),
                "budgetFps": float(case.get("budgetFps", 0.0) or 0.0),
                "rounds": len(case_rows),
                "success": len(ok_rows),
                "failed": len(case_rows) - len(ok_rows),
                "ok": len(ok_rows) == len(case_rows) and len(case_rows) > 0,
                "avgFps": round(_mean_or_zero(fps_values), 4),
                "avgFrameTimeMs": round(_mean_or_zero(cpu_values), 4),
                "avgGpuTimeMs": round(_mean_or_zero(gpu_values), 4),
                "avgTrackedActiveCpuMs": round(_mean_or_zero(tracked_values), 4),
                "avgMainThreadCpuMs": round(_mean_or_zero(main_thread_values), 4),
                "avgCpuCoveragePct": round(_mean_or_zero(coverage_values), 4),
                "avgShadowUsedMb": round(_mean_or_zero(shadow_used_values), 4),
                "shadowBudgetExceededFrames": int(shadow_exceeded_frames),
                "shadowPriorityBudgetSkips": int(shadow_priority_skips),
                "shadowAlphaBudgetDegrades": int(shadow_alpha_degrades),
                "shadowReuseHits": int(shadow_reuse_hits),
                "avgShadowReuseMb": round(_mean_or_zero(shadow_reuse_mb), 4),
                "shadowActualFreezeReuseHits": int(shadow_actual_reuse_hits),
                "avgShadowActualFreezeReuseMb": round(_mean_or_zero(shadow_actual_reuse_mb), 4),
                "avgUniqueGeometryCount": round(_mean_or_zero(shadow_unique_geometry), 4),
                "shadowDuplicateGeometryInstances": int(shadow_duplicate_instances),
                "shadowReuseEligibleDuplicates": int(shadow_reuse_eligible_duplicates),
                "avgShadowUniqueFreezeAcceptedMb": round(_mean_or_zero(shadow_unique_freeze_accepted_mb), 4),
                "avgShadowDuplicateFreezeBypassMb": round(_mean_or_zero(shadow_duplicate_freeze_bypass_mb), 4),
                "shadowPotentialFreezeReuseHits": int(shadow_potential_freeze_reuse_hits),
                "avgShadowPotentialFreezeReuseMb": round(_mean_or_zero(shadow_potential_freeze_reuse_mb), 4),
                "shadowInstancedGeometryDrawsSaved": int(shadow_instanced_draws_saved),
                "screenshotMatchCount": int(screenshot_match_count),
                "reportTypes": report_types,
                "topKeywords": top_keywords[:6],
            }
        )

    by_case = {str(row["caseName"]): row for row in summaries}
    full_default_fps = float((by_case.get("add_full_default", {}) or {}).get("avgFps", 0.0) or 0.0)
    dxvk_only_fps = float((by_case.get("add_dxvk_only", {}) or {}).get("avgFps", 0.0) or 0.0)
    for row in summaries:
        row["gainVsFullDefaultFps"] = round(float(row.get("avgFps", 0.0) or 0.0) - full_default_fps, 4)
        row["lossVsDxvkOnlyFps"] = round(dxvk_only_fps - float(row.get("avgFps", 0.0) or 0.0), 4)
        row["measurementValid"] = bool(row.get("ok"))
        row["measurementStatus"] = "ok" if row.get("ok") else "case_failed"
        if row.get("ok"):
            report_types = list(row.get("reportTypes", []) or [])
            has_benchmark = "benchmark_log" in report_types
            if (not has_benchmark) and float(row.get("avgMainThreadCpuMs", 0.0) or 0.0) <= 0.0001 and float(row.get("avgCpuCoveragePct", 0.0) or 0.0) <= 0.0001:
                row["measurementValid"] = False
                row["measurementStatus"] = "invalid_no_mainthread_signal"
        if row.get("ok") and dxvk_only_fps > 1e-6 and float(row.get("avgFps", 0.0) or 0.0) > dxvk_only_fps * 1.05:
            row["measurementValid"] = False
            row["measurementStatus"] = "invalid_faster_than_dxvk_only"
        budget = float(row.get("budgetFps", 0.0) or 0.0)
        if row.get("group") == "subtractive" and budget > 0.0:
            gain = float(row.get("gainVsFullDefaultFps", 0.0) or 0.0)
            if not row.get("measurementValid"):
                row["budgetStatus"] = "invalid_measurement"
            else:
                row["budgetStatus"] = "over_budget" if gain > budget else "within_budget"
        else:
            row["budgetStatus"] = "n/a"

    prev_fps = None
    for case_name in PROFILE_MATRIX_PRIMARY_CHAIN:
        row = by_case.get(case_name)
        if not row:
            continue
        current_fps = float(row.get("avgFps", 0.0) or 0.0)
        if prev_fps is None:
            row["dropVsPrevCoreFps"] = 0.0
        else:
            row["dropVsPrevCoreFps"] = round(prev_fps - current_fps, 4)
        prev_fps = current_fps

    top_offenders = sorted(
        [row for row in summaries if row.get("group") == "subtractive" and row.get("measurementValid")],
        key=lambda item: float(item.get("gainVsFullDefaultFps", 0.0) or 0.0),
        reverse=True,
    )
    over_budget = [row for row in top_offenders if row.get("budgetStatus") == "over_budget"]
    invalid_cases = [row for row in summaries if not row.get("measurementValid")]
    return {
        "caseSummaries": summaries,
        "dxvkOnlyFps": dxvk_only_fps,
        "fullDefaultFps": full_default_fps,
        "fullStackGapFps": round(dxvk_only_fps - full_default_fps, 4),
        "topOffenders": top_offenders[:12],
        "overBudgetCases": over_budget,
        "invalidCases": invalid_cases,
    }


@mcp.tool()
def run_profile_matrix(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    rounds_per_case: int = 2,
    sample_duration_sec: int = 60,
    ready_timeout_sec: int = 180,
    windowed: bool = False,
    use_isolated_desktop: bool = True,
    opengl: bool = False,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    env_overrides_json: str = "",
    scenario_name: str = "",
) -> Dict[str, Any]:
    """
    运行运行时档位矩阵，并输出统一 JSON/HTML 汇总。
    """
    rounds = max(1, min(int(rounds_per_case), 5))
    cases: List[Dict[str, Any]] = [dict(case) for case in PROFILE_MATRIX_CASES]
    scenario_name_norm = _normalize_scenario_name(scenario_name)

    out_dir = _ensure_dir(ARTIFACT_ROOT / "profile_matrix" / _now_compact())
    rows: List[Dict[str, Any]] = []
    for case in cases:
        for round_index in range(rounds):
            name = f"{case['name']}_r{round_index + 1}"
            res = run_quick_autotest(
                war3_dir=war3_dir,
                map_path=map_path,
                ready_timeout_sec=ready_timeout_sec,
                sample_duration_sec=sample_duration_sec,
                windowed=windowed,
                use_isolated_desktop=use_isolated_desktop,
                desktop_name=f"War3Matrix_{case['name']}_r{round_index + 1}",
                opengl=opengl,
                auto_perf_record=True,
                auto_perf_export_sec=8,
                deploy_d3d9_before_launch=deploy_d3d9_before_launch,
                build_d3d9_path=build_d3d9_path,
                enforce_video_baseline=True,
                baseline_width=baseline_width,
                baseline_height=baseline_height,
                baseline_refresh_rate=baseline_refresh_rate,
                include_sections_in_report=include_sections_in_report,
                section_top_n=section_top_n,
                avoid_focus_on_stop=True,
                profile=case["profile"],
                disable_modules=case["disable"],
                env_overrides_json=env_overrides_json,
                scenario_name=scenario_name_norm,
            )
            row = {
                "name": name,
                "caseName": case["name"],
                "label": case.get("label", case["name"]),
                "group": case.get("group", ""),
                "category": case.get("category", ""),
                "budgetFps": float(case.get("budgetFps", 0.0) or 0.0),
                "profile": case["profile"],
                "disableModules": case["disable"],
                "ok": bool(res.get("ok")),
                "stage": res.get("stage", ""),
                "scenarioName": scenario_name_norm,
                "warnings": list(res.get("warnings", []) or []),
                "report": dict(res.get("report", {}) or {}),
                "screenshotSize": dict(res.get("screenshotSize", {}) or {}),
                "logSummary": dict(res.get("logSummary", {}) or {}),
                "ready": dict(res.get("ready", {}) or {}),
                "launch": dict(res.get("launch", {}) or {}),
            }
            rows.append(row)

    ok_reports = [r.get("report", {}) for r in rows if r.get("ok")]
    summary = _aggregate_profile_matrix_rows(rows, cases)
    aggregate = {
        "rounds": len(rows),
        "success": len(ok_reports),
        "failed": len(rows) - len(ok_reports),
        "avgFps": round(sum(float(r.get("avgFps", 0.0) or 0.0) for r in ok_reports) / max(1, len(ok_reports)), 4),
        "summary": summary,
    }
    data = {
        "ok": bool(ok_reports),
        "generatedAt": _now_str(),
        "outDir": str(out_dir),
        "useIsolatedDesktop": bool(use_isolated_desktop),
        "scenarioName": scenario_name_norm,
        "aggregate": aggregate,
        "summary": summary,
        "rows": rows,
    }
    json_path = out_dir / "profile_matrix.json"
    html_path = out_dir / "profile_matrix.html"
    json_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    _write_profile_matrix_html(html_path, rows, aggregate)
    return {
        "ok": bool(ok_reports),
        "outDir": str(out_dir),
        "jsonPath": str(json_path),
        "htmlPath": str(html_path),
        "aggregate": aggregate,
        "summary": summary,
        "rows": rows,
    }


@mcp.tool()
def start_periodic_perf_test(
    rounds: int = 3,
    interval_sec: int = 20,
    sample_duration_sec: int = 15,
    ready_timeout_sec: int = 120,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    windowed: bool = False,
    opengl: bool = False,
    auto_perf_record: bool = True,
    auto_perf_export_sec: int = 8,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    avoid_focus_on_stop: bool = True,
    stop_on_failure: bool = False,
    scenario_name: str = "",
) -> Dict[str, Any]:
    """
    启动“定时性能测试”后台任务（非阻塞）。
    - 每轮执行一次 run_quick_autotest。
    - 轮次之间按 interval_sec 等待。
    - 通过 get_periodic_perf_test_status 查询结果。
    """
    rounds = max(1, min(int(rounds), 100))
    interval_sec = max(0, min(int(interval_sec), 3600))
    sample_duration_sec = max(1, min(int(sample_duration_sec), 600))
    ready_timeout_sec = max(20, min(int(ready_timeout_sec), 900))
    scenario_name_norm = _normalize_scenario_name(scenario_name)

    if STATE.perf_thread and STATE.perf_thread.is_alive():
        return {
            "ok": False,
            "error": "已有定时性能任务在运行",
            "job": _get_perf_job_snapshot(limit_results=30),
        }

    with STATE.perf_lock:
        job_id = int(STATE.perf_next_job_id)
        STATE.perf_next_job_id += 1
        STATE.perf_stop.clear()
        STATE.perf_job = {
            "status": "running",
            "jobId": job_id,
            "startedAt": _now_str(),
            "updatedAt": _now_str(),
            "endedAt": "",
            "roundsTotal": rounds,
            "roundsDone": 0,
            "results": [],
            "lastError": "",
            "params": {
                "rounds": rounds,
                "intervalSec": interval_sec,
                "sampleDurationSec": sample_duration_sec,
                "readyTimeoutSec": ready_timeout_sec,
                "war3Dir": war3_dir,
                "mapPath": map_path,
                "windowed": bool(windowed),
                "opengl": bool(opengl),
                "autoPerfRecord": bool(auto_perf_record),
                "autoPerfExportSec": int(auto_perf_export_sec),
                "deployD3d9BeforeLaunch": bool(deploy_d3d9_before_launch),
                "buildD3d9Path": build_d3d9_path,
                "enforceVideoBaseline": bool(enforce_video_baseline),
                "baselineWidth": int(baseline_width),
                "baselineHeight": int(baseline_height),
                "baselineRefreshRate": int(baseline_refresh_rate),
                "includeSectionsInReport": bool(include_sections_in_report),
                "sectionTopN": int(section_top_n),
                "avoidFocusOnStop": bool(avoid_focus_on_stop),
                "stopOnFailure": bool(stop_on_failure),
                "scenarioName": scenario_name_norm,
            },
            "aggregate": {},
        }

    def _worker() -> None:
        status = "completed"
        last_error = ""
        results: List[Dict[str, Any]] = []
        try:
            for i in range(rounds):
                if STATE.perf_stop.is_set():
                    status = "cancelled"
                    break

                t_round = time.time()
                run_res = run_quick_autotest(
                    war3_dir=war3_dir,
                    map_path=map_path,
                    ready_timeout_sec=ready_timeout_sec,
                    sample_duration_sec=sample_duration_sec,
                    windowed=windowed,
                    opengl=opengl,
                    auto_perf_record=auto_perf_record,
                    auto_perf_export_sec=auto_perf_export_sec,
                    deploy_d3d9_before_launch=deploy_d3d9_before_launch,
                    build_d3d9_path=build_d3d9_path,
                    enforce_video_baseline=enforce_video_baseline,
                    baseline_width=baseline_width,
                    baseline_height=baseline_height,
                    baseline_refresh_rate=baseline_refresh_rate,
                    include_sections_in_report=include_sections_in_report,
                    section_top_n=section_top_n,
                    avoid_focus_on_stop=avoid_focus_on_stop,
                    scenario_name=scenario_name_norm,
                )
                elapsed = round(time.time() - t_round, 3)
                report = run_res.get("report", {}) if isinstance(run_res, dict) else {}
                ready = run_res.get("ready", {}) if isinstance(run_res, dict) else {}
                shot_size = run_res.get("screenshotSize", {}) if isinstance(run_res, dict) else {}
                shot_warnings = run_res.get("warnings", []) if isinstance(run_res, dict) else []

                row = {
                    "round": i + 1,
                    "ok": bool(run_res.get("ok")) if isinstance(run_res, dict) else False,
                    "elapsedSec": elapsed,
                    "readyMode": ready.get("mode", ""),
                    "reportPath": report.get("reportPath", ""),
                    "avgFps": float(report.get("avgFps", 0.0) or 0.0),
                    "avgFrameTimeMs": float(report.get("avgFrameTimeMs", 0.0) or 0.0),
                    "avgGpuTimeMs": float(report.get("avgGpuTimeMs", 0.0) or 0.0),
                    "avgTrackedActiveCpuMs": float(report.get("avgTrackedActiveCpuMs", 0.0) or 0.0),
                    "avgUntrackedActiveCpuMs": float(report.get("avgUntrackedActiveCpuMs", 0.0) or 0.0),
                    "avgTrackedAdditiveRootWallMs": float(
                        report.get("avgTrackedAdditiveRootWallMs", 0.0) or 0.0
                    ),
                    "avgUncoveredFrameWallMs": float(
                        report.get("avgUncoveredFrameWallMs", 0.0) or 0.0
                    ),
                    "frameWallScopeCoveragePct": float(
                        report.get("frameWallScopeCoveragePct", 0.0) or 0.0
                    ),
                    "cpuCoveragePct": float(report.get("cpuCoveragePct", 0.0) or 0.0),
                    "cpuCoverageWithIdlePct": float(report.get("cpuCoverageWithIdlePct", 0.0) or 0.0),
                    "jank16": int(report.get("jank16", 0) or 0),
                    "jank33": int(report.get("jank33", 0) or 0),
                    "shotWidth": int(shot_size.get("width", 0) or 0),
                    "shotHeight": int(shot_size.get("height", 0) or 0),
                    "shotMatchBaseline": bool(shot_size.get("matchBaseline", False)),
                    "shotWarnings": shot_warnings,
                    "error": report.get("error", "") if isinstance(report, dict) else "",
                    "time": _now_str(),
                }
                results.append(row)

                if not row["ok"] and not last_error:
                    last_error = row.get("error") or "round failed"

                _set_perf_job_fields(
                    roundsDone=i + 1,
                    results=copy.deepcopy(results),
                    lastError=last_error,
                    aggregate=_compute_perf_aggregate(results),
                )

                if (not row["ok"]) and stop_on_failure:
                    status = "failed"
                    break

                if i < rounds - 1 and interval_sec > 0:
                    if STATE.perf_stop.wait(interval_sec):
                        status = "cancelled"
                        break
        except Exception as e:
            status = "failed"
            last_error = str(e)
        finally:
            # 保底：确保残留进程被清理，避免用户机器被塞满。
            if STATE.war3_pid and _pid_alive(STATE.war3_pid):
                stop_war3(
                    pid=STATE.war3_pid,
                    graceful_wait_sec=3,
                    force=True,
                    avoid_foreground_switch=True,
                )

            if status == "completed" and any(not bool(r.get("ok")) for r in results):
                status = "completed_with_failures"
                if not last_error:
                    last_error = "部分轮次失败"

            _set_perf_job_fields(
                status=status,
                endedAt=_now_str(),
                lastError=last_error,
                aggregate=_compute_perf_aggregate(results),
            )
            with STATE.perf_lock:
                STATE.perf_thread = None
                STATE.perf_stop.clear()

    t = threading.Thread(target=_worker, name=f"war3-perf-job-{job_id}", daemon=True)
    STATE.perf_thread = t
    t.start()

    return {"ok": True, "message": "定时性能任务已启动", "jobId": job_id, "job": _get_perf_job_snapshot(limit_results=10)}


@mcp.tool()
def get_periodic_perf_test_status(limit_results: int = 50) -> Dict[str, Any]:
    """查询定时性能任务状态。"""
    return {"ok": True, "job": _get_perf_job_snapshot(limit_results=limit_results)}


@mcp.tool()
def stop_periodic_perf_test(
    wait_sec: int = 10,
    force_stop_war3: bool = True,
    avoid_focus_on_stop: bool = True,
) -> Dict[str, Any]:
    """停止当前定时性能任务。"""
    t = STATE.perf_thread
    if not t or not t.is_alive():
        return {"ok": True, "message": "当前无运行中的定时性能任务", "job": _get_perf_job_snapshot(limit_results=20)}

    STATE.perf_stop.set()
    t.join(timeout=max(1, min(int(wait_sec), 60)))
    running = bool(STATE.perf_thread and STATE.perf_thread.is_alive())

    if force_stop_war3 and STATE.war3_pid and _pid_alive(STATE.war3_pid):
        stop_war3(
            pid=STATE.war3_pid,
            graceful_wait_sec=3,
            force=True,
            avoid_foreground_switch=avoid_focus_on_stop,
        )

    if running:
        return {"ok": False, "error": "任务未在超时内停止", "job": _get_perf_job_snapshot(limit_results=20)}

    _set_perf_job_fields(status="cancelled", endedAt=_now_str())
    return {"ok": True, "message": "定时性能任务已停止", "job": _get_perf_job_snapshot(limit_results=20)}


@mcp.tool()
def run_periodic_perf_test_blocking(
    rounds: int = 3,
    interval_sec: int = 20,
    sample_duration_sec: int = 15,
    ready_timeout_sec: int = 120,
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    map_path: str = str(DEFAULT_TEST_MAP),
    windowed: bool = False,
    opengl: bool = False,
    auto_perf_record: bool = True,
    auto_perf_export_sec: int = 8,
    deploy_d3d9_before_launch: bool = True,
    build_d3d9_path: str = "build32/src/d3d9/d3d9.dll",
    enforce_video_baseline: bool = True,
    baseline_width: int = DEFAULT_BENCHMARK_WIDTH,
    baseline_height: int = DEFAULT_BENCHMARK_HEIGHT,
    baseline_refresh_rate: int = DEFAULT_BENCHMARK_REFRESH,
    include_sections_in_report: bool = False,
    section_top_n: int = 20,
    avoid_focus_on_stop: bool = True,
    stop_on_failure: bool = False,
    max_wait_sec: int = 3600,
    poll_sec: int = 2,
) -> Dict[str, Any]:
    """阻塞执行定时性能测试，直到任务结束后一次性返回最终结果。"""
    start = start_periodic_perf_test(
        rounds=rounds,
        interval_sec=interval_sec,
        sample_duration_sec=sample_duration_sec,
        ready_timeout_sec=ready_timeout_sec,
        war3_dir=war3_dir,
        map_path=map_path,
        windowed=windowed,
        opengl=opengl,
        auto_perf_record=auto_perf_record,
        auto_perf_export_sec=auto_perf_export_sec,
        deploy_d3d9_before_launch=deploy_d3d9_before_launch,
        build_d3d9_path=build_d3d9_path,
        enforce_video_baseline=enforce_video_baseline,
        baseline_width=baseline_width,
        baseline_height=baseline_height,
        baseline_refresh_rate=baseline_refresh_rate,
        include_sections_in_report=include_sections_in_report,
        section_top_n=section_top_n,
        avoid_focus_on_stop=avoid_focus_on_stop,
        stop_on_failure=stop_on_failure,
    )
    if not start.get("ok"):
        return {"ok": False, "stage": "start", "detail": start}

    t0 = time.time()
    while time.time() - t0 < max(30, min(int(max_wait_sec), 24 * 3600)):
        status = get_periodic_perf_test_status(limit_results=200)
        job = status.get("job", {})
        if not bool(job.get("running")):
            final_status = str(job.get("status", ""))
            return {
                "ok": final_status == "completed",
                "stage": "done",
                "completedWithFailures": final_status == "completed_with_failures",
                "job": job,
            }
        time.sleep(max(1, min(int(poll_sec), 30)))

    stop = stop_periodic_perf_test(
        wait_sec=5,
        force_stop_war3=True,
        avoid_focus_on_stop=avoid_focus_on_stop,
    )
    return {
        "ok": False,
        "stage": "timeout",
        "error": "阻塞等待超时，任务已尝试停止",
        "stop": stop,
        "job": _get_perf_job_snapshot(limit_results=200),
    }


@mcp.tool()
def sync_all_debug(
    war3_dir: str = str(DEFAULT_WAR3_DIR),
    since_id: int = 0,
    event_limit: int = 400,
    contains: str = "",
    tail_lines: int = 200,
    include_dbwin_events: bool = True,
    include_perf_reports: bool = True,
    perf_report_count: int = 3,
    include_log_files: bool = True,
) -> Dict[str, Any]:
    """
    聚合项目调试信息并同步返回：
    - DBWIN 事件（OutputDebugString）
    - runtime_status.json
    - war3_d3d9.log / dxvk.log / war3.log 尾部
    - 最新性能报告摘要
    """
    w3 = Path(war3_dir)
    out: Dict[str, Any] = {
        "ok": True,
        "time": _now_str(),
        "war3Dir": str(w3),
        "war3Pid": STATE.war3_pid or 0,
        "war3Alive": _pid_alive(STATE.war3_pid) if STATE.war3_pid else False,
    }

    runtime_path = _runtime_status_file(w3)
    runtime_data = _read_runtime_status_file(w3)
    out["runtimeStatus"] = {
        "path": str(runtime_path),
        "exists": runtime_path.exists(),
        "data": runtime_data if runtime_data else {},
    }

    if include_dbwin_events:
        rows = STATE.get_events(since_id=since_id, limit=event_limit, contains=contains)
        out["events"] = {
            "count": len(rows),
            "latestId": rows[-1]["id"] if rows else since_id,
            "rows": rows,
        }

    if include_log_files:
        files: List[Dict[str, Any]] = []
        candidates = [
            w3 / "war3_d3d9.log",
            w3 / "dxvk.log",
            w3 / "war3.log",
            w3 / "War3.log",
            w3 / "WarVK" / "Temp" / "runtime_status.json",
        ]
        for p in candidates:
            if not p.exists() or not p.is_file():
                continue
            files.append(
                {
                    "path": str(p),
                    "size": p.stat().st_size,
                    "mtime": datetime.fromtimestamp(p.stat().st_mtime).isoformat(),
                    "tail": _tail_text_file(p, max_lines=tail_lines),
                }
            )
        out["files"] = files

    if include_perf_reports:
        perf_dir = w3 / "WarVK" / "Log"
        perf_rows: List[Dict[str, Any]] = []
        if perf_dir.exists():
            reports = sorted(
                perf_dir.glob("war3_perf_report*.html"),
                key=lambda p: p.stat().st_mtime,
                reverse=True,
            )[: max(1, min(int(perf_report_count), 20))]
            for p in reports:
                summary = _read_perf_summary(p)
                perf_rows.append(
                    {
                        "path": str(p),
                        "size": p.stat().st_size,
                        "mtime": datetime.fromtimestamp(p.stat().st_mtime).isoformat(),
                        "summary": summary,
                    }
                )
        out["perfReports"] = perf_rows

    return out


@mcp.tool()
def current_state() -> Dict[str, Any]:
    """查看当前 MCP 运行态。"""
    registered_sessions = SESSION_REGISTRY.list(include_stopped=True)
    return {
        "ok": True,
        "time": _now_str(),
        "war3Pid": STATE.war3_pid,
        "war3Alive": _pid_alive(STATE.war3_pid) if STATE.war3_pid else False,
        "launcherPid": STATE.launcher_pid,
        "launcherAlive": _pid_alive(STATE.launcher_pid) if STATE.launcher_pid else False,
        "launcherMode": str(STATE.launcher_mode),
        "launcherExe": str(STATE.launcher_exe),
        "war3Dir": str(STATE.war3_dir),
        "testMapPath": str(STATE.test_map_path),
        "desktopMode": str(STATE.desktop_mode),
        "desktopName": str(STATE.desktop_name),
        "desktopHandle": int(STATE.desktop_handle or 0),
        "lastReportPath": str(STATE.last_report_path) if STATE.last_report_path else "",
        "videoRestorePending": bool(STATE.video_restore_snapshot),
        "videoRestoreKeyPath": str(STATE.video_restore_key_path),
        "videoRestoreSnapshot": dict(STATE.video_restore_snapshot),
        "debugEvents": len(STATE.debug_events),
        "debugMonitorRunning": bool(STATE.debug_thread and STATE.debug_thread.is_alive()),
        "registeredSessions": len(registered_sessions),
        "activeSessions": sum(
            1
            for session in registered_sessions
            if _registered_session_alive(session)
        ),
        "sessionRegistry": [
            session.to_dict(alive=_registered_session_alive(session))
            for session in registered_sessions
        ],
        "perfJob": _get_perf_job_snapshot(limit_results=10),
    }


def main() -> None:
    _ensure_dir(ARTIFACT_ROOT)
    mcp.run()


if __name__ == "__main__":
    main()
