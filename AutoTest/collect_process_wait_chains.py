#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""使用 Windows Wait Chain Traversal 对指定进程做只读取证。

该工具只按 PID/TID 查询内核等待关系，不读取目标进程地址，也不依赖目标进程位数。
因此 64 位 Python 可以直接检查 32 位 Warcraft III。正常模式必须显式提供
``--pid`` 与 ``--output``；``--self-test`` 只检查当前 Python 进程。
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence


WCT_MAX_NODE_COUNT = 16
WCT_OBJNAME_LENGTH = 128

TH32CS_SNAPTHREAD = 0x00000004
ERROR_NO_MORE_FILES = 18
ERROR_BAD_LENGTH = 24
ERROR_MORE_DATA = 234
ERROR_TOO_MANY_THREADS = 565
ERROR_NOT_ALL_ASSIGNED = 1300

WCT_OUT_OF_PROC_FLAG = 0x00000001
WCT_OUT_OF_PROC_COM_FLAG = 0x00000002
WCT_OUT_OF_PROC_CS_FLAG = 0x00000004
WCT_NETWORK_IO_FLAG = 0x00000008
WCT_SUPPORTED_GET_FLAGS = (
    WCT_OUT_OF_PROC_FLAG
    | WCT_OUT_OF_PROC_COM_FLAG
    | WCT_OUT_OF_PROC_CS_FLAG
    | WCT_NETWORK_IO_FLAG
)
DEFAULT_WCT_FLAGS = WCT_OUT_OF_PROC_FLAG | WCT_OUT_OF_PROC_CS_FLAG

THREAD_QUERY_LIMITED_INFORMATION = 0x00000800
SYNCHRONIZE = 0x00100000
WAIT_OBJECT_0 = 0x00000000
WAIT_TIMEOUT = 0x00000102
WAIT_FAILED = 0xFFFFFFFF

TOKEN_ADJUST_PRIVILEGES = 0x0020
TOKEN_QUERY = 0x0008
SE_PRIVILEGE_ENABLED = 0x00000002


OBJECT_TYPE_NAMES = {
    1: "CriticalSection",
    2: "SendMessage",
    3: "Mutex",
    4: "Alpc",
    5: "Com",
    6: "ThreadWait",
    7: "ProcessWait",
    8: "Thread",
    9: "ComActivation",
    10: "Unknown",
    11: "SocketIo",
    12: "SmbIo",
    13: "Max",
}

OBJECT_STATUS_NAMES = {
    1: "NoAccess",
    2: "Running",
    3: "Blocked",
    4: "PidOnly",
    5: "PidOnlyRpcss",
    6: "Owned",
    7: "NotOwned",
    8: "Abandoned",
    9: "Unknown",
    10: "Error",
    11: "Max",
}


class WctLockObject(ctypes.Structure):
    _fields_ = [
        ("ObjectName", wintypes.WCHAR * WCT_OBJNAME_LENGTH),
        ("Timeout", ctypes.c_longlong),
        ("Alertable", wintypes.BOOL),
    ]


class WctThreadObject(ctypes.Structure):
    _fields_ = [
        ("ProcessId", wintypes.DWORD),
        ("ThreadId", wintypes.DWORD),
        ("WaitTime", wintypes.DWORD),
        ("ContextSwitches", wintypes.DWORD),
    ]


class WctObjectUnion(ctypes.Union):
    _fields_ = [
        ("LockObject", WctLockObject),
        ("ThreadObject", WctThreadObject),
    ]


class WaitChainNodeInfo(ctypes.Structure):
    _anonymous_ = ("Object",)
    _fields_ = [
        ("ObjectType", ctypes.c_int),
        ("ObjectStatus", ctypes.c_int),
        ("Object", WctObjectUnion),
    ]


class ThreadEntry32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ThreadID", wintypes.DWORD),
        ("th32OwnerProcessID", wintypes.DWORD),
        ("tpBasePri", wintypes.LONG),
        ("tpDeltaPri", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
    ]


class Luid(ctypes.Structure):
    _fields_ = [
        ("LowPart", wintypes.DWORD),
        ("HighPart", wintypes.LONG),
    ]


class LuidAndAttributes(ctypes.Structure):
    _fields_ = [
        ("Luid", Luid),
        ("Attributes", wintypes.DWORD),
    ]


