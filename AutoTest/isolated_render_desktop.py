"""Explicit, non-input Win32 Desktop launcher for standalone render labs.

Importing this module has no Win32 side effects. Entering the context creates a
Desktop; run() is the sole process-creation authority. There is no game lookup,
deployment, display-mode change, input injection, or default-desktop fallback.
The launch is native STARTUPINFOEX, not Popen: JOB_LIST and HANDLE_LIST must both
be applied atomically before the first instruction of the child can execute.
Input-name witnesses prove no observed Desktop switch, NOT driver/GPU safety.

Usage (the caller supplies already-built standalone test executables):
    with IsolatedRenderDesktop() as desktop:
        result = desktop.run(argv, cwd=work, stdout_path=new_out,
                             stderr_path=new_err, timeout_s=30)
    # Inspect both result and desktop.receipt; context exit can fail cleanup.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes as wt
import math
import os
from pathlib import Path
import re
import secrets
import subprocess
import threading
import time
from typing import Any, Mapping, Sequence


class DesktopSafetyError(RuntimeError):
    def __init__(self, message: str, receipt: dict[str, Any] | None = None):
        super().__init__(message)
        self.receipt = receipt


class _SecurityAttributes(ctypes.Structure):
    _fields_ = [("nLength", wt.DWORD), ("lpSecurityDescriptor", wt.LPVOID),
                ("bInheritHandle", wt.BOOL)]


class _StartupInfo(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR),
                ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
                ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
                ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.POINTER(ctypes.c_ubyte)),
                ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class _StartupInfoEx(ctypes.Structure):
    _fields_ = [("StartupInfo", _StartupInfo), ("lpAttributeList", wt.LPVOID)]


class _ProcessInfo(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


class _BasicLimits(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                ("PerJobUserTimeLimit", ctypes.c_int64), ("LimitFlags", wt.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wt.DWORD), ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wt.DWORD), ("SchedulingClass", wt.DWORD)]


class _IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                 "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", _BasicLimits), ("IoInfo", _IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t)]


class _JobAccounting(ctypes.Structure):
    _fields_ = [(name, ctypes.c_int64) for name in
                ("TotalUserTime", "TotalKernelTime", "ThisPeriodTotalUserTime",
                 "ThisPeriodTotalKernelTime")] + [(name, wt.DWORD) for name in
                ("TotalPageFaultCount", "TotalProcesses", "ActiveProcesses",
                 "TotalTerminatedProcesses")]


def _require(value: Any, message: str) -> None:
    if not value:
        raise DesktopSafetyError(message)


def _launch_arguments(argv: Sequence[str | os.PathLike[str]], cwd: str | os.PathLike[str],
                      timeout_s: float) -> tuple[list[str], Path, float]:
    _require(not isinstance(argv, (str, bytes)) and len(argv) > 0, "argv must be a nonempty sequence")
    args = [os.fspath(value) for value in argv]
    _require(all(isinstance(value, str) and "\0" not in value for value in args), "invalid argv")
    executable = Path(args[0])
    directory = Path(cwd)
    _require(executable.is_absolute() and executable.is_file(), "executable must be an absolute existing file")
    _require(directory.is_absolute() and directory.is_dir(), "cwd must be an absolute existing directory")
    _require(type(timeout_s) in (int, float) and math.isfinite(timeout_s)
             and 0 < timeout_s <= 300, "timeout must be finite and within (0,300] seconds")
    args[0] = str(executable.resolve())
    return args, directory.resolve(), float(timeout_s)


class _WinApi:
    def __init__(self) -> None:
        _require(os.name == "nt", "Win32 non-input Desktop launcher is Windows-only")
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.u = ctypes.WinDLL("user32", use_last_error=True)
        signatures = (
            (self.u, "GetProcessWindowStation", wt.HANDLE, ()),
            (self.u, "GetThreadDesktop", wt.HANDLE, (wt.DWORD,)),
            (self.u, "OpenInputDesktop", wt.HANDLE, (wt.DWORD, wt.BOOL, wt.DWORD)),
            (self.u, "CloseDesktop", wt.BOOL, (wt.HANDLE,)),
            (self.u, "GetUserObjectInformationW", wt.BOOL,
             (wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD))),
            (self.u, "CreateDesktopW", wt.HANDLE,
             (wt.LPCWSTR, wt.LPCWSTR, wt.LPVOID, wt.DWORD, wt.DWORD,
              ctypes.POINTER(_SecurityAttributes))),
            (self.k, "GetCurrentThreadId", wt.DWORD, ()),
            (self.k, "CreateJobObjectW", wt.HANDLE, (wt.LPVOID, wt.LPCWSTR)),
            (self.k, "SetInformationJobObject", wt.BOOL,
             (wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD)),
            (self.k, "QueryInformationJobObject", wt.BOOL,
             (wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD))),
            (self.k, "TerminateJobObject", wt.BOOL, (wt.HANDLE, wt.UINT)),
            (self.k, "CloseHandle", wt.BOOL, (wt.HANDLE,)),
            (self.k, "CreateFileW", wt.HANDLE,
             (wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.POINTER(_SecurityAttributes),
              wt.DWORD, wt.DWORD, wt.HANDLE)),
            (self.k, "InitializeProcThreadAttributeList", wt.BOOL,
             (wt.LPVOID, wt.DWORD, wt.DWORD, ctypes.POINTER(ctypes.c_size_t))),
            (self.k, "UpdateProcThreadAttribute", wt.BOOL,
             (wt.LPVOID, wt.DWORD, ctypes.c_size_t, wt.LPVOID, ctypes.c_size_t,
              wt.LPVOID, ctypes.POINTER(ctypes.c_size_t))),
            (self.k, "DeleteProcThreadAttributeList", None, (wt.LPVOID,)),
            (self.k, "CreateProcessW", wt.BOOL,
             (wt.LPCWSTR, wt.LPWSTR, wt.LPVOID, wt.LPVOID, wt.BOOL, wt.DWORD,
              wt.LPVOID, wt.LPCWSTR, ctypes.POINTER(_StartupInfoEx), ctypes.POINTER(_ProcessInfo))),
            (self.k, "GetProcessTimes", wt.BOOL,
             (wt.HANDLE,) + (ctypes.POINTER(wt.FILETIME),) * 4),
            (self.k, "QueryFullProcessImageNameW", wt.BOOL,
             (wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD))),
            (self.k, "IsProcessInJob", wt.BOOL, (wt.HANDLE, wt.HANDLE, ctypes.POINTER(wt.BOOL))),
            (self.k, "WaitForSingleObject", wt.DWORD, (wt.HANDLE, wt.DWORD)),
            (self.k, "GetExitCodeProcess", wt.BOOL, (wt.HANDLE, ctypes.POINTER(wt.DWORD))),
            (self.k, "ResumeThread", wt.DWORD, (wt.HANDLE,)),
        )
        for library, name, result, arguments in signatures:
            function = getattr(library, name)
            function.restype, function.argtypes = result, arguments

    def check(self, value: Any, operation: str) -> Any:
        if not value:
            raise DesktopSafetyError(f"{operation} failed: Win32={ctypes.get_last_error()}")
        return value

    def name(self, handle: int) -> str:
        self.check(handle, "user-object handle")
        size = wt.DWORD()
        self.u.GetUserObjectInformationW(handle, 2, None, 0, ctypes.byref(size))
        _require(0 < size.value <= 4096, "user-object name size invalid")
        buffer = ctypes.create_unicode_buffer((size.value + 1) // 2)
        self.check(self.u.GetUserObjectInformationW(handle, 2, buffer,
                   ctypes.sizeof(buffer), ctypes.byref(size)), "GetUserObjectInformationW")
        _require(bool(buffer.value), "empty user-object name")
        return buffer.value

    def input_name(self) -> str:
        handle = self.check(self.u.OpenInputDesktop(0, False, 0x0001), "OpenInputDesktop")
        try:
            name = self.name(handle)
        except BaseException as original:
            if not self.u.CloseDesktop(handle) and hasattr(original, "add_note"):
                original.add_note(f"CloseDesktop(input witness):{ctypes.get_last_error()}")
            raise
        self.check(self.u.CloseDesktop(handle), "CloseDesktop(input witness)")
        return name

    def active_processes(self, job: int) -> int:
        counters = _JobAccounting()
        self.check(self.k.QueryInformationJobObject(job, 1, ctypes.byref(counters),
                   ctypes.sizeof(counters), None), "QueryInformationJobObject")
        return counters.ActiveProcesses


class IsolatedRenderDesktop:
    """Own exactly one Desktop and sequential, atomically Job-contained launches."""
    def __init__(self, name_prefix: str = "WarVK_RenderLab", *, restricted_medium: bool = False) -> None:
        _require(isinstance(name_prefix, str) and re.fullmatch(r"[A-Za-z0-9_]{1,64}", name_prefix),
                 "invalid Desktop prefix")
        _require(type(restricted_medium) is bool, "restricted_medium must be an explicit bool")
        self.restricted_medium = restricted_medium
        self.desktop_name = f"{name_prefix}_{os.getpid()}_{secrets.token_hex(8)}"
        self.desktop_path = ""
        self.input_before = ""
        self.window_station = ""
        self._api: _WinApi | None = None
        self._desktop = 0
        self._pending: list[tuple[int, int, dict[str, Any]]] = []
        self._creator: threading.Thread | None = None
        self._creator_result: dict[str, Any] = {}
        self._creator_adopted = False
        self._entered = False
        self.receipt: dict[str, Any] = dict(
            schema=1, scope="NON_INPUT_DESKTOP_LAUNCH_CONTEXT", ok=False,
            desktop=self.desktop_name, desktopCreated=False, desktopClosed=False,
            desktopHandleInheritable=False, desktopSwitchRequested=False,
            restrictedMediumRequested=restricted_medium,
            globalInputUsed=False, gameTouched=False, launches=[], cleanupErrors=[])

    def __enter__(self) -> "IsolatedRenderDesktop":
        _require(not self._entered, "Desktop contexts are single-use")
        self._entered = True
        self._api = _WinApi()
        api = self._api
        try:
            self.window_station = api.name(api.u.GetProcessWindowStation())
            _require(self.window_station.casefold() == "winsta0", "launcher is not on WinSta0")
            self.input_before = api.input_name()
            launcher_desktop = api.name(api.u.GetThreadDesktop(api.k.GetCurrentThreadId()))
            _require(self.desktop_name.casefold() != self.input_before.casefold(), "input Desktop name collision")
            self.desktop_path = self.window_station + "\\" + self.desktop_name
            self.receipt.update(windowStation=self.window_station, inputBefore=self.input_before,
                                launcherDesktop=launcher_desktop, desktopPath=self.desktop_path)
            # CreateDesktopW assigns its Desktop to the calling thread. Confine
            # that association to a short-lived worker, then join it before the
            # coordinator can launch/close anything. Never reassign the caller.
            def create() -> None:
                try:
                    security = _SecurityAttributes(ctypes.sizeof(_SecurityAttributes), None, False)
                    ctypes.set_last_error(0)
                    handle = api.u.CreateDesktopW(self.desktop_name, None, None, 0,
                        0x0001 | 0x0002 | 0x0040 | 0x0080, ctypes.byref(security))
                    error = ctypes.get_last_error()
                    self._creator_result.update(handle=handle or 0, error=error)
                except BaseException as error:
                    self._creator_result["exception"] = f"{type(error).__name__}: {error}"

            self._creator = threading.Thread(target=create, name="WarVK-Desktop-Creator", daemon=True)
            self._creator.start()
            self._creator.join(5.0)
            _require(not self._creator.is_alive(), "CreateDesktop creator unresolved; no process launch allowed")
            self._adopt_creator()
            _require(bool(self._desktop), "CreateDesktopW failed: " + str(self._creator_result))
            _require(self._creator_result.get("error") != 183, "Desktop unexpectedly already existed")
            _require(api.name(self._desktop) == self.desktop_name, "created Desktop identity mismatch")
            _require(api.name(api.u.GetThreadDesktop(api.k.GetCurrentThreadId())) == launcher_desktop,
                     "launcher thread Desktop changed during creation")
            self._witness("inputAfterCreate")
            return self
        except BaseException as original:
            errors = self._close()
            if errors and hasattr(original, "add_note"):
                original.add_note("Desktop cleanup: " + "; ".join(errors))
            if isinstance(original, DesktopSafetyError):
                original.receipt = self.receipt
            raise

    def _adopt_creator(self) -> None:
        if self._creator is not None and not self._creator.is_alive() and not self._creator_adopted:
            self._creator_adopted = True
            self._desktop = int(self._creator_result.get("handle", 0) or 0)
            self.receipt["creatorExited"] = True
            self.receipt["desktopCreated"] = bool(self._desktop)

    def _witness(self, field: str) -> None:
        assert self._api is not None
        station = self._api.name(self._api.u.GetProcessWindowStation())
        actual = self._api.input_name()
        self.receipt[field] = actual
        _require(station == self.window_station, "launcher window station changed")
        _require(actual == self.input_before, "input Desktop changed; no continuation allowed")

    def _settle(self, process: int, job: int, row: dict[str, Any], force: bool) -> bool:
        assert self._api is not None
        api = self._api
        if force:
            row["forcedTermination"] = True
            if not api.k.TerminateJobObject(job, 2):
                row.setdefault("cleanupErrors", []).append(f"TerminateJobObject:{ctypes.get_last_error()}")
        result = api.k.WaitForSingleObject(process, 5000 if force else 0)
        row["processSettled"] = result == 0
        deadline = time.monotonic() + (5.0 if force else 2.0)
        while True:
            count = api.active_processes(job)
            row["jobActiveProcesses"] = count
            if not count or time.monotonic() >= deadline:
                break
            time.sleep(0.02)
        row["jobEmpty"] = row["jobActiveProcesses"] == 0
        return row["processSettled"] and row["jobEmpty"]

    def run(self, argv: Sequence[str | os.PathLike[str]], *, cwd: str | os.PathLike[str],
            stdout_path: str | os.PathLike[str], stderr_path: str | os.PathLike[str],
            timeout_s: float = 30, env: Mapping[str, str] | None = None) -> dict[str, Any]:
        """Run one owned executable. Timeout/forced cleanup never yields ok=True.

        Output paths must not exist. Native failure raises DesktopSafetyError with
        this context's receipt; an ordinary nonzero exit/timeout returns ok=False.
        Neither context cleanup nor timeout cleanup masks the initial exception.
        """
        _require(bool(self._desktop) and not self._pending, "Desktop not open or prior process unresolved")
        args, directory, timeout = _launch_arguments(argv, cwd, timeout_s)
        out_path, err_path = Path(stdout_path).resolve(), Path(stderr_path).resolve()
        _require(out_path != err_path and not out_path.exists() and not err_path.exists(), "outputs must be distinct new files")
        _require(out_path.parent.is_dir() and err_path.parent.is_dir(), "output parents must exist")
        assert self._api is not None
        api = self._api
        row: dict[str, Any] = dict(schema=1, scope="NON_INPUT_DESKTOP_PROCESS_LAUNCH", ok=False,
            argv=args, desktop=self.desktop_name, desktopPath=self.desktop_path,
            inputBefore=self.input_before, stdout=str(out_path), stderr=str(err_path),
            timeoutSeconds=timeout, timedOut=False, forcedTermination=False,
            processSettled=False, jobEmpty=False, jobAtomic=False, handleListApplied=False,
            desktopHandleInherited=False, exitCode=None, cleanupErrors=[])
        self.receipt["launches"].append(row)
        job = process = thread = 0
        handles: list[int] = []
        attribute_storage = None
        attribute_list = None
        attribute_initialized = False
        original: BaseException | None = None
        reduced_token = None
        try:
            self._witness("inputBeforeLaunch")
            job = api.check(api.k.CreateJobObjectW(None, None), "CreateJobObjectW")
            limits = _ExtendedLimits()
            limits.BasicLimitInformation.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE
            api.check(api.k.SetInformationJobObject(job, 9, ctypes.byref(limits),
                       ctypes.sizeof(limits)), "SetInformationJobObject")
            security = _SecurityAttributes(ctypes.sizeof(_SecurityAttributes), None, True)
            invalid = ctypes.c_void_p(-1).value
            for path, access, disposition in ((str(out_path), 0x40000000, 1),
                                               (str(err_path), 0x40000000, 1),
                                               ("NUL", 0x80000000, 3)):
                handle = api.k.CreateFileW(path, access, 1, ctypes.byref(security), disposition, 0x80, None)
                _require(handle not in (None, 0, invalid), f"CreateFileW failed:{ctypes.get_last_error()}")
                handles.append(handle)
            size = ctypes.c_size_t()
            api.k.InitializeProcThreadAttributeList(None, 2, 0, ctypes.byref(size))
            _require(0 < size.value <= 65536, "attribute-list size invalid")
            attribute_storage = ctypes.create_string_buffer(size.value)
            attribute_list = ctypes.cast(attribute_storage, wt.LPVOID)
            api.check(api.k.InitializeProcThreadAttributeList(attribute_list, 2, 0,
                       ctypes.byref(size)), "InitializeProcThreadAttributeList")
            attribute_initialized = True
            jobs = (wt.HANDLE * 1)(job)
            inherited = (wt.HANDLE * 3)(*handles)
            api.check(api.k.UpdateProcThreadAttribute(attribute_list, 0, 0x0002000D,
                       jobs, ctypes.sizeof(jobs), None, None), "JOB_LIST")
            api.check(api.k.UpdateProcThreadAttribute(attribute_list, 0, 0x00020002,
                       inherited, ctypes.sizeof(inherited), None, None), "HANDLE_LIST")
            startup = _StartupInfoEx()
            startup.StartupInfo.cb = ctypes.sizeof(startup)
            startup.StartupInfo.lpDesktop = self.desktop_path
            startup.StartupInfo.dwFlags = 0x00000100  # USESTDHANDLES
            startup.StartupInfo.hStdOutput, startup.StartupInfo.hStdError = handles[:2]
            startup.StartupInfo.hStdInput = handles[2]
            startup.lpAttributeList = attribute_list
            info = _ProcessInfo()
            environment = None
            if env is not None:
                _require(all(isinstance(k, str) and isinstance(v, str) and k and
                         "=" not in k and "\0" not in k and "\0" not in v for k, v in env.items()),
                         "invalid environment")
                _require(len({k.casefold() for k in env}) == len(env), "duplicate environment names")
                environment = ctypes.create_unicode_buffer("\0".join(
                    f"{key}={env[key]}" for key in sorted(env, key=str.casefold)) + "\0\0")
            command = ctypes.create_unicode_buffer(subprocess.list2cmdline(args))
            flags = 0x08000000 | 0x00004000 | 0x00080000 | 0x00000400
            if self.restricted_medium:
                # No global token/UAC changes and no High-integrity fallback.
                # Both branches use this exact prebuilt JOB_LIST/HANDLE_LIST.
                from render_lab_token import RestrictedMediumToken, CREATE_SUSPENDED
                reduced_token = RestrictedMediumToken()
                row['restrictedToken'] = reduced_token.receipt
                reduced_token.__enter__()
                reduced_token.create_suspended(args[0], command, flags | CREATE_SUSPENDED,
                                               environment, str(directory), startup, info)
            else:
                api.check(api.k.CreateProcessW(args[0], command, None, None, True, flags,
                           environment, str(directory), ctypes.byref(startup), ctypes.byref(info)), "CreateProcessW")
            process, thread = info.hProcess, info.hThread
            row.update(pid=info.dwProcessId, initialThreadId=info.dwThreadId,
                       jobAtomic=True, handleListApplied=True)
            stamps = [wt.FILETIME() for _ in range(4)]
            api.check(api.k.GetProcessTimes(process, *(ctypes.byref(t) for t in stamps)), "GetProcessTimes")
            row["creation100ns"] = stamps[0].dwLowDateTime | (stamps[0].dwHighDateTime << 32)
            in_job = wt.BOOL()
            api.check(api.k.IsProcessInJob(process, job, ctypes.byref(in_job)), "IsProcessInJob")
            _require(in_job.value, "created process not in exact Job")
            buffer = ctypes.create_unicode_buffer(32768)
            chars = wt.DWORD(len(buffer))
            api.check(api.k.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(chars)), "QueryFullProcessImageNameW")
            row["actualExecutable"] = buffer.value
            _require(os.path.normcase(str(Path(buffer.value).resolve())) == os.path.normcase(args[0]),
                     "created process image mismatch")
            self._witness("inputAfterLaunch")
            if reduced_token is not None:
                reduced_token.verify_child(process)
                previous_suspend_count = api.k.ResumeThread(thread)
                row['restrictedToken']['resumePreviousCount'] = previous_suspend_count
                _require(previous_suspend_count == 1, "restricted child ResumeThread did not release exactly one suspension")
                row['restrictedToken']['resumedAfterVerification'] = True
                reduced_token.close()
            result = api.k.WaitForSingleObject(process, max(1, math.ceil(timeout * 1000)))
            row["timedOut"] = result == 258
            _require(result in (0, 258), "process wait failed")
            if not self._settle(process, job, row, row["timedOut"]):
                self._settle(process, job, row, True)
            if row["processSettled"]:
                code = wt.DWORD()
                api.check(api.k.GetExitCodeProcess(process, ctypes.byref(code)), "GetExitCodeProcess")
                row["exitCode"] = code.value
            self._witness("inputAfterRun")
            row["inputAfter"] = self.receipt["inputAfterRun"]
            row["ok"] = bool(row["exitCode"] == 0 and row["processSettled"] and row["jobEmpty"]
                             and not row["timedOut"] and not row["forcedTermination"] and not row["cleanupErrors"])
        except BaseException as exc:
            original = exc
            row["error"] = f"{type(exc).__name__}: {exc}"
            row["ok"] = False
        finally:
            if process and not (row["processSettled"] and row["jobEmpty"]):
                try:
                    self._settle(process, job, row, True)
                except BaseException as exc:
                    row["cleanupErrors"].append(f"settle:{exc}")
            if process and not (row["processSettled"] and row["jobEmpty"]):
                # Retain exact authority and Desktop on unresolved kernel work.
                self._pending.append((process, job, row))
                process = job = 0
            if attribute_initialized:
                api.k.DeleteProcThreadAttributeList(attribute_list)
            if reduced_token is not None:
                try:
                    reduced_token.close()
                except BaseException as exc:
                    row['cleanupErrors'].append(f"restricted-token-close:{exc}")
                if reduced_token.receipt['cleanupErrors']:
                    row['cleanupErrors'].extend(reduced_token.receipt['cleanupErrors'])
            for handle in (thread, process, job, *handles):
                if handle and not api.k.CloseHandle(handle):
                    row["cleanupErrors"].append(f"CloseHandle:{ctypes.get_last_error()}")
            if row["cleanupErrors"] or self._pending:
                row["ok"] = False
        if original is not None:
            if isinstance(original, DesktopSafetyError):
                original.receipt = self.receipt
            if row["cleanupErrors"] and hasattr(original, "add_note"):
                original.add_note("Process cleanup: " + "; ".join(row["cleanupErrors"]))
            raise original
        return row

    def _close(self) -> list[str]:
        errors: list[str] = []
        api = self._api
        if api is None:
            return errors
        self._adopt_creator()
        if self._creator is not None and self._creator.is_alive():
            # No unsafe thread termination or close of a Desktop still assigned
            # to its creator. Preserve ownership for caller recovery; never pass.
            errors.append("Desktop creator unresolved; any late handle retained")
            self.receipt["creatorExited"] = False
        remaining = []
        for process, job, row in self._pending:
            try:
                settled = self._settle(process, job, row, True)
            except BaseException as exc:
                settled = False
                errors.append(f"pending-settle:{exc}")
            if settled:
                for handle in (process, job):
                    if not api.k.CloseHandle(handle):
                        errors.append(f"CloseHandle(pending):{ctypes.get_last_error()}")
            else:
                remaining.append((process, job, row))
                errors.append("exact process/Job unresolved; Desktop handle retained")
        self._pending = remaining
        if self._desktop and not remaining and (self._creator is None or not self._creator.is_alive()):
            if api.u.CloseDesktop(self._desktop):
                self._desktop = 0
                self.receipt["desktopClosed"] = True
            else:
                errors.append(f"CloseDesktop:{ctypes.get_last_error()}")
        if self.input_before:
            try:
                self._witness("inputAfterClose")
            except BaseException as exc:
                errors.append(str(exc))
        self.receipt["cleanupErrors"].extend(errors)
        self.receipt["ok"] = bool(self.receipt["desktopCreated"] and self.receipt["desktopClosed"]
            and self.receipt["launches"] and all(row["ok"] for row in self.receipt["launches"])
            and not self.receipt["cleanupErrors"])
        return errors

    def __exit__(self, kind: Any, original: BaseException | None, traceback: Any) -> bool:
        errors = self._close()
        if original is not None:
            self.receipt["ok"] = False
            self.receipt["bodyError"] = f"{type(original).__name__}: {original}"
        if errors:
            if original is not None:
                if hasattr(original, "add_note"):
                    original.add_note("Desktop cleanup: " + "; ".join(errors))
            else:
                raise DesktopSafetyError("Desktop cleanup failed: " + "; ".join(errors), self.receipt)
        return False


def validate_probe_receipt(row: Mapping[str, Any], *, desktop: str, input_desktop: str,
                           station: str, bits: int, child: Mapping[str, Any] | None = None,
                           launch: Mapping[str, Any] | None = None) -> None:
    """Pure strict reader. This does not launch processes or query Windows."""
    _require(type(row) is dict and row.get("schema") == 1 and type(row.get("schema")) is int,
             "invalid probe schema")
    _require(set(row) == {"schema", "scope", "bits", "ok", "pid", "creation100ns", "main", "pump",
             "childRequested", "childDesktopExplicit", "childPid", "childCreation", "childExit",
             "gpuModulesAbsent", "gameTouched", "globalInputUsed", "desktopSwitchUsed", "normalTeardown"},
             "probe field set")
    _require(row.get("scope") == "NON_INPUT_DESKTOP_PROBE" and row.get("ok") is True,
             "probe did not pass")
    _require(type(row.get("bits")) is int and row["bits"] == bits and bits in (32, 64), "probe bitness")
    for key in ("pid", "creation100ns"):
        _require(type(row.get(key)) is int and row[key] > 0, f"invalid {key}")
    _require(row["pid"] <= 0xFFFFFFFF and row["creation100ns"] <= 0xFFFFFFFFFFFFFFFF, "process identity range")
    if launch is not None:
        _require(type(launch) is dict and launch.get("ok") is True
                 and launch.get("processSettled") is True and launch.get("jobEmpty") is True
                 and launch.get("jobAtomic") is True and launch.get("handleListApplied") is True
                 and launch.get("timedOut") is False and launch.get("forcedTermination") is False,
                 "launch is not a normally settled atomic Job")
        _require(type(launch.get("pid")) is int and type(launch.get("creation100ns")) is int
                 and launch["pid"] == row["pid"] and launch["creation100ns"] == row["creation100ns"],
                 "probe does not match original process HANDLE identity")
        _require(launch.get("desktop") == desktop and launch.get("inputBefore") == input_desktop
                 and launch.get("inputAfter") == input_desktop, "launch Desktop witness mismatch")
    _require(desktop.casefold() != input_desktop.casefold(), "requested Desktop is input Desktop")
    for key in ("gpuModulesAbsent", "normalTeardown"):
        _require(row.get(key) is True, f"missing {key}")
    for key in ("gameTouched", "globalInputUsed", "desktopSwitchUsed"):
        _require(row.get(key) is False, f"unexpected {key}")
    for role in ("main", "pump"):
        sample = row.get(role)
        _require(type(sample) is dict, f"missing {role}")
        fields = {"threadId", "station", "desktop", "inputBefore", "inputAfter"}
        if role == "pump":
            fields |= {"windowCreated", "windowHidden", "windowDestroyed", "classUnregistered",
                       "width", "height", "pings", "messagesDispatched"}
        _require(set(sample) == fields, f"{role} field set")
        _require(type(sample.get("threadId")) is int and 0 < sample["threadId"] <= 0xFFFFFFFF, "thread identity")
        _require(sample.get("station") == station and sample.get("desktop") == desktop, f"{role} actual Desktop mismatch")
        _require(sample.get("inputBefore") == input_desktop and sample.get("inputAfter") == input_desktop,
                 f"{role} input Desktop changed")
    _require(row["main"]["threadId"] != row["pump"]["threadId"], "pump is not an independent thread")
    pump = row["pump"]
    for key in ("windowCreated", "windowHidden", "windowDestroyed", "classUnregistered"):
        _require(pump.get(key) is True, f"pump {key}")
    for key, expected in (("width", 64), ("height", 64), ("pings", 1)):
        _require(type(pump.get(key)) is int and pump[key] == expected, f"pump {key}")
    _require(type(pump.get("messagesDispatched")) is int and pump["messagesDispatched"] >= 1, "no pump dispatch")
    _require(type(row.get("childRequested")) is bool, "child request type")
    if child is None:
        _require(row["childRequested"] is False and row.get("childDesktopExplicit") is False, "child witness missing")
        for key in ("childPid", "childCreation", "childExit"):
            _require(type(row.get(key)) is int and row[key] == 0, f"unexpected {key}")
    else:
        _require(row["childRequested"] is True and row.get("childDesktopExplicit") is True, "child Desktop not explicit")
        validate_probe_receipt(child, desktop=desktop, input_desktop=input_desktop, station=station, bits=64)
        _require(type(row.get("childExit")) is int and row["childExit"] == 0, "child exit")
        _require(type(row.get("childPid")) is int and type(row.get("childCreation")) is int
                 and row["childPid"] == child["pid"] and row["childCreation"] == child["creation100ns"],
                 "child process identity mismatch")
        _require(row["pid"] != child["pid"], "child is parent")
