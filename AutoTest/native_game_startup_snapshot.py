"""One bounded pre-ready window diagnostic on an owned isolated game.

No IPC/ready prerequisite, launch, PID reopen, global input, Desktop switching,
screen-copy fallback, or forced exit. PrintWindow success is a captured window,
not evidence that a map loaded, Vulkan rendered, or nonblack pixels are correct.
The synchronous PrintWindow call runs on a disposable worker. Timeout is failure;
an in-flight native call cannot be cancelled, so the worker retains its duplicate
process handle and own resources. It can never publish a final output afterwards.
"""
from __future__ import annotations

import copy
import ctypes
from ctypes import wintypes as wt
import os
from pathlib import Path
import re
import struct
import tempfile
import threading

from autotest_sessions import DEFAULT_SANDBOX_ROOT, DEFAULT_INSTANCE_POOL_NAME
from native_game_exit_acceptance import _WindowsExitApi


ARTIFACT_ROOT = Path(__file__).resolve().parent / "artifacts"
MAX_WINDOWS = 32
MAX_ENUMERATED_WINDOWS = 128
MAX_PIXEL_BYTES = 15 * 1024 * 1024
COPY_CHUNK = 64 * 1024
WORKER_TIMEOUT_SECONDS = 3.0
_UNSETTLED_WORKERS = []


def _require(value, message):
    if not value:
        raise ValueError(message)


def _layout(width, height):
    _require(type(width) is int and type(height) is int and 0 < width <= 8192 and
             0 < height <= 8192, "positive bounded integer dimensions required")
    stride = ((width * 3 + 3) // 4) * 4
    pixels = stride * height
    _require(pixels <= MAX_PIXEL_BYTES, "window exceeds fixed pixel staging budget")
    return stride, pixels


def _bitmap_header(width, height):
    _, pixels = _layout(width, height)
    return (struct.pack("<2sIHHI", b"BM", 54 + pixels, 0, 0, 54) +
            struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, pixels, 0, 0, 0, 0))