class TokenPrivilegesOne(ctypes.Structure):
    _fields_ = [
        ("PrivilegeCount", wintypes.DWORD),
        ("Privileges", LuidAndAttributes * 1),
    ]


def _format_win32_error(error: int) -> str:
    if not error:
        return ""
    try:
        return ctypes.FormatError(int(error)).strip()
    except Exception:
        return f"Win32 error {int(error)}"


def _error_record(stage: str, error: int, message: str = "") -> Dict[str, Any]:
    return {
        "stage": stage,
        "win32Error": int(error),
        "win32Message": message or _format_win32_error(error),
    }


def _handle_value(handle: Any) -> int:
    if handle is None:
        return 0
    try:
        return int(handle)
    except (TypeError, ValueError):
        value = ctypes.cast(handle, ctypes.c_void_p).value
        return int(value or 0)


def _layout_report() -> Dict[str, Any]:
    actual = {
        "wcharBytes": ctypes.sizeof(wintypes.WCHAR),
        "threadEntryBytes": ctypes.sizeof(ThreadEntry32),
        "lockObjectBytes": ctypes.sizeof(WctLockObject),
        "threadObjectBytes": ctypes.sizeof(WctThreadObject),
        "objectUnionBytes": ctypes.sizeof(WctObjectUnion),
        "nodeBytes": ctypes.sizeof(WaitChainNodeInfo),
        "lockTimeoutOffset": WctLockObject.Timeout.offset,
        "lockAlertableOffset": WctLockObject.Alertable.offset,
        "nodeUnionOffset": WaitChainNodeInfo.Object.offset,
    }
    expected = {
        "wcharBytes": 2,
        "threadEntryBytes": 28,
        "lockObjectBytes": 272,
        "threadObjectBytes": 16,
        "objectUnionBytes": 272,
        "nodeBytes": 280,
        "lockTimeoutOffset": 256,
        "lockAlertableOffset": 264,
        "nodeUnionOffset": 8,
    }
    return {
        "ok": actual == expected,
        "actual": actual,
        "expected": expected,
        "nativePointerBits": ctypes.sizeof(ctypes.c_void_p) * 8,
        "contextTypeBits": ctypes.sizeof(ctypes.c_size_t) * 8,
    }


class WinApi:
    """集中声明 ctypes 签名，避免 64 位默认 ``c_int`` 截断原生句柄。"""

    def __init__(self) -> None:
        if os.name != "nt":
            raise OSError("Wait Chain Traversal 只支持 Windows")

        self.kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
        self.advapi32 = ctypes.WinDLL("advapi32.dll", use_last_error=True)

        self.CreateToolhelp32Snapshot = self.kernel32.CreateToolhelp32Snapshot
        self.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        self.CreateToolhelp32Snapshot.restype = wintypes.HANDLE

        self.Thread32First = self.kernel32.Thread32First
        self.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry32)]
        self.Thread32First.restype = wintypes.BOOL

        self.Thread32Next = self.kernel32.Thread32Next
        self.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry32)]
        self.Thread32Next.restype = wintypes.BOOL

        self.OpenThread = self.kernel32.OpenThread
        self.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.OpenThread.restype = wintypes.HANDLE

        self.GetProcessIdOfThread = self.kernel32.GetProcessIdOfThread
        self.GetProcessIdOfThread.argtypes = [wintypes.HANDLE]
        self.GetProcessIdOfThread.restype = wintypes.DWORD

        self.WaitForSingleObject = self.kernel32.WaitForSingleObject
        self.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.WaitForSingleObject.restype = wintypes.DWORD

        self.CloseHandle = self.kernel32.CloseHandle
        self.CloseHandle.argtypes = [wintypes.HANDLE]
        self.CloseHandle.restype = wintypes.BOOL

        self.GetCurrentProcess = self.kernel32.GetCurrentProcess
        self.GetCurrentProcess.argtypes = []
        self.GetCurrentProcess.restype = wintypes.HANDLE

        self.OpenProcessToken = self.advapi32.OpenProcessToken
        self.OpenProcessToken.argtypes = [
            wintypes.HANDLE,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.HANDLE),
        ]
        self.OpenProcessToken.restype = wintypes.BOOL

        self.LookupPrivilegeValueW = self.advapi32.LookupPrivilegeValueW
        self.LookupPrivilegeValueW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.LPCWSTR,
            ctypes.POINTER(Luid),
        ]
        self.LookupPrivilegeValueW.restype = wintypes.BOOL

        self.AdjustTokenPrivileges = self.advapi32.AdjustTokenPrivileges
        self.AdjustTokenPrivileges.argtypes = [
            wintypes.HANDLE,
            wintypes.BOOL,
            ctypes.POINTER(TokenPrivilegesOne),
            wintypes.DWORD,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self.AdjustTokenPrivileges.restype = wintypes.BOOL

        self.OpenThreadWaitChainSession = (
            self.advapi32.OpenThreadWaitChainSession
        )
        self.OpenThreadWaitChainSession.argtypes = [
            wintypes.DWORD,
            ctypes.c_void_p,
        ]
        self.OpenThreadWaitChainSession.restype = ctypes.c_void_p

        self.CloseThreadWaitChainSession = (
            self.advapi32.CloseThreadWaitChainSession
        )
        self.CloseThreadWaitChainSession.argtypes = [ctypes.c_void_p]
        self.CloseThreadWaitChainSession.restype = None

        self.GetThreadWaitChain = self.advapi32.GetThreadWaitChain
        self.GetThreadWaitChain.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(WaitChainNodeInfo),
            ctypes.POINTER(wintypes.BOOL),
        ]
        self.GetThreadWaitChain.restype = wintypes.BOOL


