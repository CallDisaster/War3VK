"""Narrow, opt-in normal-exit gate for an already observed sandbox game.

Importing this module does nothing. The caller must own the launch/Desktop and
keep its NativeObserver bound until this function returns. No launch, pipe
implementation, DBWIN listener, input injection, Desktop switching, or forced
termination is provided here. The caller must separately opt its existing
NativeObserver.request into exactly EXIT_COMMAND + EXIT_PAYLOAD, and launch
with DXVK_WAR3_INTERNAL_EXIT_TEST=1 and DXVK_WAR3_INTERNAL_TEST_API=1.

Passing means: ready/capture evidence -> stock EndGame -> observed map teardown
-> exact owned HWND WM_CLOSE -> exit code zero on the original retained HANDLE.
It does not certify GPU completion, visual correctness, or a release. Failure
never requests fallback termination; a launcher's later Job cleanup must remain
separate and must not be relabelled a successful normal exit.
"""
from __future__ import annotations

import copy
import ctypes
from ctypes import wintypes as wt
import math
import os
from pathlib import Path
import re
import threading
import time

from autotest_sessions import DEFAULT_SANDBOX_ROOT, DEFAULT_INSTANCE_POOL_NAME


EXIT_COMMAND = "invoke_test_command"
EXIT_PAYLOAD = {"command": "game.end_for_exit_test", "payload": {}, "timeoutMs": 3000}
_UNSETTLED_WORKERS = []


def _require(value, message):
    if not value:
        raise ValueError(message)


