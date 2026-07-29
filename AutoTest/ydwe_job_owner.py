#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""AutoTest 专用的 Win32 Job Object 所有权能力。

本模块只管理 Job 的创建、验证与关闭，不负责启动 Warcraft/YDWE。调用方只能登记
根进程 PID；Job/Process 的原始 HANDLE 始终封装在本对象内部，不参与描述符、JSON
或 ``repr``。
"""

from __future__ import annotations

import ctypes
import json
import os
import re
import secrets
import threading
import time
from dataclasses import asdict, dataclass
from typing import Any, Protocol


SCHEMA_VERSION = "war3-autotest-owned-job-v1"
JOB_NAME_PREFIX = "Local\\War3AutoTest_"
_JOB_NAME_RE = re.compile(r"^Local\\War3AutoTest_[0-9a-f]{32}$")
_ERROR_ALREADY_EXISTS = 183
_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000


@dataclass(frozen=True)
class JobSnapshot:
    kill_on_job_close: bool
    active_processes: int


@dataclass(frozen=True, repr=False)
class JobCreation:
    """Backend 的创建结果；repr 有意隐藏 capability。"""

    capability: object
    already_exists: bool

    def __repr__(self) -> str:
        return f"JobCreation(already_exists={self.already_exists}, capability=<redacted>)"


class OwnedJobBackend(Protocol):
    def create_job(self, name: str) -> JobCreation: ...
    def set_kill_on_job_close(self, capability: object) -> None: ...
    def query_job(self, capability: object) -> JobSnapshot: ...
    def terminate_job(self, capability: object, exit_code: int) -> None: ...
    def capture_root_process(self, process_id: int) -> object: ...
    def query_root_process_creation_time(self, capability: object) -> int: ...
    def is_process_in_job(self, process: object, job: object) -> bool: ...
    def is_root_process_signaled(self, capability: object) -> bool: ...
    def wait_root_process(self, capability: object, timeout_ms: int) -> bool: ...
    def wait_zero_active_processes(self, capability: object, timeout_ms: int) -> bool: ...
    def close_root_process(self, capability: object) -> None: ...
    def close_job(self, capability: object) -> None: ...


class _SecretCapability:
    __slots__ = ("value",)

    def __init__(self, value: object) -> None:
        self.value = value

    def __repr__(self) -> str:
        return "<opaque capability>"


@dataclass(frozen=True)
class OwnedJobDescriptor:
    schemaVersion: str
    jobName: str
    state: str
    killOnJobClose: bool | None
    activeProcesses: int | None
    rootProcessTracked: bool
    rootProcessInJob: bool | None
    cleanupAttempts: int
    lastFailure: str | None

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), ensure_ascii=False, sort_keys=True)


class OwnedJobError(RuntimeError):
    """失败时携带可重试清理的 owner，但不会格式化其内部 capability。"""

    def __init__(self, message: str, owner: "OwnedJobOwner | None" = None) -> None:
        super().__init__(message)
        self.owner = owner


class JobNameCollisionError(OwnedJobError):
    pass


def _safe_failure(operation: str, error: BaseException | str) -> str:
    if isinstance(error, str):
        return f"{operation}: {error}"
    details: list[str] = []
    for label in ("winerror", "errno"):
        value = getattr(error, label, None)
        if value is not None:
            details.append(f"{label}={int(value)}")
    suffix = f"({', '.join(details)})" if details else ""
    return f"{operation}: {type(error).__name__}{suffix}"


class OwnedJobOwner:
    """唯一拥有 Job HANDLE 的状态机。

    ``create`` 不接收 Job 名、外部 HANDLE 或“仍被持有”布尔值。名称由密码学随机源
    生成；只有本对象成功设置并反查 Job 策略后才进入 ``ready``。
    """

    __slots__ = (
        "_backend", "_job", "_root_process", "_root_process_in_job", "_job_name", "_state",
        "_snapshot", "_cleanup_attempts", "_last_failure", "_foreign_collision", "_lock",
    )

    def __init__(self, backend: OwnedJobBackend, name: str, capability: object) -> None:
        self._backend = backend
        self._job = _SecretCapability(capability)
        self._root_process: _SecretCapability | None = None
        self._root_process_in_job: bool | None = None
        self._job_name = name
        self._state = "created"
        self._snapshot: JobSnapshot | None = None
        self._cleanup_attempts = 0
        self._last_failure: str | None = None
        self._foreign_collision = False
        self._lock = threading.RLock()

    @classmethod
    def create(cls, backend: OwnedJobBackend | None = None) -> "OwnedJobOwner":
        selected_backend = backend if backend is not None else Win32OwnedJobBackend()
        name = JOB_NAME_PREFIX + secrets.token_hex(16)
        if not _JOB_NAME_RE.fullmatch(name):
            raise AssertionError("内部 Job 名生成器违反命名契约")

        creation = selected_backend.create_job(name)
        owner = cls(selected_backend, name, creation.capability)
        if creation.already_exists:
            owner._foreign_collision = True
            owner._state = "cleanup_failed"
            owner._last_failure = _safe_failure("CreateJobObjectW", "ERROR_ALREADY_EXISTS")
            try:
                selected_backend.close_job(owner._job.value)
                owner._job = None
                owner._state = "closed"
            except Exception as error:
                owner._last_failure = _safe_failure("CloseHandle(existing job)", error)
            raise JobNameCollisionError("随机 Job 名已存在，已拒绝复用", owner)

        try:
            selected_backend.set_kill_on_job_close(owner._job.value)
            owner._verify_empty_and_protected("create")
            owner._state = "ready"
            return owner
        except Exception as error:
            owner._state = "cleanup_failed"
            owner._last_failure = _safe_failure("configure_job", error)
            raise OwnedJobError("Job 创建后的策略验证失败；owner 已保留，可重试清理", owner) from None

    def _require_job(self) -> _SecretCapability:
        if self._job is None:
            raise OwnedJobError("Job owner 已关闭", self)
        return self._job

    def _verify_empty_and_protected(self, operation: str) -> JobSnapshot:
        snapshot = self._backend.query_job(self._require_job().value)
        self._snapshot = snapshot
        if not snapshot.kill_on_job_close:
            raise OwnedJobError(f"{operation}: KILL_ON_JOB_CLOSE 反查失败", self)
        if snapshot.active_processes != 0:
            raise OwnedJobError(f"{operation}: 启动前 ActiveProcesses 必须为 0", self)
        return snapshot

    def verify_ready_for_launch(self) -> OwnedJobDescriptor:
        with self._lock:
            if self._state != "ready" or self._root_process is not None:
                raise OwnedJobError("Job 不处于可启动状态", self)
            try:
                self._verify_empty_and_protected("pre_launch")
            except Exception as error:
                self._state = "cleanup_failed"
                self._last_failure = _safe_failure("pre_launch", error)
                raise OwnedJobError("启动前 Job 验证失败；必须先清理", self) from None
            return self.descriptor()

    def register_root_process(
        self,
        process_id: int,
        creation_time_100ns: int,
    ) -> OwnedJobDescriptor:
        """登记根进程身份；只接受 PID/创建时间，不接受 bool 或任何 HANDLE。"""

        with self._lock:
            if (isinstance(process_id, bool) or not isinstance(process_id, int) or
                    not 1 <= process_id <= 0xFFFFFFFF):
                raise ValueError("process_id 必须是 1..0xFFFFFFFF 的 PID，不能是 bool/HANDLE")
            if (isinstance(creation_time_100ns, bool) or not isinstance(creation_time_100ns, int) or
                    creation_time_100ns <= 0):
                raise ValueError("creation_time_100ns 必须是正整数，不能是 bool/HANDLE")
            if self._state != "ready" or self._root_process is not None:
                raise OwnedJobError("Job 不允许登记根进程", self)
            try:
                root = self._backend.capture_root_process(process_id)
                self._root_process = _SecretCapability(root)
                self._root_process_in_job = self._backend.is_process_in_job(
                    root, self._require_job().value)
                actual_creation_time = self._backend.query_root_process_creation_time(root)
                if actual_creation_time != creation_time_100ns:
                    raise OwnedJobError("根进程创建时间与 helper 证据不匹配", self)
                if self._backend.is_root_process_signaled(root):
                    raise OwnedJobError("根进程在登记前已经退出", self)
                if not self._root_process_in_job:
                    try:
                        self._backend.close_root_process(root)
                        self._root_process = None
                        self._root_process_in_job = None
                    except Exception:
                        pass
                    raise OwnedJobError("根进程不属于 owner Job", self)
                snapshot = self._backend.query_job(self._require_job().value)
                self._snapshot = snapshot
                if not snapshot.kill_on_job_close or snapshot.active_processes != 1:
                    raise OwnedJobError("根进程登记后 Job accounting 不成立", self)
                self._state = "active"
                return self.descriptor()
            except Exception as error:
                self._state = "cleanup_failed"
                self._last_failure = _safe_failure("register_root_process", error)
                raise OwnedJobError("根进程登记失败；owner 已保留，可重试清理", self) from None

    def cleanup(self, *, timeout_ms: int = 15000, exit_code: int = 0xE0010001) -> OwnedJobDescriptor:
        with self._lock:
            if not isinstance(timeout_ms, int) or isinstance(timeout_ms, bool) or not 1 <= timeout_ms <= 60000:
                raise ValueError("timeout_ms 必须位于 1..60000")
            if self._state == "closed":
                return self.descriptor()
            job = self._require_job()
            self._cleanup_attempts += 1

            if self._foreign_collision:
                try:
                    self._backend.close_job(job.value)
                except Exception as error:
                    self._fail_cleanup("close_existing_job_reference", error)
                    return self.descriptor()
                self._job = None
                self._state = "closed"
                self._last_failure = None
                return self.descriptor()

            try:
                self._backend.terminate_job(job.value, exit_code)
                if self._root_process is not None and self._root_process_in_job is True:
                    if not self._backend.wait_root_process(self._root_process.value, timeout_ms):
                        raise TimeoutError("root process wait timed out")
                if not self._backend.wait_zero_active_processes(job.value, timeout_ms):
                    raise TimeoutError("ActiveProcesses did not reach zero")
                snapshot = self._backend.query_job(job.value)
                self._snapshot = snapshot
                if snapshot.active_processes != 0:
                    raise RuntimeError("ActiveProcesses remained non-zero")
                if self._root_process is not None:
                    self._backend.close_root_process(self._root_process.value)
                    self._root_process = None
                    self._root_process_in_job = None
                self._backend.close_job(job.value)
            except Exception as error:
                self._fail_cleanup("cleanup", error)
                return self.descriptor()

            self._job = None
            self._state = "closed"
            self._last_failure = None
            return self.descriptor()

    def _fail_cleanup(self, operation: str, error: BaseException) -> None:
        self._state = "cleanup_failed"
        self._last_failure = _safe_failure(operation, error)

    def descriptor(self) -> OwnedJobDescriptor:
        with self._lock:
            snapshot = self._snapshot
            return OwnedJobDescriptor(
                schemaVersion=SCHEMA_VERSION,
                jobName=self._job_name,
                state=self._state,
                killOnJobClose=None if snapshot is None else snapshot.kill_on_job_close,
                activeProcesses=None if snapshot is None else snapshot.active_processes,
                rootProcessTracked=self._root_process is not None,
                rootProcessInJob=self._root_process_in_job,
                cleanupAttempts=self._cleanup_attempts,
                lastFailure=self._last_failure,
            )

    def __repr__(self) -> str:
        return f"OwnedJobOwner({self.descriptor().to_json()})"


class _Win32Handle:
    __slots__ = ("value",)

    def __init__(self, value: int) -> None:
        self.value = int(value)

    def __repr__(self) -> str:
        return "<Win32 HANDLE redacted>"


class Win32OwnedJobBackend:
    """真实 Win32 backend；构造本身不创建 Job。"""

    def __init__(self) -> None:
        if os.name != "nt":
            raise OSError("Win32OwnedJobBackend 仅支持 Windows")
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._bind_functions()

    def _bind_functions(self) -> None:
        k32 = self._kernel32
        k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
        k32.CreateJobObjectW.restype = ctypes.c_void_p
        k32.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        k32.SetInformationJobObject.restype = ctypes.c_int
        k32.QueryInformationJobObject.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        k32.QueryInformationJobObject.restype = ctypes.c_int
        k32.TerminateJobObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        k32.TerminateJobObject.restype = ctypes.c_int
        k32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        k32.OpenProcess.restype = ctypes.c_void_p
        k32.GetProcessTimes.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
        ]
        k32.GetProcessTimes.restype = ctypes.c_int
        k32.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        k32.IsProcessInJob.restype = ctypes.c_int
        k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        k32.WaitForSingleObject.restype = ctypes.c_uint32
        k32.CloseHandle.argtypes = [ctypes.c_void_p]
        k32.CloseHandle.restype = ctypes.c_int

    @staticmethod
    def _value(capability: object) -> int:
        if not isinstance(capability, _Win32Handle):
            raise TypeError("backend capability 类型不匹配")
        return capability.value

    @staticmethod
    def _raise_last_error(operation: str) -> None:
        raise ctypes.WinError(ctypes.get_last_error(), operation)

    @staticmethod
    def _structures() -> tuple[type[ctypes.Structure], type[ctypes.Structure]]:
        class BasicLimit(ctypes.Structure):
            _fields_ = [
                ("perProcessUserTime", ctypes.c_longlong), ("perJobUserTime", ctypes.c_longlong),
                ("limitFlags", ctypes.c_uint32), ("minimumWorkingSet", ctypes.c_size_t),
                ("maximumWorkingSet", ctypes.c_size_t), ("activeProcessLimit", ctypes.c_uint32),
                ("affinity", ctypes.c_size_t), ("priorityClass", ctypes.c_uint32),
                ("schedulingClass", ctypes.c_uint32),
            ]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in (
                "readOperationCount", "writeOperationCount", "otherOperationCount",
                "readTransferCount", "writeTransferCount", "otherTransferCount",
            )]

        class ExtendedLimit(ctypes.Structure):
            _fields_ = [
                ("basic", BasicLimit), ("io", IoCounters),
                ("processMemoryLimit", ctypes.c_size_t), ("jobMemoryLimit", ctypes.c_size_t),
                ("peakProcessMemoryUsed", ctypes.c_size_t), ("peakJobMemoryUsed", ctypes.c_size_t),
            ]

        class BasicAccounting(ctypes.Structure):
            _fields_ = [
                ("totalUserTime", ctypes.c_longlong), ("totalKernelTime", ctypes.c_longlong),
                ("thisPeriodUserTime", ctypes.c_longlong), ("thisPeriodKernelTime", ctypes.c_longlong),
                ("totalPageFaultCount", ctypes.c_uint32), ("totalProcesses", ctypes.c_uint32),
                ("activeProcesses", ctypes.c_uint32), ("totalTerminatedProcesses", ctypes.c_uint32),
            ]

        return ExtendedLimit, BasicAccounting

    def _query(self, capability: object) -> tuple[ctypes.Structure, ctypes.Structure]:
        extended_type, accounting_type = self._structures()
        extended = extended_type()
        accounting = accounting_type()
        value = self._value(capability)
        if not self._kernel32.QueryInformationJobObject(value, 9, ctypes.byref(extended), ctypes.sizeof(extended), None):
            self._raise_last_error("QueryInformationJobObject(limit)")
        if not self._kernel32.QueryInformationJobObject(value, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None):
            self._raise_last_error("QueryInformationJobObject(accounting)")
        return extended, accounting

    def create_job(self, name: str) -> JobCreation:
        ctypes.set_last_error(0)
        handle = self._kernel32.CreateJobObjectW(None, name)
        error = ctypes.get_last_error()
        if not handle:
            self._raise_last_error("CreateJobObjectW")
        return JobCreation(_Win32Handle(handle), error == _ERROR_ALREADY_EXISTS)

    def set_kill_on_job_close(self, capability: object) -> None:
        extended, _ = self._query(capability)
        extended.basic.limitFlags |= _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self._kernel32.SetInformationJobObject(
                self._value(capability), 9, ctypes.byref(extended), ctypes.sizeof(extended)):
            self._raise_last_error("SetInformationJobObject")

    def query_job(self, capability: object) -> JobSnapshot:
        extended, accounting = self._query(capability)
        return JobSnapshot(
            bool(extended.basic.limitFlags & _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE),
            int(accounting.activeProcesses),
        )

    def terminate_job(self, capability: object, exit_code: int) -> None:
        if not self._kernel32.TerminateJobObject(self._value(capability), int(exit_code) & 0xFFFFFFFF):
            self._raise_last_error("TerminateJobObject")

    def capture_root_process(self, process_id: int) -> object:
        handle = self._kernel32.OpenProcess(0x00100000 | 0x00001000, 0, process_id)
        if not handle:
            self._raise_last_error("OpenProcess(root)")
        return _Win32Handle(handle)

    def query_root_process_creation_time(self, capability: object) -> int:
        class FileTime(ctypes.Structure):
            _fields_ = [("low", ctypes.c_uint32), ("high", ctypes.c_uint32)]

        creation = FileTime()
        exit_time = FileTime()
        kernel = FileTime()
        user = FileTime()
        if not self._kernel32.GetProcessTimes(
                self._value(capability), ctypes.byref(creation), ctypes.byref(exit_time),
                ctypes.byref(kernel), ctypes.byref(user)):
            self._raise_last_error("GetProcessTimes(root)")
        return (int(creation.high) << 32) | int(creation.low)

    def is_process_in_job(self, process: object, job: object) -> bool:
        in_job = ctypes.c_int(0)
        if not self._kernel32.IsProcessInJob(
                self._value(process), self._value(job), ctypes.byref(in_job)):
            self._raise_last_error("IsProcessInJob(root)")
        return bool(in_job.value)

    def is_root_process_signaled(self, capability: object) -> bool:
        result = self._kernel32.WaitForSingleObject(self._value(capability), 0)
        if result == 0:
            return True
        if result == 0x102:
            return False
        self._raise_last_error("WaitForSingleObject(root poll)")
        return True

    def wait_root_process(self, capability: object, timeout_ms: int) -> bool:
        result = self._kernel32.WaitForSingleObject(self._value(capability), timeout_ms)
        if result == 0:
            return True
        if result == 0x102:
            return False
        self._raise_last_error("WaitForSingleObject(root)")
        return False

    def wait_zero_active_processes(self, capability: object, timeout_ms: int) -> bool:
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            if self.query_job(capability).active_processes == 0:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(min(0.01, max(0.0, deadline - time.monotonic())))

    def close_root_process(self, capability: object) -> None:
        self._close(capability)

    def close_job(self, capability: object) -> None:
        self._close(capability)

    def _close(self, capability: object) -> None:
        value = self._value(capability)
        if not self._kernel32.CloseHandle(value):
            self._raise_last_error("CloseHandle")
        capability.value = 0


__all__ = [
    "JOB_NAME_PREFIX",
    "JobCreation",
    "JobNameCollisionError",
    "JobSnapshot",
    "OwnedJobBackend",
    "OwnedJobDescriptor",
    "OwnedJobError",
    "OwnedJobOwner",
    "Win32OwnedJobBackend",
]