def _try_enable_debug_privilege(api: WinApi) -> Dict[str, Any]:
    """尽力启用本 helper 的 SeDebugPrivilege；失败不会弹 UAC，也不中断取证。"""

    report: Dict[str, Any] = {
        "attempted": True,
        "enabled": False,
        "name": "SeDebugPrivilege",
        "lifetime": "仅当前短命 Python 取证进程",
    }
    token = wintypes.HANDLE()
    ctypes.set_last_error(0)
    if not api.OpenProcessToken(
        api.GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        ctypes.byref(token),
    ):
        error = ctypes.get_last_error()
        report["error"] = _error_record("OpenProcessToken", error)
        return report

    try:
        privileges = TokenPrivilegesOne()
        privileges.PrivilegeCount = 1
        ctypes.set_last_error(0)
        if not api.LookupPrivilegeValueW(
            None,
            "SeDebugPrivilege",
            ctypes.byref(privileges.Privileges[0].Luid),
        ):
            error = ctypes.get_last_error()
            report["error"] = _error_record("LookupPrivilegeValueW", error)
            return report

        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED
        ctypes.set_last_error(0)
        adjusted = bool(api.AdjustTokenPrivileges(
            token,
            False,
            ctypes.byref(privileges),
            ctypes.sizeof(privileges),
            None,
            None,
        ))
        error = ctypes.get_last_error()
        report["adjustCallSucceeded"] = adjusted
        report["win32Error"] = int(error)
        report["win32Message"] = _format_win32_error(error)
        report["enabled"] = bool(adjusted and error != ERROR_NOT_ALL_ASSIGNED)
        if adjusted and error == ERROR_NOT_ALL_ASSIGNED:
            report["reason"] = "当前令牌不含 SeDebugPrivilege；未触发提权提示"
        elif not adjusted:
            report["reason"] = "AdjustTokenPrivileges 调用失败"
        return report
    finally:
        ctypes.set_last_error(0)
        closed = bool(api.CloseHandle(token))
        report["tokenHandleClosed"] = closed
        if not closed:
            error = ctypes.get_last_error()
            report["tokenCloseError"] = _error_record("CloseHandle(token)", error)