def _qualification(native, launch, observation, instance, desktop):
    # Lazy import avoids an import cycle when the survey opts into this helper.
    # These are the same identity/path/capture checks used by the real observer.
    from run_native_d3d9_survey import checked_path, identity, bmp_dimensions, launch_identity
    root = checked_path(DEFAULT_SANDBOX_ROOT, exists=True)
    instance = checked_path(instance, exists=True)
    pool = root / DEFAULT_INSTANCE_POOL_NAME
    _require(instance.is_relative_to(pool), "exit target is outside sandbox instance pool")
    parts = instance.relative_to(pool).parts
    _require(len(parts) == 3 and parts[-1] == "root" and all(
        re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", item) for item in parts[:2]),
        "exit target is not a unique run/session/root instance")
    executable = checked_path(instance / "war3.exe", exists=True)
    _require(executable.is_file() and executable.stat().st_nlink == 1,
             "sandbox executable is missing or has a hardlink alias")
    expected = launch_identity(launch, executable)
    _require(launch.get("timedOut", False) is False and
             launch.get("forcedTermination", False) is False and
             launch.get("processSettled", False) is False,
             "launch already timed out, forced, or settled")
    _require(native.process and native.expected == expected and not native.pending,
             "exact bound NativeObserver without pending IPC required")
    _require(observation.get("processIdentity") == expected and
             observation.get("ready") is True and observation.get("modulesVerified") is True and
             observation.get("screenshotVerified") is True and
             not observation.get("ipcDisabled") and not observation.get("cleanupErrors"),
             "ready/modules/screenshot evidence for this exact process required")
    screenshot = observation.get("screenshot", {})
    _require(type(screenshot) is dict and bool(screenshot.get("path")), "capture identity missing")
    picture = checked_path(screenshot["path"], exists=True)
    _require(identity(picture) == screenshot and
             checked_path(observation.get("capture", {}).get("outputPath", "")) == picture and
             bmp_dimensions(picture) == observation.get("actualResolution"),
             "verified capture changed or belongs to a different output")
    row = desktop.receipt
    name, input_name = expected["desktop"], launch.get("inputBefore", "")
    _require(type(name) is str and re.fullmatch(r"[A-Za-z0-9_]{1,120}", name) and
             name.casefold() != "default" and type(input_name) is str and input_name and
             name.casefold() != input_name.casefold(), "target must be a non-Default non-input Desktop")
    _require(getattr(desktop, "_entered", False) and getattr(desktop, "_desktop", 0) and
             desktop.desktop_name == name and desktop.input_before == input_name and
             row.get("desktop") == name and row.get("inputBefore") == input_name and
             row.get("windowStation", "").casefold() == "winsta0" and
             row.get("desktopCreated") is True and row.get("desktopClosed") is False and
             row.get("restrictedMediumRequested") is True and
             row.get("globalInputUsed") is False and row.get("desktopSwitchRequested") is False and
             not row.get("cleanupErrors"), "owned active isolated Desktop receipt required")
    launches = row.get("launches", [])
    same_exe = [item for item in launches if item.get("actualExecutable") and
                checked_path(item["actualExecutable"]) == executable]
    _require(len(same_exe) == 1 and same_exe[0] == launch,
             "sandbox executable must have exactly one owned launch")
    native.verify_process()
    return copy.deepcopy(expected), input_name


def _validated_request(native, process, expected, command, payload, timeout, nonces, receipt):
    from run_native_d3d9_survey import validate_reply
    _require(native.process == process and native.expected == expected and not native.pending,
             "process authority changed or IPC pending")
    row = native.request(command, copy.deepcopy(payload), timeout=timeout)
    receipt["ipc"].append(row)
    _require(type(row) is dict and type(row.get("request")) is dict, "request receipt missing")
    request = row["request"]
    nonce = request.get("requestId")
    _require(type(nonce) is str and nonce and nonce not in nonces and
             request.get("command") == command and request.get("payload") == payload and
             type(request.get("pid")) is int and request["pid"] == expected["pid"],
             "request receipt does not identify this exact operation")
    nonces.add(nonce)
    reply = validate_reply(row.get("response"), request, row.get("pipeServerPid"))
    _require(reply["ok"] is True, "normal exit IPC operation refused")
    _require(native.process == process and native.expected == expected and not native.pending,
             "authority changed after IPC")
    return reply["result"]


def _status_fields(status):
    _require(type(status) is dict and status.get("source") == "control-plane" and
             type(status.get("timestampMs")) is int and status["timestampMs"] >= 0,
             "fresh control-plane runtime status required")
    runtime, render = status.get("runtime", {}), status.get("render", {})
    values = [runtime.get("gameStarted"), render.get("isInGame"),
              render.get("isLoading"), render.get("inGameRenderReady")]
    _require(all(type(value) is bool for value in values), "map-state fields must be explicit booleans")
    return values


def _close_worker(api, process, expected, input_name, cancelled, result):
    """Only this short-lived worker binds to the target Desktop, never input.

    Cancellation stops a worker which has not passed its final check; it cannot
    revoke an already checked/in-flight PostMessage. HWND ownership is checked
    immediately before posting, not claimed to be an atomic Win32 transaction.
    Any unresolved worker prevents acceptance and retains its own process copy.
    """
    handle, previous, bound = 0, 0, False
    try:
        api.verify_handle(process, expected)
        api.guard_desktop(expected["desktop"], input_name)
        previous = api.thread_desktop()
        handle = api.open_desktop(expected["desktop"])
        _require(api.name(handle) == expected["desktop"], "opened Desktop identity mismatch")
        api.bind_desktop(handle)
        bound = True
        windows = api.owned_windows(handle, expected["pid"])
        _require(len(windows) == 1, "unique owned Warcraft III window required")
        hwnd, thread = windows[0]
        api.verify_handle(process, expected)
        api.guard_desktop(expected["desktop"], input_name)
        _require(api.window_owner(hwnd) == (expected["pid"], thread) and
                 api.window_title(hwnd) == "Warcraft III" and
                 api.owned_windows(handle, expected["pid"]) == [(hwnd, thread)],
                 "game HWND owner/Desktop membership changed")
        # EnumDesktopWindows on this verified, explicitly opened Desktop is
        # the membership witness. GetThreadDesktop(remoteTid) is not needed:
        # Windows can deny it across the restricted child token even though
        # this exact HWND was enumerated here. Re-enumerate immediately before
        # posting, and keep the retained process + PID/TID/title checks above.
        _require(not cancelled.is_set(), "close worker cancelled before posting")
        api.post_close(hwnd)
        result.update(posted=True, hwnd=int(hwnd), windowThreadId=int(thread))
        api.guard_desktop(expected["desktop"], input_name)
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        try:
            if bound:
                api.bind_desktop(previous)
            result["workerDesktopRestored"] = True
            if handle:
                api.close_desktop(handle)
            result["desktopHandleClosed"] = True
        except Exception as error:
            # A still-bound Desktop handle is deliberately retained, not closed.
            result.setdefault("cleanupErrors", []).append(f"{type(error).__name__}: {error}")
        try:
            api.close_process_copy(process)
        except Exception as error:
            result.setdefault("cleanupErrors", []).append(f"{type(error).__name__}: {error}")


class _WindowsExitApi:
    def __init__(self, native):
        _require(os.name == "nt", "normal-exit Desktop gate is Windows-only")
        self.k, self.u = native.k, ctypes.WinDLL("user32", use_last_error=True)
        self.callback_type = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        signatures = (
            (self.k, "GetCurrentProcess", wt.HANDLE, ()),
            (self.k, "GetCurrentThreadId", wt.DWORD, ()),
            (self.k, "GetProcessId", wt.DWORD, (wt.HANDLE,)),
            (self.k, "DuplicateHandle", wt.BOOL, (wt.HANDLE, wt.HANDLE, wt.HANDLE,
                ctypes.POINTER(wt.HANDLE), wt.DWORD, wt.BOOL, wt.DWORD)),
            (self.k, "GetExitCodeProcess", wt.BOOL, (wt.HANDLE, ctypes.POINTER(wt.DWORD))),
            (self.u, "GetProcessWindowStation", wt.HANDLE, ()),
            (self.u, "OpenInputDesktop", wt.HANDLE, (wt.DWORD, wt.BOOL, wt.DWORD)),
            (self.u, "OpenDesktopW", wt.HANDLE, (wt.LPCWSTR, wt.DWORD, wt.BOOL, wt.DWORD)),
            (self.u, "GetThreadDesktop", wt.HANDLE, (wt.DWORD,)),
            (self.u, "SetThreadDesktop", wt.BOOL, (wt.HANDLE,)),
            (self.u, "CloseDesktop", wt.BOOL, (wt.HANDLE,)),
            (self.u, "GetUserObjectInformationW", wt.BOOL,
                (wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD))),
            (self.u, "GetWindowThreadProcessId", wt.DWORD, (wt.HWND, ctypes.POINTER(wt.DWORD))),
            (self.u, "GetWindowTextW", ctypes.c_int, (wt.HWND, wt.LPWSTR, ctypes.c_int)),
            (self.u, "EnumDesktopWindows", wt.BOOL, (wt.HANDLE, self.callback_type, wt.LPARAM)),
            (self.u, "PostMessageW", wt.BOOL, (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)),
        )
        for library, name, result, arguments in signatures:
            function = getattr(library, name)
            function.restype, function.argtypes = result, arguments

    @staticmethod
    def check(value, operation):
        _require(value, operation + ": Win32=" + str(ctypes.get_last_error()))
        return value

    def name(self, handle):
        self.check(handle, "user object handle")
        size = wt.DWORD()
        self.u.GetUserObjectInformationW(handle, 2, None, 0, ctypes.byref(size))
        _require(0 < size.value <= 4096, "user object name size")
        value = ctypes.create_unicode_buffer((size.value + 1) // 2)
        self.check(self.u.GetUserObjectInformationW(handle, 2, value,
                   ctypes.sizeof(value), ctypes.byref(size)), "user object name")
        return value.value

    def guard_desktop(self, target, input_name):
        _require(target.casefold() not in ("default", input_name.casefold()), "interactive Desktop refused")
        _require(self.name(self.u.GetProcessWindowStation()).casefold() == "winsta0", "window station changed")
        handle = self.check(self.u.OpenInputDesktop(0, False, 1), "open input witness")
        try:
            _require(self.name(handle) == input_name, "input Desktop changed")
        finally:
            self.close_desktop(handle)

    def verify_handle(self, handle, expected):
        _require(self.k.WaitForSingleObject(handle, 0) == 258, "owned process already exited")
        _require(self.k.GetProcessId(handle) == expected["pid"], "retained process PID mismatch")
        stamps = [wt.FILETIME() for _ in range(4)]
        self.check(self.k.GetProcessTimes(handle, *(ctypes.byref(item) for item in stamps)), "process times")
        value = ctypes.create_unicode_buffer(32768)
        size = wt.DWORD(len(value))
        self.check(self.k.QueryFullProcessImageNameW(handle, 0, value, ctypes.byref(size)), "process image")
        _require((stamps[0].dwLowDateTime | stamps[0].dwHighDateTime << 32) == expected["creation100ns"] and
                 os.path.normcase(value.value) == os.path.normcase(expected["actualExecutable"]),
                 "copied retained process identity mismatch")

    def thread_desktop(self, thread=None):
        return self.check(self.u.GetThreadDesktop(thread or self.k.GetCurrentThreadId()), "thread Desktop")

    def open_desktop(self, name):
        return self.check(self.u.OpenDesktopW(name, 0, False, 0x1 | 0x40 | 0x80), "open owned Desktop")

    def bind_desktop(self, handle):
        self.check(self.u.SetThreadDesktop(handle), "bind worker Desktop")

    def close_desktop(self, handle):
        self.check(self.u.CloseDesktop(handle), "close Desktop")

    def window_owner(self, hwnd):
        pid = wt.DWORD()
        thread = self.check(self.u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid)), "window owner")
        return pid.value, thread

    def window_title(self, hwnd):
        value = ctypes.create_unicode_buffer(256)
        self.u.GetWindowTextW(hwnd, value, len(value))
        return value.value

    def owned_windows(self, desktop, pid):
        rows, errors = [], []

        def visit(hwnd, unused):
            try:
                owner, thread = self.window_owner(hwnd)
                if owner == pid and self.window_title(hwnd) == "Warcraft III":
                    rows.append((hwnd, thread))
                return True
            except Exception as error:
                errors.append(str(error))
                return False

        callback = self.callback_type(visit)
        self.check(self.u.EnumDesktopWindows(desktop, callback, 0), "enumerate owned Desktop")
        _require(not errors, "window enumeration failed: " + "; ".join(errors))
        return rows

    def post_close(self, hwnd):
        self.check(self.u.PostMessageW(hwnd, 0x0010, 0, 0), "owned WM_CLOSE")

    def close_process_copy(self, handle):
        self.check(self.k.CloseHandle(handle), "close copied process authority")

    def close_window(self, native, expected, input_name):
        # A duplicate refers to the same kernel process, never a PID reopen. If
        # a Win32 call stalls, the worker owns this copy independently of the
        # caller's HANDLE; late cleanup cannot touch a reused HANDLE number.
        parent, copied = self.k.GetCurrentProcess(), wt.HANDLE()
        self.check(self.k.DuplicateHandle(parent, native.process, parent, ctypes.byref(copied),
                   0, False, 2), "duplicate retained read-only process authority")
        result = dict(posted=False, workerDesktopRestored=False, desktopHandleClosed=False,
                      workerPending=False, lateCloseMayBeInFlight=False, cleanupErrors=[])
        cancelled = threading.Event()
        thread = threading.Thread(target=_close_worker,
            args=(self, copied.value, expected, input_name, cancelled, result),
            name="WarVK-Exact-Normal-Exit", daemon=True)
        try:
            thread.start()
        except Exception:
            self.close_process_copy(copied.value)
            raise
        thread.join(3.0)
        if thread.is_alive():
            cancelled.set()
            result["workerPending"] = True
            result["lateCloseMayBeInFlight"] = True
            _UNSETTLED_WORKERS.append((thread, cancelled, result, self))
        return copy.deepcopy(result)

    def wait_exit(self, handle, seconds):
        result = self.k.WaitForSingleObject(handle, math.ceil(seconds * 1000))
        _require(result in (0, 258), "retained HANDLE wait failed")
        if result == 258:
            return {"settled": False, "timedOut": True, "exitCode": None}
        code = wt.DWORD()
        self.check(self.k.GetExitCodeProcess(handle, ctypes.byref(code)), "exit code on retained HANDLE")
        return {"settled": True, "timedOut": False, "exitCode": code.value}