def _qualify(native, launch, instance, desktop, output):
    from run_native_d3d9_survey import checked_path, launch_identity
    root = checked_path(DEFAULT_SANDBOX_ROOT, exists=True)
    instance = checked_path(instance, exists=True)
    pool = root / DEFAULT_INSTANCE_POOL_NAME
    _require(instance.is_relative_to(pool), "target is outside sandbox instance pool")
    parts = instance.relative_to(pool).parts
    _require(len(parts) == 3 and parts[-1] == "root" and all(
        re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", item) for item in parts[:2]),
        "unique run/session/root required")
    executable = checked_path(instance / "war3.exe", exists=True)
    _require(executable.is_file() and executable.stat().st_nlink == 1,
             "unique non-hardlinked sandbox executable required")
    expected = launch_identity(launch, executable)
    _require(native.process and native.expected == expected and not native.pending,
             "bound exact retained process authority required")
    _require(all(launch.get(key, False) is False for key in
        ("timedOut", "forcedTermination", "processSettled")), "launch already stopped or timed out")
    name, input_name = expected["desktop"], launch.get("inputBefore", "")
    _require(type(name) is str and re.fullmatch(r"[A-Za-z0-9_]{1,120}", name) and
        name.casefold() != "default" and type(input_name) is str and input_name and
        name.casefold() != input_name.casefold(), "non-Default non-input Desktop required")
    row = desktop.receipt
    _require(getattr(desktop, "_entered", False) and getattr(desktop, "_desktop", 0) and
        desktop.desktop_name == name and desktop.input_before == input_name and
        row.get("desktop") == name and row.get("inputBefore") == input_name and
        row.get("windowStation", "").casefold() == "winsta0" and
        row.get("desktopCreated") is True and row.get("desktopClosed") is False and
        row.get("restrictedMediumRequested") is True and row.get("globalInputUsed") is False and
        row.get("desktopSwitchRequested") is False and not row.get("cleanupErrors"),
        "active owned isolated Desktop receipt required")
    launches = [item for item in row.get("launches", []) if item.get("actualExecutable") and
                checked_path(item["actualExecutable"]) == executable]
    _require(len(launches) == 1 and launches[0] == launch, "unique owned launch required")
    output = checked_path(output)
    artifact_root = checked_path(ARTIFACT_ROOT, exists=True)
    _require(output != artifact_root and output.is_relative_to(artifact_root) and
        output.suffix.casefold() == ".bmp" and not output.exists() and
        output.parent.is_dir(), "fresh BMP inside existing artifact directory required")
    process = native.process
    native.verify_process()
    _require(native.process == process and native.expected == expected and not native.pending,
             "retained authority changed during qualification")
    return process, copy.deepcopy(expected), input_name, output


def _validate_window(row, expected):
    _require(type(row) is dict and type(row.get("hwnd")) is int and row["hwnd"] > 0 and
        type(row.get("thread")) is int and row["thread"] > 0 and
        type(row.get("pid")) is int and row["pid"] == expected["pid"] and
        type(row.get("visible")) is bool and type(row.get("title")) is str and
        len(row["title"]) <= 255 and row.get("desktop") == expected["desktop"],
        "window is not an exact owned Desktop observation")
    rect = row.get("rect")
    _require(type(rect) is list and len(rect) == 4 and all(type(x) is int for x in rect),
             "window rectangle malformed")
    return row


def _select_window(rows, expected):
    _require(type(rows) is list and 0 < len(rows) <= MAX_WINDOWS, "owned window count outside bound")
    for row in rows:
        _validate_window(row, expected)
    _require(len({row["hwnd"] for row in rows}) == len(rows), "duplicate window handle")
    visible = [row for row in rows if row["visible"]]
    primary = [row for row in visible if row["title"] == "Warcraft III"]
    choices = primary if primary else visible
    _require(len(choices) == 1, "startup capture requires one unambiguous visible owned window")
    chosen = choices[0]
    left, top, right, bottom = chosen["rect"]
    _layout(right - left, bottom - top)
    return copy.deepcopy(chosen)


def _new_state():
    return dict(lock=threading.Lock(), cancel=threading.Event(), spool=None, windowObservation=None,
        result=dict(workerDesktopRestored=False, desktopHandleClosed=False,
                    processCopyClosed=False, cleanupErrors=[], windows=[]))


def _snapshot_worker(api, process, expected, input_name, spool_dir, state):
    handle, previous, bound, spool = 0, 0, False, None
    result = state["result"]
    try:
        _require(not state["cancel"].is_set(), "startup worker already cancelled")
        api.verify_handle(process, expected)
        api.guard_desktop(expected["desktop"], input_name)
        previous = api.thread_desktop()
        handle = api.open_desktop(expected["desktop"])
        _require(api.name(handle) == expected["desktop"], "opened Desktop mismatch")
        api.bind_desktop(handle)
        bound = True
        result["windows"] = rows = api.startup_windows(handle, expected)
        selected = _select_window(rows, expected)
        result["selectedWindow"] = selected
        with state["lock"]:
            state["windowObservation"] = copy.deepcopy(dict(windows=rows, selectedWindow=selected))
        api.verify_handle(process, expected)
        api.guard_desktop(expected["desktop"], input_name)
        _require(api.window_details(selected["hwnd"]) == selected, "window changed before PrintWindow")
        _require(not state["cancel"].is_set(), "startup worker cancelled before PrintWindow")
        width = selected["rect"][2] - selected["rect"][0]
        height = selected["rect"][3] - selected["rect"][1]
        # Anonymous staging is never the final output. Only a fully joined
        # successful worker may hand it to the caller for publication.
        spool = tempfile.TemporaryFile(mode="w+b", dir=spool_dir)
        api.print_window_to(selected["hwnd"], width, height, spool, state["cancel"])
        _require(not state["cancel"].is_set(), "startup worker cancelled after PrintWindow")
        api.verify_handle(process, expected)
        api.guard_desktop(expected["desktop"], input_name)
        _require(api.window_details(selected["hwnd"]) == selected, "window changed during PrintWindow")
        _, pixels = _layout(width, height)
        spool.flush()
        _require(spool.tell() == 54 + pixels, "PrintWindow bitmap length mismatch")
        spool.seek(0)
        _require(spool.read(54) == _bitmap_header(width, height), "PrintWindow bitmap header mismatch")
        spool.seek(0)
        result.update(printWindowSucceeded=True, dimensions=[width, height], pixelBytes=pixels)
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
            result["cleanupErrors"].append(f"{type(error).__name__}: {error}")
        try:
            api.close_process_copy(process)
            result["processCopyClosed"] = True
        except Exception as error:
            result["cleanupErrors"].append(f"{type(error).__name__}: {error}")
        with state["lock"]:
            if (spool is not None and not state["cancel"].is_set() and
                    not result.get("error") and not result["cleanupErrors"]):
                state["spool"], spool = spool, None
        if spool is not None:
            spool.close()


class _BitmapInfoHeader(ctypes.Structure):
    _fields_ = [("size", wt.DWORD), ("width", wt.LONG), ("height", wt.LONG),
        ("planes", wt.WORD), ("bits", wt.WORD), ("compression", wt.DWORD),
        ("imageSize", wt.DWORD), ("xppm", wt.LONG), ("yppm", wt.LONG),
        ("used", wt.DWORD), ("important", wt.DWORD)]


class _WindowsSnapshotApi(_WindowsExitApi):
    def __init__(self, native):
        super().__init__(native)
        _require(ctypes.sizeof(_BitmapInfoHeader) == 40, "Win32 bitmap header ABI mismatch")
        self.g = ctypes.WinDLL("gdi32", use_last_error=True)
        for library, name, result, arguments in (
            (self.u, "GetWindowRect", wt.BOOL, (wt.HWND, ctypes.POINTER(wt.RECT))),
            (self.u, "IsWindowVisible", wt.BOOL, (wt.HWND,)),
            (self.u, "PrintWindow", wt.BOOL, (wt.HWND, wt.HDC, wt.UINT)),
            (self.g, "CreateCompatibleDC", wt.HDC, (wt.HDC,)),
            (self.g, "DeleteDC", wt.BOOL, (wt.HDC,)),
            (self.g, "CreateDIBSection", wt.HBITMAP, (wt.HDC, ctypes.POINTER(_BitmapInfoHeader),
                wt.UINT, ctypes.POINTER(wt.LPVOID), wt.HANDLE, wt.DWORD)),
            (self.g, "SelectObject", wt.HANDLE, (wt.HDC, wt.HANDLE)),
            (self.g, "DeleteObject", wt.BOOL, (wt.HANDLE,)),
            (self.g, "GdiFlush", wt.BOOL, ()),
        ):
            function = getattr(library, name)
            function.restype, function.argtypes = result, arguments

    def duplicate_process(self, process):
        parent, copied = self.k.GetCurrentProcess(), wt.HANDLE()
        self.check(self.k.DuplicateHandle(parent, process, parent, ctypes.byref(copied),
                   0, False, 2), "duplicate retained startup authority")
        return copied.value

    def window_details(self, hwnd):
        pid, thread = self.window_owner(hwnd)
        rect = wt.RECT()
        self.check(self.u.GetWindowRect(hwnd, ctypes.byref(rect)), "startup window rectangle")
        return dict(hwnd=int(hwnd), pid=pid, thread=thread, title=self.window_title(hwnd),
            visible=bool(self.u.IsWindowVisible(hwnd)), desktop=self.name(self.thread_desktop(thread)),
            rect=[rect.left, rect.top, rect.right, rect.bottom])

    def startup_windows(self, desktop, expected):
        rows, errors, visits = [], [], 0
        def visit(hwnd, unused):
            nonlocal visits
            try:
                visits += 1
                _require(visits <= MAX_ENUMERATED_WINDOWS, "Desktop enumeration budget exceeded")
                pid, _ = self.window_owner(hwnd)
                if pid == expected["pid"]:
                    _require(len(rows) < MAX_WINDOWS, "owned window enumeration budget exceeded")
                    rows.append(self.window_details(hwnd))
                return True
            except Exception as error:
                errors.append(str(error))
                return False
        callback = self.callback_type(visit)
        ok = self.u.EnumDesktopWindows(desktop, callback, 0)
        _require(ok and not errors, "owned startup window enumeration failed: " + "; ".join(errors))
        return rows

    def print_window_to(self, hwnd, width, height, spool, cancelled):
        _, pixels = _layout(width, height)
        info = _BitmapInfoHeader(40, width, height, 1, 24, 0, pixels, 0, 0, 0, 0)
        dc, bitmap, previous = 0, 0, 0
        cleanup = []
        try:
            dc = self.check(self.g.CreateCompatibleDC(None), "startup memory DC")
            bits = wt.LPVOID()
            bitmap = self.check(self.g.CreateDIBSection(dc, ctypes.byref(info), 0,
                ctypes.byref(bits), None, 0), "startup bounded DIB")
            self.check(bits.value, "startup DIB data")
            previous = self.check(self.g.SelectObject(dc, bitmap), "select startup DIB")
            _require(previous != ctypes.c_void_p(-1).value, "invalid previous GDI object")
            # PrintWindow must paint the exact window. No desktop/screen DC,
            # BitBlt, or CopyFromScreen fallback exists on any failure path.
            self.check(self.u.PrintWindow(hwnd, dc, 2), "startup PrintWindow")
            self.check(self.g.GdiFlush(), "flush startup DIB")
            _require(not cancelled.is_set(), "PrintWindow returned after cancellation")
            spool.write(_bitmap_header(width, height))
            for offset in range(0, pixels, COPY_CHUNK):
                _require(not cancelled.is_set(), "startup bitmap copy cancelled")
                spool.write(ctypes.string_at(bits.value + offset, min(COPY_CHUNK, pixels - offset)))
        finally:
            if previous and previous != ctypes.c_void_p(-1).value:
                if not self.g.SelectObject(dc, previous):
                    cleanup.append("restore GDI object failed")
            if bitmap and not self.g.DeleteObject(bitmap):
                cleanup.append("delete startup DIB failed")
            if dc and not self.g.DeleteDC(dc):
                cleanup.append("delete startup DC failed")
            _require(not cleanup, "; ".join(cleanup))


def capture_startup_snapshot(native, *, launch, instance, desktop, output,
                             _api=None, _thread_factory=threading.Thread):
    """Capture once per NativeObserver; no readiness/capture IPC is performed.

    Fixed owned pixel staging <=15 MiB plus one <=64 KiB copy chunk (<16 MiB).
    This is not a bound on Windows/driver internals or total Python process RSS.
    Final artifact is created only after worker join, revalidation and cleanup.
    """
    result = dict(schema="warvk-native-startup-snapshot/v1", ok=False, imageAvailable=False,
        readyProven=False, gpuRenderingProven=False, visualContentProven=False,
        globalInputUsed=False, desktopSwitchRequested=False, forcedTermination=False,
        workerPending=False, captureMethod="PrintWindow", phase="qualification")
    spool, state, thread, created_output = None, None, None, None
    try:
        process, expected, input_name, output = _qualify(native, launch, instance, desktop, output)
        _require(not getattr(native, "_startup_snapshot_attempted", False), "startup snapshot already attempted")
        _UNSETTLED_WORKERS[:] = [(thread, state) for thread, state in _UNSETTLED_WORKERS if thread.is_alive()]
        _require(not _UNSETTLED_WORKERS, "another startup snapshot worker is still pending")
        native._startup_snapshot_attempted = True
        result["processIdentity"] = expected
        api = _api if _api is not None else _WindowsSnapshotApi(native)
        api.guard_desktop(expected["desktop"], input_name)
        copied = api.duplicate_process(process)
        state = _new_state()
        result["phase"] = "window-worker"
        try:
            thread = _thread_factory(target=_snapshot_worker,
                args=(api, copied, expected, input_name, str(output.parent), state),
                name="WarVK-Exact-Startup-Snapshot", daemon=True)
            thread.start()
        except Exception:
            api.close_process_copy(copied)
            raise
        thread.join(WORKER_TIMEOUT_SECONDS)
        with state["lock"]:
            if state["windowObservation"] is not None:
                result["windowObservation"] = copy.deepcopy(state["windowObservation"])
            if thread.is_alive():
                state["cancel"].set()
                # Completion may have raced join; cancel and remove any spool
                # atomically. No worker owns the final destination pathname.
                if state["spool"] is not None:
                    state["spool"].close()
                    state["spool"] = None
                result.update(workerPending=True, phase="worker-timeout")
                _UNSETTLED_WORKERS.append((thread, state))
                raise TimeoutError("startup PrintWindow worker did not settle; no image published")
            spool, state["spool"] = state["spool"], None
        result["worker"] = worker = copy.deepcopy(state["result"])
        _require(worker.get("printWindowSucceeded") is True and spool is not None and
            worker.get("workerDesktopRestored") is True and worker.get("desktopHandleClosed") is True and
            worker.get("processCopyClosed") is True and not worker.get("error") and
            not worker.get("cleanupErrors"), "startup worker failed or cleanup incomplete")
        _require(native.process == process and native.expected == expected and not native.pending,
                 "retained authority changed before image publication")
        native.verify_process()
        api.guard_desktop(expected["desktop"], input_name)
        result["phase"] = "publish"
        from run_native_d3d9_survey import checked_path, identity, bmp_dimensions
        _require(checked_path(output) == output and not output.exists(), "output identity changed")
        with output.open("xb") as destination:
            created_output = output
            for chunk in iter(lambda: spool.read(COPY_CHUNK), b""):
                destination.write(chunk)
        result["image"] = identity(output)
        _require(bmp_dimensions(output) == worker["dimensions"], "published bitmap dimensions mismatch")
        result.update(ok=True, imageAvailable=True, phase="complete")
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        if created_output is not None:
            # Preserve a partial write/failed-validation artifact as evidence,
            # never as imageAvailable or a successful diagnostic frame.
            result["unverifiedOutputPath"] = str(created_output)
    finally:
        if state is not None:
            # Also settle ownership if thread construction/join or any future
            # caller-side step raises unexpectedly. No late spool handoff.
            with state["lock"]:
                state["cancel"].set()
                if state["spool"] is not None:
                    state["spool"].close()
                    state["spool"] = None
            if thread is not None and thread.is_alive():
                result["workerPending"] = True
                if not any(item[0] is thread for item in _UNSETTLED_WORKERS):
                    _UNSETTLED_WORKERS.append((thread, state))
        if spool is not None:
            spool.close()
    return result