def _enumerate_threads(
    api: WinApi,
    pid: int,
    max_threads: int,
) -> Dict[str, Any]:
    report: Dict[str, Any] = {
        "ok": False,
        "pid": int(pid),
        "requestedMaxThreads": int(max_threads),
        "threadIds": [],
        "snapshotAttempts": [],
    }

    invalid_handle = int(ctypes.c_void_p(-1).value or 0)
    snapshot: Any = None
    for attempt in range(1, 4):
        ctypes.set_last_error(0)
        candidate = api.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
        value = _handle_value(candidate)
        if value and value != invalid_handle:
            snapshot = candidate
            report["snapshotAttempts"].append({
                "attempt": attempt,
                "ok": True,
            })
            break
        error = ctypes.get_last_error()
        report["snapshotAttempts"].append({
            "attempt": attempt,
            "ok": False,
            "error": _error_record("CreateToolhelp32Snapshot", error),
        })
        if error != ERROR_BAD_LENGTH:
            break

    if snapshot is None:
        report["error"] = report["snapshotAttempts"][-1].get("error", {
            "stage": "CreateToolhelp32Snapshot",
            "win32Error": 0,
            "win32Message": "未返回有效快照句柄",
        })
        return report

    thread_ids: List[int] = []
    try:
        entry = ThreadEntry32()
        entry.dwSize = ctypes.sizeof(entry)
        ctypes.set_last_error(0)
        has_entry = bool(api.Thread32First(snapshot, ctypes.byref(entry)))
        if not has_entry:
            error = ctypes.get_last_error()
            if error == ERROR_NO_MORE_FILES:
                report["ok"] = True
                report["enumerationTerminalError"] = int(error)
            else:
                report["error"] = _error_record("Thread32First", error)
            return report

        while True:
            if int(entry.th32OwnerProcessID) == int(pid):
                thread_ids.append(int(entry.th32ThreadID))

            entry.dwSize = ctypes.sizeof(entry)
            ctypes.set_last_error(0)
            if api.Thread32Next(snapshot, ctypes.byref(entry)):
                continue
            error = ctypes.get_last_error()
            report["enumerationTerminalError"] = int(error)
            if error not in (0, ERROR_NO_MORE_FILES):
                report["error"] = _error_record("Thread32Next", error)
                return report
            break

        unique_ids = sorted(set(thread_ids))
        selected_ids = (
            unique_ids[:max_threads]
            if max_threads > 0
            else unique_ids
        )
        report.update({
            "ok": True,
            "totalMatched": len(unique_ids),
            "selectedCount": len(selected_ids),
            "limitReached": bool(
                max_threads > 0 and len(unique_ids) > len(selected_ids)
            ),
            "threadIds": selected_ids,
        })
        return report
    finally:
        ctypes.set_last_error(0)
        closed = bool(api.CloseHandle(snapshot))
        report["snapshotClosed"] = closed
        if not closed:
            error = ctypes.get_last_error()
            report["snapshotCloseError"] = _error_record(
                "CloseHandle(snapshot)", error,
            )


def _decode_node(node: WaitChainNodeInfo, index: int) -> Dict[str, Any]:
    object_type = int(node.ObjectType)
    object_status = int(node.ObjectStatus)
    decoded: Dict[str, Any] = {
        "index": int(index),
        "objectType": object_type,
        "objectTypeName": OBJECT_TYPE_NAMES.get(
            object_type, f"Unknown({object_type})",
        ),
        "objectStatus": object_status,
        "objectStatusName": OBJECT_STATUS_NAMES.get(
            object_status, f"Unknown({object_status})",
        ),
    }
    if object_type == 8:
        decoded["thread"] = {
            "processId": int(node.ThreadObject.ProcessId),
            "threadId": int(node.ThreadObject.ThreadId),
            "waitTimeRaw": int(node.ThreadObject.WaitTime),
            "contextSwitches": int(node.ThreadObject.ContextSwitches),
        }
    else:
        name = str(node.LockObject.ObjectName).split("\x00", 1)[0]
        decoded["lock"] = {
            "objectName": name,
            "timeoutRawReserved": int(node.LockObject.Timeout),
            "alertableRawReserved": int(node.LockObject.Alertable),
        }
    return decoded