def accept_normal_exit(native, *, launch, observation, instance, desktop,
                       wait_seconds=10.0, _api=None, _clock=time.monotonic, _sleep=time.sleep):
    """Return a fail-visible receipt. Call before native.close(), exactly once.

    `_api`, `_clock`, `_sleep` are fake-test seams, not command-line options.
    Bounds: 4s EndGame IPC, <=10s map-state polling (2s/request), 3s worker join,
    then <=30s retained HANDLE wait. No wait or error authorizes a forced exit.
    """
    result = dict(schema="warvk-native-normal-exit/v1", ok=False, normalExitProven=False,
        forcedTermination=False, globalInputUsed=False, desktopSwitchRequested=False,
        endGameAccepted=False, mapTeardownObserved=False, phase="qualification", ipc=[])
    try:
        _require(type(wait_seconds) in (int, float) and math.isfinite(wait_seconds) and
                 0 < wait_seconds <= 30, "exit wait bound must be (0,30] seconds")
        expected, input_name = _qualification(native, launch, observation, instance, desktop)
        _require(not getattr(native, "_normal_exit_test_attempted", False),
                 "normal-exit attempt already consumed for this bound observer")
        native._normal_exit_test_attempted = True
        process = native.process
        result["processIdentity"] = expected
        api = _api if _api is not None else _WindowsExitApi(native)
        api.guard_desktop(expected["desktop"], input_name)
        nonces = set()
        result["phase"] = "before-endgame"
        before = _validated_request(native, process, expected, "get_runtime_status", {}, 2.0, nonces, result)
        _require(_status_fields(before) == [True, True, False, True], "expected observed in-game state")
        result["phase"] = "stock-endgame"
        api.guard_desktop(expected["desktop"], input_name)
        ended = _validated_request(native, process, expected, EXIT_COMMAND, EXIT_PAYLOAD, 4.0, nonces, result)
        _require(ended.get("handled") is True and ended.get("command") == "game.end_for_exit_test" and
                 ended.get("stockEndGameInvoked") is True and ended.get("processExitProven") is False,
                 "stock EndGame invocation was not confirmed")
        result["endGameAccepted"] = True
        result["phase"] = "map-teardown"
        deadline, previous, consecutive = _clock() + 10.0, before["timestampMs"], 0
        for _ in range(100):
            remaining = deadline - _clock()
            _require(remaining > 0, "map teardown observation deadline")
            status = _validated_request(native, process, expected, "get_runtime_status", {},
                min(2.0, remaining), nonces, result)
            state = _status_fields(status)
            _require(status["timestampMs"] >= previous, "runtime status timestamp regressed")
            fresh = status["timestampMs"] > previous
            consecutive = consecutive + 1 if fresh and state == [False, False, False, False] else 0
            previous = status["timestampMs"]
            if consecutive == 2:
                result["mapTeardownObserved"] = True
                break
            _sleep(min(0.1, max(0.0, deadline - _clock())))
        _require(result["mapTeardownObserved"], "two fresh map-teardown observations missing")
        result["phase"] = "owned-window-close"
        _require(native.process == process and native.expected == expected and not native.pending,
                 "retained process authority changed before WM_CLOSE")
        native.verify_process()
        api.guard_desktop(expected["desktop"], input_name)
        result["closeWindow"] = close = api.close_window(native, expected, input_name)
        _require(close.get("posted") is True and close.get("workerDesktopRestored") is True and
                 close.get("desktopHandleClosed") is True and close.get("workerPending") is False and
                 not close.get("error") and not close.get("cleanupErrors"), "owned close worker did not complete safely")
        result["phase"] = "retained-handle-exit"
        _require(native.process == process and native.expected == expected and not native.pending,
                 "retained process authority changed before exit wait")
        result["exit"] = exit_row = api.wait_exit(process, wait_seconds)
        api.guard_desktop(expected["desktop"], input_name)
        _require(exit_row.get("settled") is True and exit_row.get("timedOut") is False and
                 type(exit_row.get("exitCode")) is int and exit_row["exitCode"] == 0,
                 "zero-code normal process exit not proven")
        result.update(ok=True, normalExitProven=True, phase="complete")
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    return result