def _query_thread_wait_chain(
    api: WinApi,
    session: int,
    pid: int,
    tid: int,
    flags: int,
) -> Dict[str, Any]:
    report: Dict[str, Any] = {
        "threadId": int(tid),
        "targetProcessId": int(pid),
        "queryAttempted": False,
        "ok": False,
        "partialValid": False,
        "attributionExact": False,
        "nodes": [],
    }

    access = THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE
    ctypes.set_last_error(0)
    thread_handle = api.OpenThread(access, False, int(tid))
    if not _handle_value(thread_handle):
        error = ctypes.get_last_error()
        report["error"] = _error_record("OpenThread", error)
        report["reason"] = "无法冻结并核对线程身份，拒绝仅凭可复用 TID 查询"
        return report

    try:
        ctypes.set_last_error(0)
        owner_pid = int(api.GetProcessIdOfThread(thread_handle))
        owner_error = ctypes.get_last_error() if owner_pid == 0 else 0

        ctypes.set_last_error(0)
        wait_before = int(api.WaitForSingleObject(thread_handle, 0))
        wait_before_error = (
            ctypes.get_last_error() if wait_before == WAIT_FAILED else 0
        )
        binding_before = bool(
            owner_pid == int(pid) and wait_before == WAIT_TIMEOUT
        )
        report["threadBinding"] = {
            "requestedProcessId": int(pid),
            "actualProcessId": owner_pid,
            "processIdQueryError": (
                _error_record("GetProcessIdOfThread", owner_error)
                if owner_error else None
            ),
            "waitBefore": wait_before,
            "waitBeforeName": {
                WAIT_OBJECT_0: "terminated",
                WAIT_TIMEOUT: "alive",
                WAIT_FAILED: "failed",
            }.get(wait_before, f"other({wait_before})"),
            "waitBeforeError": (
                _error_record("WaitForSingleObject(before)", wait_before_error)
                if wait_before_error else None
            ),
            "bindingExactBefore": binding_before,
        }
        if not binding_before:
            report["reason"] = "线程所有者或存活状态已漂移，拒绝 WCT 查询"
            return report

        nodes = (WaitChainNodeInfo * WCT_MAX_NODE_COUNT)()
        count = wintypes.DWORD(WCT_MAX_NODE_COUNT)
        is_cycle = wintypes.BOOL(False)
        ctypes.set_last_error(0)
        report["queryAttempted"] = True
        succeeded = bool(api.GetThreadWaitChain(
            ctypes.c_void_p(int(session)),
            ctypes.c_size_t(0),
            wintypes.DWORD(int(flags)),
            wintypes.DWORD(int(tid)),
            ctypes.byref(count),
            nodes,
            ctypes.byref(is_cycle),
        ))
        error = 0 if succeeded else int(ctypes.get_last_error())
        partial_valid = bool(
            not succeeded and error in (ERROR_MORE_DATA, ERROR_TOO_MANY_THREADS)
        )
        captured_count = (
            min(int(count.value), WCT_MAX_NODE_COUNT)
            if succeeded or partial_valid
            else 0
        )
        decoded_nodes = [
            _decode_node(nodes[index], index)
            for index in range(captured_count)
        ]

        first_thread = (
            decoded_nodes[0].get("thread", {})
            if decoded_nodes
            and decoded_nodes[0].get("objectType") == 8
            else {}
        )
        first_node_exact = bool(
            first_thread.get("processId") == int(pid)
            and first_thread.get("threadId") == int(tid)
        )

        ctypes.set_last_error(0)
        wait_after = int(api.WaitForSingleObject(thread_handle, 0))
        wait_after_error = (
            ctypes.get_last_error() if wait_after == WAIT_FAILED else 0
        )
        report["threadBinding"].update({
            "waitAfter": wait_after,
            "waitAfterName": {
                WAIT_OBJECT_0: "terminated",
                WAIT_TIMEOUT: "alive",
                WAIT_FAILED: "failed",
            }.get(wait_after, f"other({wait_after})"),
            "waitAfterError": (
                _error_record("WaitForSingleObject(after)", wait_after_error)
                if wait_after_error else None
            ),
            "firstNodeExact": first_node_exact,
        })
        report.update({
            "ok": succeeded,
            "partialValid": partial_valid,
            "win32Error": error,
            "win32Message": _format_win32_error(error),
            "nodeCountReported": int(count.value),
            "nodeCountCaptured": captured_count,
            "truncated": bool(
                int(count.value) > WCT_MAX_NODE_COUNT
                or error in (ERROR_MORE_DATA, ERROR_TOO_MANY_THREADS)
            ),
            "isCycle": bool(is_cycle.value),
            "deadlockDetected": bool(is_cycle.value),
            "nodes": decoded_nodes,
            # 线程句柄从查询前一直保持到查询后；首节点还要独立闭合
            # PID/TID，且原句柄必须仍未终止。否则 TID 在查询期间漂移时不得
            # 仍标记为精确归因。
            "attributionExact": bool(
                binding_before
                and first_node_exact
                and wait_after == WAIT_TIMEOUT
            ),
            "threadAliveAfterQuery": wait_after == WAIT_TIMEOUT,
        })
        if not succeeded and not partial_valid:
            report["error"] = _error_record("GetThreadWaitChain", error)
        return report
    finally:
        ctypes.set_last_error(0)
        closed = bool(api.CloseHandle(thread_handle))
        report["threadHandleClosed"] = closed
        if not closed:
            error = ctypes.get_last_error()
            report["threadHandleCloseError"] = _error_record(
                "CloseHandle(thread)", error,
            )


def collect_process_wait_chains(
    pid: int,
    max_threads: int = 0,
    flags: int = DEFAULT_WCT_FLAGS,
) -> Dict[str, Any]:
    started = time.time()
    layout = _layout_report()
    report: Dict[str, Any] = {
        "schemaVersion": 1,
        "collector": "AutoTest/collect_process_wait_chains.py",
        "pid": int(pid),
        "requestedMaxThreads": int(max_threads),
        "flags": int(flags),
        "flagsHex": f"0x{int(flags):X}",
        "captureStartedEpochMs": int(started * 1000),
        "caller": {
            "pid": os.getpid(),
            "pythonExecutable": sys.executable,
            "pythonVersion": sys.version.split()[0],
            "pointerBits": ctypes.sizeof(ctypes.c_void_p) * 8,
        },
        "targetBitnessRequired": False,
        "diagnosticOnly": True,
        "authority": False,
        "classificationAuthority": 0,
        "layout": layout,
        "ok": False,
    }

    if not layout["ok"]:
        report["error"] = {
            "stage": "ctypesLayout",
            "message": "WCT ctypes 结构与 Windows SDK ABI 不一致，拒绝调用原生 API",
        }
        return report
    if pid <= 0 or pid > 0xFFFFFFFF:
        report["error"] = {
            "stage": "argumentValidation",
            "message": "PID 必须位于 1..0xFFFFFFFF",
        }
        return report
    if max_threads < 0:
        report["error"] = {
            "stage": "argumentValidation",
            "message": "max_threads 不能为负数；0 表示不限制",
        }
        return report
    if flags < 0 or flags & ~WCT_SUPPORTED_GET_FLAGS:
        report["error"] = {
            "stage": "argumentValidation",
            "message": (
                "flags 只能包含 WCT get-info 位 0x1/0x2/0x4/0x8"
            ),
        }
        return report

    try:
        api = WinApi()
        report["api"] = {
            "ok": True,
            "kernel32": "kernel32.dll",
            "advapi32": "advapi32.dll",
            "sessionMode": "synchronous",
            "comCallbacksRegistered": False,
            "warning": (
                "请求了 COM 位，但最小取证器未注册 COM callback；COM ownership 可能不完整"
                if flags & WCT_OUT_OF_PROC_COM_FLAG else ""
            ),
        }
    except Exception as exc:
        report["api"] = {
            "ok": False,
            "error": f"{type(exc).__name__}: {exc}",
        }
        report["error"] = {
            "stage": "apiLoad",
            "message": report["api"]["error"],
        }
        return report

    report["debugPrivilege"] = _try_enable_debug_privilege(api)
    thread_enumeration = _enumerate_threads(api, pid, max_threads)
    report["threadEnumeration"] = thread_enumeration
    if not thread_enumeration.get("ok"):
        report["error"] = {
            "stage": "threadEnumeration",
            "detail": thread_enumeration.get("error"),
        }
        return report

    ctypes.set_last_error(0)
    session = api.OpenThreadWaitChainSession(0, None)
    session_value = _handle_value(session)
    if not session_value:
        error = ctypes.get_last_error()
        report["session"] = {
            "ok": False,
            "error": _error_record("OpenThreadWaitChainSession", error),
        }
        report["error"] = {
            "stage": "sessionOpen",
            "detail": report["session"]["error"],
        }
        return report

    report["session"] = {"ok": True, "closed": False}
    queries: List[Dict[str, Any]] = []
    try:
        for tid in thread_enumeration.get("threadIds", []):
            queries.append(_query_thread_wait_chain(
                api,
                session_value,
                pid,
                int(tid),
                flags,
            ))
    finally:
        api.CloseThreadWaitChainSession(ctypes.c_void_p(session_value))
        report["session"]["closed"] = True

    report["threads"] = queries
    report["summary"] = {
        "totalMatched": int(thread_enumeration.get("totalMatched", 0)),
        "selected": len(thread_enumeration.get("threadIds", [])),
        "records": len(queries),
        "queriesAttempted": sum(bool(row.get("queryAttempted")) for row in queries),
        "queriesSucceeded": sum(bool(row.get("ok")) for row in queries),
        "partialValid": sum(bool(row.get("partialValid")) for row in queries),
        "queryErrors": sum(
            bool(row.get("queryAttempted"))
            and not bool(row.get("ok"))
            and not bool(row.get("partialValid"))
            for row in queries
        ),
        "identityRejects": sum(
            not bool(row.get("queryAttempted")) for row in queries
        ),
        "attributionExact": sum(
            bool(row.get("attributionExact")) for row in queries
        ),
        "cycles": sum(bool(row.get("isCycle")) for row in queries),
        "accessDenied": sum(
            int(row.get("win32Error", row.get("error", {}).get("win32Error", 0))) == 5
            for row in queries
        ),
    }
    report["ok"] = bool(
        layout["ok"]
        and thread_enumeration.get("ok")
        and report["session"].get("closed")
        and len(queries) == len(thread_enumeration.get("threadIds", []))
    )
    report["allSelectedThreadsQueried"] = bool(
        queries and all(row.get("queryAttempted") for row in queries)
    )
    report["allAttemptedQueriesSucceeded"] = bool(
        queries and all(
            (not row.get("queryAttempted"))
            or row.get("ok")
            or row.get("partialValid")
            for row in queries
        )
    )
    report["captureFinishedEpochMs"] = int(time.time() * 1000)
    report["durationMs"] = round((time.time() - started) * 1000.0, 3)
    return report


def _write_json_atomic(path: Path, data: Dict[str, Any]) -> None:
    """在目标目录内落临时文件，再用同卷 ``os.replace`` 原子发布。"""

    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Optional[Path] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=str(path.parent),
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            json.dump(data, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(str(temporary), str(path))
        temporary = None
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _parse_int(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"无法解析整数 {value!r}；十六进制请使用 0x 前缀"
        ) from exc


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="只读采集指定 PID 的 Windows Wait Chain Traversal 等待链",
    )
    parser.add_argument("--pid", type=_parse_int, help="目标进程 PID")
    parser.add_argument("--output", type=Path, help="原子写入的 JSON 路径")
    parser.add_argument(
        "--max-threads",
        type=int,
        default=0,
        help="最多查询多少个线程；0 表示全部（默认）",
    )
    parser.add_argument(
        "--flags",
        type=_parse_int,
        default=DEFAULT_WCT_FLAGS,
        help="GetThreadWaitChain 标志，默认 0x5",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="只验证 ABI/API，并有限查询当前 Python 进程",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.self_test:
        if args.pid is not None and int(args.pid) != os.getpid():
            parser.error("--self-test 只允许当前 Python PID")
        target_pid = os.getpid()
        max_threads = min(args.max_threads if args.max_threads > 0 else 4, 8)
    else:
        if args.pid is None or args.output is None:
            parser.error("正常模式必须同时提供 --pid 与 --output")
        target_pid = int(args.pid)
        max_threads = int(args.max_threads)

    try:
        report = collect_process_wait_chains(
            pid=target_pid,
            max_threads=max_threads,
            flags=int(args.flags),
        )
    except Exception as exc:
        report = {
            "schemaVersion": 1,
            "collector": "AutoTest/collect_process_wait_chains.py",
            "pid": int(target_pid),
            "ok": False,
            "diagnosticOnly": True,
            "authority": False,
            "classificationAuthority": 0,
            "fatalError": f"{type(exc).__name__}: {exc}",
        }

    if args.output is not None:
        _write_json_atomic(args.output, report)
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))

    # 自检要求基础设施和至少一次精确归因查询闭合；SeDebug 可用性不是硬门。
    if args.self_test:
        summary = dict(report.get("summary", {}) or {})
        return 0 if (
            report.get("ok") is True
            and summary.get("queriesAttempted", 0) >= 1
            and summary.get("attributionExact", 0) >= 1
        ) else 1
    return 0 if report.get("ok") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
